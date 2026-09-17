//==============================================================================
//
//  OvenMediaEngine - Offline segment/part boundary plan from SampleIndex
//
//==============================================================================
#pragma once

#include "sample_index.h"
#include "sample_index_sidecar.h"

#include <base/ovlibrary/ovlibrary.h>

#include <vector>

namespace segment_cache
{
	struct PlannedPart
	{
		int64_t start_dts = 0;
		int64_t end_dts = 0;
		size_t video_start = 0;  // inclusive sample index
		size_t video_end = 0;	 // exclusive
	};

	struct PlannedSegment
	{
		int64_t start_dts = 0;
		int64_t end_dts = 0;
		size_t video_start = 0;
		size_t video_end = 0;
		// Empty for MPEG-TS (format_id=1). For LLHLS fMP4, one or more parts.
		std::vector<PlannedPart> parts;
	};

	struct BoundaryPlan
	{
		PackagerFingerprint fingerprint;
		std::vector<PlannedSegment> segments;
	};

	class BoundaryPlanner
	{
	public:
		// Plan one full pass of the source (keyframe-aligned segments).
		// LLHLS (format_id=2) with chunk_duration_ms > 0 also fills parts.
		static std::shared_ptr<BoundaryPlan> Build(const SampleIndex &index,
												   const PackagerFingerprint &fingerprint);
	};
}  // namespace segment_cache
