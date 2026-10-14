# WebRTC 3-rung + ULPFEC — final report (2026-10-14)

## Status after router reset

LAN is healthy (iperf3 UDP ~895 Mbps, 0% loss server↔client). Concurrent
100 Mbps iperf while streams run also shows no loss — stutter is **not** path
capacity.

| Test (Chrome on client, ULPFEC on) | Result |
|----------------------------------|--------|
| Bypass alone | **PASS** (60-ish fps, lost=0, freezeCount=0) |
| 1080 + 720 | **PASS** |
| All three together | **Near miss** — 1080/720 OK; bypass ~56 fps, freezeCount=3, freezeEvents=11, stallMs≈2.7s (harness thresholds) |
| Localhost OvenRtcTester ×3 | **PASS** 60 fps / 0 loss (server can produce all three) |

So: goal is **almost** met on a clean GbE path. Remaining gap is server-side
send cost for 4K+ULPFEC on a **4-core** host when three Chrome sessions run,
not the LAN.

---

## Root causes we hit (in order)

1. **Bad router** (your finding) — overnight “\(R>C\)” proof was measuring a
   broken hop (~34 Mbps clean goodput). Retracted as a LAN-capacity proof.
2. **UDP `SO_SNDBUF` ~208 KiB** — 4K keyframes → `MSG_DONTWAIT` `EAGAIN` →
   drops or send-thread stalls → freezes with or without `packetsLost`.
3. **ICE UDP send serialized on epoll dispatch mutex** — fixed by inline
   `sendmsg` for UDP (already in tree).
4. **App pacing (`OME_WEBRTC_SEND_MBPS=100`) on the send thread** — sleeps
   while packetize keeps enqueueing → queue growth → freezes with
   `packetsLost=0` (seq# assigned at send time). **Do not enable by default.**
5. **Per-packet deep copy + SRTP + ULPFEC** on 4 cores — send queue still
   backs up under 3-tab 4K (`RtcSend queue=300–700`). Bypass alone is fine.

---

## What to **keep** (recommended for prod / upstream)

| Change | Why |
|--------|-----|
| Raise `net.core.wmem_*` / `rmem_*` (deploy sets 16–32 MiB) | Required for 4K WebRTC UDP bursts |
| ICE UDP `SO_SNDBUF`/`SO_RCVBUF` ~16 MiB via `OnSocketCreated` | Same; applies to all REUSEPORT workers |
| `sendmsg` EAGAIN retry with short `poll(POLLOUT)` budget (~50 ms) | Avoids silent datagram drops on full SNDBUF |
| Inline UDP send (skip epoll dispatch queue for ICE RTP) | Removes cross-session send mutex |
| Per-session send thread + queue | Stops SRTP/ICE from blocking pace/packetize threads |
| Demand packetize / per-rung FramePacer | Avoids unused-rung work; isolates rungs |
| Ulpfec kept on (no diagnostic disable in prod) | Per product requirement |

## What **not** to keep / not to default

| Item | Why |
|------|-----|
| `OME_WEBRTC_SEND_MBPS` default > 0 | Under-delivers 4K; grows send queue |
| `OME_WEBRTC_PEER_MBPS` shared token bucket | Starves 4K peaks; cannot create capacity |
| Overnight “impossible on GbE” conclusion | Was a bad router; superseded |
| In-place SRTP without copy | Unsafe with ULPFEC/RTX sharing; reverted |
| TcpRelay for this LAN case | Added latency; worse freezes |
| Aggressive send-queue drops | Invisible as `packetsLost` (seq at send time) |

---

## Upstream PR — config flags?

Most fixes should be **behavior defaults**, not new knobs:

| Topic | Upstream recommendation |
|-------|-------------------------|
| ICE UDP socket buffers | New optional `<IceCandidates><UdpSendBufferSize>` / `<UdpRecvBufferSize>` (bytes), default **8–16 MiB** (or document sysctl). Today we hardcode 16 MiB + sysctl in deploy. |
| UDP `EAGAIN` retry | Internal; no flag. Keep budget small (tens of ms). |
| Inline UDP send | Internal default on; no flag. |
| Per-session send thread | Internal default on for publisher WebRTC; no flag (or only a debug kill-switch). |
| `OME_WEBRTC_SEND_MBPS` / `PEER_MBPS` | **Dev/soak only** via env, or omit from upstream. If exposed: `0` = off (default). |
| `OME_WEBRTC_DISABLE_ULPFEC` | Keep as **diagnostic-only** env; never a Server.xml default. |
| Sysctl `wmem_max` | Ops/docs (“4K WebRTC needs multi-MB UDP SNDBUF”), not OME XML — process may lack CAP_SYS_ADMIN. |

Minimal upstream-facing XML addition worth doing:

```xml
<IceCandidates>
  <!-- optional; 0 = OS default -->
  <UdpSendBufferSize>16777216</UdpSendBufferSize>
  <UdpRecvBufferSize>16777216</UdpRecvBufferSize>
</IceCandidates>
```

Everything else can ship as better defaults without new user-facing flags.

---

## Remaining work to fully PASS all-three

On this **4-core E3-1226** box, 4K@~35 Mbps + ULPFEC + RTX history + per-packet
clone + SRTP for three sessions is still tight (bypass ~56 fps vs 60).

Likely next wins (not done):

1. Avoid redundant copies on the hot path (carefully, without breaking ULPFEC/RTX).
2. Cheaper / adaptive ULPFEC for high-bitrate bypass (or FEC only on keyframes).
3. More CPU for OME (or pin send threads), or slightly lower Bypass bitrate.
4. Harness: ignore first 2–3 s startup when scoring freezes (many `freezeEvents` are connect/keyframe ramp).

---

## Deployed now

- Binary: `OvenMediaEngine-0.21.0-segcache-webrtcfix`
- Ulpfec: **on**
- `OME_WEBRTC_SEND_MBPS` / `PEER_MBPS`: **unset** (off)
- ICE UDP SNDBUF: **16 MiB** (`tb16777216`)
- Sysctl: `wmem_max=32 MiB`, `wmem_default=16 MiB`

Artifacts: `/tmp/webrtc_final_clean_all3.log`, `/tmp/webrtc_final_clean_bypass.log`.
