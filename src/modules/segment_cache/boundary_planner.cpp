//==============================================================================
//
//  OvenMediaEngine - Offline segment/part boundary plan from SampleIndex
//
//==============================================================================
#include "boundary_planner.h"

#include "mpegts_materializer.h"

#include <algorithm>

#define OV_LOG_TAG "SegmentCache.Boundary"

namespace segment_cache
{
	namespace
	{
		// Every LLHLS part must begin on a keyframe. Mid-GOP cuts are not
		// reorder-safe for B-frame content in the offline materializer (unlike
		// the live packager, which can wait for the reorder buffer to drain).
		void FillPartsForSegment(const TrackIndex &video,
								 PlannedSegment &segment,
								 uint32_t chunk_duration_ms)
		{
			if (chunk_duration_ms == 0 || segment.video_end <= segment.video_start)
			{
				PlannedPart part;
				part.start_dts = segment.start_dts;
				part.end_dts = segment.end_dts;
				part.video_start = segment.video_start;
				part.video_end = segment.video_end;
				segment.parts.push_back(part);
				return;
			}

			const int64_t chunk_ticks = static_cast<int64_t>(chunk_duration_ms) * 90;
			size_t part_start = segment.video_start;

			// Segment windows are already keyframe-aligned, but be defensive.
			while (part_start < segment.video_end && video.samples[part_start].keyframe == false)
			{
				part_start++;
			}

			while (part_start < segment.video_end)
			{
				const int64_t part_start_dts = video.samples[part_start].dts;
				const int64_t target_end = part_start_dts + chunk_ticks;

				// Advance at least to the chunk target, then to the next keyframe
				// (start of the following part) or the segment end.
				size_t part_end = part_start + 1;
				while (part_end < segment.video_end && video.samples[part_end].dts < target_end)
				{
					part_end++;
				}
				if (part_end < segment.video_end)
				{
					while (part_end < segment.video_end && video.samples[part_end].keyframe == false)
					{
						part_end++;
					}
				}

				if (part_end <= part_start)
				{
					part_end = std::min(part_start + 1, segment.video_end);
				}

				PlannedPart part;
				part.video_start = part_start;
				part.video_end = part_end;
				part.start_dts = part_start_dts;
				part.end_dts = (part_end < video.samples.size())
								   ? video.samples[part_end].dts
								   : (video.samples.back().dts + video.samples.back().duration);
				if (part_end >= segment.video_end)
				{
					part.end_dts = segment.end_dts;
					part.video_end = segment.video_end;
				}
				segment.parts.push_back(part);
				part_start = part_end;
			}
		}
	}  // namespace

	std::shared_ptr<BoundaryPlan> BoundaryPlanner::Build(const SampleIndex &index,
														 const PackagerFingerprint &fingerprint)
	{
		const auto *video = index.FindTrackByMediaType(cmn::MediaType::Video);
		if (video == nullptr || video->samples.empty() || fingerprint.target_duration_ms == 0)
		{
			return nullptr;
		}

		auto plan = std::make_shared<BoundaryPlan>();
		plan->fingerprint = fingerprint;

		int64_t cursor_dts = video->samples.front().dts;
		const int64_t last_dts = video->samples.back().dts;

		while (cursor_dts <= last_dts)
		{
			SegmentWindow window;
			if (ResolveSegmentWindow(index, cursor_dts, fingerprint.target_duration_ms, window) == false)
			{
				break;
			}

			PlannedSegment segment;
			segment.start_dts = window.start_dts;
			segment.end_dts = window.end_dts;
			segment.video_start = window.video_start;
			segment.video_end = window.video_end;

			const bool llhls_parts =
				(fingerprint.format_id == 2) || (fingerprint.chunk_duration_ms > 0);
			if (llhls_parts)
			{
				FillPartsForSegment(*video, segment, fingerprint.chunk_duration_ms);
			}

			plan->segments.push_back(std::move(segment));

			if (window.end_dts <= cursor_dts)
			{
				break;
			}
			cursor_dts = window.end_dts;
		}

		if (plan->segments.empty())
		{
			return nullptr;
		}

		logti("Boundary plan: %zu segments (target=%ums chunk=%ums format=%u)",
			  plan->segments.size(), fingerprint.target_duration_ms,
			  fingerprint.chunk_duration_ms, fingerprint.format_id);
		return plan;
	}
}  // namespace segment_cache
