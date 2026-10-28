#!/usr/bin/env python3
"""Structural H.264 check of a webrtc_bitstream_probe capture (no decoding).

Per frame (frames.jsonl gives byte ranges into probe.h264):
  - NAL types present, slice count, first_mb_in_slice layout
  - frame_num continuity (gaps => a reference frame never reached the decoder)
  - IDR/key consistency, SPS/PPS presence on keyframes
Prints anomalies with wall-clock time so they can be joined with OME logs.

Usage: h264_frame_check.py <probe_dir>
"""
import collections
import datetime as dt
import json
import sys


def split_nals(buf):
    i, n, starts = 0, len(buf), []
    while i + 3 <= n:
        if buf[i] == 0 and buf[i + 1] == 0 and (buf[i + 2] == 1 or (buf[i + 2] == 0 and i + 3 < n and buf[i + 3] == 1)):
            sc = 3 if buf[i + 2] == 1 else 4
            starts.append((i, i + sc))
            i += sc
        else:
            i += 1
    return [buf[s:(starts[k + 1][0] if k + 1 < len(starts) else n)] for k, (_, s) in enumerate(starts)]


def rbsp(nal, limit=64):
    out, zeros = bytearray(), 0
    for b in nal[1:limit]:
        if zeros >= 2 and b == 3:
            zeros = 0
            continue
        out.append(b)
        zeros = zeros + 1 if b == 0 else 0
    return bytes(out)


class Bits:
    def __init__(self, data):
        self.d, self.p = data, 0

    def u(self, n):
        v = 0
        for _ in range(n):
            v = (v << 1) | ((self.d[self.p >> 3] >> (7 - (self.p & 7))) & 1)
            self.p += 1
        return v

    def ue(self):
        z = 0
        while self.u(1) == 0:
            z += 1
        return (1 << z) - 1 + self.u(z)


def parse_sps(nal):
    b = Bits(rbsp(nal, 128))
    profile = b.u(8)
    b.u(16)
    b.ue()
    if profile in (100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135):
        cf = b.ue()
        if cf == 3:
            b.u(1)
        b.ue(); b.ue(); b.u(1)
        if b.u(1):
            raise ValueError('scaling matrices not supported by this checker')
    log2_max_frame_num = b.ue() + 4
    poc_type = b.ue()
    return {'profile': profile, 'log2_max_frame_num': log2_max_frame_num, 'poc_type': poc_type}


def main(d):
    data = open(f'{d}/probe.h264', 'rb').read()
    frames = [json.loads(l) for l in open(f'{d}/frames.jsonl')]
    # The encoded transform sits before Chrome's frame buffer, so frames that a
    # retransmission completed late arrive after their successors. Check them in
    # RTP order, as the decoder sees them.
    base = frames[0]['rtp'] if frames else 0
    frames.sort(key=lambda f: (f['rtp'] - base) & 0xFFFFFFFF)
    # A frame re-assembled after a spurious retransmission is handed over again;
    # the frame buffer drops the repeat, so keep the first copy only.
    seen = set()
    frames = [f for f in frames if not (f['rtp'] in seen or seen.add(f['rtp']))]
    sps = None
    prev_fn = None
    layout_counter = collections.Counter()
    per_frame = []
    anomalies = []
    for idx, f in enumerate(frames):
        if f['offset'] + f['size'] > len(data):
            break
        nals = split_nals(data[f['offset']:f['offset'] + f['size']])
        types = [n[0] & 0x1F for n in nals if n]
        slices = []
        for n in nals:
            t = n[0] & 0x1F
            if t == 7:
                sps = parse_sps(n)
            elif t in (1, 5) and sps:
                b = Bits(rbsp(n))
                first_mb = b.ue()
                slice_type = b.ue() % 5
                b.ue()  # pps id
                frame_num = b.u(sps['log2_max_frame_num'])
                slices.append((first_mb, slice_type, frame_num, len(n), t, (n[0] >> 5) & 3))
        if sps is None:
            continue  # capture started mid-GOP: nothing parseable before the first SPS
        rec = {'idx': idx, 't': f['t'], 'type': f['type'], 'nal_types': types, 'slices': len(slices),
               'layout': tuple(s[0] for s in slices)}
        problems = []
        if not slices:
            problems.append('no slice NAL in frame')
        else:
            fns = {s[2] for s in slices}
            if len(fns) > 1:
                problems.append(f'mixed frame_num across slices {sorted(fns)}')
            fn = slices[0][2]
            is_idr = slices[0][4] == 5
            if (f['type'] == 'key') != is_idr:
                problems.append(f"key flag {f['type']} but IDR={is_idr}")
            if is_idr and (7 not in types or 8 not in types):
                problems.append('IDR without SPS/PPS in same frame')
            if prev_fn is not None and not is_idr:
                expect = (prev_fn[0] + (1 if prev_fn[1] else 0)) % (1 << sps['log2_max_frame_num'])
                if fn != expect:
                    problems.append(f'frame_num {fn} expected {expect} (missing reference frame upstream?)')
            if slices[0][0] != 0:
                problems.append(f'first slice starts at MB {slices[0][0]} (leading slice missing)')
            if any(s[3] < 8 for s in slices):
                problems.append('suspiciously short slice NAL')
            prev_fn = (fn, slices[0][5] != 0)
            layout_counter[rec['layout']] += 1
        if problems:
            anomalies.append((rec, problems))
        per_frame.append(rec)

    common_layout = layout_counter.most_common(1)[0][0] if layout_counter else ()
    print(f'frames checked: {len(per_frame)}   SPS: {sps}')
    print(f'slice layouts: {len(layout_counter)} distinct; most common ({layout_counter[common_layout]}x): {common_layout}')
    for lay, c in layout_counter.most_common(6)[1:]:
        print(f'   other layout {c}x: {lay}')
    for rec in per_frame:
        if rec['slices'] and rec['layout'] != common_layout and len(layout_counter) > 1 and layout_counter[rec['layout']] < 3:
            anomalies.append((rec, [f"rare slice layout {rec['layout']} (common {common_layout})"]))
    anomalies.sort(key=lambda a: a[0]['idx'])
    print(f'anomalous frames: {len(anomalies)}')
    for rec, problems in anomalies[:200]:
        wall = dt.datetime.fromtimestamp(rec['t'] / 1000).strftime('%H:%M:%S.%f')[:-3]
        print(f"  #{rec['idx']} {wall} {rec['type']} nals={rec['nal_types']} :: {'; '.join(problems)}")


if __name__ == '__main__':
    main(sys.argv[1])
