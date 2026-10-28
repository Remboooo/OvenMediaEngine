#!/usr/bin/env python3
"""ABR settling per phase of limits.py `rate` steps run with auto ABR (--playlist abr --rendition auto).

For each phase (clean, impair = the bottleneck, recover) it prints the rung timeline the viewer showed, when
the rung last changed (the settle time, from the phase start), and whether the final rung is the best the
link can carry. Goal: the best rung within 20 s, and no change after that.

  ./abr_settle.py logs/<run>            # every step of a run
"""
import collections
import csv
import json
import pathlib
import sys

# Video bitrate per rung (the abr playlist, conf/Server.xml). A rung fits a link when its wire rate (~1.2x:
# ULPFEC, audio, RTX, headers; measured 7.2 Mbps for the 6 Mbps rung) is at most ~80% of it: above that its
# keyframe bursts delay the frames behind them (1080p at 83% of a link dropped 8% of its frames). The same
# rule as RtcBandwidthEstimator::kStayHeadroom.
RUNGS = {1080: 6.0, 720: 2.5, 480: 0.5}
WIRE_OVERHEAD = 1.5
SETTLE_GOAL_S = 20


def best_rung(link_mbps):
    fit = [h for h, mbps in RUNGS.items() if link_mbps is None or mbps * WIRE_OVERHEAD <= link_mbps]
    return max(fit) if fit else min(RUNGS)


def phase_report(frames, t0, t1, link_mbps):
    sel = [f for f in frames if t0 <= f['when'] < t1]
    if not sel:
        return None
    changes = [(sel[0]['when'], sel[0]['h'])]
    for f in sel[1:]:
        if f['h'] != changes[-1][1]:
            changes.append((f['when'], f['h']))
    final = collections.Counter(f['h'] for f in sel if f['when'] >= t1 - 3000).most_common(1)[0][0]
    settle_s = (changes[-1][0] - t0) / 1000 if len(changes) > 1 else 0.0
    best = best_rung(link_mbps)
    ok = final == best and settle_s <= SETTLE_GOAL_S
    timeline = ' '.join(f"{(t - t0) / 1000:.1f}s:{h}" for t, h in changes)
    return dict(final=final, best=best, settle_s=round(settle_s, 1), switches=len(changes) - 1, ok=ok, timeline=timeline)


def sent_frames(step):
    """RTP timestamp -> time OME sent the frame's marker packet (relay trace, --keep-trace)."""
    out = {}
    try:
        for r in csv.DictReader(open(step / 'trace.csv')):
            if r['dir'] == 'd' and r['kind'] == 'rtp' and r['pt'] in ('98', '120') and r['marker'] == '1':
                out.setdefault(int(r['ts']), float(r['t']) * 1000)
    except OSError:
        pass
    return out


def shown_ratio(sent, frames, t0, t1):
    """Frames shown / frames OME sent in [t0, t1): drops at any rung's frame rate."""
    window = {ts for ts, t in sent.items() if t0 <= t < t1}
    if not window:
        return None
    shown = {f['rtp'] for f in frames}
    return 100 * len(window & shown) / len(window)


def render_drops(stats, t0, t1):
    """Frames the renderer dropped before display in [t0, t1) (getVideoPlaybackQuality): dropped, total."""
    sel = [s['playback'] for s in stats if t0 <= s['t'] < t1 and s.get('playback')]
    if len(sel) < 2:
        return None
    return sel[-1]['dropped'] - sel[0]['dropped'], sel[-1]['total'] - sel[0]['total']


def main(run):
    for step in sorted(pathlib.Path(run, 'steps').iterdir()):
        try:
            res = json.loads((step / 'result.json').read_text())
            frames = [json.loads(line) for line in open(step / 'presented.jsonl')]
        except (OSError, ValueError):
            continue
        ph = res.get('phases_ms')
        if not ph or not frames:
            continue
        link = res.get('link_mbps')
        sent = sent_frames(step)
        try:
            stats = [json.loads(line) for line in open(step / 'stats.jsonl')]
        except OSError:
            stats = []
        imp, c = res.get('impair') or {}, res.get('chrome') or {}
        print(f"{res['step']}  (link {link} Mbps)  impair: {imp.get('freezes')} freeze(s), longest {imp.get('max_freeze_s')} s, "
              f"{imp.get('skipped')} skipped; PLIs {c.get('pliCount')}")
        # The clean phase starts at the first frame shown: frames sent while the viewer joined (before the
        # first keyframe) are not drops
        first_shown = min(f['when'] for f in frames)
        for name, t0, t1, l in (('clean', max(ph['connect'], first_shown), ph['impair'], None), ('impair', ph['impair'], ph['recover'], link),
                                ('recover', ph['recover'], ph['end'], None)):
            r = phase_report(frames, t0, t1, l)
            if r:
                ratio = shown_ratio(sent, frames, t0, t1)
                shown = '' if ratio is None else f", {ratio:.1f}% of sent frames called back"
                rd = render_drops(stats, t0, t1)
                if rd and rd[1]:
                    shown += f", renderer dropped {rd[0]}/{rd[1]} ({100 * rd[0] / rd[1]:.1f}%)"
                print(f"  {name:8} {'ok  ' if r['ok'] else 'MISS'} final {r['final']}p (best {r['best']}p), settled {r['settle_s']} s, "
                      f"{r['switches']} switch(es){shown}: {r['timeline']}")


if __name__ == '__main__':
    for run in sys.argv[1:]:
        main(run)
