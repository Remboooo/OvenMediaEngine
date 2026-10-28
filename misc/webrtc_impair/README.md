# WebRTC impairment harness and end-to-end smoke test

Checks what a real Chrome receiver assembles from OME's WebRTC output, under clean and lossy network conditions, without touching the production OME's config or viewers.

It exists because the October 2026 stutter hunt found that `packetsLost=0` in Chrome tells you nothing about OME's correctness. OME renumbers every packet per session, so a dropped, misplaced or wrongly retransmitted packet upstream of the send thread never shows up as loss. It shows up as frames Chrome can't complete or decode. The only reliable check is to capture the frames a receiver actually assembles and decode them.

## When to run what

| When | Command | Takes |
|---|---|---|
| After any change to `src/publishers/webrtc`, `src/modules/rtp_rtcp`, the pacer or the transcoder, **before deploying** | `SMOKE_VIEWER_CDP=http://<desktop-a-ip>:19222 SMOKE_VIEWER_BIDI=ws://<desktop-a-ip>:19224 ./smoke_test.sh harness` | ~14 min |
| **After deploying** (production idle) | `./smoke_test.sh production` | ~2 min |
| Reproducing a problem interactively, incl. with a real browser | `./run.sh start <profile>` | — |

Each check prints `PASS`/`FAIL`. The exit code is 0 only if every check passes, and artefacts go to `logs/smoke_<timestamp>/`.

### What `smoke_test.sh` does

1. **Pushes a real source** as stream `live/e2etest`: the newest `misc/segcache_soak/dogfood/media/*_src_*.ts`, played with `-re` and `-c copy`. Use a recording of the actual production encoder (4K60 Constrained Baseline). Synthetic sources miss the bitstream and timing details that matter. It refuses to run if `live/e2etest` already exists.
2. **Waits until the stream has started.** The LL-HLS master must list variants. With GPU rungs this takes ~20 s, and WebRTC signalling answers 404 until then.
3. **Starts one headless Chromium receiver per rendition, in parallel**, using `misc/segcache_soak/hlsjs_soak/webrtc_bitstream_probe.mjs`. The probe uses encoded insertable streams to dump every frame after Chrome's depacketizer and before the decoder. It doesn't decode, so it costs almost no CPU.
   With `SMOKE_VIEWER_CDP` / `SMOKE_VIEWER_BIDI` set (a Chrome / Firefox on a real desktop, e.g. Desktop A: `http://<desktop-a-ip>:19222`, `ws://<desktop-a-ip>:19224`), a **decoding viewer** in each joins at the same time (`webrtc_oracle.mjs`, no encoded transform: it plays the stream like any viewer). `misc/remote_viewer/Setup-RemoteViewer.ps1` sets up such a desktop.
4. **Judges each capture:**
   - **frames:** frames completed / frames expected from the RTP timestamp span, after skipping the connect ramp (a timestamp jump over 5 s counts as a discontinuity). The bar is this profile's lowest run in `profiles/baseline.json` minus one point; without a baseline, 99% clean and 97% impaired. Also printed: `buffer_clears`, how often the probe's Chrome cleared its packet buffer (below). **This number is a regression guard for delivery, not a viewer metric**: at 1% loss upstream OME and this branch both score ~97.8%, while upstream's real viewers froze 22–25 times a minute and ours 0–1.
   - **decode:** a strict single-threaded ffmpeg decode of the capture in RTP order (`h264_decode_check.sh`) must report **0** frames with decoder errors. That's the check that catches "the hardware decoder rejects what we send".
5. **Judges each decoding viewer** (`viewer_check.py`, from 3 s after the impairment starts): freezes (W3C: a gap > max(3 frames, frame + 150 ms)), frames the renderer dropped (Chrome's `getVideoPlaybackQuality`; Firefox: decoded − presented), and send-to-display delay per frame (the relay's RTP trace: when OME sent the frame → when the browser showed it; OME's own pacer hold before sending is not included). The bar is the baseline's worst run plus one freeze and plus one point of dropped frames; without a baseline, clean needs 0 freezes and ≤ 2% dropped (desktop A's Chrome drops ~0.4% at 4K60 on a clean path), impaired profiles are reported only.
6. **Judges the server:** it must still be the same PID, with no new crash dump, and the log since stream start must have no `E`/`C` lines, `ULPFEC worker behind`, `RtcSendGap` or `PacketizeVideo took ≥100ms`. It also prints memory before and after, and the number of `RtcFec` threads (one per FEC-protected packetizer, never one per frame).

In **harness** mode it starts the harness OME (`build/Release/bin/OvenMediaEngine`, i.e. the build you're about to deploy). It runs step 4 once per impairment profile: `clean,loss_1pct,burst_loss,jitter_reorder,stalls,real_world_mix` by default (the profiles the baseline covers). Loss is where the recovery paths run (NACK→RTX via the per-session sent log, RED/ULPFEC), and where every bug found so far lived.

In **production** mode it uses the live OME (API token read from `/usr/share/ovenmediaengine/conf/Server.xml`, or `OME_TOKEN`). That covers the clean path for Bypass+opus and 1080p (transcoded). It doesn't change OME's config, but it does add one test stream and two viewers, and the GPU transcodes the test stream, so run it while production is idle.

The relay needs `net.core.rmem_max` ≥ 16 MB (`sudo sysctl -w net.core.rmem_max=16777216`). With the 208 KB default the kernel drops part of each 4K keyframe at the relay's socket, and the clean profile loses about 19% of its packets. The relay logs `udp receive buffer … bytes` when the kernel caps its request.

Environment overrides: `SMOKE_DURATION_S` (60), `SMOKE_RENDITIONS`, `SMOKE_PROFILES`, `SMOKE_SRC`, `SMOKE_MIN_COMPLETE`, `OME_IMPAIR_NICE` (19 while production is live; contention would skew timing).

### Baseline (2026-10-24, the build deployed that day, 3 runs: `profiles/baseline.json`)

Harness, Bypass+opus 4K60, source `sim_2160p60_baseline.ts` from 240 s (~19 Mbps), `<PlayoutDelay>` 50:200 as production. Probe frames complete; freezes per ~50 s judged window for Chrome / Firefox on desktop A (ranges over the 3 runs):

| Profile | Frames complete | Freezes Chrome | Freezes Firefox | Decode errors |
|---|---|---|---|---|
| clean | 100.0% | 0 | 0 | 0 |
| loss_1pct | 97.7–97.9% | 0–1 | 0 | 0 |
| burst_loss | 96.7–97.6% | 1–3 | 2–4 | 0 |
| jitter_reorder | 93.3–96.5% | 0–1 | 0 | 0 |
| stalls | 98.8–100.0% | 0–1 | 1 | 0 |
| real_world_mix | 95.9–97.3% | 0–3 | 0–1 | 0 |

Regenerate it with `smoke_baseline.py logs/<run> …` after a deploy that changes delivery (three runs of the deployed build, idle machine). Upstream OME on the same profiles, and the theoretical best case: `compare_builds.sh` and the report (misc/traffic_sim/FINDINGS-2026-10.md, "Upstream vs this branch").

**The loss profiles' frame count includes a probe artifact** (loss_1pct: 59 buffer clears a minute). A packet that FEC recovers is usually also NACKed, and the second copy can arrive after its frame has left Chrome's packet buffer. A decoding receiver frees such leftovers when it decodes past them (`PacketBuffer::ClearTo`, called from `FrameDecoded`). The probe never decodes, so the leftover stays until a packet 2,048 sequence numbers later needs its slot. Chrome then logs "PacketBuffer is already at max size … Clear PacketBuffer and request key frame" and drops frames until the next keyframe. `smoke_test.sh` counts these lines in the probe's log (`buffer_clears`). The decoding viewers do not have them, which is why they, not the probe, judge what a viewer sees.

### Reading a failure

| Symptom | Likely area | Next step |
|---|---|---|
| `frames` < threshold only under loss, decode clean | Retransmission doesn't repair holes | Rerun with `OME_DEBUG_RTX=1` (harness OME logs every NACK: `RtxDebug … log_seq/osn` or `miss=sent_log|history`) and `IMPAIR_TRACE=<file>` (relay logs cleartext RTP headers incl. drops), then compare NACKed vs dropped sequence numbers |
| `decode` errors | Corrupt data reached the decoder: FEC parity, RTX payload/OSN, packetizer | Look at `decode_errors.txt` in the capture dir; frame index and wall time map back to the OME log |
| `frames` low on the clean path | Delivery or packetize stalls | Look for `RtcSendGap` / `PacketizeVideo took` / `RtcKfDrain` in `ome_during_test.log`, and per-thread CPU |
| `server log` fails | — | The offending lines are printed; the full log is `ome_during_test.log` |
| Chrome's own reasoning | — | `OME_SOAK_CHROME_ARGS="--enable-logging=stderr --vmodule=*packet_buffer*=2,*rtp_video_stream_receiver*=2" DEBUG=pw:browser` on the probe. "PacketBuffer is already at max size" + "Clear PacketBuffer and request key frame" means an unfilled sequence gap. |

## The harness

`run.sh` starts a separate OME (`conf/`) on its own ports: signalling 23333, API 28081, RTMP 21935, ICE 24000–24005. It uses production-like WebRTC settings (Rtx, Ulpfec, JitterBuffer, `auto` playlist with WebRtcAutoAbr), Bypass only, with no GPU use. OME binds `127.0.0.1` only. `impair_relay.py` exposes signalling and ICE on `<lan-ip>`, and OME advertises **only** the relay's ICE ports, so every media packet goes through the impairment.

```bash
./run.sh start real_world_mix          # also loops the recorded source into live/test
./run.sh profile burst_loss            # switch impairment live; no reconnect needed
./run.sh ingest ingest_jitter          # impair the RTMP ingest (encoder uplink) instead
./run.sh status
./run.sh stop
```

A real browser (e.g. to test a Windows Chrome's hardware decoder) points OvenPlayer at `ws://<lan-ip>:23333/live/test/auto` (type `webrtc`).

Profiles live in `profiles/*.json`, each with a `_what` line. Keys are per direction (`down` = OME → viewer, `up` = viewer → OME):

- `loss`, plus `burst` (Gilbert-Elliott `p_enter`/`p_exit`/`loss`);
- `delay_ms`, plus `jitter_ms` (FIFO-preserving like a real queue; `jitter_reorders: true` for netem-style independent delays);
- `reorder` / `reorder_ms`, `duplicate`;
- `rate_mbps` with `queue_ms` drop-tail;
- `stall_every_s` / `stall_ms`;
- `start_after_s`, which lets ICE/DTLS settle before impairment starts.

Randomness is seeded (`--seed`). Measured loss and reordering match the configured values; the relay was self-tested.
