#!/usr/bin/env python3
"""What a decoding viewer (webrtc_oracle.mjs, normally a real desktop browser) experienced in one smoke-test run.

    viewer_check.py VIEWER_DIR [--trace RELAY_TRACE] [--from-ms EPOCH_MS] [--to-ms EPOCH_MS]

Prints one JSON object: frames shown and dropped (the browser's own counts), freezes (W3C freeze definition, as
limits.py), its jitter-buffer delay, and the send-to-display delay per frame (relay trace:
when OME sent the frame's first packet -> when the compositor showed it). The window defaults to 3 s after the first
frame shown until 1 s before the last.
"""
import argparse
import json
import statistics
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'traffic_sim'))
from limits import jb_delay, load_jsonl, render_events, stat_delta  # noqa: E402


def first_sent(trace_path, t_from, t_to):
    """(ssrc, rtp ts) -> epoch ms the relay first saw a packet of it from OME, within the window (plus slack)."""
    out = {}
    lo, hi = (t_from - 5000) / 1000, (t_to + 1000) / 1000
    with open(trace_path) as f:
        for line in f:
            if ',' in line:
                # traffic_sim's sim_relay trace (sim_relay.TRACE_FIELDS: t,dir,fate,due_ms,kind,size,ssrc,pt,seq,ts,marker)
                p = line.rstrip('\n').split(',')
                if len(p) < 11 or p[1] != 'd' or p[4] != 'rtp' or not p[6] or not p[9]:
                    continue
                t, key = float(p[0]), (int(p[6]), int(p[9]))
            else:
                p = line.split()
                if len(p) < 8 or p[1] != 'd':
                    continue
                t, key = float(p[0]), (int(p[2]), int(p[5]))
            if lo <= t <= hi:
                out.setdefault(key, t * 1000)
    return out


def check(d, trace=None, t_from=None, t_to=None):
    meta = json.loads((d / 'meta.json').read_text()) if (d / 'meta.json').exists() else {}
    presented = load_jsonl(d / 'presented.jsonl')
    stats = load_jsonl(d / 'stats.jsonl')
    if not presented:
        return dict(ok=False, error=meta.get('error') or 'nothing shown')
    t_from = t_from or presented[0]['when'] + 3000
    t_to = t_to or presented[-1]['when'] - 1000
    rtps = sorted({r['rtp'] for r in presented if r.get('rtp') is not None})
    deltas = sorted(((b - a) & 0xFFFFFFFF) for a, b in zip(rtps, rtps[1:]) if 0 < ((b - a) & 0xFFFFFFFF) < 9000)
    # 59.94/60 fps alternates 1440/1530 ticks: average the normal deltas (skipped frames excluded), as smoke_test.sh
    med = statistics.median(deltas) if deltas else 1500
    normal = [x for x in deltas if x < 1.5 * med]
    frame_ms = statistics.mean(normal) / 90 if normal else 1000 / 60
    ev = render_events(presented, frame_ms, t_from, t_to)
    secs = (t_to - t_from) / 1000
    # Frames shown: the compositor's own count (requestVideoFrameCallback fires at most once per refresh, so at
    # 60 fps its callbacks miss frames that were shown). Dropped: Chrome's getVideoPlaybackQuality; Firefox fills
    # that only for media it decodes itself, so there frames decoded (getStats) minus frames presented.
    n = [r['n'] for r in presented if t_from <= r['when'] < t_to and r.get('n') is not None]
    shown = n[-1] - n[0] if len(n) > 1 else ev['frames']
    pb = [x['playback'] for x in stats if t_from <= x['t'] < t_to and x.get('playback')]
    dec = stat_delta(stats, t_from, t_to, 'video', 'framesDecoded')
    if len(pb) > 1 and pb[-1]['total'] - pb[0]['total'] > 0:
        dropped, total = pb[-1]['dropped'] - pb[0]['dropped'], pb[-1]['total'] - pb[0]['total']
    else:
        dropped, total = (max(0, dec - shown), dec) if dec else (None, None)
    expected = secs * 1000 / frame_ms
    res = dict(ok=True, browser=('firefox' if str(meta.get('remote', '')).startswith('ws') else 'chrome') + ('' if meta.get('remote') else ' (headless, local)'),
               window_s=round(secs, 1), frame_ms=round(frame_ms, 2),
               shown=shown, shown_ratio=round(min(1.0, shown / expected), 4) if expected else None,
               dropped=dropped, dropped_ratio=round(dropped / total, 4) if total else None,
               freezes=len(ev['freezes']), freeze_s=round(sum(g for _, g in ev['freezes']) / 1000, 2),
               longest_freeze_s=round(max([g for _, g in ev['freezes']], default=0) / 1000, 2),
               freeze_list=[[round(t), round(g)] for t, g in ev['freezes']])
    for k in ('framesDecoded', 'freezeCount', 'totalFreezesDuration', 'pliCount', 'nackCount', 'packetsLost'):
        res[k] = stat_delta(stats, t_from, t_to, 'video', k)
    jb = jb_delay(stats, t_from, t_to)
    res['jitter_buffer_ms'] = round(jb) if jb is not None else None
    if trace and Path(trace).exists():
        sent = first_sent(trace, t_from, t_to)
        by_ts = {}
        for (ssrc, ts), t in sent.items():
            by_ts.setdefault(ts, []).append(t)
        e2e = []
        for r in presented:
            if t_from <= r['when'] < t_to and r.get('rtp') in by_ts:
                # OME sends a frame to every session within a few ms: the earliest copy is the send time
                e2e.append(r['when'] - min(by_ts[r['rtp']]))
        e2e = [x for x in e2e if 0 < x < 10000]
        if e2e:
            e2e.sort()
            res['e2e_ms'] = dict(p50=round(e2e[len(e2e) // 2]), p95=round(e2e[int(len(e2e) * 0.95)]), n=len(e2e))
    return res


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('dir', type=Path)
    ap.add_argument('--trace')
    ap.add_argument('--from-ms', type=float)
    ap.add_argument('--to-ms', type=float)
    a = ap.parse_args()
    print(json.dumps(check(a.dir, a.trace, a.from_ms, a.to_ms)))


if __name__ == '__main__':
    main()
