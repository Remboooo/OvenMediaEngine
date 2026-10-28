#!/usr/bin/env python3
"""Byte-for-byte delivery check of a limits.py probe capture against reference probes on a clean path.

Run limits.py with --keep-capture --reference-probe 1080p,720p,480p (one pinned reference per rung). Every
frame the impaired probe captured must equal a frame a reference captured with the same RTP timestamp: OME
sent it, the network may delay or lose it, but never change it. A frame that differs from every reference
while a reference has a frame of about its size (the same rung) at that timestamp is corrupt delivery. (The offline decoder alone cannot tell: at ABR
switches and late repairs it mixes rungs and misattributes errors.)

  ./delivery_check.py logs/<run>/steps/<step> [...]
"""
import json
import pathlib
import sys


def frames(pd):
    data = (pd / 'probe.h264').read_bytes()
    out = {}
    for line in open(pd / 'assembled.jsonl'):
        f = json.loads(line)
        out.setdefault(f['rtp'], []).append((f, data[f['offset']:f['offset'] + f['size']]))
    return out


def strip_optional_nals(b):
    """The frame without access unit delimiters, SEI and parameter sets (NAL types 9, 6, 7, 8), wherever they
    are: units that travel in a packet of their own (a STAP-A ahead of the slice), so a viewer still assembles
    the frame when that packet is lost. A keyframe without its SPS/PPS decodes with the stream's earlier,
    identical ones (a change of parameter sets would be masked here). OME no longer sends AUD and timing SEI
    (cd480268b), so on a keyframe that packet is SPS + PPS: 49 bytes on the gpu4k rungs."""
    starts = []
    i = 0
    while True:
        j = b.find(b'\x00\x00\x01', i)
        if j < 0:
            break
        starts.append(j - 1 if j > 0 and b[j - 1] == 0 else j)
        i = j + 3
    if not starts:
        return b
    out = b[:starts[0]]
    for k, st in enumerate(starts):
        end = starts[k + 1] if k + 1 < len(starts) else len(b)
        unit = b[st:end]
        header = unit[4] if unit.startswith(b'\x00\x00\x00\x01') else unit[3] if len(unit) > 3 else 0
        if (header & 0x1F) not in (6, 7, 8, 9):
            out += unit
    return out


def check(step):
    """(matched, missing optional NALs, corrupt, uncovered, examples) for one step directory."""
    step = pathlib.Path(step)
    probe = frames(step / 'probe')
    refs = [frames(p) for p in sorted(step.glob('reference_*')) if p.is_dir() and (p / 'probe.h264').exists()]
    matched = optional = corrupt = uncovered = 0
    examples = []
    # Judge only where every reference was capturing: past a reference's end its rung's frames would be
    # compared with another rung's frames of about the same size
    first = max((min(r) for r in refs if r), default=0)
    last = min((max(r) for r in refs if r), default=0)
    for ts, copies in probe.items():
        candidates = [b for r in refs for _, b in r.get(ts, [])]
        if not candidates or not first <= ts <= last:
            uncovered += len(copies)
            continue
        for f, b in copies:
            if b in candidates:
                matched += 1
            elif strip_optional_nals(b) in [strip_optional_nals(c) for c in candidates]:
                optional += 1
            elif not any(abs(len(c) - len(b)) <= 0.2 * len(b) + 64 for c in candidates):
                # Only other rungs' frames at this timestamp: the same rung's reference missed it
                uncovered += 1
            else:
                corrupt += 1
                if len(examples) < 5:
                    examples.append(f"rtp {ts} {f['type']} {f['size']} B (references: {sorted(len(c) for c in candidates)})")
    return dict(references=len(refs), matched=matched, missing_optional_nals=optional, corrupt=corrupt,
                uncovered=uncovered, examples=examples)


def main(step):
    r = check(step)
    print(f"{step}: {r['references']} reference(s); frames matching a reference {r['matched']}, "
          f"the same without AUD/SEI/SPS/PPS {r['missing_optional_nals']}, differing from all {r['corrupt']}, "
          f"no reference at that timestamp {r['uncovered']}")
    for e in r['examples']:
        print('   ', e)


if __name__ == '__main__':
    for s in sys.argv[1:]:
        main(s)
