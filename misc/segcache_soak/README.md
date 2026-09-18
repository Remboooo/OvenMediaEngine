# Segment-cache soak / stress harness

Validates Scheduled Channel **SegmentCache** idle playback (LLHLS, HLS, WebRTC) on a localhost dogfood OME — does not touch the live systemd instance.

## Requirements under test

- Glitch-free multi-client churn across LLHLS / HLS / WebRTC
- Cold start: clients connect while indexing/hydrate is still underway
- After idle grace: disk `read_bytes` ≈ 0 and low CPU
- Long-lived **hls.js** playback across file loop wraps (DISC-SEQ / PDT / window)

SegmentCache applies to **H.264 + AAC MP4** with `BypassTranscoder=true`. Unsupported media logs a warning and falls back to demux (schedule still loads). See design doc.

## Localhost dogfood OME

| Service | Port |
|---|---|
| LLHLS / HLS / WebRTC signalling | `13333` |
| API | `18081` |
| ICE UDP | `11000-11005` |

```bash
# start / stop / rebuild+restart
./dogfood/start.sh
./dogfood/stop.sh
./dogfood/restart.sh --rebuild   # rebuild Release binary, then restart

# soak against dogfood
SOAK_DOGFOOD=1 OME_SOAK_TRANSPORTS=llhls,hls,webrtc OME_SOAK_ITERS=2 python3 soak.py

# randomized multi-client stress (cold-start + churn + idle 0-I/O)
SOAK_DOGFOOD=1 OME_STRESS_DURATION_S=180 python3 stress.py

# full hour soak-style stress
SOAK_DOGFOOD=1 OME_STRESS_DURATION_S=3600 OME_STRESS_MAX_CLIENTS=6 python3 stress.py
```

Logs: `dogfood/logs/` and `logs/stress_summary_*.json`.

### Schedules

| File | Purpose |
|---|---|
| `dogfood/schedules/_filler.sch` | **Default wrap soak:** `short_a.mp4` (~60s) with a 24h schedule slot so idle `loop_count` advances in-place; `scheduled` stamped to ~now on dogfood start |
| `dogfood/schedules_optional/_filler_short_rotate.sch` | `short_a/b/c` rotating at file length — item joins / cache-serve toggles (copy over `_filler.sch` to use) |
| `dogfood/schedules_optional/_filler_long.sch` | trains4 ~2h loop for idle-I/O / hour stress |

Generate / refresh short media + symlinks into `/opt/ovenmediaengine/media`:

```bash
./gen_short_media.sh
```

After changing `_filler.sch`, restart dogfood so Schedule provider reloads.

## Wrap / DISC-SEQ soak (hls.js)

Targets hypotheses for long-tab stalls that refresh clears (ffmpeg often misses):

1. `#EXT-X-DISCONTINUITY-SEQUENCE` decreases or vanishes after wrap scrolls out
2. `#EXT-X-PROGRAM-DATE-TIME` jumps backward without discontinuity
3. Live window stays collapsed across wraps

```bash
./dogfood/restart.sh          # pick up short-clip _filler.sch
OME_WRAP_DURATION_S=400 ./run_wrap_soak.sh
```

Pieces:

- `playlist_invariants.py` — polls `ts:playlist.m3u8`, hard-fails on DISC-SEQ/PDT/window invariants
- `hlsjs_soak/play.mjs` — headless Chromium + hls.js; fails on fatal / media stall / client-side DISC-SEQ regression

WebRTC tester: `../oven_rtc_tester/` (`OvenRtcTester` binary).

## Live instance (optional)

```bash
OME_SOAK_HOST=127.0.0.1 OME_SOAK_PORT=3333 OME_SOAK_TRANSPORTS=llhls python3 soak.py
```
