#!/usr/bin/env python3
"""How evenly OME sends a stream's frames: per smoke-test run and profile, each video frame's send time (the relay
trace: first packet from OME) against its RTP timestamp, relative to the earliest frame of the window. A source's
arrival jitter that the server passes on shows here; viewers buffer for it.

    send_timing.py logs/cmp_*_r* [--out rows.json]
"""
import argparse
import collections
import json
from pathlib import Path


def run_rows(d):
    res = [json.loads(l) for l in open(d / 'results.jsonl')]
    trace = d / 'relay_trace.txt'
    if not trace.exists():
        return []
    windows = [(r['profile'], r['t'] / 1000 + 10, r['t'] / 1000 + 55) for r in res]
    frames = {p: collections.defaultdict(list) for p, *_ in windows}
    counts = {p: collections.Counter() for p, *_ in windows}
    with open(trace) as f:
        for line in f:
            x = line.split()
            if len(x) < 8 or x[1] != 'd':
                continue
            t = float(x[0])
            for p, lo, hi in windows:
                if lo <= t <= hi:
                    frames[p][(x[2], int(x[5]))].append(t)
                    counts[p][x[2]] += 1
                    break
    out = []
    for p, *_ in windows:
        if not counts[p]:
            continue
        v = max(counts[p], key=counts[p].get)  # the video SSRC carries most packets
        off = sorted((min(ts_) - ts / 90000) * 1000 for (s, ts), ts_ in frames[p].items() if s == v)
        m = off[0]
        dev = [x - m for x in off]
        q = lambda f: round(dev[int(len(dev) * f)], 1)
        out.append(dict(run=d.name, build=d.name.split('_')[1], profile=p, frames=len(dev), p50_ms=q(.5), p95_ms=q(.95), p99_ms=q(.99)))
    return out


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('runs', nargs='+', type=Path)
    ap.add_argument('--out', type=Path)
    a = ap.parse_args()
    rows = [r for d in a.runs for r in run_rows(d)]
    if a.out:
        a.out.write_text(json.dumps(rows))
    for r in rows:
        print(f"{r['run']:18} {r['profile']:15} p50 {r['p50_ms']:6} p95 {r['p95_ms']:6} p99 {r['p99_ms']:6}")
