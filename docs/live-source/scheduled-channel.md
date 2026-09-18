---
title: Scheduled Channel
description: "Build pre-scheduled, playlist-style live channels in OvenMediaEngine with the Schedule Provider."
sidebar_position: 16
---

Scheduled Channel that allows you to create a live channel by scheduling pre-recorded files has been added to OvenMediaEngine. Other services or software call this Pre-recorded Live or File Live, but OvenMediaEngine plans to expand the function to organize live channels as a source, so we named it Scheduled Channel.

## Getting Started

To use this feature, activate Schedule Provider as follows.

```xml
<!-- /Server/VirtualHosts/VirtualHost/Applications/Application -->
<Providers>
    ...
    <Schedule>
        <MediaRootDir>/opt/ovenmediaengine/media</MediaRootDir>
        <ScheduleFilesDir>/opt/ovenmediaengine/media</ScheduleFilesDir>
        <PreserveRemovedScheduleFile>false</PreserveRemovedScheduleFile> <!-- optional, default: false -->
    </Schedule>
    ...
</Providers>
```

`<MediaRootDir>`\
Root path where media files are located. If you specify a relative path, the directory where the config file is located is root.

`<ScheduleFilesDir>`\
Root path where the schedule file is located. If you specify a relative path, the directory where the config file is located is root.

`<PreserveRemovedScheduleFile> (optional, default: false)`\
When OvenMediaEngine removes a schedule file, such as when the channel is deleted with the DELETE API or by `<MaxFallbackDurationMs>`, setting this to true renames the file by appending the removal time, for example `today.sch.20260831T204512`, instead of deleting it. A renamed file is not loaded as a channel. Renaming it back to the original .sch name recreates the channel.

## Schedule Files

Scheduled Channel creates/updates/deletes streams by creating/editing/deleting files with the .sch extension in the ScheduleFilesDir path. Schedule files (`.sch`) use the following XML format. When a `{Stream Name}.sch` file is created in ScheduleFilesDir, OvenMediaEngine analyzes the file and creates a Schedule Channel with `{Stream Name}`. If the contents of `{Stream Name}.sch` are changed, the Schedule Channel is updated, and if the file is deleted, the stream is deleted.

```xml
<?xml version="1.0" encoding="UTF-8"?>
<Schedule>
    <Stream>
        <Name>tv1</Name> <!-- optional, using filename without ext -->
        <BypassTranscoder>false</BypassTranscoder>
        <VideoTrack>true</VideoTrack>
        <AudioTrack>true</AudioTrack>
        <AudioMap> <!-- optional, only needed if you want to enable multilingual audio -->
            <Item>
                <Name>English</Name>
                <Language>en</Language>
            </Item>
            <Item>
                <Name>Korean</Name>
                <Language>ko</Language>
            </Item>
            <Item>
                <Name>Japanese</Name>
                <Language>ja</Language>
            </Item>
        </AudioMap>
        <MaxFallbackDurationMs>60000</MaxFallbackDurationMs> <!-- optional, default: 0 (unlimited) -->
    </Stream>

    <FallbackProgram>
        <Item url="file://sample.mp4" start="0" duration="60000" />
    </FallbackProgram>

    <Program name="1" scheduled="2023-09-27T13:21:15.123+09:00" repeat="true">
        <Item url="stream://default/app/stream1" duration="60000" />
    </Program>
    <Program name="2" scheduled="2022-03-14T15:10:0.0+09:00" repeat="true">
        <Item url="file://sample.mp4" start="0" duration="60000" />
        <Item url="stream://default/app/stream1" duration="60000" />
        <Item url="file://sample.mp4" start="60000" duration="120000" />
    </Program>
</Schedule>
```

`<Stream> (required)`\
This is the stream information that the Channel needs to create.

`<Stream>/<Name> (optional)`\
It's the stream's name. This is a reference value extracted from the file name for usage. It's recommended to set it same for consistency, although it's for reference purposes.

`<Stream>/<BypassTranscoder> (optional, default: false)`\
Set to true if transcoding is not desired.

`<Stream>/<VideoTrack> (optional, default: true)`\
Determines whether to use the video track. If `VideoTrack` is set to true and there's no video track in the Item, an error will occur.

`<Stream>/<AudioTrack> (optional, default: true)`\
Determines whether to use the audio track. If `AudioTrack` is set to true and there's no audio track in the Item, an error will occur.

`<Stream>/<AudioMap> (optional, default: false)`\
To enable multiple audio tracks (multilingual audio) in Scheduled Channel, enable `AudioMap`. It is important that all scheduled live sources and file sources provide audio tracks equal to or greater than the number of audio tracks defined in `AudioMap`. If you define 3 `AudioMaps`, but the file source or live source provides less than 3 audio tracks, the Program will generate an error. If you provide more audio tracks than the defined `AudioMaps`, they will be mapped in order and the rest will be ignored.

`<Stream>/<MaxFallbackDurationMs> (optional, default: 0)`\
Automatically deletes the channel when fallback lasts longer than this duration, in milliseconds. The result is the same as deleting the channel with the DELETE API, so the schedule file is also removed according to `<PreserveRemovedScheduleFile>`. The duration is measured while no scheduled program item is playing, which includes the time before the first program starts, and it starts over whenever a scheduled item plays again. Changing the value while fallback is running also restarts the count. If set to 0 or not set, the channel is never deleted automatically.

`<FallbackProgram> (optional)`\
It is a program that switches automatically when there is no program scheduled at the current time or an error occurs in an item. If the program is updated at the current time or the item returns to normal, it will fail back to the original program. Both files and live can be used for items in FallbackProgram. However, it is recommended to use a stable file.

`<Program> (optional)`\
Schedules a program. The `name` is an optional reference value. If not set, a random name will be assigned. Set the start time in ISO8601 format in the `scheduled` attribute. Decide whether to repeat the `Items` when its playback ends.

`<Program>/<Item> (optional)`\
Configures the media source to broadcast.

The `url` points to the location of the media source. If it starts with `file://`, it refers to a file within the `<MediaRootDir>` directory. If it starts with `stream://`, it refers to another stream within the same OvenMediaEngine. stream:// has the following format: `stream://{VHost Name}/{App Name}/{Stream Name}`

For 'file' cases, the `start` attribute can be set in milliseconds to indicate where in the file playback should start.\
`duration` indicates the playback time of that item in milliseconds. After the duration ends, it moves to the next item.\
Both 'start' and 'duration' are optional. If not set, `start` defaults to 0, and `duration` defaults to the file's duration; if not specified, the media file will be played until its full duration.

#### Supported Formats for File Live

<table><thead><tr><th width="290">Title</th><th>Description</th></tr></thead><tbody><tr><td>Formats</td><td><p>MP4, TS, MP3, and more.</p><ul><li>All formats supported by FFmpeg are supported for normal Scheduled Channel playback.</li></ul></td></tr></tbody></table>

## Segment Cache

Optional under `Providers/Schedule`. When enabled, eligible looping `file://` items can stop the demux media pump while still serving HLS and LLHLS from a sample index (segments are remuxed on demand from the MP4). WebRTC / OVT viewers keep the demux pump running.

:::warning H.264 + AAC MP4 only (per file)
SegmentCache does **not** transcode. It applies only to `file://` items that are **H.264** (+ **AAC** if `AudioTrack` is true) in **MP4/MOV**, and only when `BypassTranscoder=true` and `VideoTrack=true`. Other codecs/containers, or those stream settings, log a warning and **fall back to normal demux** — the schedule still loads. Invalid `SegmentCache` enums or out-of-range values fail Server.xml load.
:::

```xml
<!-- /Server/VirtualHosts/VirtualHost/Applications/Application/Providers -->
<Schedule>
    <MediaRootDir>/opt/ovenmediaengine/media</MediaRootDir>
    <ScheduleFilesDir>/opt/ovenmediaengine/media</ScheduleFilesDir>
    <SegmentCache>
        <Enable>true</Enable>
        <Mode>persist</Mode> <!-- persist | memory -->
        <Hydrate>
            <Mode>greedy</Mode> <!-- lazy | greedy -->
            <MaxThreads>2</MaxThreads>
            <MaxThroughputMbps>50</MaxThroughputMbps> <!-- 0 = unlimited -->
        </Hydrate>
        <IdleGracePeriodMs>30000</IdleGracePeriodMs>
        <!-- Classic HLS: list N segments past wall-clock playhead so players that
             join ~2–3 segments behind the edge land near LLHLS / schedule time.
             LLHLS is unchanged. 0 = default (edge at playhead). -->
        <HlsLookaheadSegments>3</HlsLookaheadSegments>
    </SegmentCache>
</Schedule>
```

`<SegmentCache> (optional)`\
Enables the segment cache for this application's Schedule Provider. Omit the whole element (or leave `Enable` false) to keep the classic always-on demux path.

`<SegmentCache>/<Enable> (optional, default: false)`\
Master switch. When `false`, SegmentCache is off for all streams unless a `.sch` file explicitly enables it (see below). When `true`, eligible `file://` items are indexed and can idle-serve HLS/LLHLS without demux.

`<SegmentCache>/<Mode> (optional, default: persist)`\
How the sample index is stored after the first build:

- `persist` — write a sidecar next to the media file (e.g. `clip.mp4.ome-segcache…`). Reuses the sidecar on later starts when the source and packager fingerprint still match.
- `memory` — keep the index in RAM only for this process lifetime; rebuild after every restart.

Invalid values fail Server.xml load.

`<SegmentCache>/<Hydrate> (optional)`\
Controls how segment bytes are pre-materialized into the hot cache after indexing.

`<SegmentCache>/<Hydrate>/<Mode> (optional, default: lazy)`\

- `lazy` — materialize a segment only when a client (or idle playlist sync) first needs it.
- `greedy` — after indexing, background-materialize the whole plan so later GETs hit memory.

Invalid values fail Server.xml load.

`<SegmentCache>/<Hydrate>/<MaxThreads> (optional, default: 1)`\
Worker threads used for greedy hydrate (and shared index/materialize throughput work). Allowed range: **1–64**. Out of range fails Server.xml load.

`<SegmentCache>/<Hydrate>/<MaxThroughputMbps> (optional, default: 0)`\
Soft cap on aggregate sample-index build + hydrate throughput across workers, in megabits per second. `0` means unlimited. Negative values fail Server.xml load.

`<SegmentCache>/<IdleGracePeriodMs> (optional, default: 30000)`\
When SegmentCache is active and there are **no** WebRTC/OVT sessions, HLS/LLHLS alone do not keep the demux pump running. If the pump is already running and demand drops to zero, OvenMediaEngine keeps pumping for this many milliseconds before entering idle cache playback (avoids thrashing on brief viewer gaps). `0` means leave idle as soon as demand is zero. Allowed range: **0–600000**. Out of range fails Server.xml load.

`<SegmentCache>/<HlsLookaheadSegments> (optional, default: 0)`\
Classic HLS only. When SegmentCache idle-serves a scheduled `file://` item, the media playlist normally ends at the wall-clock playhead. Classic HLS players typically start **2–3 segments behind** that edge, so they run ~10s+ behind LLHLS on the same schedule. Set this to the number of **future** segments to advertise past the playhead (media is already on disk). Players that still join near `edge − 3` then land near schedule / LLHLS time. `0` keeps the previous behavior. LLHLS chunklists stay edge-accurate (parts only up to now). Allowed range: **0–100**. Out of range fails Server.xml load. A value around **2–3** matches typical HLS join depth for common segment durations.

### Per-stream override in `.sch`

Omit `<Stream>/<SegmentCache>` to inherit Server.xml. To force on or off for one channel:

```xml
<Stream>
    <Name>_filler</Name>
    <BypassTranscoder>true</BypassTranscoder>
    <VideoTrack>true</VideoTrack>
    <AudioTrack>true</AudioTrack>
    <SegmentCache>
        <Enable>true</Enable>
    </SegmentCache>
</Stream>
```

`<SegmentCache>true</SegmentCache>` (boolean text) is also accepted. Mode, Hydrate, IdleGracePeriodMs, and HlsLookaheadSegments always come from Server.xml — the `.sch` override is enable/disable only.

`stream://` items in the same schedule are never cached; only matching `file://` items use the cache.

## Multiple Audio Track

The Scheduled Channel supports multiple audio tracks. This is automatically applied to the LLHLS Publisher. You can configure the `<AudioMap>` settings as follows to prepare multiple audio tracks in a Scheduled Channel.

```xml
<?xml version="1.0"?>
<Schedule>
    <Stream>
        <Name>today</Name>
        <BypassTranscoder>false</BypassTranscoder>
        <VideoTrack>true</VideoTrack>
        <AudioTrack>true</AudioTrack>
        <AudioMap>
            <Item>
                <Name>English</Name>
                <Language>en</Language> <!-- Optioanl, RFC 5646 -->
                <Characteristics>public.accessibility.describes-video</Characteristics> <!-- Optional -->
            </Item>
            <Item>
                <Name>Korean</Name>
                <Language>ko</Language> <!-- Optioanl, RFC 5646 -->
                <Characteristics>public.alternate</Characteristics> <!-- Optional -->
            </Item>
            <Item>
                <Name>Japanese</Name>
                <Language>ja</Language> <!-- Optioanl, RFC 5646 -->
                <Characteristics>public.alternate</Characteristics> <!-- Optional -->
            </Item>
        </AudioMap>
    </Stream>
</Schedule>
```


:::warning

A Scheduled Channel creates streams in advance and copies tracks from files or other streams. Therefore, all source content used in a Scheduled Channel with multiple audio tracks must provide at least the same number of audio tracks. Otherwise, the content will not be scheduled.

:::


## Application : Persistent Live Channel

This function is a scheduling channel, but it can be used for applications such as creating a permanent stream as follows.

```xml
<?xml version="1.0"?>
<Schedule>
    <Stream>
        <Name>stream</Name>
        <BypassTranscoder>false</BypassTranscoder>
        <VideoTrack>true</VideoTrack>
        <AudioTrack>true</AudioTrack>
    </Stream>
    <FallbackProgram>
        <Item url="file://hevc.mov" />
        <Item url="file://avc.mov" />
    </FallbackProgram>

    <Program name="origin" scheduled="2000-01-01T20:57:00.000+09" repeat="true">
        <Item url="stream://default/app/input" duration="-1" />
    </Program>
</Schedule>
```

This channel normally plays `default/app/input`, but when live input is stopped, it plays the file in `<FallbackProgram>`. This will last forever until the .sch file is deleted. One trick was to set the origin program's schedule time to year 2000 so that this stream would play unconditionally.

If the channel should end by itself when the live input stops for a long time, such as when the broadcast is over, set `<Stream>/<MaxFallbackDurationMs>`. The channel is then deleted automatically, together with its schedule file, once fallback lasts longer than the set duration.


:::warning

You may experience some buffering when going from file to live. This is unavoidable due to the nature of the function and low latency. If this is inconvenient, buffering issues can disappear if you add a little delay in advance by setting PartHoldBack in LLHLS to 5 or more. It is a choice between delay and buffering.

:::


## REST API

ScheduledChannel can also be controlled via API. Please refer to the page below.


[scheduledchannel-api.md](../rest-api/v1/virtualhost/application/scheduledchannel-api.md)

