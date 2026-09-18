<?xml version="1.0" encoding="UTF-8"?>
<!--
  Short-clip wrap soak schedule.

  One ~60s file, schedule duration = 24h so PlayFileIdle stays on the same
  SourceSession and IdlePlaylistDriver loop_count advances every minute.
  `scheduled` is rewritten to ~now by dogfood/stamp_schedule.sh on start.
-->
<Schedule>
    <Stream>
	<Name>_filler</Name>
        <BypassTranscoder>true</BypassTranscoder>
        <VideoTrack>true</VideoTrack>
        <AudioTrack>true</AudioTrack>
    </Stream>
    <Program scheduled="2026-09-18T11:33:29.000+02:00" repeat="true">
        <Item url="file://short_a.mp4" duration="86400000"/>
    </Program>
</Schedule>
