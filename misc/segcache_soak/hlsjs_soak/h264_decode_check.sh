#!/usr/bin/env bash
# Strict software decode of a webrtc_bitstream_probe capture, mapped back to
# per-frame wall-clock times. Runs at idle CPU priority (safe next to a live OME).
#
# The capture is split into keyframe runs (a keyframe and the frames that
# follow it in arrival order), and each run is decoded by its own
# single-threaded ffmpeg. Within a run, error lines printed before showinfo's
# line for output frame N belong to frame N.
#
# Why separate runs: a capture can hold several rungs (ABR) and late repairs.
# Decoded as one stream, an old rung's late keyframe started a run that
# swallowed the new rung's frames, and one run's missing outputs shifted every
# later attribution (2574 frames in, 2478 out): "corrupt" frames that were
# byte-identical to what OME sent.
#
# Usage: h264_decode_check.sh <probe_dir>
# Output: <probe_dir>/decode_log.txt, decode_errors.txt, ordered_frames.jsonl
set -euo pipefail
DIR="$1"
cd "$DIR"

python3 -I - <<'PY'
import datetime as dt
import json
import re
import subprocess

frames = [json.loads(l) for l in open('frames.jsonl')]
data = open('probe.h264', 'rb').read()


def newer(a, b):
    """RTP timestamp a is at or after b (wrap-aware)."""
    return ((a - b) & 0xFFFFFFFF) < 0x80000000


# Keyframe runs in arrival order. A frame older than its run's keyframe is a late repair of the previous
# rung or GOP: set aside, not decoded inside a run it does not belong to.
runs, late = [], 0
for f in frames:
    if f['type'] == 'key':
        runs.append([f])
    elif runs and newer(f['rtp'], runs[-1][0]['rtp']):
        runs[-1].append(f)
    elif runs:
        late += 1
    # frames before the first keyframe reference a picture we never saw

info = re.compile(r'Parsed_showinfo.* n:\s*(\d+) pts:')
err = re.compile(r'^\[h264 @')
decoded, bad, trailing = 0, [], 0
ordered = []
with open('decode_log.txt', 'w') as log, open('trimmed.h264', 'wb') as trimmed:
    for run in runs:
        # The transform hands over late-completed frames after their successors; Chrome's frame buffer
        # reorders before decode. Re-assembled duplicates (same frame, same size): the first copy.
        seen = set()
        run = [f for f in run if not ((f['rtp'], f['size']) in seen or seen.add((f['rtp'], f['size'])))]
        # With B-frames, decode order is not timestamp order: OME sends in decode order, so a run whose
        # arrival order steps back in RTP time for many frames is decoded as it arrived
        back = sum(1 for x, y in zip(run, run[1:]) if not newer(y['rtp'], x['rtp']))
        if back < max(2, len(run) // 10):
            run = sorted(run, key=lambda f: (f['rtp'] - run[0]['rtp']) & 0xFFFFFFFF)
        stream = b''.join(data[f['offset']:f['offset'] + f['size']] for f in run)
        trimmed.write(stream)
        out = subprocess.run(['chrt', '-i', '0', 'ffmpeg', '-hide_banner', '-nostdin', '-v', 'info', '-threads', '1',
                              '-f', 'h264', '-i', 'pipe:0', '-vf', 'showinfo=checksum=0', '-f', 'null', '-'],
                             input=stream, capture_output=True).stderr.decode(errors='replace')
        log.write(out)
        pending = []
        for line in out.splitlines():
            m = info.search(line)
            if m:
                decoded += 1
                if pending:
                    n = int(m.group(1))
                    bad.append((run[n] if n < len(run) else None, pending))
                    pending = []
            elif err.match(line) and 'Increasing reorder buffer' not in line:  # info with B-frames, not an error
                pending.append(line.split('] ', 1)[-1].strip())
        trailing += len(pending)
        ordered.extend(run)

with open('ordered_frames.jsonl', 'w') as idx:
    for f in ordered:
        idx.write(json.dumps(f) + '\n')


def describe(f):
    if f is None:
        return '#? (beyond the run)'
    wall = dt.datetime.fromtimestamp(f['t'] / 1000).strftime('%H:%M:%S.%f')[:-3]
    return f"{wall} {f['type']} size={f['size']} rtp={f['rtp']}"


with open('decode_errors.txt', 'w') as out:
    for f, msgs in bad:
        out.write(f"{describe(f)} :: {' | '.join(msgs)}\n")
print(f'decoded frames: {decoded}   frames preceded by decoder errors: {len(bad)}   trailing errors: {trailing}   '
      f'(keyframe runs: {len(runs)}, late frames set aside: {late})')
for f, msgs in bad[:40]:
    print(f"  {describe(f)} :: {' | '.join(msgs)[:200]}")
PY
