# Pre-release real-world test

Before a change to the media path is released, run it through the tests in this folder. They measure what a viewer
sees and what the server does under real-world conditions: **frame drops, freezes, delay and memory**. A unit test
or a clean-network demo misses these. Every streaming bug found in October 2026 passed both.
([traffic_sim/FINDINGS-2026-10.md](traffic_sim/FINDINGS-2026-10.md) has the details.)

The tests run their own OME on their own ports, never a server you already run, and put every viewer behind a
simulated network: fiber, residential, Wi-Fi, 4G and a terrible connection, with loss, jitter, stalls and outages.
Real Chrome and Firefox on real desktops are part of it, because a headless browser decodes and schedules frames
differently from one with a display and a hardware decoder.

Run it for any change to `src/publishers/webrtc`, `src/modules/rtp_rtcp`, the pacer, the transcoder, the providers
(RTMP, SRT) or the LL-HLS publisher. Include the summaries in your pull request (see the last section).

## What you need

- A Linux machine with a Release build (`build/Release/bin/OvenMediaEngine`), `ffmpeg`, Python 3.10+, Node 18+
  and Go. An NVIDIA GPU is optional: without one, run the traffic sim with `--no-gpu` (it then skips the
  transcoded stream).
- One or two Windows desktops with Chrome and Firefox on the same network, recommended. `remote_viewer/` sets one up
  in one command. Without desktops the tests still run, with headless and lightweight viewers only.
- Socket buffers big enough for a 4K keyframe:
  `sudo sysctl -w net.core.rmem_max=16777216 net.core.wmem_max=16777216` (add them to `/etc/sysctl.d/` to keep them).

One-time setup:

```bash
(cd misc/oven_rtc_tester && go build)                 # the lightweight WebRTC viewer (pion)
(cd misc/segcache_soak/hlsjs_soak && npm ci && npx playwright install chromium)
misc/traffic_sim/make_sources.sh                      # the three publisher sources (synthetic)
```

`make_sources.sh` makes a 4K60 Constrained Baseline source with a 1 s GOP, a 1080p60 High source with B-frames and a
720p30 Main source with an 8 s GOP. A recording of a real encoder is better than the synthetic 4K source, because
keyframe sizes and bitrate swings are what the send path has to absorb. Put the recording in
`misc/segcache_soak/dogfood/media/` and set `OME_SIM_4K_SRC` (traffic sim) and `SMOKE_SRC` (smoke test) to it.

The tests find this machine's LAN address from the default route. Set `OME_TEST_LAN_IP` if that is the wrong
interface.

## The steps

| # | What | Command | Takes |
|---|---|---|---|
| 1 | Smoke test: what Chrome assembles and decodes, on 6 network profiles | `misc/webrtc_impair/smoke_test.sh harness` | ~14 min |
| 2 | Release soak: churn, joins, leaves and network classes, with desktop browsers | `misc/traffic_sim/traffic_sim.py --duration-s 9000 …` | 2.5 h |
| 3 | The same soak on the build you are replacing, if step 2 shows anything | `traffic_sim.py --bin <old binary> …` | 2.5 h |
| 4 | Optional, for changes to a viewer-protection guard: each impairment up to its limit | `misc/traffic_sim/limits.py` | ~1.5 h |
| 5 | After deploying, on the live server while it is idle | `misc/webrtc_impair/smoke_test.sh production` | ~2 min |

**1. Smoke test.** With desktop browsers (`SMOKE_VIEWER_CDP=http://<desktop-ip>:19222
SMOKE_VIEWER_BIDI=ws://<desktop-ip>:19224`) their decoding viewers are the verdict; the headless probe guards
delivery and decodability. Bars come from `webrtc_impair/profiles/baseline.json`. Details:
[webrtc_impair/README.md](webrtc_impair/README.md).

**2. Release soak.** Three publishers go live and offline at random (neatly, killed, or with their uplink dying
silently). Lightweight WebRTC and LL-HLS viewers come and go around a mean of 12 (at most 24). Each desktop browser
watches back to back, each session with a drawn stream, rendition, network class, outage and way of leaving:

```bash
cd misc/traffic_sim
./traffic_sim.py --duration-s 9000 --seed 21 --playout-delay 50:200 --adaptive-playout \
  --desktop deskA-chrome=http://<desktop-a-ip>:19222 --desktop deskA-firefox=ws://<desktop-a-ip>:19224 \
  --desktop deskB-chrome=http://<desktop-b-ip>:19222 --desktop deskB-firefox=ws://<desktop-b-ip>:19224 \
  --out logs/release_<name>
```

Use the config you will deploy: `conf/` plus the override options (`--playout-delay`, `--rtmp-silence-ms`,
`--no-switches`; `./traffic_sim.py -h`). Details: [traffic_sim/README.md](traffic_sim/README.md).

**3. A/B.** Results depend on the machine, the desktops and luck. A number on its own says little, the same soak on
the build you are replacing (`--bin`, same `--seed` and desktops) says a lot. An older build that refuses newer config
elements runs with `--no-switches`.

## Reading the results

Each run ends with PASS/FAIL lines in `summary.txt`; the exit code is 0 only if every check passed.

| Measure | Where | Good | Look closer |
|---|---|---|---|
| **Correctness** | smoke `decode`, soak `sessions`/`streams`/`server log`/`no crash dump` | always PASS | any FAIL blocks a release |
| **Frame drops** | soak "desktop viewers" `dropped`; smoke viewer `dropped` | fiber/residential < 2% | more on one build than the other |
| **Freezes** | `freezes/min` and `longest` per browser and network class; smoke viewer freezes | fiber/residential well under 1/min, ≤ 1.5 s | WARN lines on fiber or residential |
| **Delay** | `e2e p50/p95`: when OME sent a frame until the browser showed it | fiber 60–150 ms p50 | a p95 that grows over the run |
| **Memory** | soak `leaks`: threads, fds and RSS at the final quiesce vs. the warm-up quiesce | PASS | a FAIL: rerun, and A/B (below) |

Wi-Fi, 4G and "terrible" are meant to hurt. Their stalls and reloads are the network, and the comparison between
builds is what counts there.

Known causes that are not the build under test:
- The 1080p source has B-frames: Chrome drops about a third of its frames and Firefox about two thirds.
- The 720p source has an 8 s GOP, and OME does not answer a viewer's keyframe request (PLI), so a viewer that loses a
  keyframe waits up to 8 s. A freeze on s720 that a reload fixes is this.
- The `uplink_stalls` class stalls the publisher's uplink: a ~1.5 s freeze for everyone watching it, every ~45 s.

**Memory.** RSS at the final quiesce includes memory the allocator keeps after freeing it, so one run can be 100 MB
higher than another without a leak. When `leaks` fails: rerun the same seed and run the old build the same way. Log
`/proc/<pid>/smaps_rollup` every 10 s during the run (Anonymous is the heap, Private_Clean is mapped libraries such as
CUDA's). A real leak grows on every run at a steady rate. One run that grows is allocator retention.

## In your pull request

For each run: the command, the build (`git describe`), the `summary.txt` and, for an A/B, the same for the other
build. Say which desktops (GPU, browser versions, refresh rate) ran the browsers. Never post logs of a production
server you do not own.

## Also in this folder

| Folder | What |
|---|---|
| [webrtc_impair/](webrtc_impair/) | impairment relay, smoke test, upstream-vs-branch comparison (`compare_builds.sh`) |
| [traffic_sim/](traffic_sim/) | traffic simulator, limits test, findings and lab notes |
| [remote_viewer/](remote_viewer/) | prepares a Windows desktop as a remote test browser |
| [segcache_soak/](segcache_soak/) | segment-cache soak, LL-HLS client, the browser oracle (`hlsjs_soak/`) |
| [oven_rtc_tester/](oven_rtc_tester/) | the lightweight WebRTC viewer |
| `conf_examples/`, `stubs/`, launchers | from upstream OvenMediaEngine |
