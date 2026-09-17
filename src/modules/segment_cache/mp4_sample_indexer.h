//==============================================================================
//
//  OvenMediaEngine - Build SampleIndex by scanning an MP4 with FFmpeg
//
//==============================================================================
#pragma once

#include "sample_index.h"

namespace segment_cache
{
	// Result of a cheap compatibility probe (no full sample index).
	struct CompatibilityResult
	{
		bool ok = false;
		// Human-readable reason when ok == false (log at prepare; do not reject the schedule).
		ov::String error;
		bool has_h264_video = false;
		bool has_aac_audio = false;
	};

	class Mp4SampleIndexer
	{
	public:
		// Eligibility check for SegmentCache sources (H.264+AAC MP4/MOV). Does not build a sample index.
		// require_audio: when true (schedule AudioTrack=true), AAC audio is mandatory.
		static CompatibilityResult Probe(const ov::String &file_path, bool require_audio = true);

		// Scan the file once. Returns nullptr on failure.
		// max_throughput_mbps soft-caps demux read rate (0 = unlimited).
		static std::shared_ptr<SampleIndex> Build(const ov::String &file_path,
												  double max_throughput_mbps = 0.0);
	};
}  // namespace segment_cache
