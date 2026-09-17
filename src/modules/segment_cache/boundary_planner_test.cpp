//==============================================================================
//
//  Boundary planner tests
//
//==============================================================================
#include <gtest/gtest.h>

#include "boundary_planner.h"
#include "mp4_sample_indexer.h"

#include <cstdlib>
#include <unistd.h>

namespace
{
	bool EnsureTinyFixture(const ov::String &path)
	{
		if (::access(path.CStr(), R_OK) == 0)
		{
			return true;
		}
		ov::String cmd = ov::String::FormatString(
			"ffmpeg -y -hide_banner -loglevel error "
			"-f lavfi -i testsrc=size=320x240:rate=30 "
			"-f lavfi -i sine=frequency=1000:sample_rate=48000 "
			"-t 4 -c:v libx264 -pix_fmt yuv420p -profile:v baseline "
			"-x264-params keyint=30:min-keyint=30:scenecut=0 "
			"-c:a aac -b:a 64k -movflags +faststart %s",
			path.CStr());
		return std::system(cmd.CStr()) == 0 && ::access(path.CStr(), R_OK) == 0;
	}
}  // namespace

TEST(SegmentCacheBoundary, PlansKeyframeAlignedSegments)
{
	ov::String fixture = "/tmp/ome_segcache_fixture.mp4";
	ASSERT_TRUE(EnsureTinyFixture(fixture));

	auto index = segment_cache::Mp4SampleIndexer::Build(fixture);
	ASSERT_NE(index, nullptr);

	segment_cache::PackagerFingerprint fp;
	fp.format_id = 1;
	fp.target_duration_ms = 1000;

	auto plan = segment_cache::BoundaryPlanner::Build(*index, fp);
	ASSERT_NE(plan, nullptr);
	ASSERT_GE(plan->segments.size(), 2u);

	// Segments abut and cover increasing DTS.
	for (size_t i = 0; i < plan->segments.size(); i++)
	{
		const auto &seg = plan->segments[i];
		EXPECT_LT(seg.start_dts, seg.end_dts);
		EXPECT_LT(seg.video_start, seg.video_end);
		EXPECT_TRUE(seg.parts.empty());  // TS: no parts
		if (i > 0)
		{
			EXPECT_EQ(seg.start_dts, plan->segments[i - 1].end_dts);
		}
	}
}

TEST(SegmentCacheBoundary, LlhlsFillsParts)
{
	ov::String fixture = "/tmp/ome_segcache_fixture.mp4";
	ASSERT_TRUE(EnsureTinyFixture(fixture));

	auto index = segment_cache::Mp4SampleIndexer::Build(fixture);
	ASSERT_NE(index, nullptr);

	segment_cache::PackagerFingerprint fp;
	fp.format_id = 2;
	fp.target_duration_ms = 1000;
	fp.chunk_duration_ms = 500;

	auto plan = segment_cache::BoundaryPlanner::Build(*index, fp);
	ASSERT_NE(plan, nullptr);
	ASSERT_FALSE(plan->segments.empty());

	const auto &seg = plan->segments.front();
	ASSERT_GE(seg.parts.size(), 1u);

	// Parts cover the segment without gaps.
	EXPECT_EQ(seg.parts.front().start_dts, seg.start_dts);
	EXPECT_EQ(seg.parts.back().end_dts, seg.end_dts);
	EXPECT_EQ(seg.parts.front().video_start, seg.video_start);
	EXPECT_EQ(seg.parts.back().video_end, seg.video_end);

	for (size_t i = 1; i < seg.parts.size(); i++)
	{
		EXPECT_EQ(seg.parts[i].start_dts, seg.parts[i - 1].end_dts);
		EXPECT_EQ(seg.parts[i].video_start, seg.parts[i - 1].video_end);
	}

	// Every part (except possibly a trailing remainder past the last keyframe
	// inside the segment) must start on a keyframe.
	const auto *video = index->FindTrackByMediaType(cmn::MediaType::Video);
	ASSERT_NE(video, nullptr);
	for (const auto &part : seg.parts)
	{
		ASSERT_LT(part.video_start, video->samples.size());
		EXPECT_TRUE(video->samples[part.video_start].keyframe)
			<< "part starting at sample " << part.video_start << " is not a keyframe";
	}
}
