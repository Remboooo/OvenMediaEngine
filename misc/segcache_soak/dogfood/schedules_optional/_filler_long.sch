<?xml version="1.0" encoding="UTF-8"?>
<!-- Long filler (trains4 ~2h). Use for idle-I/O / multi-hour soak, not wrap races. -->
<Schedule>
    <Stream>
	<Name>_filler</Name>
        <BypassTranscoder>true</BypassTranscoder>
        <VideoTrack>true</VideoTrack>
        <AudioTrack>true</AudioTrack>
    </Stream>
    <Program scheduled="2024-01-01T13:33:33.337+01:00" repeat="true">
        <Item url="file://trains4.mp4"/>
    </Program>
</Schedule>
