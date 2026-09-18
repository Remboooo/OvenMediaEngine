<?xml version="1.0" encoding="UTF-8"?>
<!--
  Multi-clip variant: three shorts, 60s schedule slots (= file length).
  Exercises item transitions (cache-serve off/on) more than in-session wrap.
  Copy over schedules/_filler.sch and restart to use.
-->
<Schedule>
    <Stream>
	<Name>_filler</Name>
        <BypassTranscoder>true</BypassTranscoder>
        <VideoTrack>true</VideoTrack>
        <AudioTrack>true</AudioTrack>
    </Stream>
    <Program scheduled="2026-09-18T11:18:00.000+02:00" repeat="true">
        <Item url="file://short_a.mp4" duration="60000"/>
        <Item url="file://short_b.mp4" duration="60000"/>
        <Item url="file://short_c.mp4" duration="55000"/>
    </Program>
</Schedule>
