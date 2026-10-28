# Traffic simulator

Imitates an evening of normal use on an imperfect network with imperfect clients, against a separate OME. It checks that the server stays correct while that happens. Where `misc/webrtc_impair/smoke_test.sh` checks *what one viewer receives*, this checks *what the server does* under churn: streams starting and stopping, viewers joining and leaving by every route, networks that drop, stall and vanish.

```bash
./traffic_sim.py                          # 30 min (default), seed 1
./traffic_sim.py --duration-s 300 -v      # quick, printing every event
./traffic_sim.py --duration-s 28800       # overnight soak
./traffic_sim.py --seed 7 --no-gpu        # another schedule, Bypass-only
```

Part of the [pre-release real-world test](../README.md), which says when to run it and how to read the results. Setup: `make_sources.sh` makes the three publisher sources; the LAN address that desktop browsers reach is detected from the default route (`OME_TEST_LAN_IP` overrides it).

Output goes to `logs/sim_<timestamp>/`: `summary.txt`, `events.jsonl` (every join, leave, publish and FAIL, with times), `samples.jsonl` (OME RSS, threads, fds, viewers and live streams every 5 s), `relay.log` and `ome_stdout.log`. `clients/` holds logs of the clients that failed (`--keep-client-logs` keeps all of them). The OME's own log is `logs/ovenmediaengine.log`. The exit code is 0 only if every check passed.

## Safety

- It uses its own OME (`build/Release/bin/OvenMediaEngine -c conf/`) on its own ports: API 38081, signalling/LL-HLS/HLS 33333, RTMP 31935, SRT 39999, ICE 34000–34005. Production (3333/1935/10000+), the webrtc_impair harness (23333/21935/24000+) and the segcache dogfood (13333/11000+) are not touched.
- It refuses to start while production has a live stream (Scheduled channels don't count). It ends the run early (ABORTED) if one appears, because the simulator's `livegpu` stream uses the same GPU as production's transcoder. `--allow-production-live` overrides this.
- It stops admitting viewers when the load average exceeds 90% of the cores. Client processes run at `nice 10`.

## What it simulates

**Publishers.** Three slots go live and offline at random. Each restart uses the same stream name, often while viewers are still attached.

| Slot | App/stream | Source | Path in OME |
|---|---|---|---|
| p0 | `livegpu/gpu4k` | `sim_2160p60_baseline.ts`: 4K60 Constrained Baseline, 1 s GOP (`make_sources.sh`, or a real recording via `OME_SIM_4K_SRC`) | a production-like ladder: NVDEC + NVENC 1080/720/480, opus, thumbnails |
| p1 | `live/s1080` | `sim_1080p60_high.ts`: 1080p60 High with B-frames (OBS-like) | Bypass + opus |
| p2 | `live/s720` | `sim_720p30_main.ts`: 720p30 Main, 8 s GOP | Bypass + opus |

Each live period gets an uplink class (`uplink_clean`, `uplink_jitter`, `uplink_stalls`) and ends one of three ways:
- **neat:** the encoder closes the RTMP session;
- **kill:** the encoder process dies, and the kernel closes the TCP connection;
- **uplink_dead:** the network vanishes. The relay drops everything and never closes toward OME. The encoder then exits, and OME only has the silence to go on.

**Viewers.** They arrive at random, around `--viewers` (12) at once and at most `--max-viewers`. Each one draws:
- **Lifetime:** 60% zappers (5–30 s), 30% medium (1–5 min), 10% long (10–30 min).
- **Stream:** gpu4k is twice as likely as either of the others.
- **Protocol:** WebRTC (70%) or LL-HLS (30%).
- **Rendition:** `auto` (ABR) or a fixed one. WebRTC uses `?rendition=`; LL-HLS uses that rendition's playlist.
- **Network class** (`classes.json`): fiber, residential, wifi, mobile_4g or terrible.
- **How it leaves:** polite, killed (tab closed), or vanishing (WebRTC only: the network goes dark, and the tab is closed 2–20 s later).
- **Mid-session outage:** a Wi-Fi/4G/terrible WebRTC viewer has a 30% chance of a 2–8 s outage that then recovers.
- **Patience:** a viewer retries an offline or failed stream 1, 3 or many times. A WebRTC viewer whose packets stop for 10 s reloads.

**Desktop browsers** (`--desktop NAME=ENDPOINT`, repeatable): a Chrome (`http://<desktop-ip>:19222`, CDP) or Firefox (`ws://<desktop-ip>:19224`, WebDriver BiDi) on a desktop set up with [`../remote_viewer/`](../remote_viewer/). Each watches live streams back to back, every session with a drawn stream, rendition, network class, outage and way of leaving, and each session is judged frame by frame (`viewer_check.py`): freezes, renderer drops, and send-to-display delay from the relay's trace. They are in `desktop.jsonl` and the report's "desktop viewers" table, per browser and network class. After three quick failures in a row the simulator restarts that browser through the desktop's control service.

WebRTC viewers are `OvenRtcTester` (pion; cheap, no decoding). LL-HLS viewers are `segcache_soak/llhls_client.py`, which uses blocking playlist reloads and fetches parts the way hls.js does. The bitstream probe (real Chromium plus a decode check) is the smoke test's job.

**Network.** `sim_relay.py` carries every path and gives each one its own network class:

- **WebRTC signalling and ICE:** OME advertises only the relay's ICE ports. Before starting a viewer, the simulator tells the relay over a control socket which viewer comes next and what its network is. WebRTC connects are serialised so that flows can't be mixed up. Vanishing and outages are per viewer.
- **RTMP ingest:** tagged per publisher, so `uplink_dead` black-holes exactly one encoder.
- **LL-HLS:** one relay port per class, `127.0.0.1:30340` upwards (below the ephemeral port range).

UDP impairment is webrtc_impair's `Shaper`. TCP is a delay line: order and throughput are kept, delay, jitter and stalls are added, and a lost chunk costs 200 ms. `classes.json` is re-read live.

**Quiesce.** After `--warmup-s` (300 s), and again at the end, every publisher and viewer leaves. The simulator then waits Timeout (30 s) plus 15 s and samples the server. The first sample is the warm baseline for the leak check. The run then continues with fresh traffic.

## Checks

| Check | Fails when |
|---|---|
| server alive / no crash dump | OME exited, or a new file appeared in `conf/dumps/` |
| server log | any `E`/`C` line not matched by `allowlist.txt` (grouped, top 15 printed) |
| sessions | OME's WebRTC session count for a stream exceeds what can exist: viewers alive + viewers vanished < Timeout + 15 s ago + viewers that left < 10 s ago. Or any session (any protocol) is still counted after a quiesce, even after another 60 s of waiting. |
| streams | a live publisher's stream isn't listed after `--stream-ready-s` (30 s); a neatly stopped or killed one is still listed 10 s later; one whose uplink died silently is still listed after `--ingest-dead-limit-s` (60 s); or a stream is still listed after a quiesce |
| leaks | final quiesce vs. warm quiesce: threads > +4, fds > +16, RSS > +max(100 MB, 15%). RSS includes memory the allocator keeps: one failing run is not a leak until a rerun and an A/B against the old build agree (see [../README.md](../README.md#reading-the-results)) |
| simulator | a simulator task crashed (that's a bug here, not in OME, but it would make the run lie) |

These are informational and never fail a run:
- **Viewers per network class:** played at least once, gave up, freeze reloads, freezes caused by the stream ending, LL-HLS stalls, and WebRTC time to first media.
- **Peak sessions.**
- **Relay event-loop lag:** WARN above 50 ms means the relay itself was overloaded, so the network conditions were harsher than configured.

Viewer experience on a network class depends on luck, not just the server. Judging it frame by frame is the smoke test's and bitstream probe's job.

## Allow-list

`allowlist.txt` holds regexes for E/C lines that this traffic causes legitimately, each with a reason. Examples are "Cannot find stream" when a viewer asks for an offline stream, and the empty AAC sequence header that ffmpeg's FLV muxer sends. Anything added there can never fail a run again, so add a line only with a reason.

## Trying a config change

`conf/` is a production-like config, with this fork's WebRTC options on and a 10 s RTMP `PacketSilenceTimeoutMs` (without one, a silently dead uplink holds its stream: see the findings below). Test with the config you deploy. To change it without editing `conf/`, these options run a copy (`conf_override/`): `--rtmp-silence-ms`, `--rtmp-first-media-ms`, `--playout-delay MIN:MAX` (`--adaptive-playout`), and `--no-switches` for a build that predates this fork's options (`--bin` runs another binary).

```bash
# Does a silence timeout stop dead uplinks from holding streams? Make dead uplinks common:
./traffic_sim.py --duration-s 600 --seed 3 --stop-weights 1,1,4 --rtmp-silence-ms 5000 --rtmp-first-media-ms 30000
```

Publishers always start on a keyframe of their source. Otherwise `ffmpeg -ss -c copy` sends nothing while `-re` paces the skipped stretch, which can last up to a GOP (8 s on `s720`), and a silence timeout would reap that.

## Findings (October 2026, first runs)

| Finding | Evidence | Status |
|---|---|---|
| **Zombie publisher.** An encoder whose uplink dies silently leaves its stream held until TCP gives up, because production sets no RTMP `PacketSilenceTimeoutMs`. Every reconnect in the meantime is refused, with `Input create fail` and C `Could not find stream to be removed`. | 5 zombies and 8 refused reconnects in 20 min, seed 2, on all three slots | `PacketSilenceTimeoutMs` 5000 removes it: dead channels are reaped 5 s after going dark, and reconnects succeed. Production gets 10000, which still tolerates a short uplink hiccup |
| ~~New RTMP publish stalls under load.~~ A fresh `s720` publish was sometimes reaped 5 s after connecting. This was the simulator: offsets like 16.05 were printed as `-ss 16.1`, past the keyframe, so ffmpeg dropped video until the next one (8 s on `s720`) and its muxer held the audio back meanwhile. OME rightly saw silence. | Only offsets 16.1 and 24.1 failed; every whole-second offset worked | Fixed in the simulator: offsets are exact keyframe times |
| **Session counters drift after stream teardown.** API counts ended at LL-HLS +1/−1 or WebRTC −1/−2 with no viewers. The input stream's deletion bulk-subtracted its sessions, but publishers keep reporting sessions on the output stream until it is deleted too: leaving ones were subtracted twice, joining ones never. `WebRtcPublisher::DisconnectSessionInternal` also uncounted a session that another call had already removed. | Most runs | Fixed: `Monitoring` releases each stream's sessions once, under a lock, and ignores later events on it (`src/monitoring/monitoring_session_count_test.cpp`) |
| Leaks | threads, fds and RSS back to the warm baseline after every run | none found |

## Reading a failure

- **Find the moment:** `grep FAIL logs/sim_*/events.jsonl` gives the time `t`. Then look at the events around it: which publisher stopped, and how, and which viewers joined or left.
- **Trace sessions:** `conf/Logger.xml` sets the `Monitor` tag to info. The OME log then has a line for every session start and stop ("A new session has started playing …" / "A session has been stopped playing …"), with per-stream and app totals. A count that never returns to 0 can be matched to the session that never stopped.
- **Reproduce:** the same `--seed` replays the same schedule of choices. Timing still varies, so the same race may need a few runs.

# Limits test (`limits.py`)

The traffic simulator checks what the server does under churn. `limits.py` checks what a viewer sees, one impairment at a time, and where each viewer-protection guard stops absorbing it. Like any failure test it goes past the limits. It checks two things:
- **Below** the limit, the guards absorb the impairment and nothing is visible.
- **Beyond** it, things degrade gracefully: never corruption, freezes no longer than the guard model's worst case, and a full return to the viewer's baseline once the impairment ends.

```bash
./limits.py                                   # every WebRTC dimension, s720 Bypass+opus (~1.5 h)
./limits.py --dims loss,outage --quick        # a subset, 3-4 values each
./limits.py --dims outage --values 250,1000   # chosen values
./limits.py --ulpfec off                      # the same, with WebRTC ULPFEC off (NACK/RTX on)
./limits.py --bottleneck 10:100               # every step behind a 10 Mbps link with a 100 ms drop-tail queue
./limits.py --stream gpu4k --rendition 720p   # a transcoded rung (GPU)
```

It uses the traffic simulator's OME, conf, ports and production check. The two must not run at the same time.

## Limits findings (October 2026)

The full write-up of this phase, with what is still open, is in [FINDINGS-2026-10.md](FINDINGS-2026-10.md).

Production config, s720 (8 s GOP), Bypass+opus:

- **Fixed: RTX sent minutes-old packets.** `RtpHistory` cached each RTX packet it built and never dropped the cached copy when its history slot was reused. The only check was the 16-bit original sequence number, which matches again after the stream's sequence wraps (65536 packets, about 3.5 minutes). A NACK then got the payload from an earlier wrap, sent under the NACKed number. Chrome built a broken frame and waited a GOP. The trace showed 1–20% of RTX packets with RTP timestamps minutes behind the media, and 3–7 s freezes at 2% loss, 50 ms reordering and 20 ms RTT. A freshly started OME does not show it. The test is `src/modules/rtp_rtcp/rtp_history_test.cpp`.
- **Fixed: ULPFEC never recovered anything.** Two bugs. The generator XORed only the payload after the header extensions, while RFC 5109 and libwebrtc protect everything after the fixed 12-byte header. And the SN base used the stream's sequence numbers, while each session renumbers what it sends. Measured on frames that lost one packet, at 1% loss and rtt 200/400/800 ms, each compared with the clean frames sent within 1 s of it (relay trace and displayed frames):
  - Before: 0% shown on time, with ULPFEC on or off.
  - After: 96%, 88% and 92% shown on time. Freezes plus stutters fell from about 65 to about 16 per step.
  - The test `RtpPacketizerUlpfec.EveryProtectedPacketRecovers` rebuilds every protected packet the way libwebrtc does.
- **Fixed: ULPFEC turned slow repairs into keyframe waits.** RED-wrapped ULPFEC parity takes video sequence numbers between frames. Chrome bridges them as padding, but its H.264 reference finder (`RtpSeqNumOnlyRefFinder`, `kMaxPaddingAge` 100) keeps padding only 100 sequence numbers back. A frame repaired later than that (RTT ≥ 50 ms, or a lost retransmission) found no bridge to the next one, and every later frame waited for a keyframe: up to a GOP, since OME ignores PLI. With ULPFEC on, rtt 50–800 ms with 1% loss froze for 7–8 s; with it off, 0.85 s.
  - The fix (`RtcSession::ReannounceFecPadding`): after repairing a packet older than 80 sequence numbers, OME re-sends the FEC sequence numbers Chrome has forgotten, each as an empty RTX packet (sent twice, at most 64 per NACK). With it, ULPFEC on gives 0.83 s and 0 PLI at rtt 50–800 ms. The overhead is nothing below about 50 ms RTT, 3 kbps at 200 ms and about 200 kbps at 800 ms.
  - Re-measured after the ULPFEC recovery fix (same seed, `OME_TEST_NO_PADDING_REPLAY=1` turns it off): still needed. Recovered FEC does not help a frame whose loss waits on a retransmission. At 1% loss and rtt 200/400/800 ms, with the replay off, Chrome sent 4/10/1 PLIs and froze for up to 7.5/7.3/4.8 s. With it on: 0 PLIs, longest freeze 0.23/0.43/0.8 s. Loss, burst and outage are about the same either way, except loss 20% (1 vs 4 PLIs).
  - Still open: at 20–35% loss a keyframe wait can still happen, when a replay is lost twice. ULPFEC off avoids that.
  - Caveat: Chrome's opt-in H.26x packet buffer (field trial `WebRTC-Video-H26xPacketBuffer`, unlaunched for H.264) tracks only 5 continuous runs. It is untested with these replays.
- **Fixed: OME sent bursts no bottleneck could carry.** A NACK after an outage was answered all at once (440 retransmissions for a 1 s outage, per viewer), and a backlog released after an ingest stall went out at loopback speed. A drop-tail bottleneck on the path loses much of such a burst. Any retransmission lost after a long outage reaches Chrome past its 100 sequence-number padding age, so the frames can no longer be chained and the viewer waits for a keyframe (6–7 s here).
  - The fix (`RtcSession::PaceSend`): a token bucket over everything a session sends. A burst up to the session's largest keyframe (with its parity) still goes out at once, as before. Anything beyond that is sent at 2.5× the session's media rate (libwebrtc's pacing factor). Retransmissions now go through the send thread, ahead of queued media.
  - Measured behind `--bottleneck 10:100` (`limits.py --webrtc-features shedding,adaptive-playout` turns pacing off; it was `OME_TEST_NO_PACING=1` before `<SendPacing>`). Unpaced → paced: outage 1 s 6.8 s freeze / 3 PLIs → 1.0 s / 0; outage 2 s 6.1 s / 2 → 2.4 s / 0; uplink_hold 2 s 6.1 s / 2 → 1.95 s / 0. The bottleneck dropped 20–627 packets per step unpaced and none paced. Loss 1% and RTT 200 ms were unchanged. The 4K smoke test (`misc/webrtc_impair/smoke_test.sh harness`) is the same or slightly better paced (loss_1pct 97.7% vs 97.0%).
  - Cost: a repair larger than a keyframe now takes longer, about 0.4 s extra after a 2 s outage of the 720p stream. The outage model includes it.
- **The test relays need `net.core.rmem_max` ≥ 16 MB.** A 4K keyframe (about 320 kB), a NACK answer or a released backlog reaches a relay at loopback speed. With the 208 KB default the kernel drops part of it before the relay sees it: the 4K smoke test lost 19% of its packets on the clean profile, and outages got 7 s keyframe waits. Both relays now request 8 MB per socket (doubled by the kernel) and log when the kernel caps that. `limits.py` still counts the relay's socket drops and does not judge the sequence gaps they explain, and it reports drops elsewhere (likely Chrome's own sockets) as info. Set the limit with `sudo sysctl -w net.core.rmem_max=16777216`, and add it to `/etc/sysctl.d/` to keep it across reboots.
- **Fixed in the relay: a bottleneck's queueing delay compounded.** The rule that keeps packet order applied before the rate queue's delay was added, so each packet's queueing stacked on the previous one's. `rate` steps (and any `--bottleneck`) grew delays of tens of seconds. Those `rate` FAILs in earlier sweeps were the relay's, not OME's.
- **OME ignores viewer PLI/FIR,** so a join or an unrepairable loss waits up to one source GOP.
- **No jitter guard toward the viewer.** The pacer's fixed hold passes ingest jitter through by design, because DTS pacing caused 4K+ULPFEC freezes. From σ ≈ 60 ms on either side, Chrome stutters until its buffer adapts, then keeps the larger delay for tens of seconds.

## How a step runs

The publisher is `rtmp_pub.py`. It sends the source's `.flv` copy (`ffmpeg -i X.ts -c copy -bsf:a aac_adtstoasc X.flv`) frame by frame, each within a millisecond of its timestamp, like a live encoder. This matters because ffmpeg `-re` output jitters: ±20 ms from an MPEG-TS source, and p95 100 ms from the 4K recording. OME's send-side pacer holds frames a fixed time from arrival and passes that timing on unchanged, so viewers would see the test publisher's judder. `--pub ffmpeg` brings ffmpeg back for comparison.

For each value of a dimension, two headless Chromium viewers connect through the relay's `lim` class. Both run `segcache_soak/hlsjs_soak/webrtc_oracle.mjs`:
- **The viewer**, which is judged. It decodes and renders exactly like a real one, with no encoded transform. It records every frame the compositor shows (`requestVideoFrameCallback`, with the RTP timestamp), `getStats()` every 200 ms, and its own timer lateness. A freeze while the oracle's machine stalled is not counted against the server.
- **The probe**, which only captures what Chrome assembles (encoded transform, no decoder), for a strict offline decode check.

The probe is not judged because a transform changes what Chrome does. It delays frames on their way to Chrome's reference finder (`RtpSeqNumOnlyRefFinder` for H.264), while ULPFEC padding notifications go straight there. In a burst after an outage or hold, padding then outruns its frames by more than the 100 sequence numbers the finder keeps, and the viewer waits for a keyframe that a real viewer doesn't need. A real viewer hits the same limit in a different way, when repairing one frame takes longer than 100 sequence numbers' worth of stream (see the findings below).

Each step runs three phases:
1. **clean** (`--clean-s`): starts once the viewer displays frames (a mid-GOP join waits for a keyframe). The baseline sized from it covers jitter-buffer delay, RTT, bitrate, packet rate, frame rate, GOP, and send-to-display delay. The join time is recorded and must be under one GOP plus setup.
2. **impair** (`--impair-s`): holds, outages and uplink stalls fire once, a third of the way in.
3. **recover** (`--recover-s`): the network is clean again.

The relay traces the viewer's packets on the same clock (`trace` command). Every displayed frame therefore has a send-to-display delay, and OME's own packets can be checked before any impairment touches them. The `drop` column counts the video packets the relay actually took. Chrome's `packetsLost` falls back to zero once retransmissions repair the gap.

## Judging

`predict()` turns each guard's parameters and the step's baseline into the number of frames per step expected to arrive later than the viewer can hide:
- **What a viewer can hide:** a frame that arrives late, but by less than the viewer's buffer plus 2 frame periods, shows nothing visible. Any later and the frame before it stays up for 3 periods: a stutter.
- **below** (≤ 0.1 expected): any freeze or stutter is a FAIL. The guard falls short of its own theory.
- **zone**: reported, never judged.
- **beyond** (≥ 3): a freeze longer than the model's worst case is a FAIL, and so is not being back to normal by the recovery bound.

Two models are measured rather than derived:
- **NACK rounds:** the first round takes RTT plus about 30 ms. Chrome re-NACKs only after about 100 ms (libwebrtc's default RTT, since OME sends no RTCP XR), so a second round costs max(RTT, 100 ms).
- **Queueing jitter, both ways:** OME has no guard for it. The pacer holds each frame a fixed time from arrival, so ingest jitter passes through. Chrome's buffer adapts after the late frames have already shown: below while 3σ fits the slack, beyond once σ exceeds 2 frame periods.

A send-to-display delay that has not come back down by the end of a step fails only on a below step, and only if it sits outside Chrome's jitter buffer. Inside the buffer, it is Chrome shrinking the buffer at its own pace, which is reported.

Always judged, at every value:
- Decode errors, from a strict offline decode of what Chrome assembled. Chrome must never assemble broken frames, so a decode error means the server sent wrong data.
- RTP sequence continuity per SSRC, as OME sent it.
- A marker on every video frame.
- The OME log against the allow-list, and whether OME is still alive.

The guard parameters the model uses are constants at the top of `limits.py`, each noting where it comes from (OME source, libwebrtc).

`--chrome-log` makes the oracle's Chromium log its receive path (frame buffer, NACK, packet buffer, reference finder) to `oracle.log`. With `OME_DEBUG_RTX=1` in the environment, OME logs every NACK and whether it found the packet. `--keep-trace` keeps the relay's packet trace.
