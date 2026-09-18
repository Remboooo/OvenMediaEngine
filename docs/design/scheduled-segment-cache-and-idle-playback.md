# Scheduled Channel: Segment Cache & Idle-Efficient Playback

## Codec / container scope (v1)

**SegmentCache is H.264 + AAC in MP4/MOV only** (no transcoder on this path). Samples are remuxed into HLS MPEG-TS and LLHLS fMP4. Unsupported codecs/containers (HEVC, AV1, Opus, MP3, MPEG-TS files, MKV, …) log a **warning** and that item **falls back to demux** — the schedule is not rejected (SegmentCache config and `.sch` files have separate lifetimes).

| | For cache path |
|---|---|
| Container | MP4 / MOV (ISOBMFF), `file://` |
| Video | H.264 |
| Audio | AAC when `AudioTrack=true` (AAC-only tracks) |
| Schedule | `BypassTranscoder=true` (else demux fallback) |

WebRTC still expects **Opus** for audio in browsers; bypass AAC fillers can idle-serve (LL)HLS from cache, while WebRTC audio may need the demux/transcode pump. Extending SegmentCache to other codecs is out of scope for v1.

## Goal

For Scheduled Channel `file://` items (especially looping fillers with `BypassTranscoder`):

1. Stop continuous remux/disk read when nobody is consuming.
2. Avoid re-indexing on every process restart when sources and settings are unchanged.
3. Keep middleware dumb — efficiency lives inside OvenMediaEngine.
4. Apply the same idle policy to WebRTC (and other push publishers), not only HLS/LLHLS.

Primary success metric for an idle filler with no clients: **~0 media disk I/O** and no realtime demux/remux loop.

---

## Background (current behavior)

- `ScheduledStream` runs a worker that continuously demuxes schedule items and `SendFrame`s, whether or not anyone is watching.
- HLS/LLHLS packagers consume those frames continuously, maintain an in-memory sliding window, and update playlists; HTTP only serializes already-built state.
- Result: a looping filler at ~N Mbps produces ~N/8 MB/s sustained reads forever (multiple `.sch` ladders multiply that).

Wall-clock schedule position is already computable (`GetFirstItemWithPosition` + seek). Continuous reading is not required just to “stay on schedule.”

---

## Part A — HLS / LLHLS virtual segment cache

### Idea

Treat each playlist segment (and LLHLS part) as a **virtual object** defined by metadata, not as a stored byte copy of the media.

| Phase | Behavior |
|---|---|
| Index / first build | Demux (or probe) source; record byte ranges, timestamps, keyframes, part/segment boundaries, init/codec config |
| Persist (optional) | Write sidecar next to the video file |
| Serve | On HTTP GET, materialize segment bytes from `metadata + source mmap/pread` + thin remux wrapper |
| Later loops / restarts | Drive playlists from index + wall-clock playhead; do **not** realtime-remux the whole file again |

Serving still reads compressed media for **active** viewers (or from page cache). Idle with no fetches ≈ no media I/O.

### Cache record (per source file × packager fingerprint)

**Identity / invalidation**

- Cache format version
- Source identity: path, size, mtime (optional content hash)
- Packager fingerprint: HLS vs LLHLS, `SegmentDuration`, `ChunkDuration` / part target, TS vs fMP4, relevant codec expectations, OME schema version

**Data**

- Init segment bytes or equivalent codec config (`moov` / AvCC/HvCC/ASC)
- Ordered segment list; for LLHLS, part list per segment
- Per segment/part: media sequence role, duration, earliest PTS/DTS, keyframe alignment flag, list of sample (or contiguous byte-range) references into the source
- Optional: precomputed playlist templates

**Size order of magnitude** (long filler, ~hours @ 30 fps): tens of MB of metadata per ladder — not a second copy of the video.

### Virtual segment store

In-process API (not FUSE):

- `GetInit(key) → Data` (usually fully cached in memory)
- `GetSegment(key) / GetPart(key) → Data` (generate on demand)
- Generation: map index → source ranges → wrap `moof`/`mdat` (LLHLS) or PES/TS (HLS)

Publisher playlist code holds **keys + durations + sequence numbers**, not owned segment buffers (except a small hot window if useful).

### Live playlist behavior across loops

- Keep `EXT-X-MEDIA-SEQUENCE` (and part numbers) monotonically increasing across file loops.
- On wrap, insert discontinuity if the encoded timeline resets.
- Sliding window length stays as configured (`SegmentCount`); window content is “which virtual keys are visible now,” derived from wall-clock playhead. Optional `HlsLookaheadSegments` (SegmentCache config) extends classic HLS past the playhead so players that join behind the live edge can align with LLHLS; LLHLS remains edge/part-accurate.

### Hydration modes

| Mode | When index is built |
|---|---|
| `lazy` (default) | As segments are first produced during normal realtime play, or on first request miss |
| `greedy` | At channel/item start, index the whole file as fast as I/O allows, then switch to serve-from-index |

Greedy avoids waiting a full realtime playthrough (e.g. multi-hour `trains4.mp4`) before the cache is complete.

### Persistence

Optional sidecar next to the source, e.g.:

```text
/opt/ovenmediaengine/media/trains4.mp4
/opt/ovenmediaengine/media/trains4.mp4.ome-segcache
```

(or a suffix that includes a short fingerprint hash if multiple packager profiles share one file)

**Rebuild only when:** sidecar missing/corrupt, source changed, fingerprint mismatch, or cache format version bump.

**Write atomically:** `*.tmp` + `rename`.

Optional override: `SegmentCachePath` when media is on a read-only volume.

### Configuration sketch

See **Configuration (Server.xml)** below for the wired schema. Schedule-level:

```xml
<Stream>
  <Name>_filler</Name>
  <BypassTranscoder>true</BypassTranscoder>
  <SegmentCache><Enable>true</Enable></SegmentCache>
</Stream>
```

### Memory model (mmap / page cache)

- Userspace holds the index (+ tiny init).
- Source file(s) may be `mmap`’d; resident RAM ≈ recently faulted pages (active segment window), not file size.
- Do **not** `mlock` whole files by default.
- Rough ballparks for current fillers: ~40–120 MB index if all ladders indexed; ~5–60 MB hot media pages for a ~60s window depending on how many ladders are active.

### Scope: shared sample index → LLHLS (fMP4) **and** HLS (MPEG-TS)

Do **not** ship LLHLS-only and keep a forever-remux fallback for classic HLS. Investigate/implement TS materialization on the **same** index so both publishers benefit.

| Direction | Format | OME today | Cache plan |
|---|---|---|---|
| Input | `file://` MP4 | Scheduled | **Required** (v1) |
| Output LLHLS | fMP4 `.m4s` | LLHLS publisher | Materializer A |
| Output HLS | MPEG-2 TS `.ts` | HLS publisher (`?format=ts` / `ts:`) | Materializer B |

**Feasibility conclusion: TS is viable.** The expensive part (knowing which source bytes belong to segment N) is shared. TS adds a heavier wrap step, not a second indexing problem.

#### Shared (once)

- Sample/GOP index into the MP4 (offsets, sizes, DTS/CTS, keyframe flags)
- Segment boundary plan for a given `SegmentDuration` (keyframe-aligned cuts)
- Source + settings invalidation / sidecar persistence
- Virtual playlist driver (no pump, no I/O until GET)

#### fMP4 materialize (LLHLS) — lighter

- Init `moov` + per-part/seg `moof`/`mdat`
- Samples often already AVCC/AAC-RAW in the MP4 `mdat` → mostly copy + box headers
- Parts multiply index entries but same mechanism

#### TS materialize (HLS) — heavier but straightforward

OME already has the building blocks (`mpegts::Packetizer` / `Packager`):

1. **Bitstream convert:** AVCC→Annex-B, AAC RAW→ADTS (same as live packager path)
2. **A/V mux:** classic HLS segments are **muxed** audio+video in one TS (OME requires `EnableTsPackaging`)
3. **PES + 188-byte TS packets**, PAT/PMT at segment start
4. **Continuity counters / PCR:** regenerate per materialize; only need to be consistent **within** a segment (segment boundaries already allow discontinuity signaling)

No need to store finished `.ts` bytes in the sidecar — only sample ranges + boundary plan. On GET: pull samples → run packetizer for that segment’s sample set → return TS blob.

**Cost vs fMP4:** more CPU per GET (convert + PES/TS), still far cheaper than a permanent realtime demux loop; idle still ≈ 0 I/O.

**Risk if we skip TS:** LLHLS path is efficient, but any client on classic HLS keeps the old always-on remux (or forces the media pump), i.e. a permanent half-solution.

#### Spike before committing implementation order

Minimal proof (hours–days, not a full feature):

1. Build sample index for one bypass MP4.
2. Pick one keyframe-aligned window (~`SegmentDuration`).
3. Materialize one TS segment via existing `mpegts::Packetizer` (or equivalent).
4. Confirm a stock HLS player plays that segment in a tiny VOD playlist.
5. Compare CPU time vs live packager producing the same window.

#### Spike status (2026-09-16)

Implemented under `src/modules/segment_cache/`:

- `Mp4SampleIndexer` — scan MP4 → sample offsets/DTS/keyframes
- `MpegTsMaterializer` — index + `mpegts::Packetizer`/`Packager` → one muxed HLS TS segment
- GTest: `SegmentCacheMpegTsMaterialize.*` in `ome_test_modules`

Results on this host:

| Source | Window | Output | CPU (materialize) |
|---|---|---|---|
| Tiny fixture (320×240, 2s) | ~1s | ~32 KB TS, `ffprobe` OK | **0.3–0.8 ms** |
| `480p/trains3.mp4` | ~2s (GOP-aligned) | ~180 KB TS | **~1.3 ms** |

**Conclusion:** shared index → TS materialize is **go**. Byte-identical to demux-fed packager path after multi-loop joins (`SegmentCacheParity.*` — all passing). Cache reads only the segment’s sample bytes (~160 KB in for ~180 KB TS out on 480p).

### Configuration (Server.xml)

Under `Providers/Schedule`:

```xml
<Schedule>
  <MediaRootDir>/opt/ovenmediaengine/media</MediaRootDir>
  <ScheduleFilesDir>/opt/ovenmediaengine/media</ScheduleFilesDir>
  <SegmentCache>
    <Enable>true</Enable>
    <Mode>persist</Mode>              <!-- persist | memory -->
    <Hydrate>
      <Mode>greedy</Mode>             <!-- lazy | greedy -->
      <MaxThreads>2</MaxThreads>
      <MaxThroughputMbps>50</MaxThroughputMbps>  <!-- aggregate for index+hydrate; 0 = unlimited -->
    </Hydrate>
    <IdleGracePeriodMs>30000</IdleGracePeriodMs>
    <!-- Classic HLS: advertise N segments past playhead so join latency ≈ LLHLS -->
    <HlsLookaheadSegments>3</HlsLookaheadSegments>
  </SegmentCache>
</Schedule>
```

**Bypass preferred.** SegmentCache only remuxes bypass H.264+AAC MP4. Enabling it with `BypassTranscoder=false` logs a warning and falls back to demux/transcode — the schedule still loads.

### Media / config requirements

When SegmentCache is enabled (Server.xml, `.sch` override, or `OME_SEGMENT_CACHE`):

| Requirement | Behavior if violated |
|---|---|
| `BypassTranscoder=true` | **Warn**; schedule still loads; that stream uses demux/transcode (no cache) |
| `VideoTrack=true` | **Warn**; schedule still loads; cache skipped for that stream |
| Each `file://` item is **MP4/MOV + H.264** (+ **AAC** if `AudioTrack=true`) | **Warn** at prepare; that item falls back to demux; schedule is not rejected |
| Invalid `SegmentCache` XML enums/ranges | Server.xml **ConfigError** (load fails) |

SegmentCache is configured at the application level; schedule files have separate lifetimes — unsupported media must not fail schedule load.

Non-`file://` items (`stream://`) are allowed in the same schedule but are **not** cached.

Per-stream override in `.sch` (inherits Server.xml when omitted):

```xml
<Stream>
  <Name>_filler</Name>
  <BypassTranscoder>true</BypassTranscoder>
  <SegmentCache><Enable>true</Enable></SegmentCache>
</Stream>
```

Sidecar files encode the packager fingerprint so HLS/LLHLS (and different durations) do not collide:

```text
source.mp4.ome-segcache.f2.td6000.cd500.sv1
```

(`f` = format_id 1 HLS-TS / 2 LLHLS-fMP4, `td` = target duration ms, `cd` = chunk duration ms, `sv` = schema version)

Fingerprint durations are taken from the application’s LLHLS (preferred) or HLS publisher `SegmentDuration` / `ChunkDuration` when present.

### Optional env overrides

XML is the primary gate. Env vars remain for unit tests / temporary enable without editing Server.xml:

| Variable | Default | Effect |
|---|---|---|
| `OME_SEGMENT_CACHE` | unset / off | Force-enable when XML `Enable` is false |
| `OME_SEGMENT_CACHE_TARGET_MS` | from publisher / 2000 | Override boundary plan target duration |
| `OME_SEGMENT_CACHE_CHUNK_MS` | from publisher / 500 | Override LLHLS chunk duration |
| `OME_SEGMENT_CACHE_HYDRATE` | XML Hydrate/Mode | `greedy` / `lazy` override |
| `OME_SEGMENT_CACHE_PUMP_GRACE_MS` | XML IdleGracePeriodMs / 30000 | Pump grace after demand drops |

**What runs when enabled + bypass filler:**

1. `PrepareFilePlayback` probes media (H.264+AAC MP4), enables cache-serve **before** demux so packager never advertises MSN 0…N; opens/loads sidecar, builds boundary plan, registers `SourceSession` with join-offset playhead; greedy hydrate warms a **playhead-centered hot window**.
2. HLS/LLHLS GETs prefer cache; miss rematerializes on demand. Demux `TickCachePlayhead` may re-warm while WebRTC is active; idle with no clients does not warm (keeps ~0 disk I/O).
3. Idle playlists advance from wall-clock elapsed (absolute ms; `ResolvePlayhead` derives loop/MSN).
4. Demux skips when no WebRTC/OVT demand; resumes + seeks on demand; grace applies when leaving an active pump.

**Tests:** `ome_test_modules --gtest_filter='SegmentCache*'`.

**Not done:** metrics, install over live, hot-cache idle RSS reclaim timer.

### Non-goals (Part A)

- Storing full segment byte payloads in the sidecar (metadata + optional tiny init only).
- Replacing Dump/DVR (write-only / rewind-window features stay as they are).
- Caching re-encoded transcoder output (bypass/`file://` MP4 path first).
- Middleware-orchestrated create/delete of channels.

### Hot segment byte cache (short TTL / sliding window)

Materialize-on-every-GET is correct for correctness, but **TS wrap is CPU-heavy enough to share**.

Pessimistic napkin math: ~80 ms CPU to build one full-quality 6 s TS segment → **~12 concurrent materializes/s ≈ 1 core**. Many players refreshing near the live edge would amplify that if each GET rebuilt the same segment.

In practice viewers of the **same** stream request the **same** segment URLs, so:

- Keep a **short-lived byte cache** of materialized segments/parts (the payload, not just metadata), keyed by virtual segment id.
- Bound it like today’s live packager window: e.g. last `SegmentCount` (+ a few) segments, or TTL ≈ window duration (tens of seconds), or max MB.
- First GET (or playlist promotion) materializes once; subsequent GETs are memcpy/sendfile from RAM.
- Idle with no viewers: drop the hot byte cache after grace → back to **metadata-only** (+ source on disk); media RSS ≈ 0.
- Optional: materialize when a segment **enters the playlist** (prefetch) so the first player does not pay the spike — still no demux pump, just one wrap per segment per loop position.

Memory: one full-quality 6 s TS ≈ **4–5 MB**; a 10-segment window ≈ **40–50 MB** per rendition (less for 720p/480p). Acceptable vs continuous remux of the whole library.

So two layers:

| Layer | What | Lifetime |
|---|---|---|
| **Index / sidecar** | Sample ranges, boundaries, init | Persists (optional on disk) |
| **Hot segment bytes** | Finished `.ts` / `.m4s` | Sliding window / short TTL while demanded |

fMP4 is cheaper to rebuild (~5–15 ms); hot cache still helps under fanout but matters most for **HLS TS**.

### Implementation phases (Part A)

0. **TS feasibility spike** — done (`SegmentCacheMpegTsMaterialize` / `SegmentCacheParity`).
1. **Sample index + sidecar I/O** — done (`.ome-segcache` v2).
2. **Boundary planner** — done (HLS segments + LLHLS parts).
3. **Materializers** — done (MPEG-TS + fMP4).
4. **Wire into publishers** — done (XML `SegmentCache` / optional `OME_SEGMENT_CACHE` env; GET + idle playlist sync).
5. **Idle playlist driver** — done (HLS media playlist + LLHLS chunklist).
6. **Greedy hydrate** — done (`Hydrate/Mode`, `MaxThreads`, `MaxThroughputMbps`).
7. **Config/docs** — Server.xml `Providers/Schedule/SegmentCache` wired; env remains optional override.

---

## Part B — Demand-driven Scheduled playback (WebRTC & push publishers)

### Problem

WebRTC (and OVT, etc.) does not fetch segments. It expects a continuous RTP/media timeline while sessions exist. Today that timeline is produced by always running `ScheduledStream::WorkerThread` demux → `SendFrame`, which keeps disk I/O alive with **zero** WebRTC clients.

HLS segment cache alone does not fix WebRTC idle I/O.

### Idea

Split “schedule clock” from “media pump”:

| Component | Role |
|---|---|
| Schedule clock | Always knows program/item/position from wall clock (cheap; no media read) |
| Media pump | Demux/read source and emit frames **only while there is demand** |
| Shared sample/segment index (from Part A) | Fast keyframe-aligned seek to the correct file offset when the pump starts |

### Demand signal

**(LL)HLS does not use the pump signal.** Once the segment cache is filled, keeping the live playlist up to date is playhead arithmetic (which virtual keys are in the window, bumping `MEDIA-SEQUENCE`) — negligible CPU, **no media I/O** until a client GETs a segment/part (materialize-on-read). Playlist refresh alone must not wake the demux pump.

Pump demand is only for publishers that need a continuous frame feed:

- WebRTC: real viewer sessions (`GetSessionCount()` / connect-disconnect hooks) — **not** idle origin session pools if those stay permanently allocated
- OVT (and similar push/relay sinks): downstream sessions

**Pump on** when that demand &gt; 0. **Pump off** after demand stays 0 for a configurable grace period (e.g. 15–60s) to avoid thrashing.

Stream metadata / codecs should remain published so clients can still discover the stream; only the heavy read path stops.

### Start path (client arrives while idle)

1. Resolve wall-clock position → item + offset (existing schedule math).
2. Seek via index (keyframe-aligned) or `avformat_seek_file` fallback.
3. Start demux loop; feed transcoder/publishers as today.
4. Accept short warm-up (keyframe wait, WebRTC buffering). Document expected first-frame latency.

### Stop path (last client leaves)

1. Grace timer.
2. Stop demux; release file handles / allow mmap pages to be reclaimed.
3. Keep schedule clock and (if present) HLS/LLHLS virtual playlist drivers running so segment delivery stays idle-efficient without waking the pump when Part A can materialize on GET.

When HLS/LLHLS **and** WebRTC are both enabled:

- **(LL)HLS-only viewers:** Part A materialize-on-GET; pump stays off if index is complete; playlists still advance.
- **WebRTC viewers:** pump must run for the duration of the session.
- **Both:** pump runs for WebRTC; HLS/LLHLS GETs still prefer cached materialize so the packager is not double-fed (optimization).

### WebRTC-specific notes

- Pre-created LLHLS origin sessions must not count as demand; WebRTC must use **actual peer connections / subscribed sessions**.
- Opus / transcoded audio tracks require the pump (and transcoder) while WebRTC clients need them; bypass-only Video+Audio is the best idle case.
- On pump restart, RTP timestamps / WebRTC timeline continuity need a defined policy (discontinuity / new epoch is acceptable if documented; seamless continuity is harder and not required for v1).

### Configuration sketch

```xml
<Stream>
  <Name>_filler</Name>
  <BypassTranscoder>true</BypassTranscoder>
  <IdlePolicy>
    <Enabled>true</Enabled>
    <GracePeriodMs>30000</GracePeriodMs>
    <!-- Demand sources: WebRTC, OVT, HlsRequestTtlMs, ... -->
  </IdlePolicy>
  <SegmentCache>persist</SegmentCache>
  <HydrateSegmentCache>greedy</HydrateSegmentCache>
</Stream>
```

### Implementation phases (Part B)

1. **Demand aggregator** — done (WebRTC + OVT `GetSessionCount`; grace via `OME_SEGMENT_CACHE_PUMP_GRACE_MS`).
2. **ScheduledStream pump gate** — done (idle `PlayFileIdle` / `RESUME_PUMP`; seek on wake).
3. Reuse Part A index for seek — partial (`avformat_seek_file` from elapsed; index-guided seek polish TBD).
4. Confirm LLHLS playlist advancement never increments pump demand — by design (HLS/LLHLS not counted).
5. Metrics: `idle`, `pump_active`, `demand_count`, … — **not started**.
6. Docs + tests — env flags in this doc; idle I/O / WebRTC wake still need on-host validation (do not install over live).

### Non-goals (Part B)

- Zero I/O **while WebRTC clients are connected** (impossible without caching decoded/encoded bitstreams; out of scope).
- Perfect RTP timeline continuity across idle gaps in v1.

---

## Shared index (Parts A + B)

One sidecar / in-memory index per `source × fingerprint` should serve both:

- HLS/LLHLS virtual segment materialization
- Fast seek when the WebRTC/media pump wakes

Fingerprint for WebRTC-only seek might be weaker (sample index without HLS part boundaries), but a single richer LLHLS-oriented index can satisfy both if HLS is enabled.

---

## Rollout plan

1. Ship Part B pump gate first if idle disk I/O + WebRTC is the immediate pain — works before segment cache.
2. **Part A spike:** prove TS materialize from MP4 sample index (and fMP4 in the same pass if cheap).
3. Ship shared index + **both** HLS-TS and LLHLS-fMP4 materializers + greedy hydrate.
4. Add persist sidecar.
5. With Part A complete, HLS/LLHLS-only traffic never starts the pump; playlists keep moving in memory.
6. Optional: `madvise` / prefault tuning.

Feature flags default **off** until validated on `_filler`.

---

## Test plan (summary)

- Idle filler, no clients: process media read rate ≈ 0 (cgroup `io.stat` / `pidstat`).
- Restart with `persist`: no re-index if source+settings unchanged; re-index when file or segment duration changes.
- Greedy hydrate: cache complete ≪ realtime duration.
- HLS (TS) and LLHLS (fMP4): second loop serves from index; playlist seq monotonic across wrap; idle playlist updates cause no media reads.
- WebRTC: connect after long idle → playback starts near schedule position; disconnect → I/O drops after grace.
- Mixed: (LL)HLS-only does not keep the WebRTC pump warm when Part A complete.

---

## Open questions

1. Sidecar naming when one MP4 is used with multiple segment/chunk duration profiles (and HLS vs LLHLS fingerprints).
2. Whether greedy hydrate should cap CPU/I/O bandwidth.
3. WebRTC timeline policy after idle gap (hard discontinuity vs attempt continuity).
4. Interaction with non-bypass / transcoder output profiles on the same Scheduled stream.
5. TS spike: reuse `mpegts::Packetizer` in isolation vs a thinner one-shot segment builder.
