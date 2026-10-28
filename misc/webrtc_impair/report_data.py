#!/usr/bin/env python3
"""Collect compare_builds.sh results (smoke and limits) plus best_case.py's model into one JSON for the report.

    report_data.py [--out report_data.json]

Smoke: logs/cmp_<build>_r<N>/results.jsonl, per profile and build: each run's probe completeness and buffer clears,
and per browser freezes, freeze time, dropped frames and send-to-display delay. Limits:
../traffic_sim/logs/cmp_lim_<build>_<browser>/results.json, per step: the impaired window's freezes, freeze time,
dropped frames and delay. Every value keeps its runs, so the report can show the spread, not just a mean.
"""
import argparse
import json
import statistics
from pathlib import Path

import best_case

HERE = Path(__file__).resolve().parent
SIM_LOGS = HERE.parent / 'traffic_sim' / 'logs'


def smoke():
    out = {}
    for d in sorted(HERE.glob('logs/cmp_*_r*')):
        build = d.name.split('_')[1]
        f = d / 'results.jsonl'
        if not f.exists():
            continue
        for line in f.read_text().splitlines():
            r = json.loads(line)
            row = out.setdefault(r['profile'], {}).setdefault(build, dict(complete=[], buffer_clears=[], decode_errors=[], viewers={}))
            probe = next((v for k, v in r.items() if k.startswith('probe_')), None)
            if probe:
                row['complete'].append(probe['complete'])
                row['buffer_clears'].append(probe['buffer_clears'])
            row['decode_errors'] += [v for k, v in r.items() if k.startswith('decode_errors_')]
            for k, v in r.items():
                if not k.startswith('viewer_'):
                    continue
                vw = row['viewers'].setdefault(k[len('viewer_'):], dict(freezes=[], freeze_s=[], longest_s=[], dropped=[], e2e_p50=[], e2e_p95=[],
                                                                        jb_ms=[], failed=0))
                if not v.get('ok'):
                    vw['failed'] += 1
                    continue
                vw['freezes'].append(v['freezes'])
                vw['freeze_s'].append(v['freeze_s'])
                vw['longest_s'].append(v['longest_freeze_s'])
                vw['dropped'].append(v['dropped_ratio'])
                e = v.get('e2e_ms') or {}
                vw['e2e_p50'].append(e.get('p50'))
                vw['e2e_p95'].append(e.get('p95'))
                vw['jb_ms'].append(v.get('jitter_buffer_ms'))
    return out


def limits():
    out = {}
    for d in sorted(SIM_LOGS.glob('cmp_lim*_*_*')):
        _, kind, build, browser = d.name.split('_', 3)
        stream = kind[3:] or 'gpu4k'
        f = d / 'results.json'
        if not f.exists():
            continue
        for r in json.loads(f.read_text()):
            i = r.get('impair', {})
            e = r.get('e2e_ms', {})
            out.setdefault(f"{stream}:{r['step']}", dict(stream=stream, dim=r['dim'], value=r['value'], ctx=r.get('ctx'), runs={}))['runs'][f'{build}/{browser}'] = dict(
                freezes=i.get('freezes'), freeze_s=i.get('freeze_s'), longest_s=i.get('max_freeze_s'), dropped=i.get('dropped'),
                stutters=i.get('stutters'), e2e_clean=e.get('clean'), e2e_impair=e.get('impair'), e2e_tail=e.get('tail'),
                plis=(r.get('chrome') or {}).get('pliCount'), fails=r.get('fails'), pred=r.get('pred', {}).get('verdict'))
    return out


def model(lim):
    """The best case per smoke profile, and per limits step for each stream, with that stream's measured pps, bitrate,
    keyframe size and frame period (the first step's baseline of the patched run; best_case.STREAM_1080 if none)."""
    sm = best_case.smoke_profiles(HERE / 'profiles')
    steps = {}
    for stream in sorted({row['stream'] for row in lim.values()}) or ['gpu4k']:
        ctxs = [row['ctx'] for row in lim.values() if row['stream'] == stream and (row.get('ctx') or {}).get('pps')]
        c = ctxs[0] if ctxs else {}
        d = best_case.STREAM_1080
        ctx = dict(d, pps=c.get('pps', d['pps']), bitrate_bps=c.get('bitrate_bps', d['bitrate_bps']),
                   kf_bytes=c.get('kf_bytes') or d['kf_bytes'], frame_ms=c.get('frame_ms') or d['frame_ms'])
        for name, b in best_case.limits_steps(ctx).items():
            steps[f'{stream}:{name}'] = b
    return dict(smoke=sm, limits=steps)


def med(xs):
    xs = [x for x in xs if x is not None]
    return statistics.median(xs) if xs else None


def rows(out_dir):
    """Flat row files for the report's charts: one JSON array of row objects each."""
    out_dir.mkdir(parents=True, exist_ok=True)
    smoke_rows = []
    for d in sorted(HERE.glob('logs/cmp_*_r*')):
        build, run = d.name.split('_')[1], int(d.name.split('_r')[-1])
        f = d / 'results.jsonl'
        if not f.exists():
            continue
        for line in f.read_text().splitlines():
            r = json.loads(line)
            probe = next((v for k, v in r.items() if k.startswith('probe_')), {}) or {}
            for k, v in r.items():
                if not k.startswith('viewer_') or not v.get('ok'):
                    continue
                e = v.get('e2e_ms') or {}
                smoke_rows.append(dict(profile=r['profile'], build=build, browser=k[len('viewer_'):], run=run,
                                       window_s=v['window_s'], freezes=v['freezes'],
                                       freezes_per_min=round(v['freezes'] * 60 / v['window_s'], 2),
                                       freeze_s=v['freeze_s'], frozen_share=round(v['freeze_s'] / v['window_s'], 4),
                                       longest_freeze_s=v['longest_freeze_s'], dropped=v['dropped_ratio'],
                                       e2e_p50_ms=e.get('p50'), e2e_p95_ms=e.get('p95'), jitter_buffer_ms=v.get('jitter_buffer_ms'),
                                       plis=v.get('pliCount'), probe_complete=probe.get('complete'),
                                       probe_buffer_clears=probe.get('buffer_clears')))
    lim_rows = []
    for d in sorted(SIM_LOGS.glob('cmp_lim*_*_*')):
        _, kind, build, browser = d.name.split('_', 3)
        stream = kind[3:] or 'gpu4k'
        f = d / 'results.json'
        if not f.exists():
            continue
        for r in json.loads(f.read_text()):
            i, e = r.get('impair', {}), r.get('e2e_ms', {})
            win = (r['phases_ms']['recover'] - r['phases_ms']['impair']) / 1000 + 2 if r.get('phases_ms') else 32
            lim_rows.append(dict(stream=stream, step=f"{r['dim']}={r['value']}", dim=r['dim'], value=r['value'], build=build, browser=browser,
                                 freezes=i.get('freezes'), freezes_per_min=round(i['freezes'] * 60 / win, 2) if i.get('freezes') is not None else None,
                                 freeze_s=i.get('freeze_s'), frozen_share=round(i['freeze_s'] / win, 4) if i.get('freeze_s') is not None else None,
                                 longest_freeze_s=i.get('max_freeze_s'), dropped=i.get('dropped'),
                                 e2e_clean_ms=e.get('clean'), e2e_impair_ms=e.get('impair'), e2e_tail_ms=e.get('tail'),
                                 plis=(r.get('chrome') or {}).get('pliCount'), verdict='FAIL' if r.get('fails') else 'ok'))
    best_rows = []
    m = model(limits())
    for name, b in m['smoke'].items():
        best_rows.append(dict(scope='smoke', condition=name, freezes_per_min=round(b['freezes'] * 60 / 52, 2),
                              frozen_share=round(b['freeze_s'] / 52, 4), e2e_ms=b['e2e_ms'], buffer_ms=b['buffer_ms'],
                              buffer_for=b['buffer_for'], why=b['why']))
    for name, b in m['limits'].items():
        stream, step = name.split(':')
        dim, val = step.split('=')
        best_rows.append(dict(scope='limits', stream=stream, condition=name, dim=dim, value=float(val), freezes_per_min=round(b['freezes'] * 60 / 32, 2),
                              frozen_share=round(b['freeze_s'] / 32, 4), e2e_ms=b['e2e_ms'], buffer_ms=b['buffer_ms'],
                              buffer_for=b['buffer_for'], why=b['why']))
    for name, data in (('smoke_runs', smoke_rows), ('limits_runs', lim_rows), ('best_case', best_rows)):
        (out_dir / f'{name}.json').write_text(json.dumps(data))
        print(f'{name}: {len(data)} rows')


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--out', type=Path, default=HERE / 'logs' / 'report_data.json')
    ap.add_argument('--rows', type=Path, help='also write flat row files (smoke_runs, limits_runs, best_case .json) here')
    a = ap.parse_args()
    if a.rows:
        rows(a.rows)
    lim = limits()
    data = dict(smoke=smoke(), limits=lim, best=model(lim))
    a.out.write_text(json.dumps(data, indent=1))
    for p, builds in data['smoke'].items():
        for b, row in builds.items():
            vs = '  '.join(f"{n}: fz {med(v['freezes'])} drop {med(v['dropped'])} e2e {med(v['e2e_p50'])}" for n, v in row['viewers'].items())
            print(f"{p:15} {b:9} complete {med(row['complete'])} clears {med(row['buffer_clears'])}  {vs}")
    print(f'{len(lim)} limits steps -> {a.out}')
