# Lab notes: WebRTC viewer limits

A running log of what we chase, what we find, and what we park. Newest entries at the bottom of each day.
FINDINGS-2026-10.md is the tidy summary; this file keeps the trail, including dead ends, so we can go back
to earlier results. Result directories are under `misc/traffic_sim/logs/` (local, not in git) unless noted;
smoke-test runs are under `misc/webrtc_impair/logs/`.

Changes to a production server (config, deploys) are tracked outside the repository, with the reason and evidence for each.

Commit hashes here and in FINDINGS-2026-10.md name commits of the development branch, which was squashed
before publishing; the published history has the same changes in a few commits.

Status tags: **CHASING**, **PARKED** (with why), **DONE** (with commit), **DEAD END** (a hypothesis that failed).

## Goal (set by the user, 2026-10-22)

The best performance theoretically achievable, measured in latency and dropped frames, under varied network
latency, reordering and loss, on both browsers (Chrome, Firefox), with several viewers, given the protocols,
protocol features and clients we chose. ABR settles on the best quality the network supports, without issues,
within 20 s.

Work order: ABR stepping up → corruption under congestion → several viewers at 4K → the parked items
(pinned-rendition congestion control, Firefox, join losses, the small items). The smoke test before
deploying is skipped for now (user).

## Open threads

| Thread | Status | Last entry |
|---|---|---|
| Clean baseline below 100% of frames shown | DONE for the episodes (`<PlayoutDelay>` 50 ms, config choice); startup misses PARKED | 2026-10-22 |
| ABR rarely steps back up after a bottleneck ends | DONE 2d646a700, 046f79cbd (`abr10`: all phases best rung, ≤ 6.1 s) | 2026-10-23 |
| Probe shows 21–36 corrupt frames under heavy congestion (ABR 0.8×/0.5×) | DONE: not real (byte comparison), harness fixed ffb312615 | 2026-10-23 |
| No congestion control for a pinned rendition (`?rendition=`) | DONE bf558d5a2 (frame shedding); a shed waits up to a GOP | 2026-10-23 |
| ULPFEC parity has no header extensions (BWE blind to its bytes) | PARKED | 2026-10-22 |
| Freezes at ABR down-switches (wait for the new rung's keyframe) | PARKED: needs keyframe-request plumbing, safe only in TIME keyframe mode | 2026-10-23 |
| AUD/SEI sent as separate RTP packets | DONE cd480268b (~7% fewer packets/NACKs) | 2026-10-23 |
| RTX copies carry the original packet's TWCC number | DONE 046f79cbd: it corrupted capacity samples under loss | 2026-10-23 |
| 4K bypass: 2 small frames sent after a newer frame (one trace) | PARKED: unexplained, seen once | 2026-10-22 |
| Firefox as remote viewer (needs Puppeteer BiDi) | DONE 56d8fb8be: matrix, ABR, 4K multi-viewer run on both browsers | 2026-10-23 |
| First step of a gpu4k run has a poor clean baseline (46–48 fps, 300 ms delay) | not seen in any desktop A run (2026-10-23; likely the CUDA JIT cache) | 2026-10-23 |
| False frame shedding under path jitter | DONE a47a364cf | 2026-10-23 |
| Join waits for the next keyframe (≤ 1 GOP) | by design (a backlog start costs latency); lever: source GOP | 2026-10-23 |
| High RTT with loss: freezes (Chrome's buffer < repair time) | DONE 20811abe9 (adaptive playout-delay min per session; needs `<PlayoutDelay>`) | 2026-10-23 |
| Smoke test re-run on the final build before any deploy | DONE 2026-10-23 (`smoke_20261023_224352`), deployed 2026-10-24 | 2026-10-24 |
| Final two-browser pass on desktop A | DONE (`fin_*`); one FAIL: Chrome RTT 800 ms | 2026-10-23 |
| Mid-session RTT jump: adaptive playout raise lags 30 s | PARKED: documented; a real long path is long from the start | 2026-10-24 |
| Smoke-test bar (fixed 97% on a non-decoding probe) | DONE ea1c9fe40: decoding viewers on desktop A + per-profile baseline | 2026-10-24 |
| Patched build passes source arrival jitter to viewers (+33–36 ms clean, up to ~100 ms impaired, vs upstream; more freezing at 240 ms jitter) | DONE bb2f1b7c1 (capped release; deployed 16:40, prod smoke PASS 17:57): delay −30–55 ms, send p99 125 → 72 ms. Early-side jitter: early anchor tried; no regression and no viewer-visible gain (2026-10-25), kept as a patch. 240 ms step unchanged | 2026-10-24 |
| Decode check on B-frame streams (s1080) flags late-completed frames | OPEN: d636baefa decodes in arrival order; a frame completed late keeps its place ("reference picture missing during reorder", ~10/step under reorder or loss). Needs decode order from slice headers (frame_num, nal_ref_idc). Viewer metrics unaffected | 2026-10-24 |

## 2026-10-22

Environment: sim OME = `build/Release/bin/OvenMediaEngine` (production PID 3457 is never touched). Relays need
`net.core.rmem_max` ≥ 16 MB (set by hand today; lost on reboot). Remote viewer: Desktop B <desktop-b-ip>, a desktop
with a physical display, GbE, Chrome 155 on CDP :19222 (Firefox BiDi :19224). Never close the browser or its last tab.

- **RTX served stale packets after a sequence wrap.** DONE 04291b34f. Seen as 3–7 s freezes in the first full sweep.
- **ULPFEC padding age → keyframe waits on slow repairs.** DONE fa57a376e (padding replay). A/B after the FEC fix: `lim_full4` (on) vs `noreplay` (off). Replay still needed: rtt 200–800 ms, 0 vs 1–10 PLIs.
- **ULPFEC recovered nothing.** DONE 8d8afd14c. Measured with scratchpad `fecuse2.py` (single-loss frames on time).
- **Sequence gaps in uplink_hold steps.** Relay socket overflow (464 drops), not OME. `gapchk`. Accounting in limits.py 811722cf1.
- **1 s outage froze 7 s.** First suspect: padding replay. DEAD END: same with the replay off. Cause: relay socket drops of the NACK burst (967 vs 0–168 in good runs; `out1000*`). OME_DEBUG_RTX "fixed" it by slowing the RTX loop. Harness fix ddc913a5b (8 MB buffers).
- **Pacing.** DONE 1362edab0. First version had two bugs: RTX queued behind media (repairs past the padding age), and a 1.5× keyframe burst overflowing a 100 ms queue (`bn_pace_*` second round). A/B `bn_nopace_*` vs `bn_pace_*`, behind `--bottleneck 10:100`.
- **Relay rate queue delay compounded** (5.6 s RTT on a clean 10 Mbps path; `bn_*` first round invalid). DONE ddc913a5b.
- **4K smoke test clean profile lost 19%.** Relay socket buffer again (impair_relay had no big buffer). DONE ddc913a5b.
- **Smoke loss profiles 1 point down vs the 21 Oct table.** Not a viewer problem. Baseline noise (97.5/98.9% on the same code); the rest is a probe artifact of working ULPFEC (double repairs + no decode → "Clear PacketBuffer", 59/min at 4K). ULPFEC off: 100.0% (`smoke_nofec*.out`). Decoding viewer clean: `fec1080`. Replay off did not change it (`smoke_noreplay.out`). Documented in webrtc_impair/README.md.
- **4K judged on this machine is invalid:** headless Chromium decodes 4K60 in software at 17–37 fps (`fec4k_on/off`). Use the remote viewer.
- **Auto ABR fell to the lowest rung on a clean path.** Phantom loss: ULPFEC parity numbered for TWCC without the extension. DONE c9781a303. Debug: `abr_dbg` (FEC on, 5–22% phantom loss) vs `abr_dbg_nofec`.
- **Switch to an unwatched rung lost the frame after the keyframe.** DONE c9781a303. First "corrupt frames" were partly the decode check (dedupe by RTP timestamp, sort across a switch), fixed 631a8bd7c. A/B with the old filter: `abr_old`.
- **RTX collapse under congestion** (made worse by the pacing change). DONE c9781a303. `abr_cc` (before: 0.5× 19.6 s freeze) vs `g_abr` (after: 1.57 s).
- **Remote viewer could not signal.** https page → mixed content; intercepted origin → Local Network Access block. DONE 333308e07 (page served from the LAN address).
- **Remote viewer skipped ~20% of frames on a clean path.** Suspected VM/RDP. DEAD END: a physical desktop. Not the network (frames arrive on time, `rv_sanity`), not the display (a 30 fps file plays 299/299). ULPFEC off fixed it (`rv_nofec`). Cause: parity one frame late. DONE 63fd99b13. After: `rv_fecfix*` 92–99% shown, the same as `rv_nofec*` 95–99%.
- **ULPFEC at 4K on the remote viewer:** fewer skips (108 vs 156 at 1%, 156 vs 230 at 2%) and fewer stutters at 2% (11 vs 26). `rv4k_on`, `rv4k_off`. One run each.
- **Clean baseline below 100%.** CHASING. Remote clean phases: 92–99% with ULPFEC, 95–96% without. A clean path should show every frame.
  - Exact count (frames OME sent in the clean window vs frames shown, scratchpad `clean_gap.py`): rv_fecfix 92.0%, rv_fecfix2 98.4%, rv_fecfix3 99.3%, rv_nofec 95.6%, rv_nofec2 94.8%, local g_loss 98.6%.
  - Parity is no longer late in any of these (3,580 parity packets right behind their frame, 0 late). DEAD END for this question.
  - Two patterns. (a) Startup: about 1 missing frame per 300 ms in the first ~1.6 s, in every run, local too. (b) Episodes on the remote box: 1 frame per ~250 ms for several seconds, **also with ULPFEC off** (rv_nofec2 9.3–14 s).
  - (b) in rv_nofec2: OME send timing within 4 ms of the media clock, network delivery ±1 ms, Chrome decoded 30/s, showed 26/s, and its jitter-buffer target rose 16 → 35 → 51 ms. So it's inside Chrome at display, not delivery.
  - (a) on the local viewer: frames shown alternately ~13/30 ms after receipt (30 fps on 60 Hz). Each missing frame follows one held a whole extra refresh (~45 ms) or one arriving 3–5 ms late. Two frames land on one refresh and one is dropped.
  - Hypothesis: OME sends no playout-delay extension (`<PlayoutDelay>` exists in OME, unset in sim and production), so Chrome runs a ~16 ms jitter-buffer target and renders with almost no slack. A few ms of timing variation then drops frames at the display. File playback (299/299) buffers and smooths. Test: `limits.py --playout-delay 50:200` (option added), remote viewer, clean-phase count.
  - BLOCKED 21:3x: production went live; limits.py refuses (correctly). Waiting for it to go idle; not overriding.
  - Unblocked by the user: production is one user experimenting, so runs go alongside it (`--allow-production-live`). The machine is shared with a live stream from here on, so each test run gets an interleaved control in the same conditions: `rv_pd1/2` (`--playout-delay 50:200`) vs `rv_ctl1/2` (none).
  - Result (alongside production, remote viewer, 1% loss step). `--playout-delay 50:200`: clean 99.8% / 99.1% shown, misses only in the first 1.3 s, impair skipped 3 / 9, Chrome jitter-buffer target ~38 ms, clean delay 53–60 ms. Controls: 98.3% / 94.1% with episodes (0.5–8 s, 12–14 s), impair skipped 11 / 56, delay 19–32 ms. `--playout-delay 25:200` (`rv_pd25_1/2`): 98.1% / 99.3%, run 1 had episodes at 4–6 s and 251 skipped under loss. So it's not enough.
  - Conclusion: the episodes are Chrome rendering with too little slack. A 50 ms minimum playout delay removes them for ~30 ms of extra delay. A config choice for the user (production has no `<PlayoutDelay>`). Two runs per arm.
  - Left: 1–4 frames in the first ~1.3 s after joining, even at 50 ms. PARKED: small, Chrome's start-up ramp; look again if it matters.
  - Side note: OME's frame send times vary ±2–4 ms around the media clock (pacer passes the publisher's timing through). Not the cause here; PARKED.
- **ABR stepping up.** CHASING.
  - Why it never stepped up (`abr_cc` debug log): the estimator needs 10 s of uninterrupted "optimal" (smoothed delay slope < 0.2, loss < 1%). On a clean path that slope hovers at 0.18–0.35, so any frame over the threshold resets the timer ("TimeInOptimal=0 ms" throughout), and each failed try doubles the wait up to 120 s.
  - Noise sources: audio packets mixed into the per-frame table (48 kHz timestamps beside 90 kHz video), decisions per frame (3 frames = 50 ms at 60 fps counts as overuse), and up-probing by actually switching onto the bigger rung.
  - Rework (uncommitted while testing):
    - video-only frame statistics;
    - a packet-train capacity estimate: bytes after the first packet over the receive spread (floor 3 ms), frames with ≥ 4 packets, median over 4 s;
    - up when capacity ≥ 1.4 × the next rung's bitrate and 3 s without overuse;
    - back-off 10 → 40 s after a failed upgrade (overuse within 5 s);
    - overuse must hold 100 ms.
  - Tool: `abr_settle.py` reports per phase the final rung, the best rung for the link, the settle time and the switches. limits.py now records phase times and `link_mbps`.
  - First test run: `abr2` (rate 3, 1.2, 1, 0.8, 0.5; clean/recover 25 s; remote viewer; estimator debug log).
  - Multi-viewer note from the user: the RTX 3070 desktop handles about 4 × 4K60 streams.
  - `abr2` (first rework): it steps up now (recovery to 1080p in 0.5–20 s) but oscillates under tight links. It upgraded onto rungs that don't fit, because the capacity estimate read 16–30 Mbps on a 3.7 Mbps link.
  - Cause: **OME's TWCC parser took every receive delta from the reference time instead of accumulating them** (each delta is relative to the previous packet). A feedback's packets all landed within one delta, which squeezed every train (12 packets in 4 ms where the link needs 32 ms). The relay's planned spacing was right (2.9 ms/packet, `abr_train`). Fixed in rtcp_info/transport_cc.cpp, test `TransportCcParse.ArrivalTimesAccumulateDeltas` (fails without). The same bug fed the delay trend.
  - Also noted: OME's own TWCC feedback writer (WebRTC ingest) pads in a way its parser rejects. PARKED: ingest path.
  - After the parser fix (`abr_train2`): capacity median 3.67 Mbps behind 3.69 (p10–p90 3.51–3.86). Recovery 480 → 720 → 1080 in 3.8 s. Still overshoots: 1080 → 720 → 480 in 2 s (the old rung's queue draining reads as overuse), and 720 at 85% of the link dropped after 21 s (keyframe queueing ~110 ms > the 100 ms overuse hold).
  - Tuning: delay overuse must hold 500 ms; drain tolerance of 3 s after a downgrade while capacity ≥ 1.2 × the new rung. Run `abr3` (adds 0.3×).
  - `abr3` (500 ms hold + drain tolerance): **17/18 phases on the best rung, all within 12.5 s.** Down at 2.2 Mbps → 480p (7.3 s); 3.7 / 5.8 / 7.2 Mbps → 720p (8–12.5 s); up to 1080p in 2.8–6.2 s.
    - Left: an overshoot to 480p for ~3 s on the way down (0.5×, 0.8×, 1.0×); freezes at switches (2–3 per step, up to 3.4 s; 6 PLIs at 0.8× and 1.0×); one spurious downswitch at the start of the 1.2× recovery.
  - Spurious downswitch: the estimator logged 80–100% loss and slope 1.0 right after the release, while the relay dropped nothing and Chrome counted 0 lost. Not reproduced in two re-runs (`abr_rel` single session, `abr_rel2` with probe) with per-feedback debug counters (received, lost, reported again): zero lost reports. PARKED with the counters left in (debug level) for later sweeps.
  - Overshoot cause: the heavy-loss path bypassed the drain tolerance, and the smoothed loss carries the old rung's drops past the switch. Drain tolerance now covers loss too while capacity ≥ 1.2 × the new rung.
  - `abr4` (+ capacity-based downswitch: the last-1 s capacity below 1.2 × the rung counts as overuse): **18/18 phases on the best rung, settled in 2.7–7.2 s.** Detection now +1.1–1.2 s after the bottleneck starts.
    - Left: an overshoot to 480p at 0.5×/0.8×, and freezes at downswitches (1–5, longest 1.3–2.9 s, 7 PLIs at 0.8×).
    - Shown/sent frames (new in abr_settle.py; limits.py's "skipped" counts 30 fps rungs against 60 fps and is meaningless for ABR): 83–85% in bottleneck phases, 95–96% in recovery.
  - Overshoot cause (`abr4` log, 0.8×): at the second downswitch the 4 s capacity median read 0.44 Mbps (the last 1 s read 5.7) and the slope was pinned at 1.0. Two defects:
    - (a) Frame statistics were keyed by RTP timestamp only. The old and new rung's keyframes share one, so their packets merged into one "frame" with a huge spread.
    - (b) The "accumulated delay > 500 ms" rule used an ever-growing sum that keeps every past error.
    - Fix: key by (track, timestamp); trend and loss follow the track being sent and restart at a switch (capacity samples from any track); the 500 ms rule now uses the standing queue (accumulated delay minus the window's minimum). Run `abr5`.
  - Keyframe requests to the transcoder (for faster switches and PLI on transcoded rungs): no plumbing exists. On-demand IDRs are safe only in KeyFrameIntervalType TIME (OME forces the cadence); in FRAME (the default) the encoder's own GOP counter would likely shift, misaligning rungs for LL-HLS. Sim and production use the default. PARKED until the switch freezes are understood better.
  - `abr5` (per-track statistics, standing-queue rule): 18/18 phases on the best rung; 0.5× now one clean switch (2.6 s); 1.0× switches in 1.7 s with no freeze. 0.8× still overshoots to 480p.
  - With shown/sent measured from the first frame shown, **clean 1080p60 phases show only 93–98%** on the desktop. 60 fps on 60 Hz has even less slack than the 30 fps case. Playout delay 50 ms (`abr5_pd`): 1.0× 97.7 / 98.3%, but the 3× step (the run's first, warm-up) 84–87%. Inconclusive. Re-test `c60_nopd` vs `c60_pd`: three clean steps each, ignore the first.
    - DEAD RUN: the first c60 attempt was stopped because I rebuilt OME mid-run (the arms would have run different builds).
  - 0.8× overshoot (`abr5` log, two sessions interleaved): one estimator's 4 s capacity read 0.5–0.8 Mbps (its last 1 s 5.7) with the slope at 1.0. Likely cause: **retransmissions carried the original packet's TWCC number**, so a lost packet read as received an RTT later, and its frame's train as hugely spread.
    - Fix: RTX gets fresh TWCC numbers (as in libwebrtc) and is excluded from frame statistics. Train rate counts every packet that arrived during the train (RTX, audio, other frames), so interleaved traffic doesn't understate the link.
    - Runs: `abr6`, then `c60_nopd`/`c60_pd` (script `abr_round.sh` in the scratchpad).
  - `abr6` (RTX with fresh TWCC numbers): **0.8× now one clean switch (2.0 s, freezes ≤ 0.45 s, 0 PLIs).** 18/18 phases best rung, settle ≤ 6.4 s. 0.5× still overshoots once (1080 → 720 → 480 → 720).
  - 0.5× overshoot (`abr6` log): the second downswitch had "slope 1.0, loss 0%" and a "capacity" of 0.13 Mbps, while no train sample was below 1.5 Mbps. That is the **REMB path**: Chrome sends REMB as well as TWCC (both negotiated: goog-remb + transport-cc), and OnRemb calls the decision with slope 1.0 and the REMB value whenever REMB < the rung's bitrate. Chrome's REMB read 0.1–0.4 Mbps. That likely also explains the earlier unexplained "slope 1.0, loss 0%" overuses. Fix (built after `c60b`): ignore REMB while TWCC feedback arrives (2 s).
  - **Measurement: requestVideoFrameCallback undercounts at 60 fps.** A 1080p60 file on the desktop: 573 callbacks for 600 frames, but presentedFrames 599 and getVideoPlaybackQuality dropped only 2. So the "93–98% shown" for 1080p60 is partly the tool. The oracle now also records getVideoPlaybackQuality (`stats.jsonl` → `playback`), and abr_settle.py prints "renderer dropped x/y". At 30 fps (two refreshes per frame) the callbacks matched the file 299/299, so the 30 fps playout-delay result stands. Re-run: `c60b_nopd` / `c60b_pd`.
  - `c60b` (renderer counts, getVideoPlaybackQuality). 1080p60 clean, frames dropped before display:
    - no playout delay: 0.8–5.4%;
    - playout delay 50 ms: 0.3–0.9% (one phase 2.7%);
    - a 1080p60 file in the same browser: 0.3%.
    So **with the playout delay, WebRTC 1080p60 is at the browser's own floor.** The callbacks read as low as 85% where the renderer dropped 0.4%, so judge 60 fps by the renderer count.
  - Next: `abr7` = REMB fix + playout delay 50:200 (the recommended configuration).
  - `abr7` (REMB ignored while TWCC flows, playout delay 50:200): clean phases 0.1–0.6% renderer drops; 0 PLIs in the whole sweep; all phases settle (≤ 7.6 s). 0.5× still 3 switches.
    - The renderer counts show the old "best rung" was too greedy: 1080p held at 8.65 Mbps (rung wire ~7.2, 83% utilisation) dropped 8.1%; 720p at 3.66 Mbps dropped 6%.
    - New rule (estimator and abr_settle.py alike): a rung fits while capacity ≥ 1.5 × its configured bitrate (wire ~1.2×, ≤ ~80% utilisation). Upgrade at 1.7× (hysteresis). Expected: 22 → 1080, 8.65 / 7.2 / 5.76 → 720, 3.66 / 2.17 → 480. To build after `cor_*`.
  - Corruption under congestion: the probe saw 11–39 corrupt frames at 0.3–0.8× in abr5/abr6, 0 at ≥ 1.0×. Possible serious cause: ULPFEC recovering a packet wrongly (most recoveries happen under heavy loss). A/B `cor_on` / `cor_off` (0.5×, 0.3×, captures kept).
- **Corruption under congestion: not real.** DONE (harness fixes, uncommitted).
  - ULPFEC on vs off (`cor_on` / `cor_off`): more "corrupt" frames with FEC off (40–76 vs 0–16), so not an FEC mis-recovery. DEAD END: "deltas whose predecessor never arrived": every flagged frame had all frames since its keyframe captured.
  - All flagged frames in the 0.3–0.5× runs were 0.2–5.3 s after the probe joined, long before the bottleneck. Misattributed.
  - Byte comparison with clean reference probes (`--reference-probe`, `cor3`/`cor4`/`cor5`): the flagged frames are byte-identical to what a clean viewer got, and the reference decodes them cleanly. Identical spans decode with 0 errors in isolation.
  - Causes, all in the decode check (h264_decode_check.sh):
    - (a) It fed 2,574 frames and ffmpeg output 2,478, so every later error was attributed to the wrong frame.
    - (b) A late-repaired keyframe of the old rung (older timestamp) started a run that swallowed the new rung's frames.
    - (c) Under heavy congestion the probe (encoded transform, no decode) hands over frames seconds out of order.
  - With 3 references pinned to 1080p/720p/480p (`cor5`, `delivery_check.py`): **4,984 of 4,984 frames delivered intact**; 2 lacked only their leading AUD + SEI, which travel in their own packet. A viewer assembles the frame from the slice alone.
  - Changes:
    - delivery_check.py (new);
    - limits.py takes the corruption verdict from it when `--reference-probe` is given;
    - h264_decode_check.sh now decodes each keyframe run in its own ffmpeg and sets aside frames older than their run's keyframe, which cut false flags from 61 to 8–44 (the rest is (c)).
  - Side note: OME sends AUD and SEI as separate RTP packets (2 extra small packets per frame on some rungs). libwebrtc drops AUD. Possible saving. PARKED.
- **ABR "fits" tightened** (stay ≥ 1.5 × configured, upgrade ≥ 1.7×). Run `abr8` with references and the playout delay.
  - `abr8`: 1.2× now settles on 720p (3.0% renderer drops vs 8.1% at 1080p). Bottlenecks settle in 1.7–4.6 s; delivery 0 corrupt except one false flag.
    - The false flag: a 1080p-sized frame whose only reference copy was another rung's (the 1080p reference missed it). delivery_check.py now counts a frame as corrupt only if a reference frame of about its size exists.
    - **Recovery regressed:** 0.3× 18 s / 4 switches, 0.5× still flapping after 20 s (MISS).
  - Recovery flapping (`abr_rec`, debug): overuse with slope ≈ 0 and loss 0, from the capacity rule. On a fast path a train's spread is at the 3 ms floor, so its sample is only a lower bound (bytes / 3 ms), which small frames keep low (1 s median 2.5–8 Mbps on a LAN). Fix: floor-bounded trains count as unlimited. Run `abr9`.
  - `abr9` (lower bounds count as unlimited): 18/18 phases ok, 0 corrupt frames (delivery check) on every step. But recovery from 480p takes ~17.7 s with 4 switches: 1080p fails 2 s after the upgrade, then a 10 s back-off.
    - With one session it recovered cleanly (`abr_rec2`). With all 5 sessions (`abr_rec3`) it stuck at 480p.
    - Cause: **OME's own pacer throttled the new rung.** Pacing runs at 2.5 × the smoothed media rate, which after 20 s on 480p is ~0.6 Mbps. 720p frames just after the switch were paced at ~3.5 Mbps, and the capacity estimate measured the pacer (3.48 Mbps < 1.5 × 2.56), so it switched back down.
    - Fixes: pacing rate ≥ 2.5 × the current rendition's configured bitrate; a train whose receive spread is not larger than its send spread measures the sender, not the link, and counts as a lower bound. Estimator decision logs now carry the instance pointer.
  - `abr_rec4` (5 sessions): recovery 480 → 720 (2.7 s) → 1080 (5.7 s), no failed upgrade, no overuse after the release. The "slope 21" overuses on 480p sessions are gone as well (pacer-stretched frames). Confirmation sweep: `abr10`.

## 2026-10-23

- `abr10` (confirmation, 5 sessions, remote viewer, playout delay 50 ms): **every phase on the best rung**; bottlenecks settled in 1.6–4.5 s, recovery to 1080p in 2.5–6.1 s; clean phases 0.1–0.8% renderer drops (one 4.8%). PLIs 0 except 2 at 0.8×. Delivery: 2 differing frames, both 480p keyframes 80 bytes short (two SEI after the PPS lost). delivery_check.py now ignores AUD/SEI anywhere, not only leading.
- Committed: 2d646a700 (TWCC parser), 046f79cbd (estimator rework, RTX TWCC numbers, REMB, pacing floor), ffb312615 (abr_settle, delivery check, decode check, playback counters).
- Next: several viewers at 4K on the desktop (the RTX 3070 handles ~4 × 4K60).
- **Several viewers.** CHASING. limits.py `--viewers N`: the extra viewers join the same class (same impairment), outputs in steps/*/viewer_N/. viewers_report.py gives per viewer and phase the renderer drops, freezes and the longest freeze. First: 4K60 bypass, 0.5% loss, playout delay, 1 / 2 / 4 viewers on the desktop (`mv4k_1/2/4`).
  - `mv4k_1/2/4` (4K60 bypass, 0.5% loss, playout delay):
    - 1 viewer: 0.2–3% renderer drops, no freezes.
    - 2 viewers: 0.2–2.8%, one freeze each.
    - **4 viewers: ~20% dropped, 6–15 freezes per phase.**
    - Not OME and not the relay: all 4 receive 58–60 frames/s (getStats framesReceived). The desktop's decode time per frame went from 1 ms (1 viewer) and 4–5 ms (2) to 15–51 ms (4), and its decoder dropped 89–360 frames per viewer. **The client can't decode 4 × 4K60** (Chrome 155, RTX 3070).
    - OME's send cadence did widen with 4 sessions (p99 28 → 36 ms, max 53 → 75 ms, 9 late frames). Watch this. Multi-viewer tests go on at 1080p60: `mv1080_loss` (0.2/1/2%), `mv1080_rtt` (100/300 ms), 4 viewers.
  - `mv1080_loss` / `mv1080_rtt` (4 viewers at 1080p60): **12–35% renderer drops even on a clean path**. Decode time 74–98 ms per frame, where a single 4K60 viewer took 1 ms, with decoder drops of 70–271 per viewer. INVALID as an OME measurement: the desktop throttles unfocused/occluded Chrome windows. My local viewers launch with --disable-backgrounding-occluded-windows etc.; the user's Chrome was started without them. Likely part of the 4K 4-viewer result too. BLOCKED: asked the user to restart Chrome with --disable-backgrounding-occluded-windows --disable-renderer-backgrounding --disable-background-timer-throttling --disable-features=CalculateNativeWinOcclusion. Then repeat `mv4k_4` and `mv1080_*`.
  - Unblocked (2026-10-23, flags verified via chrome://version). `mv1080_loss_b` (4 viewers on the desktop): decode is now fine (60/s decoded, 0–1 decoder drops, 22–49 ms per frame), but **the renderer still drops 20–39% on the clean path**. GPU is the RTX 3070 (ANGLE D3D11, HW H.264 up to 4096²), and the display runs at 60 Hz (rAF). (I first wrote "1280×720 desktop": WRONG, that was Playwright's emulated default viewport/screen in a new context; the user confirms the desktop is 1440p.) `mv1080_loss_v2` (2 viewers): 0.2–0.9% in the first step, 2.4–5% clean in later steps. DESKTOP LIMIT, not OME: OME delivered 60 frames/s to all four.
  - Method change: `--extra-viewers local`: the main viewer on the desktop (judged on display), the others local Chrome sessions that assemble but do not decode (judged on delivery: missing / >100 ms late / freezes, viewers_report.py `delivery()`), all on the same impaired class. Same OME load per session. `mv1080_loss_l4` next.

- **High RTT with loss (300 ms, 1%).** CHASING (2026-10-23).
  - 4 viewers (`mv1080_rtt_l4`): 100 ms fine for all (local extras 0.9–1.8% late, no freezes); 300 ms: main 11% dropped / 21 freezes, extras 17–31 freezes each. Not a multi-viewer effect: one viewer is the same.
  - Playout delay is not the cause (`pd_rtt_50_200`, `pd_rtt_50_600`, `pd_rtt_none`): 300 ms gives 16–21 freezes in every arm; Chrome's jitter buffer grows only to 104–137 ms.
  - ULPFEC works (`fec_rtt300_on/off`): on 10.7% dropped / 20 freezes, off 46.6% / 69 freezes. The ULPFEC header-extension item is not hiding a broken FEC.
  - What is left is what 1 parity per 7 packets cannot fix: two losses in one group (about 0.3% of about 2,900 groups, so roughly 8–10 expected; 20 observed), each then waiting on a retransmission that needs more than 300 ms against a jitter buffer of about 128 ms. Fix options: make the receiver wait (playout-delay min ≈ RTT + margin, at a latency cost), or stronger FEC on lossy long paths. Testing `--playout-delay 400:600` (`pd_rtt300_400_600`).
  - `pd_rtt300_400_600`: **1.4% dropped, 4 freezes** (from 8–11% and 16–21), jitter buffer 403–406 ms. Waiting long enough for retransmissions fixes it, but a fixed min costs every path about 350 ms. IDEA (PARKED, design): OME sets the playout-delay min per session from its measured RTT and loss (min ≈ RTT + 100 ms only when FEC leaves losses unrepaired), so short paths keep 50 ms.
- Measurement fix: the oracle's remote context used Playwright's emulated 1280×720 viewport (device-metrics override). Now `viewport: null`. Re-checking 4 desktop viewers (`mv1080_desk4_novp`). Result: 26–36% dropped again: not the emulation. DEAD END; the desktop shows at most 2 × 1080p60 cleanly, keep `--extra-viewers local`.

- **Pinned rendition under a bottleneck.** CHASING (2026-10-23).
  - Baseline `pin_rate_base` (1080p pinned, 300 ms drop-tail): 2.0×/1.5×/1.2× play (stutters only); **1.0×/0.8×/0.5× freeze 25–28 s**, i.e. the whole 30 s impairment (2,300–3,200 NACKs, 8–16k packets dropped at the bottleneck). viewers_report.py missed it (a freeze spanning a whole phase leaves no gap inside it): fixed, it now bounds gaps by the phase edges.
  - Design: frame shedding in RtcSession (`ShedVideo`). When the session cannot switch down (pinned, or auto on its lowest rung) and the queue in front of the receiver exceeds 150 ms, video stops at the next delta frame and resumes at the next keyframe. The estimator now measures that queue: a frame's first-packet one-way delay over its 10 s minimum (`GetQueueDelayMs`). Shed frames are never numbered, so there are no gaps and no NACKs. Not the estimator's OverUse: that includes "capacity < 1.5× the rung", which would shed at 1.2×, where the viewer plays fine. Test: `pin_rate_shed`.
  - `pin_rate_shed` (threshold 150 ms): longest freeze 25–28.7 s → 1.02 s at 1.0×/0.8×/0.5×; NACKs 2,342–3,187 → 1–65; bottleneck drops 8–16k → 11–248. Cost: at 1.2× 4 freezes of ~1 s where there were none (latency 222 → 157 ms).
  - Threshold A/B at 1.2×/1.0× (`shed_q150/250/300`): 150: 4 / 15 freezes, 0 bottleneck drops; 250: 1 / 19 (longest 1.75 s), 434 drops; 300: 0 / 21, 1,512 drops (the sim's 300 ms queue overflows first). Chose **200 ms** (`shed_q200`: 1 / 17 freezes, longest 1.0 s, 161 drops at 1.0×). A real path's buffer may be smaller: then loss comes before 200 ms and shedding never triggers; the old behaviour, no worse.
  - 8 s GOP (s720 bypass, `s720_shed_q0` vs `s720_shed_q200`): 2.0×/1.5× no shedding (no false trigger); 1.2× 28 s freeze → freezes of ≤ 8 s (one GOP). A shed that starts just before the congestion ends still waits for the next keyframe (up to a GOP). DONE apart from that; keyframe-on-demand would shorten it (see the keyframe-request item).
- Desktop render floor: since about 11:00 every remote run drops 7–8% on a clean path (it was 0.2–1%), with emulated and real viewport alike (`vp_ab_*`, 2 rounds each). The last arm's recovery phase read 0.0%. Something on the desktop; asked the user. Renderer-drop numbers from this period are not comparable.
- **Firefox remote viewer.** CHASING. Desktop B's Firefox 158 answers WebDriver BiDi on :19224 (puppeteer-core 24). Its remote agent accepts only its own address as Host: the oracle sends `Host: 127.0.0.1:9224`. RVFC, RTCRtpScriptTransform and getVideoPlaybackQuality are all present. The oracle drives it with OME_SOAK_BIDI (limits.py `--remote-firefox`); a BiDi click starts the audio. Not yet run against a stream. The user is moving the desktop tests to another box (desktop A); commands given (Chrome flags, Firefox user.js prefs against background throttling, portproxy, firewall).
- **Desktop moved to desktop A (<desktop-a-ip>), 2026-10-23.** Use the IP: Chrome's debug port rejects a non-IP Host (`desktop-a.lan`). Chrome 156 with the anti-throttling flags (checked in chrome://version), GPU **GTX 970** (driver 32.0.15.6094; HW H.264 reports 4K60 smooth), display 2560×1080 at **75 Hz** (rAF 13.3 ms), Chrome window about 1278×944. Firefox 158 over BiDi on :19224, the same 75 Hz; a new BiDi context opens an 800×600 window (1080p is scaled down). Numbers are not comparable with desktop B (RTX 3070, 1440p60). New baseline: `deskA_base`.
  - Baselines on desktop A (1080p60 rung, 0.2% loss, 50:200): Chrome `deskA_base` 0.5% / 0.2% / 0.0% dropped, no freezes, e2e 56>74 ms. Desktop A's clock runs 80 s off this host; the oracle maps it.
- **Firefox through the sim relay.** First run: ICE failed (`deskA_ff_base`, `deskA_ff_ice`). The relay trace showed STUN both ways and nothing after; decoding the messages (relay `SIM_RELAY_STUN_DUMP`, integrity checked against the SDP's ice-pwd: valid) showed Firefox resending the same transactions despite valid responses. The responses' XOR-MAPPED-ADDRESS was 127.0.0.1: the relay forwarded to OME from loopback, and Firefox discards loopback mapped addresses (Chrome does not). HARNESS ARTIFACT; fixed in 56d8fb8be (the relay sends from the LAN address). `deskA_ff_ice3`: plays, join 2.1 s, 0 freezes, e2e 46>64 ms, 0.0% dropped. Production is not affected (no relay).
  - Firefox's getVideoPlaybackQuality is 0/0 for WebRTC. Drops are now frames decoded (getStats) minus frames presented (rVFC presentedFrames); on Chrome that gives exactly its own count (13 of 4,283 both ways, `deskA_base`).
  - Quick downlink matrix, Firefox then Chrome on desktop A, same binary and settings: `deskA_mx_ff`, `deskA_mx_cr`.
  - Quick matrix (`deskA_mx_ff`, `deskA_mx_cr`; gpu4k 1080p60 pinned, 50:200): **both browsers alike in freezes** in every dimension (loss 0.5/2%, bursts, reorder 25–200 ms: none; holds and outages: one freeze of about their length; loss 10%: 10 vs 13; rtt 200: 23 vs 14 short ones of ≤ 0.22 s; rtt 800: 31 vs 30). Firefox's e2e is 5–10 ms lower (40–45 vs 50–55 ms). Same FAILs in both: jitter 60 ms (a 1.03 s freeze over the bound 0.66 s) and jitter 240 ms (not recovered after 10 s); Chrome also loss 10% (1.75 s vs bound 1.56 s).
  - **Jitter 60 ms FAIL = false frame shedding.** In the 1.04 s freeze only one keyframe's packets (94) arrived, nothing lost or NACKed: OME shed to the keyframe, resumed, shed again at the next delta, resumed at the next keyframe. The relay's FIFO jitter (iid Gaussian, order kept) at ~1,000 pps is a running max: a queue 150–250 ms deep that does not come from our rate. Fix (uncommitted): shed only when the queue is over 200 ms AND the link carries less than we send: the one-way delay grows (least-squares slope over 4 s > 0.1) or more than 5% of packets are lost over that window (a full drop-tail queue stops growing). Growth alone (`deskA_growth_*`): jitter 60 → 0 judged freezes, but rate 1.0× collapsed again (8.4 s freeze, 2,439 NACKs: the queue was full, flat, losing); a 2 s window still shed twice under 60 ms jitter. Testing growth-or-loss, 4 s: `deskA_gl_jit`, `deskA_gl_rate`.
- **Auto ABR on desktop A, both browsers** (`deskA_abr_ff`, `deskA_abr_cr`; abr playlist; 3.0/1.2/1.0/0.8/0.5×): **every phase on the best rung, settled in 2.0–6.0 s**, 0 PLIs. Freezes in the bottleneck phases: 0–7, longest 0.19–1.58 s (most at 0.5×, two down-switches). FAILs only from the capture probe's decode check (1.0× ff 27 frames, 0.5× 7 / 17): the known probe artifact under heavy congestion (see 2026-10-23 cor5); to be confirmed with `--reference-probe` in the final pass.
- **Chrome renderer floor on desktop A: the desktop, not OME.** From the matrix on, Chrome drops 5–8% on clean phases (the baseline that morning: 0.5%). A plain 1080p60 MP4 in the same Chrome (no WebRTC, no OME): **59 of 899 dropped (6.6%)**. No leftover contexts in the remote Chrome. Raising our window (minimise + restore via CDP, `OME_SOAK_RAISE=1`, `deskA_floor_*`): no change (5.3–8.0% either way). Firefox shows 0.0% throughout (decoded − presented). Asked the user to look at the desktop. Chrome drop percentages from this period: compare against the file control, not across time.
  - Growth-or-loss, 4 s (`deskA_gl_*`, Firefox): jitter 60 ms 1 freeze (0.25 s), 240 ms 11 (from 15 / 31); rate 1.2× 0 freezes, 1.0× / 0.8× / 0.5× longest 1.0 / 1.14 / 1.44 s, NACKs 242–414; both PASS. **DONE a47a364cf.**
- **Chrome at 10% loss (matrix FAIL, 1.75 s vs bound 1.56 s).** In the freeze 124 frames arrived, 6 decoded, 7 PLIs: a frame Chrome could not repair broke the chain and the next keyframe (≈50 packets at 10% loss) needed repairs as well. Firefox under the same loss: no PLIs, longest 0.45 s. Chrome receiver limit at extreme loss; OME cannot answer the PLIs on a transcoded rung without the keyframe-request plumbing (PARKED item). Not chased.
- **Join time.** From the matrix: ICE+DTLS connect 72–92 ms in Chrome, 240–280 ms in Firefox; then the first frame after 5–1,055 ms, spread evenly: OME starts a session on the live packets and the browser waits for the next keyframe (gpu4k GOP 1 s, so ~0.5 s on average). Shown 2–56 ms after its arrival. A GOP-backlog fast start would show a picture at once but leave every viewer up to a GOP behind (WebRTC receivers do not catch up), so not done: latency is the goal. The lever is the source's keyframe interval (an 8 s GOP like s720 means ~4 s average join). Production's GOP unknown (the API read was not allowed); asked the user.
- **4K60 with 4 viewers on desktop A** (`deskA_4k1_*`, `deskA_4k4_*`; Bypass+opus, main viewer on desktop A, 3 local extras judged on delivery). Single viewer, 0.2%: Firefox 0.0% dropped, 0 freezes (Chrome 7% = the desktop floor). The GTX 970 decodes 4K60 fine.
  - 4 viewers, main viewer: Firefox 0.2% **0 freezes, 0.0% dropped** (`deskA_4k4_ff_loss_r`), 2% 4 freezes ≤ 0.33 s, rtt 100 ms 0 freezes; Chrome 2% 5 freezes ≤ 1.0 s, rtt 100 ms 2 freezes ≤ 0.23 s. Extras: 0.2% 1.2–1.6% missing, no freezes beyond 0.22 s; 2% and rtt 100 ms 3–17% missing, 10–17% late, 9–17 short freezes, while the main viewer on the same OME path stays clean: the local headless transform viewers at 4K (three on this host), not OME. Multi-viewer delivery at 4K: PASS for the displayed viewer.
  - `deskA_4k4_ff_loss` (first run) had 18–58% missing for the extras: my first compile ran over its impairment phase. INVALID; the rerun above is the result. Lesson: never build while a run is going.
- **AUD and timing SEI in their own packet.** Every delta frame of the NVENC rungs is AUD (2 B) + SEI pic_timing (8 B) + slice (~12 kB at 1080p): the STAP-A of AUD+SEI is one extra packet per frame (~9%), and losing it delays the frame like any other. WebRTC decoders use neither (libwebrtc drops AUD). Change (uncommitted): RtpPacketizerH264 drops AUD and SEI made only of buffering_period/pic_timing; other SEI (captions) is kept (unit tests `RtpPacketizerH264.*`). WebRTC only (the only user of this packetizer). A/B on Firefox, 2 rounds: `aud_keep*` vs `aud_drop*` (`OME_TEST_KEEP_AUD_SEI=1` keeps them).
  - Desktop A dropped off the network at ~14:28 (no ping, ports closed): the Firefox A/B (`aud_*`) never connected after its first step. Redone with the local headless viewer (`audl_keep1/2`, `audl_drop1/2`): all PASS, no decode errors. Dropping: ~7% fewer packets, NACKs and bottleneck drops (10% loss: NACKs 2,205/2,105 → 2,036/1,977, drops 2,372/2,243 → 2,177/2,111; stutters 79/82 → 59/66). Freezes unchanged within noise (10%: 7/8 vs 11/8; rtt 200: 12/5 vs 11/12). Small, consistent; kept.
- **Adaptive playout-delay minimum** (the idea parked at `pd_rtt300_400_600`). Local headless viewer reproduces the problem (`lpd_50_200`: 15 freezes, 16.6% dropped at 300 ms / 1%; `lpd_400_600`: 4, 1.9%). Design: RTT from NACKs (NACK arrival − the lost packet's send time ≈ RTT, which is also when the repair arrives; OME's SRs carry media NTP time, so LSR/DLSR would be wrong). 10 NACKs in 10 s and repair > 100 ms (what Chrome buffers anyway) → min = repair + 80 ms, max ≥ min + 200, for that session only (the send thread rewrites the extension in its copy).
  - `apd_off_*` / `apd_on*` / `apd_*_r2/r3`: 300 ms freezes off 13/15/16/22/14 vs on 3/9/6/4; dropped 14.4–18.4% vs 0.8–5.3%; 100 ms dropped 11.0 → 3.0–8.0%; 800 ms longest 1.93 s (FAIL) → 1.02–1.30 s; 50 ms and 2% loss at 30 ms unchanged (no raise). Cost: ~390 ms delay for that viewer.
  - A/V sync: Chrome syncs audio to the raised video delay at ~10 ms/s: after the raise audio leads by ~245 ms for ~20 s (an RTT jump without the feature gives ±90–264 too). The first version dropped back 10 s after the last NACK, in one go: audio then lagged ~280 ms at the end of recovery (`apd_on_rtt` av +182/+260). Now down only after 60 s without NACKs, 20 ms/s (`apd_on3_rtt`, r2/r3: av −43/+11 at the end, like off). A real long path is long from the start, so the raise lands in the first seconds of a session. **DONE 20811abe9**; it needs `<PlayoutDelay>` configured.
- **Regression pass on the local viewer** (`reg_mx`, `reg_abr` with references): ABR still best rung everywhere (settle 1.7–6.3 s). But the adaptive playout delay (20811abe9) **raised the delay for viewers that were not on a long path**: recovery e2e outage 1 s 1,094 ms, burst 128 548 ms, rate 0.8× 414 ms, ABR 1.0/0.8/0.5× 412–437 ms. The NACKs' own delay included the queue or the outage behind the loss, and an outage alone gave 10 NACKs. Fix: the repair time is the path's **base RTT** from transport-wide feedback (feedback arrival − send of the newest packet it reports; minimum over 30 s), and the raise needs NACKs in 5 different seconds of 10. `apd2_*`: outage 63 ms, burst 63 ms, rate 0.8× 207 ms (as without the feature), ABR 70 / 184 ms. Cost: a mid-session RTT jump is protected only once the 30 s minimum has forgotten the shorter path (`apd3_long_*`, 70 s at 300 ms: first 35 s 25 vs 25 freezes, last 35 s 23 off vs 7 on). A shorter window would read a standing congestion queue as path RTT.
  - Also from `reg_abr`: one keyframe "differing from what OME sent" was 49 bytes short = its SPS+PPS STAP-A (now that AUD/SEI are gone, that packet holds only the parameter sets). Chrome assembled the keyframe without it; it decodes with the earlier identical SPS/PPS. delivery_check.py now treats SPS/PPS like AUD/SEI. Matrix FAILs reorder 100 ms (one 0.68 s freeze) and outage 250 ms (not recovered: e2e 349, the raise): the first to re-check on desktop A.
- **Final pass on desktop A, both browsers** (`fin_mx_ff`, `fin_mx_cr`, `fin_abr_ff`, `fin_abr_cr`, build 2cd07ad16). The first attempt (17:15) never connected: Desktop A had gone to sleep (ping is blocked there anyway; check port 19222). The user now runs DontSleep on it. Void logs in `logs/void_deskA_offline/`. limits.py crashed on a step without video (zero bitrate in the outage prediction): guarded in 2cd07ad16.
  - Matrix: both browsers alike again; reorder 100 ms and outage 250 ms (the local FAILs) pass on both. Chrome's clean drops are 0.0–0.2%: the ~6% desktop floor from this afternoon is gone (the plain-file control was not re-run). Firefox FAIL loss 2% (0.18 s freeze in recovery, noise); Chrome FAIL RTT 800 ms (2.03 s, 13 PLIs).
  - ABR: every phase on the best rung on both browsers, settled 1.8–6.1 s, 0 PLIs. The 3 "differing" frames at 1.2× (ff) and 0.8× (both) were past the end of the 1080p reference's capture (it stopped 0.4 s before the probe): compared with the 720p reference's frames of similar size. HARNESS; `delivery_check.py` now judges only the span every reference covers: 0 differing in all ten steps.
  - RTT 800 ms on Chrome, per-second getStats: jitter-buffer target 38 ms until the impairment, 38–187 ms during it, ~500 ms from second 46 (right after it ended), with 16 PLIs. The 30 s base-RTT minimum still held the clean phase's RTT for the whole impairment, so the raise came at its end, then held for the 60 s quiet time (e2e 529 ms in recovery; Firefox 511). Why Chrome's target is ~500 and not RTT + 80 = 880 is not known (no OME clamp at 500; no debug log in this run). A mid-session RTT jump is the case the 30 s window gives up. CHASING: a raise that follows a sustained RTT increase without reading a congestion queue as RTT.

## 2026-10-24

**Upstream vs this branch, residential profiles** (`misc/webrtc_impair/compare_builds.sh smoke 3`, `logs/cmp_{patched,upstream}_r{1,2,3}`).
Upstream = eeb875103 built in ../OME-upstream (only the build-side dependency check from this branch applied). Same harness conf
as production (PlayoutDelay 50:200). 4K60 Bypass, Chrome + Firefox on desktop A + the local probe at once. Medians of 3 runs:

| profile | freezes Chrome pa/up | freezes Firefox pa/up | delay p50 Chrome pa/up | probe complete pa/up |
|---|---|---|---|---|
| clean | 0 / 0 | 0 / 0 | 113 / 80 ms | 100 / 100% |
| loss_1pct | 1 / 22 | 0 / 25 | 120 / 57 | 97.7 / 97.8% |
| burst_loss | 1 / 47 | 4 / 37 | 133 / 54 | 97.1 / 43.0% |
| jitter_reorder | 0 / 2 | 0 / 3 | 184 / 150 | 95.6 / 94.8% |
| stalls | 1 / 2 | 1 / 2 | 182 / 174 | 99.3 / 99.8% |
| real_world_mix | 0 / 35 | 1 / 40 | 147 / 144 | 96.3 / 73.5% |

- The probe score cannot tell the builds apart at 1% loss (97.7 vs 97.8%) while upstream's viewers froze 22–25 times a run.
  It is the probe's packet-buffer artifact (59 clears/min at loss_1pct, counted now). The smoke test now judges by the viewers.
- **Latency cost of the patched pacer:** frames leave OME with p99 122–142 ms of jitter against their timestamps (upstream
  33–64 ms; `send_timing.py`), and the browsers' jitter buffers follow (patched 100–145 ms, upstream 37–62 ms on clean).
  c4e796aa0 releases at arrival + hold instead of the timestamp target, so the source's arrival jitter passes through.
  The production encoder is no smoother: production's pacer log has p99 arrival lateness median 114 ms (211 lines,
  2026-10-20..24). With limits.py's paced publisher (no source jitter) the patched build's clean delay is 48–57 ms.
- First upstream attempts failed to start: run.sh stop did not wait for the old OME to free its ports (fixed).
- One probe completeness of 0.1% was a 32-bit timestamp wrap from a frame older than the first (fixed: jumps > 5 s).
- Report: https://claude.ai/artifact/LJsVaKeh8qYrbrbQMhZmKz (best-case model: `misc/webrtc_impair/best_case.py`).

**Stress matrix, upstream vs patched** (`LIM_STREAM=s1080 compare_builds.sh limits`, `logs/cmp_lims1080_*`; gpu4k:
`logs/cmp_lim_*`). s1080 = 1080p60 High with B-frames passed through. Freeze time over the 22 quick steps: patched
92 / 88 s (Chrome / Firefox), upstream 224 / 213 s, best case ~13 s. Upstream behind on loss ≥ 2%, bursts, RTT 200,
reorder 100–200, short outages; ahead at jitter 240 (1 s vs 8–10 s frozen). gpu4k upstream: software transcode
(no NVENC/NVDEC in 0.21), rung at 30–42 fps, clean phases stutter: not a delivery comparison, Chrome only.
- The decode check sorted keyframe runs by RTP time: every B-frame capture failed (fixed d636baefa; remaining
  late-frame false positives in the threads table).

**Capped pacer release** (bb2f1b7c1: release at the DTS target, never later than arrival + AdaptiveDelay). Unit test
`LateFramesLeaveOnDtsCadence` fails on the old rule (send offsets 50–81 ms) and passes. Binaries kept in
`../OME-builds/` (`OvenMediaEngine-bfb75a4a9` = the deployed patched build, `-capped`).
`CMP_BUILDS="capped patched" BIN_capped=… compare_builds.sh smoke 4` (`logs/cmp_capped_r{1..4}`, `cmp_patched_r4`):

| profile | delay p50 Chrome ca/pa | delay p50 Firefox ca/pa | freezes Chrome ca/pa | freezes Firefox ca/pa | send p99 ca/pa |
|---|---|---|---|---|---|
| clean | 68 / 123 ms | 56 / 88 | 0 / 0 | 0 / 0 | 71 / 123 ms |
| loss_1pct | 79 / 115 | 47 / 76 | 0 / 0.5 | 0 / 0 | 74 / 126 |
| burst_loss | 70 / 124 | 46 / 80 | 0.5 / 1 | 0 / 3 | 71 / 122 |
| jitter_reorder | 139 / 173 | 111 / 143 | 0 / 0 | 0 / 0 | 75 / 127 |
| stalls | 137 / 182 | 104 / 156 | 1 / 1 | 2 / 1 | 73 / 123 |
| real_world_mix | 108 / 143 | 89 / 119 | 0 / 0 | 1 / 1 | 82 / 129 |

- patched r4 (drift check) matches r1–3. All 24 capped profile runs are inside profiles/baseline.json's bars.
- s1080 matrix (`LIM_STREAM=s1080`, `cmp_lims1080_capped_*`): freeze time 110 / 90 s vs patched 92 / 88 s. Chrome's
  extra is burst=4 alone: 18 s, Chrome decoding 2 frames per GOP and sending a PLI every 200 ms from 14 s into the step,
  with every lost packet repaired (231 lost, 235 RTX). Reran burst ×2 capped, ×1 patched (`rep_burst_*`): capped 0
  freezes both times, patched 1.96 s at burst=128. One-off; PARKED unless it shows again.
- jitter=240 unchanged (7.6 / 9.0 s vs upstream ~1 s): the cap never holds past AdaptiveDelay.

**Early anchor** (uncommitted: calibrate on the p5 of lateness instead of the median; `OME-builds/OvenMediaEngine-early`).
Smoke ×5 vs capped ×5 (`cmp_early_r*`, `cmp_capped_r*`; capped r5 rerun in the same batch):
- Send spread p95−p50 (median of runs): clean 4.3 → 3.2 ms, jitter_reorder 9.8 → 4.2, real_world_mix 6.3 → 2.5,
  stalls 3.8 → 5.2. The p99 tail is unchanged (22–30 ms both).
- Viewer e2e p95 is lower in 11 of 12 profile×browser rows (e.g. Firefox loss_1pct 91 → 55 ms, Chrome stalls 214 → 177).
  p50 is the same within noise.
- Freezes ≥ 0.9 s: 5 early vs 3 capped over the 30 lossy runs each. Firefox real_world_mix early 3 of 5 (1–2 s). Every
  one is a GOP-skip (gap starts and ends on a keyframe, exactly 1 s / 2 s of media, NACK+RTX and discards rising, no
  PLI). Patched (2/4) and upstream (3/3) show the same class. 3/5 vs 0/5 is not settled (one-sided Fisher p ~0.08;
  early r1-5 ran back to back, not interleaved with capped). OME side is identical between builds: per keyframe send
  time ~38 ms, ~3 relay drops, ~250 RTX (Firefox NACKs most of a 4K keyframe for a few drops), last RTX 53 ms after
  the keyframe (p50), and the frame after a keyframe goes missing 13 vs 14 times. Difference is Firefox's recovery:
  capped ~0.3 s, early waits for a later keyframe; in r1 and r4 the next keyframe arrived repaired and still was not
  shown. Cause OPEN.
- s1080 limits matrix (`cmp_lims1080_early_*`): Chrome freeze time 162 s vs capped 110, patched 92 (Firefox 94 / 90 / 88).
  The extra is rtt=50 (27.5 s), rtt=200 (17.9 s), reorder=200 (18.1 s): the same sticky state as capped's burst=4
  one-off. Chrome decodes the keyframe plus one frame per GOP, sends a PLI every 200 ms (no decodable frame) and stays
  that way after loss stops (rtt=50: lost flat at 217 from 45 s, still stuck at 64 s). The probe's arrival-vs-RTP offset
  shows no re-anchor step at the onset. Occurrences per matrix: patched 0, capped 1, early 3.
- DROPPED: two independent regressions (Firefox smoke GOP freezes, Chrome sticky state). Diff kept as
  `OME-builds/early-anchor.patch`, binary `OvenMediaEngine-early`; tree back to bb2f1b7c1's pacer.
- Chrome keyframe-only state on s1080: SOLVED 2026-10-25, B-frames (below).
- Baseline regenerated from capped r1–5 (88ec07f60).
- Next: s1080 limits matrix for early (`cmp_lims1080_early_*`).

## 2026-10-25

**Chrome keyframe-only lockup = B-frames in Chrome's software H.264 decoder.**
- Traced rerun (early, desktop A Chrome, burst/rtt/reorder, `--keep-trace`: `cmp_lims1080_early_chrome_trace1`): rtt=200
  locked at ~28 s (19 s frozen), rtt=50 too. From 44 s: no drops, no RTX, OME's stream identical in structure to the
  working stretch (no seq gaps, one marker per frame, same GOP order K P B B). Chrome receives 60 frames/s, ~2/s
  leave its jitter buffer, framesDropped +1 per GOP.
- Local headless Chromium with `--chrome-log` (software decode): locked from the clean phase on every step, both
  early (`chromelog_early_1`) and capped (`chromelog_capped_s1080`): 0.5 fps, ~50 per step of
  `h264_decoder_impl.cc avcodec_receive_frame error: -11` then "Failed to decode frame", then "Generic frame ... has no
  GoP, dropping frame". Control: NVENC 1080p rung (no B-frames, `chromelog_capped_gpu1080`) 56–58 fps, s720
  (`bframe_warn_s720`) 30 fps, zero errors.
- Source: WebRTC `H264DecoderImpl::Decode()` sends one packet, receives one frame, any negative (EAGAIN included) =
  WEBRTC_VIDEO_CODEC_ERROR. Chromium `RTCVideoDecoderAdapter` falls back to software for good on an async decode error
  or >5 consecutive pending-buffer overflows. So desktop A (hardware decode) locks only after a decode error under loss,
  and stays locked: the burst/rtt/reorder freezes in the matrices. Early 3 vs capped 1 is how often that happened.
- Production source has NO B-frames (`sim_2160p60_baseline.ts`: Constrained Baseline, IPPP). I first read 31%
  timestamp backsteps on its smoke trace as B-frames: those are ULPFEC packets in RED (timestamp of the protected
  frame, sent after the next frame starts). Check frame types with ffprobe, not RTP timestamps. (production change #6)
  is the requirement only.
- 680b3abcc: OME logs once per H.264 WebRTC track with B-frames (fires on s1080, silent on s720). Binary
  `OME-builds/OvenMediaEngine-bwarn`. compare_builds.sh got LIM_RUN / LIM_ARGS.
- Open: the early anchor's Firefox 4K GOP-skips are NOT B-frames (the 4K source has none): still unexplained. Its
  Chrome s1080 count (3 vs 1) is B-frames plus how often desktop A's hardware decoder hit an error.
- Interleaved real_world_mix x8 (`cmp_earlyx_r*`, `cmp_cappedx_r*`, builds alternating per rep): freezes >= 0.9 s
  Firefox early 0/8 vs capped 1/8, Chrome 2/8 vs 1/8 (freeze total Firefox 1.7 vs 3.2 s, Chrome 7.7 vs 3.8 s). The
  3/5 vs 0/5 does not reproduce; with all 13 runs each, Firefox 3 vs 1, Chrome 3 vs 2. No evidence left of an
  early-anchor regression. Early anchor back to CANDIDATE (patch `OME-builds/early-anchor.patch`), pending a limits
  matrix on a B-frame-free stream.
- s720 limits matrix, early vs capped, both browsers (`cmp_lims720_{early,capped}_{chrome,firefox}`): freeze total
  Chrome 160.9 vs 132.0 s, Firefox 122.9 vs 146.4 s. All of the difference is rate=1.2x, which flips both ways (Chrome
  early 29.0 / capped 0.2, Firefox early 0.2 / capped 23.6): a knife-edge step (keyframe vs the 300 ms queue), not the
  pacer. Without it: Chrome 131.9 vs 131.8, Firefox 122.7 vs 122.8. Every other step within ~1 s except one capped
  Chrome outage=1000ms (4.1 s, F1). Impaired e2e median Chrome 85 vs 90 ms, Firefox 66 vs 66. jitter=240 fails on
  both builds as before.
- Verdict: early anchor is NOT a regression, and shows no viewer-visible gain either (its halved send spread does not
  reach freezes or e2e). Kept as a patch, not committed; capped stays in production. Revisit only if a viewer
  metric needs the tighter send spread.
- Jitter 240 ms is NOT the pacer: the dimension is downlink FIFO jitter, which ratchets the path delay to ~0.9-1.2 s
  (relay due p50 919, max 1232 ms) with swings of a few hundred ms. Traced capped vs upstream
  (`cmp_lims720_{capped,upstream}_chrome_j240tr`): capped sent 1 frame per 8 s GOP from 17 to 41 s, upstream 30 fps.
  Cause: `RtcSession::ShedVideo` sheds at the delay jump, resumes at the keyframe, and re-checks on the next frame with
  the estimator still holding the pre-shed state (nothing was sent, the keyframe's feedback is ~1 s away): sheds again
  every GOP. rate=1.2x lockups (24-29 s) show the same 1-frame-per-GOP pattern. (Careful: trace due_ms is rounded to
  10 us while the relay's order clamp is 1 us, so t+due "reorders" are artifacts.)
- Fix: shed again only on feedback for a frame sent after the resume (`GetQueueSampleSendMs`). `OME-builds/OvenMediaEngine-shedfresh`.
  A/B s720 x1 (`*_sf_j240`, `*_sf_rate`): jitter 240 Chrome 27.4 -> 15.4 s, Firefox 27.7 -> 7.4 s; rate 0.8x unchanged
  (~28 s, shedding still needed there); rate 1.2x 1 shed GOP on both. What is left at jitter 240: the first shed, and
  Chrome dropping late frames + PLIs (67) that OME ignores, also after the delay drops (upstream too). (production change #7).
- First shed on a delay step (5040fea10, `OME-builds/OvenMediaEngine-shedcap2`): shed only while the estimator's
  packet-train capacity (median over 4 s) is under the stream's rate, or unknown. A delay step bunches packets, so the
  capacity reads high or unlimited; a narrow link spreads them to its rate. v1 (`OvenMediaEngine-shedcap`) compared
  against the media rate just sent, which a shed lowers for ~4 s: at 0.8x it sent full rate ~4 s per GOP, drops
  100-290/s, Chrome e2e ~3 s. v2 uses the max of the rendition bitrate and the media rate from unshed 1 s windows.
- A/B s720 x1 shedcap2 vs shedfresh (`cmp_lims720_{shedcap2,shedfresh}_{chrome,firefox}_sc2_{j240,misc}`), freeze s
  impaired: jitter 240 Chrome 1.5 vs 21.9, Firefox 2.2 vs 22.1, shed seconds (trace, <5 frames/s) 0 vs 18-19.
  0.8x shed 24/21 vs 23/20 s, e2e 587/399 vs 627/425 ms, but link drops 702/681 vs 496/481 (cause not traced; maybe the gate
  waiting for capacity samples before the first shed; freeze and fps the same; x1, not yet repeated). Everything else on par (loss 10% Chrome 1.8 vs 0.5,
  longest 0.27 both: noise). Chrome jitter 240 still "not recovered": at +35 s (5 s after the jitter ends) a 7.2 s
  keyframe wait while OME sends 30 fps with no drops; Chrome was holding 1.14 s recv->present. Same moment on
  shedfresh. Receive side + PLI ignored, not the shed. Remaining FAILs are outage=4000ms against a bound that varies
  run to run (5.89 vs 12.55 s, same 8.4-8.7 s freeze on both builds).
- **Chrome's GOP-long freeze after the jitter ends = Chrome timing snap + desktop A's hardware decoder queue.** Not OME.
  Chrome stats (`cmp_lims720_shedcap2_chrome_sc2_j240`): target delay 188 ms throughout, but from +31 s (jitter
  over, path delay ~1 s -> 20 ms) frames wait 1.12 s in the jitter buffer; at +35.3 s 18 frames dropped, then 0
  decoded and a PLI per 200 ms until the source keyframe at +42.3 s. OME sent 30 fps, nothing dropped on the link.
  Source (WebRTC branch-heads/8078 = Chrome 156): render time = TimestampExtrapolator local time + clamp(current
  delay, min, max), so the playout-delay max does not cap it. Hard outlier rejection (on by default, 2 sigma) rejects
  the now-early frames for `outlier_rejection_max_consecutive` = 150 frames (5 s at 30 fps), then SoftReset: render
  times jump ~1 s earlier and the ~33 buffered frames are released at once (late, wait 0; IPPP has one decodable
  unit, so no fast-forward). Chromium `RTCVideoDecoderAdapter` (hardware decode) holds `kMaxPendingBuffers` = 8:
  overflow clears them and requires a keyframe; WebRTC's `ForceKeyFrameReleaseImmediately` drops the rest (the 18).
  Local headless Chromium, software decode (`chromelog_j240_shedcap2`): same 1.15 s hold and snap at +35.5 s, 0
  drops, 0 PLI, PASS. So it needs both the snap and a hardware decoder.
- Same signature at outage=1000ms on shedfresh Chrome (NACK repair releases a burst, jb 267 ms, 13 dropped + PLI,
  6.9 s freeze); shedcap2 decoded 16 frames in 0.2 s there and survived: a race between burst and decoder. Likely
  also the one-off Chrome outage freezes in earlier matrices (capped F1). Not verified per case.
- OME side: playout-delay min 0 (Chrome's "render ASAP" mode skips the extrapolator) is ruled out by the 2026-10-22
  clean-phase misses. The only sender-side fix is answering PLI with a keyframe, which OME cannot do: the publisher
  drops PLI/FIR, and the transcoder has no forced-IDR path. On a shared encoder it would also give every viewer of the
  rung an extra keyframe (rate limit needed), and it cannot help passthrough. Production's 1 s GOP caps each such
  freeze at ~1 s (s720's 8 s GOP makes it look large here). PARKED: a feature decision, not a bug.
- **Opt-in switches (09c1dc6e6), for publishing the fork:** `<SendPacing>`, `<VideoShedding>`,
  `<PlayoutDelay><Adaptive>`, `<JitterBufferDelay>` (default 20:500 as upstream; we ran 80:500), all off by
  default. Test confs turn them on; `limits.py --webrtc-features` (none = upstream-readable conf),
  `OME_IMPAIR_WEBRTC_FEATURES=none`, compare_builds `FEAT_<name>=none` for older binaries. Check: s720 rate 0.8x
  all on 8.0 s longest / 494 drops vs none 28.2 s / 8112 (`featsw_{all,none}`); loss 0.5% passes both
  (`featsw2_*`); Min > Max rejected at load. Production needs #8 with the deploy.

## 2026-10-27

- **Release soak: real desktop browsers inside the traffic sim** (eb53bf560). `traffic_sim.py --desktop NAME=ENDPOINT`
  (repeatable): a remote Chrome (CDP) or Firefox (BiDi) watches live streams back to back through the sim relay with a
  drawn network class, rendition, outage and leave (polite / vanish), judged per session by viewer_check.py
  (`desktop.jsonl`, report section "desktop viewers"). `--playout-delay 50:200 --adaptive-playout` runs production's
  PlayoutDelay (conf_override). Desktops: Desktop A (<desktop-a-ip>) and desktop B (<desktop-b-ip>), both browsers.
- `misc/remote_viewer/Setup-RemoteViewer.ps1` sets a Windows desktop up (profiles, flags, portproxy, firewall to the
  controller, keep-awake) and serves a control service on :19220 (`GET /status`, `POST /restart/{chrome,firefox}`,
  test-profile processes only). The sim restarts a browser through it after 3 quick failures in a row.
- HARNESS, trial 1 (`desk_trial1`): a Firefox oracle joined an offline stream and hung on `await play()` (an element
  without a source never settles); reap killed it, and Firefox kept its one BiDi session ("Maximum number of active
  sessions") until restarted. Fixed: play() not awaited, the oracle closes its own context within 15 s of SIGTERM,
  desktops join live streams only. viewer_check now reads sim_relay's CSV trace (e2e was missing).
- Trial 1 Chrome: Desktop A 1080p residential 0.19 s longest; desktop B s720 Bypass residential one 8.0 s freeze with 37 PLIs
  (the known PLI-ignored GOP wait on the 8 s GOP); desktop B Chrome s1080 dropped 33% (desktop B, as before).
- `release_soak_nosilence` (stopped at 23 min): the sim conf lacked production's RTMP PacketSilenceTimeoutMs 10000,
  so a silent uplink death left a zombie publisher (the known #3 bug, FAIL streams). conf/ now mirrors production
  (fc02f24c1).
- **`release_soak`** (9000 s, seed 21, switches build 67912ff0 with all four switches on, PlayoutDelay 50:200 adaptive,
  4 desktops + mean 12 lightweight viewers): sessions, streams, simulator, server log, crash all PASS. **FAIL leaks:
  RSS 275 -> 418 MB at the final quiesce** (threads 73->73, fds 65->65), with every stream and session gone. Earlier
  runs: +10..15 MB in 6..25 min, so ~1 MB/min, roughly linear. Not known yet whether the switches build or the
  churn (NVENC restarts, segcache) causes it. A/B `release_soak_prodbin`: same seed and desktops on production's
  pacercap2410 binary, `--no-switches` (new option: removes the three switch elements an older build refuses).
- Desktop table oddities to check after the A/B: Chrome residential 6.5 freezes/min on desktop A (2.9 desktop B) vs Firefox
  0.4..0.7; desktop A Firefox wifi 31.6% drops; negative "min" on terrible (judge returns a negative picture time when
  the session never gets a picture; reporting bug).
- **`release_soak_prodbin`** (A/B, production's pacercap2410, same seed, desktops, PlayoutDelay 50:200, no switches):
  leaks PASS **277 -> 311 MB (+34)**, vs +143 on the switches build. At the final quiesce: Anonymous 168 MB,
  Private_Clean 117 (smaps.log). One unexpected server-log line (transcoder "Could not found decoder" on livegpu,
  once). Viewer quality comparable: Firefox fiber/residential 0.25..0.70 freezes/min (switches 0.06..0.67);
  Chrome residential desktop A 0.66 / desktop B 4.70 (switches 6.53 / 2.88), so the Chrome residential freezes are not
  the switches. Desktop A Firefox wifi drops 32.8% here too (not the build).
- `leak_repro_switches` (3600 s, switches build, switches on, smaps.log): does the extra growth reproduce?
  Result: leaks PASS **274 -> 299 MB (+25 in 1 h)**; at the final quiesce Anonymous 156 MB, Private_Clean 120,
  i.e. the same as production's binary after 2.5 h (168 / 117). The +143 of `release_soak` did not reproduce at
  this length; most likely allocator retention in that one run, not a steady leak. Not ruled out for long uptimes:
  a 2.5 h switches rerun with smaps.log would settle it.

## 2026-10-28

- **RtcKfDrain warned on every keyframe** (production: 4,635 lines in one evening, 6 viewers). Now only on a stalled
  send (a packet waited >= 100 ms, the drain took >= 100 ms or the queue stayed >= 64 deep), at most once per 10 s per
  session; every drain at debug level. Harness smoke PASS on all 6 profiles (`webrtc_impair/logs/smoke_20261028_075914`).
  **DONE** (in the squashed `fix(webrtc)` commit).
- **`release_soak2`** (9000 s, seed 21, the log-fix build = switches build + the RtcKfDrain change, same config and
  desktops as `release_soak`; heap and mapped files now in every sample): **all checks PASS**. Leaks: RSS 277 -> 261
  MB at the final quiesce; heap (Anonymous) 133 -> 170 MB, mapped files 121 -> 67 (page cache reclaimed). Under load
  the heap peaks at 2.4..2.6 GB in every 15-minute window from the first to the last, with no upward trend. Together
  with `release_soak_prodbin` (heap 168 at the end) and `leak_repro_switches` (156): no leak; the +143 of
  `release_soak` was allocator retention. **DONE.** RtcKfDrain warnings: 259 (17,429 in `release_soak`), mostly
  sessions on 4G/terrible/bottleneck classes, at most 11 for one session.
  Desktop table like `release_soak`: Firefox fiber/residential 0.00..0.88 freezes/min, Chrome fiber 0.07..0.36,
  Chrome residential 1.19 (desktop A) / 3.06 (desktop B). Residential WARNs: two 8 s freezes on s720 (8 s GOP, no
  PLI answer: known) and 2..4 s on s1080 (B-frames: known). Desktop A Firefox wifi drops 32.7% again (not the build).

## 2026-10-29

- **Chrome freezes on a live 1080p60 passthrough (remote OBS over SRT), one viewer, Chrome 156 with D3D11
  hardware decode on an RTX 3070:** 1.4–3.9 s about once a minute, zero loss and zero NACKs, many PLIs. Not OME:
  a local per-frame probe (60.0 fps, no gaps, worst frame 59 ms late) and an H.264 dump of what OME sent (FFmpeg
  decodes the exact stalled GOPs without an error) were clean. Chrome's own log (`--enable-logging
  --vmodule=generic_decoder=1,video_receive_stream2=1,...`) showed the mechanism: "Too many frames backed up in the
  decoder" for ~3 s, then "Failed to decode frame. Return code: -1", a decoder reset and a wait for the next
  keyframe (OME ignores PLI). Cause: the web player's WebGL effects, which hide the `<video>`
  (`visibility: hidden`) and copied it into a texture; with the effects off the hang lines went from 67–835/min to
  0. Copying once per decoded frame did not help; leaving the `<video>` visible did (pliCount flat) but looked
  jittery. Fixed in the player: WebRTC frames straight from the track (MediaStreamTrackProcessor), paced onto a
  steady timeline and closed right after drawing: no hangs, no jitter. **DONE** (player side). Lesson: for Chrome
  freezes with zero loss, get Chrome's log before suspecting OME.
- Side finding: OME put the framemarking extension (ID 1) on every video packet although no browser negotiates
  it; Chrome logged "Unidentified extension in the packet" for every RED packet (from `ZeroMutableExtensions` in
  its ULPFEC receiver). Removed from the SDP offer and the packets; OvenRtcTester now finds keyframes in the H.264
  payload. Harness smoke PASS on all 6 profiles (`webrtc_impair/logs/smoke_20261029_233236`, headless viewers only:
  the desktop browsers were not reachable). **DONE.**
