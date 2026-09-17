//==============================================================================
//
//  OvenMediaEngine - Convert MP4 sample payloads to MPEG-TS-friendly bitstreams
//
//==============================================================================
#pragma once

#include <base/info/media_track.h>
#include <base/mediarouter/media_buffer.h>

namespace segment_cache
{
	// AVCC/HVCC/AAC_RAW (as demuxed from MP4) -> Annex-B / ADTS for mpegts::Packetizer.
	// On H.264 IDR, prepends SPS/PPS from the track DCR when missing from the sample.
	std::shared_ptr<MediaPacket> ConvertSampleForMpegTs(
		const std::shared_ptr<const MediaTrack> &track,
		uint32_t track_id,
		const std::shared_ptr<const ov::Data> &avcc_or_raw_payload,
		int64_t pts,
		int64_t dts,
		int64_t duration,
		bool keyframe);
}  // namespace segment_cache
