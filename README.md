# OvenMediaEngine (fork)

This is a personal fork of [OvenMediaEngine](https://github.com/OvenMediaLabs/OvenMediaEngine) / [AirenSoft/OvenMediaEngine](https://github.com/AirenSoft/OvenMediaEngine). For official releases, documentation, and support, use upstream.

> **Warning:** This fork is **not** nearly as well tested as mainline OvenMediaEngine. It is maintained like a hobby project — expect rough edges, limited review, and no production support guarantees. Use at your own risk.

## What this fork adds

Most of the work is WebRTC stability: no more freezes from OME's own send path, loss recovery that works, and auto ABR that settles on the right rendition. All of it was found and measured with the [pre-release real-world test](misc/README.md) in `misc/`. That test plays streams to real Chrome and Firefox through controlled loss, delay, jitter, outages and bottlenecks, and measures frame drops, freezes, delay and memory.

### Features

* **NVIDIA NVENC / NVDEC:** restores hardware H.264/H.265 encode and decode on OME 0.21.0 and later (upstream removed this path).
* **Scheduled Channel SegmentCache:** for bypass `file://` H.264+AAC MP4 items, indexes samples and can serve HLS/LLHLS while idle without keeping the demux media pump running (near-zero media disk I/O when no WebRTC/OVT viewers are connected). Configure it under `Providers/Schedule/SegmentCache` in Server.xml; see [Scheduled Channel](docs/live-source/scheduled-channel.md#segment-cache).
* **Opt-in WebRTC features**, set under `<Publishers><WebRTC>`. Each one helps in some conditions and costs something in others, so each is off by default (upstream behaviour). See [WebRTC Streaming](docs/streaming/webrtc-publishing.md#publisher) for when to use them.

  | Setting | What it does | Trade-off |
  |---|---|---|
  | `<SendPacing>true</SendPacing>` | Spreads bursts beyond a keyframe (retransmissions after an outage, a backlog after an ingest stall) at 2.5x the media rate, retransmissions first | Behind a 10 Mbps / 100 ms queue: a 1 s outage froze 6.8 s, now 1.0 s. On a path that could take the whole burst, backlogs drain a little slower |
  | `<VideoShedding>true</VideoShedding>` | A congested session that cannot switch down (a pinned rendition, or auto ABR on its lowest one) stops video until the next keyframe. It sheds only while the link carries less than the stream and the queue grows or packets are lost, and again only on feedback for frames sent after it resumed | A pinned 1080p viewer on a too-narrow link froze 25–28 s, now about 1 s. A longer path (jitter) does not trigger it. Each shed costs up to one GOP: only for short GOPs |
  | `<PlayoutDelay><Adaptive>true</Adaptive></PlayoutDelay>` | A session that keeps losing packets on a long path raises its own playout delay to the path's base round trip + 80 ms, and lowers it again slowly once the loss stops | 300 ms round trip with 1% loss: about 16 freezes in 30 s, now about 6. Costs that viewer several hundred ms of latency |
  | `<JitterBufferDelay><Min>80</Min><Max>500</Max></JitterBufferDelay>` | The hold range of `<JitterBuffer>` (default 20 ms minimum, as upstream) | Soak-tested with several heavy renditions at once; a longer minimum adds that delay for every viewer |

* **`?rendition=` signalling query parameter:** start a WebRTC session on a given rendition ([docs](docs/streaming/webrtc-publishing.md#rendition-query-parameter)).
* **B-frame warning:** a warning is logged once per track when H.264 video for WebRTC has B-frames, which Chrome's software H.264 decoder cannot play (it then shows keyframes only).
* **Pre-release real-world test** ([misc/README.md](misc/README.md)): a WebRTC smoke test that decodes what Chrome assembles on 6 network profiles; a traffic simulator with churning publishers, lightweight WebRTC and LL-HLS viewers and real desktop browsers; a limits test that takes each impairment up to its limit; and a one-command setup for a Windows desktop as a remote test browser.

### Bug fixes

Compared with upstream OvenMediaEngine.

**WebRTC, always on:**

* **Retransmission (`<Rtx>`):**
  * a late NACK is never answered with the wrong packet (the sent log matched slots, not sequence numbers);
  * a stale cached copy is never served after the history slot was reused;
  * every NACKed packet is resent, not only the first;
  * retransmissions get fresh transport-wide sequence numbers, as in libwebrtc.
* **ULPFEC (`<Ulpfec>`)** now actually recovers packets: before, browsers recovered none.
  * The parity covers header extensions as RFC 5109 requires and uses each session's own sequence numbers.
  * It is encoded on a bounded worker, off the send path; encoded inline, it delayed every 4K frame by tens of ms.
  * After a slow repair, the sequence numbers Chrome has forgotten as padding are announced again. Without that, a repair on a path with 50 ms or more round trip could freeze the viewer until the next keyframe.
* **Auto ABR** works. Before, it fell to the lowest rendition within seconds on a clean path and stayed there, because FEC packets left gaps in the transport-wide sequence numbers that read as loss.
  * Feedback arrival times are parsed correctly (receive deltas accumulate).
  * Capacity is measured from packet trains.
  * It settles on the best rendition for the link within seconds, and switches back up after congestion.
  * REMB is ignored while transport-wide feedback flows (Chrome's REMB forced needless down-switches).
* **Several renditions at once** (a 4K passthrough beside a transcoded ladder) no longer freeze each other. Each session has its own send thread, each rendition packetizes on its own worker, and the UDP send buffer is larger.
* **Pacer (`<JitterBuffer>`):**
  * sends frames in decode order (it scheduled on PTS, so B-frames went out ahead of their references, which showed as smearing on motion);
  * releases each frame at its decode-time target but holds it no longer than its arrival plus the delay, so the source's arrival jitter and catch-up bursts no longer reach viewers as freezes.
* **H.264:** access unit delimiters and timing-only SEI are left out of the RTP stream (about 7% fewer packets; captions SEI is kept).
* **RTP header extensions:** framemarking (an expired draft that no browser negotiates) is no longer offered or sent. It went on every video packet, and Chrome logged a warning for each one.

**Elsewhere:**

* **LLHLS:** sessions sharing a keep-alive connection (e.g. an nginx upstream pool) no longer leak.
* **Monitoring:** session counters no longer drift when a stream is deleted while viewers are connected.

Everything else follows upstream OvenMediaEngine.

## What is OvenMediaEngine?
<img src="dist/OME_LLHLS_220610.svg" style="max-width: 100%; height: auto;">

OvenMediaEngine (OME) is a Sub-Second Latency Streaming Server that can stream Large-scale and High-definition live streams over Low Latency HLS (LLHLS) and WebRTC to hundreds of thousands of viewers.

OME can ingest live streams over WebRTC, SRT, RTMP, RTSP, and MPEG2-TS protocols, encode them to ABR with the embedded live transcoder, and stream them to viewers over LLHLS and WebRTC.

With OvenMediaEngine, you can easily build a powerful, sub-second latency media service.

## Demo https://space.ovenplayer.com
<img src="dist/05_OvenSpace_230214.png" style="max-width: 100%; height: auto;">

OvenSpace is a sub-second latency streaming demo service using [OvenMediaEngine](https://github.com/OvenMediaLabs/OvenMediaEngine), [OvenPlayer](https://github.com/OvenMediaLabs/OvenPlayer) and [OvenLiveKit](https://github.com/OvenMediaLabs/OvenLiveKit-Web). You can experience OvenMediaEngine live at the **[OvenSpace Demo](https://space.ovenplayer.com/)** and browse implementation examples in the [OvenSpace Repository](https://github.com/OvenMediaLabs/OvenSpace).

## Features
* Ingest
  * Push: WebRTC, WHIP(Simulcast), SRT, RTMP, E-RTMP, MPEG-2 TS/UDP
  * Pull: RTSP, OVT
  * Scheduled Channel (Pre-recorded Live)
  * Multiplex Channel (Duplicate stream / Mux tracks)
* Adaptive Bitrate Streaming (ABR) for LLHLS and WebRTC
* Low Latency Streaming using LLHLS
  * DVR (Live Rewind)
  * Dump for VoD
  * ID3v2 timed metadata
  * DRM (Widevine, FairPlay, PlayReady)
  * Subtitle (WebVTT)
* Sub-Second Latency Streaming using WebRTC
  * WebRTC over TCP (With Embedded TURN Server)
  * Embedded WebRTC Signalling Server (WebSocket based)
  * Retransmission with NACK
  * ULPFEC (Uneven Level Protection Forward Error Correction)
    * <i>VP8, H.264, H.265</i>
  * In-band FEC (Forward Error Correction)
    * <i>Opus</i>
* Legacy HLS (HLS version 3)
  * Dump for VoD
  * MPEG-2 TS Container
  * Audio/Video Muxed
  * DVR
* Sub-Second Latency Streaming using SRT
  * Secure Reliable Transport
  * MPEG-2 TS Container
  * Audio/Video Muxed
* Embedded Live Transcoder
  * Video: VP8, H.264, H.265(Hardware only), AV1, Pass-through
  * Audio: Opus, AAC, Pass-through
* Clustering (Origin-Edge Structure)
* Monitoring
* Access Control
  * Admission Webhooks
  * Signed Policy
* File Recording
* Push Publishing using SRT, RTMP and MPEG2-TS (Re-streaming)
* Thumbnail
* REST API

## Supported Platforms
Although we have tested OvenMediaEngine on the platforms listed below, it may work with other Linux packages as well:

* [Docker](https://hub.docker.com/r/ovenmedialabs/ovenmediaengine)
* Ubuntu 18+
* Rocky Linux 8+
* AlmaLinux 8+
* Fedora 28+

## Quick Start

* [Quick Start Guide](https://docs.ovenmediaengine.com/quick-start)
* [Manual](https://docs.ovenmediaengine.com/)

### Docker
```bash
docker run --name ome -d -e OME_HOST_IP=Your.HOST.IP.Address \
-p 1935:1935 -p 9999:9999/udp -p 9000:9000 -p 3333:3333 -p 3478:3478 -p 10000-10003:10000-10003/udp -p 10000:10000/tcp \
ovenmedialabs/ovenmediaengine:latest
```

You can also store the configuration files on your host:

```bash
docker run --name ome -d -e OME_HOST_IP=Your.HOST.IP.Address \
-p 1935:1935 -p 9999:9999/udp -p 9000:9000 -p 3333:3333 -p 3478:3478 -p 10000-10003:10000-10003/udp -p 10000:10000/tcp \
-v ome-origin-conf:/opt/ovenmediaengine/bin/origin_conf \
-v ome-edge-conf:/opt/ovenmediaengine/bin/edge_conf \
ovenmedialabs/ovenmediaengine:latest
```

The configuration files are now accessible under `/var/lib/docker/volumes/<volume_name>/_data`.

Following the above example, you will find them under `/var/lib/docker/volumes/ome-origin-conf/_data` and `/var/lib/docker/volumes/ome-edge-conf/_data`.

If you want to put them in a different location, the easiest way is to create a link:
```bash
ln -s /var/lib/docker/volumes/ome-origin-conf/_data/ /my/new/path/to/ome-origin-conf \
&& ln -s /var/lib/docker/volumes/ome-edge-conf/_data/ /my/new/path/to/ome-edge-conf
```

Please read the [Getting Started](https://docs.ovenmediaengine.com/getting-started) for more information.

### WebRTC Live Encoder for Testing
* https://demo.ovenplayer.com/demo_input.html

### Player for Testing
* Without TLS: http://demo.ovenplayer.com
* With TLS: https://demo.ovenplayer.com

## How to Contribute
Thank you for your interest in contributing to OvenMediaEngine.

We need your help to keep this project growing. There are many ways to contribute.
For more information on how to contribute, please see our [Guidelines](CONTRIBUTING.md) and [Rules](CODE_OF_CONDUCT.md).

**In this fork, a change to the media path (WebRTC, RTP/RTCP, the pacer, the transcoder, the providers or LL-HLS) goes through the [pre-release real-world test](misc/README.md) before it is merged:** a smoke test, a 2.5-hour soak with real desktop browsers, and an A/B against the previous build. Put the summaries in the pull request.

- [Finding Bugs](https://github.com/OvenMediaLabs/OvenMediaEngine/blob/master/CONTRIBUTING.md#finding-bugs)
- [Reviewing Code](https://github.com/OvenMediaLabs/OvenMediaEngine/blob/master/CONTRIBUTING.md#reviewing-code)
- [Writing Code: Thread Safety Analysis (TSA) Guide](https://github.com/OvenMediaLabs/OvenMediaEngine/blob/master/src/base/ovlibrary/tsa/README.md)
- [Sharing Ideas](https://github.com/OvenMediaLabs/OvenMediaEngine/blob/master/CONTRIBUTING.md#sharing-ideas)
- [Testing](https://github.com/OvenMediaLabs/OvenMediaEngine/blob/master/CONTRIBUTING.md#testing)
- [Improving Documentation](https://github.com/OvenMediaLabs/OvenMediaEngine/blob/master/CONTRIBUTING.md#improving-documentation)
- [Spreading & Use Cases](https://github.com/OvenMediaLabs/OvenMediaEngine/blob/master/CONTRIBUTING.md#spreading--use-cases)
- [Recurring Donations](https://github.com/OvenMediaLabs/OvenMediaEngine/blob/master/CONTRIBUTING.md#recurring-donations)

We hope OvenMediaEngine inspires you to build something great.

## About OvenMedia Labs and Projects
OvenMedia Labs aims to make it easier for you to build a stable broadcasting/streaming service with Sub-Second Latency.
Therefore, we will continue developing and providing the most optimized tools for smooth Sub-Second Latency Streaming.

* Information
  * [OvenMedia Labs Website](https://ovenmedialabs.com): About OvenMediaEngine, Enterprise on Marketplace, and more
  * [OvenMedia Labs' Blog](https://ovenmedialabs.com/blog): A blog researched and written directly by an OvenMediaEngine developer
* Open-Source Repository
  * [OvenMediaEngine GitHub](https://github.com/OvenMediaLabs/OvenMediaEngine): Sub-Second Latency Live Streaming Server
  * [OvenMediaEngine Docker Hub](https://hub.docker.com/r/ovenmedialabs/ovenmediaengine): Docker image for quick deployment
  * [OvenPlayer GitHub](https://github.com/OvenMediaLabs/OvenPlayer): JavaScript-based WebRTC and LLHLS Player for OvenMediaEngine
  * [OvenLiveKit GitHub](https://github.com/OvenMediaLabs/OvenLiveKit-Web): JavaScript-based Live Streaming Encoder for OvenMediaEngine
* Documentation
  * [OvenMediaEngine Getting Started](https://docs.ovenmediaengine.com): Configuration, ABR, Clustering, and more
  * [OvenPlayer Getting Started](https://docs.ovenplayer.com): UI Customization, API Reference, Examples, and more
* Demo
  * [OvenSpace Demo](https://space.ovenplayer.com): Sub-Second Latency Streaming Demo Service
  * [OvenPlayer Demo with TLS](https://demo.ovenplayer.com): For testing playback with HTTPS and WSS endpoints
  * [OvenPlayer Demo without TLS](http://demo.ovenplayer.com): For testing playback with HTTP and WS endpoints

## License
OvenMediaEngine is licensed under the [AGPL-3.0-only](LICENSE).
However, if you need another license, please feel free to email us at [contact@ovenmedialabs.com](mailto:contact@ovenmedialabs.com).
