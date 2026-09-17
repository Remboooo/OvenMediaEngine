//==============================================================================
//
//  OvenMediaEngine - Materialize one MPEG-TS HLS segment from a SampleIndex
//
//==============================================================================
#pragma once

#include "sample_index.h"

#include <base/ovlibrary/ovlibrary.h>

#include <functional>

namespace segment_cache
{
	struct MpegTsMaterializeResult
	{
		std::shared_ptr<ov::Data> data;
		double duration_ms = 0;
		int64_t first_dts = 0;
		int64_t end_dts = 0;
		size_t video_samples = 0;
		size_t audio_samples = 0;
		uint64_t bytes_read = 0;  // compressed sample payload bytes pulled from source
		double cpu_ms = 0;
	};

	struct SegmentWindow
	{
		size_t video_start = 0;
		size_t video_end = 0;  // exclusive
		int64_t start_dts = 0;
		int64_t end_dts = 0;
	};

	// Schedule-style join: after full_loops of the item, plus offset_ms into the item,
	// wrapped with modulo (same idea as GetFirstItemWithPosition).
	int64_t JoinDtsAfterLoops(int64_t item_duration_ms, uint32_t full_loops, int64_t offset_ms);

	bool ResolveSegmentWindow(const SampleIndex &index, int64_t start_dts_90k, uint32_t target_duration_ms, SegmentWindow &out);

	class MpegTsMaterializer
	{
	public:
		using PayloadReader = std::function<std::shared_ptr<ov::Data>(const TrackIndex &track, const SampleRef &sample)>;

		// Build a single muxed TS segment by reading sample bytes via file offsets (segment cache path).
		static std::shared_ptr<MpegTsMaterializeResult> Materialize(
			const std::shared_ptr<const SampleIndex> &index,
			int64_t start_dts_90k,
			uint32_t target_duration_ms);

		// Same window and packager path, but sample payloads come from a live demux of the
		// source (no-cache path). Used to prove byte-identical output.
		static std::shared_ptr<MpegTsMaterializeResult> MaterializeFromDemux(
			const std::shared_ptr<const SampleIndex> &index,
			int64_t start_dts_90k,
			uint32_t target_duration_ms);

		static std::shared_ptr<MpegTsMaterializeResult> MaterializeWithReader(
			const std::shared_ptr<const SampleIndex> &index,
			int64_t start_dts_90k,
			uint32_t target_duration_ms,
			const PayloadReader &reader);
	};
}  // namespace segment_cache
