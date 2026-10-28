#!/usr/bin/env python3
"""Turn smoke-test runs into the per-profile baseline that smoke_test.sh judges against.

    smoke_baseline.py logs/smoke_A logs/smoke_B ... [--out profiles/baseline.json]

Reads each run's results.jsonl and records, per profile, every run's frames-complete ratio (the probe's first
rendition) and each decoding viewer's freeze count and dropped-frame ratio. smoke_test.sh then fails a profile below the lowest run
minus a point, or a viewer with more freezes than the worst run plus one, or more frames dropped than the worst run plus a point. Use runs of the build that is in production,
on an otherwise idle machine, at least three per profile: the same code varies by several points under loss.
"""
import argparse
import json
from pathlib import Path

ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument('runs', nargs='+', type=Path)
ap.add_argument('--out', type=Path, default=Path(__file__).resolve().parent / 'profiles' / 'baseline.json')
a = ap.parse_args()

base = {}
for run in a.runs:
    for line in open(run / 'results.jsonl'):
        r = json.loads(line)
        b = base.setdefault(r['profile'], dict(complete=[], freezes={}, runs=[]))
        probe = next((v for k, v in r.items() if k.startswith('probe_')), None)
        if probe:
            b['complete'].append(probe['complete'])
        for k, v in r.items():
            if k.startswith('viewer_') and v.get('ok'):
                b['freezes'].setdefault(k[len('viewer_'):], []).append(v['freezes'])
                b.setdefault('dropped', {}).setdefault(k[len('viewer_'):], []).append(v['dropped_ratio'] or 0)
        b['runs'].append(run.name)
a.out.write_text(json.dumps(base, indent=1) + '\n')
for p, b in base.items():
    print(f"{p:16} complete {min(b['complete'], default=0):.1%}..{max(b['complete'], default=0):.1%}  freezes {b['freezes']}")
