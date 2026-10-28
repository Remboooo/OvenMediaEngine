# WebRTC viewer limits: findings (phase 2, October 2026)

What the limits test (`limits.py`), the 4K smoke test (`misc/webrtc_impair/smoke_test.sh`) and follow-up
experiments found, what was fixed, and what is still open. Numbers are from this machine (2026-10-22)
unless noted (2026-10-23: remote desktop A, Chrome 156 and Firefox 158). "PLI" is the viewer asking for a keyframe: it means Chrome gave up repairing the stream.
OME ignores PLI, so each one means waiting up to a source GOP (s720: 8 s, 4K: 1 s).

Commit hashes name commits of the development branch, which was squashed before publishing.

## Fixed in OME

| # | Problem | Effect for viewers | Fix | Evidence |
|---|---|---|---|---|
| 1 | RTX served minutes-old packets after the stream's sequence number wrapped (stale `RtpHistory` cache) | 3–7 s freezes at 2% loss, 50 ms reorder, 20 ms RTT | 04291b34f | RTX timestamps minutes behind the media in traces; unit test `rtp_history_test.cpp` |
| 2 | Chrome forgets ULPFEC padding 100 sequence numbers back; a slower repair left no bridge to the next frame | keyframe waits on any line with RTT ≥ 50 ms | fa57a376e: `ReannounceFecPadding` | A/B (`OME_TEST_NO_PADDING_REPLAY=1`): rtt 200/400/800 ms with 1% loss, replay off → 4/10/1 PLIs, freezes up to 7.5 s; on → 0 PLIs, ≤ 0.8 s |
| 3 | ULPFEC recovered nothing: header extensions unprotected, SN base in stream numbering while sessions renumber | FEC cost bandwidth and gave nothing | 8d8afd14c | single-loss frames shown on time at rtt 200/400/800 ms: 0/0/0% → 96/88/92% |
| 4 | Bursts larger than any bottleneck queue: a NACK after an outage answered all at once, a backlog after an ingest stall sent at loopback speed | 6–7 s keyframe waits after 1–2 s outages or uplink stalls | 1362edab0: `PaceSend` token bucket (up to one keyframe at once, then 2.5× media rate), RTX before queued media | behind 10 Mbps / 100 ms (`--bottleneck 10:100`, `OME_TEST_NO_PACING=1`): outage 1 s 6.8 s / 3 PLIs → 1.0 s / 0; uplink stall 2 s 6.1 s → 1.95 s; bottleneck drops 20–627 → 0 |
| 5 | ULPFEC parity took transport-wide (TWCC) numbers without carrying the extension; each became a "lost" packet in the feedback | auto ABR saw 5–22% loss on a clean path, fell to the lowest rung within 2 s and never came back | c9781a303: only packets carrying the extension are numbered | clean network: with FEC, 480p for the whole run; without FEC or with the fix, 1080p throughout; under a bottleneck ABR now steps 1080 → 720 (1.2×, 1.0×) → 480 (0.8×, 0.5×) |
| 6 | The first viewer to switch to a rung nobody watched lost the frame after the switch keyframe: a pending rung is packetized keyframe-only, and the next frame was cut before the keyframe had left the pacer | every later frame decoded against the wrong reference until the next keyframe (smearing for up to a GOP), at every switch to an unwatched rung | c9781a303: `RtcStream::PassesDemandFilter` lets a pending rung through from its keyframe until the switch (≤ 1 s) | OME's own output (relay trace): 720p keyframe 2430000, next 2436030 (2433000 never sent); the same at 480p. Fixed: the frame is sent |
| 7 | Retransmissions, sent first and allowed 2.5× the media rate (4), filled a congested path by themselves: lost, NACKed and resent again, while media starved, even after ABR had switched down | ABR at 0.5×: 19.6 s freeze, 5 PLIs, 22 s with 0 Mbps of media | c9781a303: RTX at most 1× the media rate; nothing older than the newest keyframe once that keyframe is 200 ms out | 0.5×: 19.6 s / 5 PLIs / 2,544 drops → 1.57 s / 0 / 341; 0.8×: 1.32 → 0.57 s. Outage behind the bottleneck still 1.1 / 2.07 s (1 / 2 s), loss 2–10% unchanged |

| 8 | ULPFEC parity was emitted only when the next frame arrived, a frame period late, with its own (older) RTP timestamp beside the next frame's media | Chrome measured 33 ms of RTP jitter on a clean path. On a real desktop (Windows, Chrome 155, GPU decode) it showed ~24 of 30 fps: 82% of frames in the clean phase, 84% after recovery. Headless Chromium hides this | the packetizer waits up to 3 ms for the frame's parity and sends it right behind the frame (else at the next frame, as before); test `RtpPacketizerUlpfec.ParityFollowsItsFrame` | remote viewer, 1% loss: jitter 33 → 2–3 ms; frames shown with ULPFEC 92–99% (3 runs), the same as with ULPFEC off (95–99%) |

| 9 | TWCC feedback parsed wrongly: every receive delta taken from the reference time instead of the previous packet | the bandwidth estimator saw packet trains squeezed ~8× (capacity 16–30 Mbps on a 3.7 Mbps link) and a bent delay trend; ABR climbed onto rungs that didn't fit | 2d646a700 | test `TransportCcParse.ArrivalTimesAccumulateDeltas`; capacity median 3.67 Mbps behind 3.69 after the fix |
| 10 | Auto ABR never stepped up after congestion, and over- or undershot on the way down. Causes: 10 s of uninterrupted "optimal" required; audio mixed into per-frame statistics; two rungs merged at each switch; REMB (0.1–0.4 Mbps, sent by Chrome next to TWCC) forcing down-switches; RTX carrying the original's TWCC number; capacity lower bounds treated as measurements; OME's own pacer throttling a new rung | viewers stayed on a lower rung for 10–120 s after any congestion; some sessions oscillated | 046f79cbd (estimator rework, see the commit) | `abr10` (desktop viewer, playout delay 50 ms): every phase on the best rung; bottlenecks settled in 1.6–4.5 s, recovery to 1080p in 2.5–6.1 s (goal 20 s); clean 0.1–0.8% renderer drops |

| 11 | A viewer pinned to one rendition (`?rendition=`), or on auto ABR's lowest rung, had no congestion control: every frame lost packets at the bottleneck, repairs competed with new media, each broken frame stalled its GOP | a link under the rung's bitrate froze the viewer for the whole congestion (25–28.7 s of 30) | bf558d5a2, a47a364cf: frame shedding. With the queue to the viewer over 200 ms while the link carries less than the session sends (the delay grows, slope > 0.1 over 4 s, or > 5% loss), video stops at the next delta frame and resumes at the next keyframe; shed frames are never numbered, so no NACKs | pinned 1080p, 1.0/0.8/0.5×: longest freeze 25–28.7 s → 1.0–1.44 s, NACKs 2,300–3,200 → 240–415. The growth/loss condition: path jitter (FIFO, 60 ms σ) built a 150–250 ms queue that shedding cannot drain, and froze the viewer a GOP at a time (15 → 1 freezes) |
| 12 | NVENC puts AUD + pic_timing SEI in front of every frame; beside a large slice they took an RTP packet of their own (~9% of packets) | one more packet per frame to lose and repair | cd480268b: the H.264 packetizer drops AUD and timing-only SEI (captions and other SEI kept; WebRTC only); `RtpPacketizerH264.*` | ~7% fewer packets, NACKs and bottleneck drops; stutters at 10% loss 79/82 → 59/66 |
| 13 | On a long lossy path a repair takes about one RTT, longer than Chrome buffers by itself (~100–140 ms, whatever the playout-delay max) | 300 ms RTT, 1% loss: 13–22 freezes in 30 s, 14–18% of frames dropped | 20811abe9, f113bf41e: with `<PlayoutDelay>`, a session with sustained loss (NACKs in 5 of 10 s) on a path whose base RTT (transport-wide feedback, lowest of 30 s) exceeds 100 ms raises its viewer's minimum to RTT + 80 ms; down again after 60 s without NACKs at 20 ms/s (Chrome syncs audio to it at ~10 ms/s) | 300 ms: freezes ~16 → ~6, dropped 14–18% → 1–5%, at ~390 ms delay for that viewer only; short paths, outages and bottlenecks keep the configured delay (a first version that used the NACKs' delay left viewers 400–1,100 ms behind after an outage or congestion) |

The test toggles `OME_TEST_NO_PADDING_REPLAY`, `OME_TEST_NO_PACING`, `OME_TEST_SHED_QUEUE_MS`, `OME_TEST_KEEP_AUD_SEI` and `OME_TEST_NO_ADAPTIVE_PLAYOUT` exist only to A/B these fixes.

## Fixed in the test harness (results before these were partly the harness's)

- **Relay socket buffers.** With `net.core.rmem_max` at the 208 KB default, the kernel dropped part of every 4K keyframe, NACK answer and released backlog before the Python relays saw it. The 4K smoke test lost 19% of its packets on the clean profile; outages got fake 7 s keyframe waits. Both relays now ask for 8 MB per socket and log when capped (ddc913a5b); `limits.py` counts the relay's socket drops and does not judge the gaps they explain. **Needs `sudo sysctl -w net.core.rmem_max=16777216` after every reboot** unless it is put in `/etc/sysctl.d/`.
- **Relay rate queue delay compounded** (ddc913a5b): the order-keeping clamp stacked each packet's queueing onto the previous one's. Every `rate` FAIL before that (30 s delays) was the relay.
- **Decode check at ABR switches** (631a8bd7c): it de-duplicated frames by RTP timestamp, but the old and new rung's keyframes share one, so the new keyframe was dropped and a whole GOP "failed". It also sorted the old rung's last frame after the new keyframe. Now: duplicates by (timestamp, size), sorted within keyframe-to-keyframe runs of arrival order. Clean ABR steps went from 29–56 "corrupt" frames to 0.

- **Firefox could not connect through the sim relay** (56d8fb8be): the relay reached OME over loopback, so OME's STUN responses gave Firefox 127.0.0.1 as its mapped address, and Firefox discards those (Chrome does not). The relay now sends from the LAN address. Production has no relay.
- **Firefox's drop count:** `getVideoPlaybackQuality` is 0/0 for WebRTC in Firefox. Drops are frames decoded (getStats) minus frames presented (rVFC `presentedFrames`); on Chrome this gives exactly Chrome's own count.
- **Several viewers on one desktop:** a 2560×1440 60 Hz desktop (RTX 3070) shows at most two 1080p60 windows cleanly. `--extra-viewers local` runs the extra viewers here, assembling without decoding, judged on delivery. They are a weak judge at 4K under loss (three local transform pipelines: 3–17% "missing" while the displayed viewer on the same OME path had none).
- **delivery_check.py:** a keyframe without its SPS/PPS packet (decodable with the earlier identical ones) no longer counts as corrupt.

## The 1-point smoke-test drop: not a viewer problem

The smoke test's loss profiles fell from 98.9% / 99.1% (2026-10-21 table) to about 97.3% frames complete.
- The baseline table is the better of two runs on the same code; the other gave 97.5% / 94.7%.
- With working ULPFEC (3), most lost packets are repaired twice: by FEC and by the retransmission Chrome already asked for. If the second copy arrives after its frame has left Chrome's packet buffer, it stays in that slot until a decode moves the buffer on (`PacketBuffer::ClearTo` from `FrameDecoded`). The smoke probe never decodes, so 2,048 packets later the slot collides and Chrome clears the buffer and waits for a keyframe ("Clear PacketBuffer and request key frame": 59 times in 60 s at 4K).
- With `<Ulpfec>false</Ulpfec>`: 100.0% in both runs. A decoding viewer (1080p rung, 774 pps, 1% loss, FEC on): 0 buffer clears, 0 PLIs, 0 freezes.
- The padding replay (2) multiplies the retransmission count at 4K (rtx ≈ 7× lost), but turning it off did not change the result.

## ULPFEC at 4K, judged on a real desktop

4K60 bypass (~19 Mbps, ~2,500 packets/s), remote Windows viewer with GPU decode, 30 ms RTT, after fix 8. One run per arm. The first step of each run had a poor clean baseline (46–48 fps, 300 ms delay: stream warm-up), so 1% is indicative only.

| Loss | ULPFEC | skipped frames | stutters | freezes (longest) | PLIs | corrupt frames |
|---|---|---|---|---|---|---|
| 1% | on | 108 | 6 | 0 | 0 | 0 |
| 1% | off | 156 | 5 | 0 | 0 | 0 |
| 2% | on | 156 | 11 | 1 (0.53 s) | 0 | 0 |
| 2% | off | 230 | 26 | 2 (0.23 s) | 0 | 0 |

ULPFEC removes about a third of the skipped frames and, at 2%, more than half of the stutters. Its cost is the parity bandwidth.

## A clean path is not 100% on a real desktop: playout delay

Counted exactly (frames OME sent vs frames Chrome showed), a clean path showed 92–99% of frames on the Windows desktop and 98.6% on the local viewer. Every frame arrived on time (OME send timing ±4 ms of the media clock, network ±1 ms) and was decoded. The frames were lost at the display.
- Without a playout-delay extension (OME's `<PlayoutDelay>` is unset in the sim and in production), Chrome renders WebRTC with a ~16 ms jitter-buffer target. A few ms of timing variation then lands two frames on one refresh, and one is dropped. On the desktop this came in episodes of one frame per ~250 ms lasting seconds, also with ULPFEC off. A plain 30 fps file plays 299/299 frames on the same machine.
- `<PlayoutDelay><Min>50</Min><Max>200</Max></PlayoutDelay>` (Applications → Publishers → WebRTC): clean 99.8% / 99.1%, no episodes, about 30 ms more delay (clean e2e 53–60 vs 19–32 ms). Controls in the same conditions: 98.3% / 94.1%. A 25 ms minimum was not enough. Two runs per arm, alongside a live production stream.
- What remains: 1–4 frames in the first ~1.3 s after joining.

## Corruption under congestion: none

The probe's offline decode reported 11–76 corrupt frames per step under heavy congestion. Compared byte for byte with three clean reference viewers pinned to each rung (`limits.py --reference-probe 1080p,720p,480p`, `delivery_check.py`), every frame was delivered exactly as OME sent it (4,984/4,984 in `cor5`; abr10 likewise). A few lacked AUD/SEI, optional NAL units in packets of their own. The flags were the decode check's:
- it misattributed errors (2,574 frames in, 2,478 out);
- it decoded an old rung's late keyframe together with the new rung;
- under heavy congestion the probe (encoded transform, no decoder) hands over frames seconds out of order.

limits.py now takes the corruption verdict from the byte comparison.

## Final pass on a real desktop, both browsers (desktop A, 2026-10-23 evening)

Build 2cd07ad16, gpu4k 1080p60 pinned for the matrix and the `abr` playlist for auto ABR, `<PlayoutDelay>` 50:200,
one browser at a time on desktop A (`fin_mx_ff`, `fin_mx_cr`, `fin_abr_ff`, `fin_abr_cr`).

| | Firefox 158 | Chrome 156 |
|---|---|---|
| Clean phases, frames dropped | 0.0% | 0.0–0.2% (the earlier ~6% desktop floor is gone) |
| Clean e2e | 36–46 ms | 44–58 ms |
| Loss 0.5 / 2% | 1 / 0 freezes (≤ 0.21 s) | 0 / 0 |
| Loss 10 / 35% | 11 / 43 freezes, longest 0.47 / 1.24 s | 12 / 35, longest 1.01 / 1.25 s (7 PLIs at 35%) |
| Bursts 4–128 packets | 0 freezes | 0 freezes |
| RTT 50 / 200 / 800 ms (1% loss) | 0 / 15 / 29, longest 1.0 s | 0 / 11 / 31, longest 2.03 s (FAIL, 13 PLIs) |
| Jitter 15 / 60 / 240 ms | 0 / 0 / 6 | 0 / 0 / 7 |
| Reorder 25–200 ms | 0 freezes | 0 freezes |
| Hold, outage 0.25–4 s | one freeze of about their length | the same |
| Bottleneck 2.0 / 1.2 / 0.8× | 0 / 0 / 30 freezes (longest 1.93 s) | 0 / 0 / 32 (1.15 s) |
| Auto ABR, every phase | best rung, settled 1.8–6.1 s, 0 PLIs | best rung, settled 1.8–6.1 s, 0 PLIs |
| ABR delivery (byte comparison) | every frame as OME sent it | the same |

- The two local FAILs to re-check (reorder 100 ms, outage 250 ms) pass on both browsers.
- Matrix verdicts: Firefox one FAIL (loss 2%: a 0.18 s freeze in recovery), Chrome one (RTT 800 ms, below).
- ABR "corrupt frames" (3 per step at 1.2× and 0.8×) were the delivery check's: the 1080p reference stopped
  capturing 0.4 s before the probe, and the probe's last 1080p frames were compared with the 720p reference's
  frames of similar size. `delivery_check.py` now judges only where every reference was capturing: 0 differing.
- **RTT 800 ms on Chrome: the adaptive playout delay comes too late for a mid-session RTT jump.** The step
  jumps from 30 to 800 ms mid-session; the base RTT is the minimum over 30 s, so the raise came at the end of
  the 30 s impairment (Chrome's jitter-buffer target 38–187 ms throughout, then ~500 ms in recovery, e2e 529 ms
  for the rest of the step). Chrome lost repairs past its buffer and asked for keyframes (13 PLIs, a 2.03 s
  freeze). A real long path is long from the start, which the feature covers; a path that becomes long mid-session is not covered (next item).

## Upstream vs this branch, and the best case (2026-10-24)

Report with charts: https://claude.ai/artifact/LJsVaKeh8qYrbrbQMhZmKz. Upstream = AirenSoft master eeb875103 (this
branch's base, built in ../OME-upstream with only the build-side dependency check changed). Both builds with
production's WebRTC config (Rtx, Ulpfec, JitterBuffer, PlayoutDelay 50:200); viewers are Chrome and Firefox on desktop A.
Tooling: `misc/webrtc_impair/compare_builds.sh` (smoke | limits), `report_data.py`, `best_case.py`, `send_timing.py`.

**Residential profiles, 4K60 Bypass** (smoke harness, 3 runs per build, both browsers watching at once; freezes per
~50 s window, median):

| profile | patched Chrome / Firefox | upstream Chrome / Firefox | best case (model) |
|---|---|---|---|
| clean | 0 / 0 | 0 / 0 | 0 |
| loss_1pct | 1 / 0 | 22 / 25 | 0 |
| burst_loss | 1 / 4 | 47 / 37 | ~0 |
| jitter_reorder | 0 / 0 | 2 / 3 | 0 |
| stalls | 1 / 1 | 2 / 2 | 0 (with a 250 ms buffer) |
| real_world_mix | 0 / 1 | 35 / 40 | ~0 (200 ms buffer) |

**Stress matrix, 1080p60 passed through** (`limits.py --stream s1080 --quick`, 22 steps, one browser at a time):
freeze time summed over the impaired windows, patched 92 s (Chrome) / 88 s (Firefox), upstream 224 s / 213 s,
best case ~13 s. Patched is ahead on every lossy step from 2% up, all bursts, RTT 200 ms (0 vs ~30 s frozen),
reordering 100–200 ms (0 vs 3–12 freezes) and short outages. Behind at jitter 240 ms (8–10 s vs ~1 s frozen).
Both fail at RTT 800 ms, 35% loss and a 0.8x link (upstream stays frozen ~30 s, patched freezes repeatedly).
On the GPU rung (gpu4k) upstream is not comparable: 0.21 as released has no NVENC/NVDEC (this fork restores them,
901f822d5, 69e6df777), so it transcodes 4K60 in software, its queues overflow and the rung runs at 30–42 fps with
703 stutters on the clean phases; only the Chrome run was made.

**Delay.** With a jittery source (ffmpeg -re, like the production encoder) the patched build adds 33–36 ms
send-to-display over upstream on the clean line (Chrome 113 vs 80 ms, Firefox 82 vs 46 ms) and 3–102 ms on the
impaired profiles (median of 3 runs each; best-case floor 60 ms clean). Frames leave OME with p99
122–142 ms of jitter against their timestamps (upstream 33–64 ms): c4e796aa0 releases at arrival + hold rather than
at the timestamp target, so arrival jitter reaches the viewer and its jitter buffer grows to cover it. Production's
pacer log shows the live encoder's p99 arrival lateness at a median 114 ms (211 adjustments, 2026-10-20..24), more
than the test source. With limits.py's smooth publisher the patched build is the faster one (clean 38 vs 50 ms).
The same pass-through explains the 240 ms jitter step.

**Fixed: capped release (bb2f1b7c1, not deployed yet).** The pacer now releases at the timestamp target, never later
than arrival + hold: a late frame waits only for the rest of its delay. Smoke test, 4 runs (`logs/cmp_capped_r{1..4}`,
plus a 4th patched run as a drift check): send-time p99 jitter 125 → 72 ms, send-to-display 30–55 ms lower in every
profile (clean Chrome 123 → 68 ms, Firefox 88 → 56 ms; loss_1pct Chrome 115 → 79 ms), freezes no worse (burst_loss
Chrome 1 → 0.5, Firefox 3 → 0), and every run inside the baseline bars. Clean delay is now at or under upstream on
Chrome (68 vs 80 ms), 10 ms over it on Firefox (56 vs 46). s1080 matrix (`logs/cmp_lims1080_capped_*`): freeze time
110 / 90 s (Chrome / Firefox) vs 92 / 88 s; the Chrome difference is one step (burst=4: 18 s, the receiver stuck
decoding 2 frames per GOP with a PLI every 200 ms after all losses were repaired) that did not repeat in 2 reruns
(`logs/rep_burst_*`: 0 freezes, patched 1 freeze at burst=128). Jitter 240 ms is unchanged (7.6 / 9.0 s): its tail
(~720 ms) is past the hold, which the cap does not exceed. What is left of the send jitter (p99 ~72 vs upstream
~35 ms) is frames arriving early against the calibrated anchor (median), which the cap releases at arrival + hold.

**The best case** (`best_case.py`): protocol and settings fixed (NACK/RTX, ULPFEC, playout delay from 50 ms, Bypass,
1 s GOP so no keyframe on demand); ideal NACK/resend timing, a per-viewer buffer sized to the condition (≤ 1 s), a
packet lost for good costs half a GOP. Its freezes are what nothing avoids (outages, a link below the stream's rate,
800 ms RTT with 1% loss); its delay is the price (e.g. stalls: 250 ms buffer). First-order; ULPFEC not credited.

**The old smoke-test bar** could not tell the builds apart: at loss_1pct the probe scored 97.7% (patched) and 97.8%
(upstream) while upstream's viewers froze 22–25 times. Since ea1c9fe40 the smoke test judges by decoding viewers
against a measured per-profile baseline.

## Release soak with real desktops (2026-10-27)

Traffic sim, 2.5 h: churning publishers (4K NVENC, 1080p with B-frames, 720p with an 8 s GOP), mean 12 lightweight
viewers, plus Chrome and Firefox on desktop A and desktop B watching back to back with drawn network classes, outages and
leaves (about 16 viewers at a time). Production's config. Results in `misc/traffic_sim/logs/`.

| run | build | leaks (RSS warm -> final quiesce) | other checks |
|---|---|---|---|
| `release_soak` | switches, all on | FAIL 275 -> 418 MB | pass |
| `release_soak_prodbin` | production's pacercap2410 | pass 277 -> 311 MB | 1 transcoder "Could not found decoder" line |
| `leak_repro_switches` (1 h) | switches, all on | pass 274 -> 299 MB, heap = prodbin's | pass |
| `release_soak2` (2026-10-28) | switches + RtcKfDrain fix, all on | pass 277 -> 261 MB, heap 133 -> 170 | pass |

- Viewer quality is the same on both builds. Firefox on fiber and residential: well under 1 freeze per minute,
  send-to-display 60..110 ms p50. Chrome on residential: 0.7..6.5 freezes per minute on both builds (not the
  switches). Wi-Fi, 4G and terrible: stalls and reloads as their impairments predict.
- No leak. The +143 MB did not reproduce in 1 h nor in a second 2.5 h run (`release_soak2`, heap logged every 5 s:
  the same 2.4..2.6 GB peak under load from the first quarter hour to the last, 170 MB heap when idle at the end,
  production's binary 168). It was memory the allocator kept in that one run.

## Still open

- **Shedding re-shed on stale feedback (fixed 2026-10-25):** a session with no lower rung that was shed once (path delay jump: 240 ms FIFO jitter, or a 1.2x link) was shed again right after every keyframe, because the estimator still held the pre-shed state: one picture per GOP for the whole impairment, worse than upstream (which does not shed). Now it re-sheds only on feedback for frames sent after resuming: jitter 240 Chrome 27.4 → 15.4 s, Firefox 27.7 → 7.4 s frozen; 0.8x unchanged. The first shed on a delay step is fixed too (5040fea10): shed only while the packet-train capacity is under the stream's rate. Jitter 240 now Chrome 1.5 s, Firefox 2.2 s frozen (from ~22 s with the first fix alone), no shed; 0.8x still sheds. Left: Chrome's own keyframe wait after the delay drops. Traced to Chrome: its timestamp extrapolator rejects the now-early frames for 5 s, then snaps and releases ~1 s of frames at once, which overflows the hardware decoder's 8-buffer queue; it then needs a keyframe and OME ignores PLI. Software decode does not freeze. On production's 1 s GOP the cost is about 1 s; the only OME-side fix would be keyframes on PLI from the transcoder (a feature, parked).

- **Freezes at ABR down-switches** (1–4 per step, up to ~2.9 s): the viewer stays on the congested rung until the next one's keyframe. A keyframe request to the rung's encoder would shorten that, but no such plumbing exists, and it is only safe in KeyFrameIntervalType TIME (in FRAME, the default, it would likely shift the rung's GOP and break LL-HLS alignment).
- **ULPFEC parity carries no header extensions at all**, so the bandwidth estimator never sees its bytes. Giving it the TWCC extension would let feedback account for it.
- **Decode check on B-frame streams** flags frames that completed late (reorder, heavy loss): it decodes in arrival order since d636baefa; the exact decode order needs the slice headers.
- **4K bypass reordering:** in one trace, 2 small frames were sent after a frame with a newer timestamp. Unexplained.
- **OME ignores PLI/FIR.**
- **Pacer send jitter, early side:** since bb2f1b7c1 late frames leave on their timestamp cadence, but frames early against the anchor (median-calibrated) still go at arrival + hold: send-time p99 ~72 ms vs upstream ~35 ms. Jitter beyond the hold (240 ms step) is passed through as before. Calibrating on the early edge (p5) instead halves the send spread but changes nothing viewers see: the 4K Firefox freezes did not reproduce interleaved (0/8 vs capped 1/8), the s1080 Chrome lockups are B-frames (below), and on s720 (no B-frames) freeze time and e2e match capped step for step. Not adopted; patch kept in `OME-builds/early-anchor.patch`.
- **Chrome keyframe-only lockup = B-frames (found 2026-10-25):** Chrome's software H.264 decoder (WebRTC `H264DecoderImpl`) fails every frame FFmpeg holds back for B-frame reordering (EAGAIN), asks for a keyframe and drops the rest of the GOP: one picture per GOP for as long as the page is open. Chrome uses it from the start without H.264 hardware decoding, and permanently after any hardware decode error. Headless Chromium: 0.5 fps on s1080 (B-frames) on every build, normal on s720 and the NVENC 1080p rung. The production 4K source has none (Constrained Baseline); the production notes record the requirement. OME now warns once per track (680b3abcc). Not a pacer regression: the early anchor's 3 vs capped's 1 lockups are how often desktop A's hardware decoder hit an error.
- **Headless Chromium hides display-side effects.** The parity timing above showed only on a real desktop browser; judge frame cadence there (`--remote-viewer`).
- **4K viewer results on this machine are invalid:** headless Chromium decodes 4K60 in software at 17–37 fps. `--remote-viewer` / `--remote-firefox` (below) run the judged viewer on a real desktop browser instead.
- **An RTT jump mid-session is protected only after 30 s** (the base-RTT window), and the viewer then keeps the raised delay into recovery. A faster raise has to tell a longer path from a standing congestion queue (the reason for the 30 s minimum).
- **Join waits for the next keyframe:** a session starts on the live packets, so the first frame comes 0–1 GOP after connecting (gpu4k: 5–1,055 ms; s720's 8 s GOP: up to 8 s). Starting from the GOP backlog would show a picture at once but leave the viewer up to a GOP behind for good (WebRTC receivers do not catch up). The lever is the source's keyframe interval.
- **A shed waits up to a GOP** for the next keyframe (1 s at gpu4k, 8 s at s720), and so do ABR down-switches: the keyframe-request item above.

## Tooling added in this phase

- `limits.py --bottleneck MBPS:QUEUE_MS` puts a drop-tail bottleneck under every step.
- `limits.py --playlist abr --rendition auto` tests auto ABR on the transcoded rungs. The `abr` playlist is test config only, because the `auto` playlist starts at Bypass 4K.
- `limits.py --remote-firefox ws://host:port` judges a Firefox on another machine over WebDriver BiDi (puppeteer-core; Firefox started with `--remote-debugging-port`; the oracle sends `Host: 127.0.0.1:<port>`, Firefox's DNS-rebinding guard). `--viewers N --extra-viewers local` adds viewers here. `viewers_report.py` reports each viewer per phase.
- `limits.py --remote-viewer http://host:port` judges a Chrome on another machine (`--remote-debugging-port`, with a dedicated `--user-data-dir`). The oracle serves its page from this machine's LAN address (Chrome's Local Network Access blocks other origins from reaching it), maps the remote clock onto ours, and only closes its own window. It never closes the browser, because closing the last tab ends the remote session.
- With `--keep-trace` the probe is traced too (`probe_trace.csv`).
