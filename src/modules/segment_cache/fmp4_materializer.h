//==============================================================================
//
//  OvenMediaEngine - Materialize LLHLS fMP4 init/segment from a SampleIndex
//
//==============================================================================
#pragma once

#include "mpegts_materializer.h"
#include "sample_index.h"

#include <base/ovlibrary/ovlibrary.h>

#include <functional>

namespace segment_cache
{
	struct Fmp4MaterializeResult
	{
		std::shared_ptr<ov::Data> data;
		double duration_ms = 0;
		int64_t first_dts = 0;
		int64_t end_dts = 0;
		size_t sample_count = 0;
		uint64_t bytes_read = 0;
		double cpu_ms = 0;
	};

	// Per-track fMP4 writer for LLHLS (unmuxed video or audio).
	class Fmp4Materializer
	{
	public:
		using PayloadReader = MpegTsMaterializer::PayloadReader;

		// ftyp + moov for one indexed track (H264 or AAC).
		static std::shared_ptr<ov::Data> MaterializeInit(const std::shared_ptr<const MediaTrack> &media_track);

		// One moof+mdat media segment for the track covering [start_dts, start+target).
		// Video uses GOP-aligned ResolveSegmentWindow; audio uses DTS overlap of that window.
		static std::shared_ptr<Fmp4MaterializeResult> Materialize(
			const std::shared_ptr<const SampleIndex> &index,
			cmn::MediaType media_type,
			int64_t start_dts_90k,
			uint32_t target_duration_ms);

		static std::shared_ptr<Fmp4MaterializeResult> MaterializeWithReader(
			const std::shared_ptr<const SampleIndex> &index,
			cmn::MediaType media_type,
			int64_t start_dts_90k,
			uint32_t target_duration_ms,
			const PayloadReader &reader);

		// One moof+mdat for an explicit video sample range [video_begin, video_end).
		// Audio uses DTS overlap of that range. Used for LLHLS parts.
		static std::shared_ptr<Fmp4MaterializeResult> MaterializeSampleRange(
			const std::shared_ptr<const SampleIndex> &index,
			cmn::MediaType media_type,
			size_t video_begin,
			size_t video_end);

		static std::shared_ptr<Fmp4MaterializeResult> MaterializeSampleRangeWithReader(
			const std::shared_ptr<const SampleIndex> &index,
			cmn::MediaType media_type,
			size_t video_begin,
			size_t video_end,
			const PayloadReader &reader);
	};
}  // namespace segment_cache
