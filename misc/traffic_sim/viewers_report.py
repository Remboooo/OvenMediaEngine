#!/usr/bin/env python3
"""Per-viewer quality for limits.py steps run with --viewers N: every viewer (the main one in the step
directory, the others in viewer_N/), per phase: frames the renderer dropped (getVideoPlaybackQuality; in Firefox decoded but never presented),
freezes (a gap over 3 frame periods and 150 ms between frames shown), the longest, and the send-to-display
delay median (presented 'when' against the relay's send time, main viewer only: the others are not traced).
Viewers that only assemble (--extra-viewers local) are judged on delivery instead: see delivery().

  ./viewers_report.py logs/<run>
"""
import json
import pathlib
import sys


def load(path):
    try:
        return [json.loads(line) for line in open(path)]
    except OSError:
        return []


def phase(pr, stats, t0, t1):
    sel = [p['when'] for p in pr if t0 <= p['when'] < t1]
    pb = [s['playback'] for s in stats if t0 <= s['t'] < t1 and s.get('playback')]
    drops = (pb[-1]['dropped'] - pb[0]['dropped'], pb[-1]['total'] - pb[0]['total']) if len(pb) > 1 else None
    if not (drops and drops[1]):
        # Firefox fills getVideoPlaybackQuality only for media elements it decodes itself (0 for WebRTC):
        # frames decoded (getStats) that the compositor never presented (requestVideoFrameCallback's count)
        dec = [(s['t'], s['video']['framesDecoded']) for s in stats
               if t0 <= s['t'] < t1 and (s.get('video') or {}).get('framesDecoded') is not None]
        shown = [p['n'] for p in pr if t0 <= p['when'] < t1 and p.get('n') is not None]
        if len(dec) > 1 and len(shown) > 1:
            total = dec[-1][1] - dec[0][1]
            drops = (max(0, total - (shown[-1] - shown[0])), total)
    inner = [b - a for a, b in zip(sel, sel[1:])]
    frame_ms = sorted(inner)[len(inner) // 2] if inner else 16.7
    # A freeze across the phase's edges counts too (one over the whole phase leaves no frame inside it):
    # bounded by the last frame shown before the phase and the phase's end
    before = [p['when'] for p in pr if p['when'] < t0]
    edges = ([max(before[-1], t0 - 30000)] if before else []) + sel + [t1]
    gaps = [b - a for a, b in zip(edges, edges[1:])]
    freezes = [g for g in gaps if g > max(150, 3 * frame_ms)]
    return drops, len(freezes), max(freezes, default=0) / 1000


def delivery(asm, t0, t1):
    """A viewer that assembles but does not decode (--extra-viewers local): frames missing from the RTP
    timestamp sequence, frames over 100 ms later than the phase's typical frame (one-way delay over its median),
    and freezes (a gap over 3 frame periods and 150 ms between assembled frames)."""
    seen, frames = set(), []
    for f in asm:
        if t0 <= f['t'] < t1 and f['rtp'] not in seen:
            seen.add(f['rtp'])
            frames.append(f)
    if len(frames) < 2:
        return 'nothing assembled'
    rtps = sorted(f['rtp'] for f in frames)
    step = sorted(b - a for a, b in zip(rtps, rtps[1:]))[len(rtps) // 2] or 1
    # the source's timestamps jitter around the frame period: count whole periods missing per gap
    missing = sum(max(0, round((b - a) / step) - 1) for a, b in zip(rtps, rtps[1:]))
    expected = len(frames) + missing
    delay = [f['t'] - f['rtp'] / 90 for f in frames]
    # against the phase's median, not its minimum: a phase that adds path delay starts with frames sent before it
    typical = sorted(delay)[len(delay) // 2]
    late = sum(x - typical > 100 for x in delay)
    ts = sorted(f['t'] for f in frames)
    gaps = [b - a for a, b in zip(ts, ts[1:])]
    freezes = [g for g in gaps if g > max(150, 3 * step / 90)]
    return (f"{100 * missing / expected:.1f}% missing, {100 * late / len(frames):.1f}% late, "
            f"{len(freezes)} freeze(s) longest {max(freezes, default=0) / 1000:.2f} s")


def main(run):
    for step in sorted(pathlib.Path(run, 'steps').iterdir()):
        try:
            res = json.loads((step / 'result.json').read_text())
        except (OSError, ValueError):
            continue
        ph = res.get('phases_ms')
        if not ph:
            continue
        viewers = [('main', step)] + [(p.name, p) for p in sorted(step.glob('viewer_*')) if p.is_dir()]
        print(f"{res['step']}  ({len(viewers)} viewer(s))")
        for name, d in viewers:
            pr, stats = load(d / 'presented.jsonl'), load(d / 'stats.jsonl')
            asm = load(d / 'assembled.jsonl')
            if not pr and asm:
                print(f"  {name:9} " + ' | '.join(f"{label}: {delivery(asm, t0, t1)}" for label, t0, t1 in (
                    ('clean', ph['connect'], ph['impair']), ('impair', ph['impair'], ph['recover']), ('recover', ph['recover'], ph['end']))))
                continue
            if not pr:
                print(f"  {name:9} nothing shown")
                continue
            first = min(p['when'] for p in pr)
            parts = []
            for label, t0, t1 in (('clean', max(ph['connect'], first), ph['impair']), ('impair', ph['impair'], ph['recover']),
                                  ('recover', ph['recover'], ph['end'])):
                drops, nfz, longest = phase(pr, stats, t0, t1)
                dr = f"{100 * drops[0] / drops[1]:.1f}% dropped" if drops and drops[1] else 'drops ?'
                parts.append(f"{label}: {dr}, {nfz} freeze(s) longest {longest:.2f} s")
            print(f"  {name:9} " + ' | '.join(parts))


if __name__ == '__main__':
    for r in sys.argv[1:]:
        main(r)
