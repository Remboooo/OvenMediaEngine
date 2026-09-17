//==============================================================================
//
//  fMP4 materialize tests
//
//==============================================================================
#include <gtest/gtest.h>

#include "boundary_planner.h"
#include "fmp4_materializer.h"
#include "mp4_sample_indexer.h"
#include "sample_index_sidecar.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
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

	bool HasBox(const std::shared_ptr<const ov::Data> &data, const char fourcc[4])
	{
		if (data == nullptr || data->GetLength() < 8)
		{
			return false;
		}
		const auto *p = data->GetDataAs<uint8_t>();
		const size_t n = data->GetLength();
		for (size_t i = 0; i + 8 <= n; i++)
		{
			if (std::memcmp(p + i + 4, fourcc, 4) == 0)
			{
				return true;
			}
		}
		return false;
	}
}  // namespace

TEST(SegmentCacheFmp4Materialize, InitAndOneSegment)
{
	ov::String fixture = "/tmp/ome_segcache_fixture.mp4";
	ASSERT_TRUE(EnsureTinyFixture(fixture));

	auto index = segment_cache::Mp4SampleIndexer::Build(fixture);
	ASSERT_NE(index, nullptr);

	const auto *video = index->FindTrackByMediaType(cmn::MediaType::Video);
	const auto *audio = index->FindTrackByMediaType(cmn::MediaType::Audio);
	ASSERT_NE(video, nullptr);
	ASSERT_NE(audio, nullptr);

	auto v_init = segment_cache::Fmp4Materializer::MaterializeInit(video->media_track);
	ASSERT_NE(v_init, nullptr);
	EXPECT_TRUE(HasBox(v_init, "ftyp"));
	EXPECT_TRUE(HasBox(v_init, "moov"));
	EXPECT_GT(v_init->GetLength(), 32u);

	auto a_init = segment_cache::Fmp4Materializer::MaterializeInit(audio->media_track);
	ASSERT_NE(a_init, nullptr);
	EXPECT_TRUE(HasBox(a_init, "ftyp"));
	EXPECT_TRUE(HasBox(a_init, "moov"));

	auto v_seg = segment_cache::Fmp4Materializer::Materialize(index, cmn::MediaType::Video, 0, 1000);
	ASSERT_NE(v_seg, nullptr);
	ASSERT_NE(v_seg->data, nullptr);
	EXPECT_TRUE(HasBox(v_seg->data, "moof"));
	EXPECT_TRUE(HasBox(v_seg->data, "mdat"));
	EXPECT_GE(v_seg->sample_count, 1u);
	EXPECT_GT(v_seg->bytes_read, 0u);
	EXPECT_NEAR(v_seg->duration_ms, 1000.0, 50.0);

	auto a_seg = segment_cache::Fmp4Materializer::Materialize(index, cmn::MediaType::Audio, 0, 1000);
	ASSERT_NE(a_seg, nullptr);
	ASSERT_NE(a_seg->data, nullptr);
	EXPECT_TRUE(HasBox(a_seg->data, "moof"));
	EXPECT_TRUE(HasBox(a_seg->data, "mdat"));
	EXPECT_GE(a_seg->sample_count, 1u);

	// Deterministic rebuild
	auto v_seg2 = segment_cache::Fmp4Materializer::Materialize(index, cmn::MediaType::Video, 0, 1000);
	ASSERT_NE(v_seg2, nullptr);
	ASSERT_EQ(v_seg2->data->GetLength(), v_seg->data->GetLength());
	EXPECT_EQ(std::memcmp(v_seg2->data->GetDataAs<uint8_t>(), v_seg->data->GetDataAs<uint8_t>(),
						  v_seg->data->GetLength()),
			  0);
}

TEST(SegmentCacheFmp4Materialize, SecondSegmentAdvances)
{
	ov::String fixture = "/tmp/ome_segcache_fixture.mp4";
	ASSERT_TRUE(EnsureTinyFixture(fixture));

	auto index = segment_cache::Mp4SampleIndexer::Build(fixture);
	ASSERT_NE(index, nullptr);

	auto first = segment_cache::Fmp4Materializer::Materialize(index, cmn::MediaType::Video, 0, 1000);
	ASSERT_NE(first, nullptr);

	auto second = segment_cache::Fmp4Materializer::Materialize(
		index, cmn::MediaType::Video, first->end_dts, 1000);
	ASSERT_NE(second, nullptr);
	EXPECT_GE(second->first_dts, first->end_dts);
	EXPECT_TRUE(HasBox(second->data, "moof"));
	EXPECT_TRUE(HasBox(second->data, "mdat"));
}

TEST(SegmentCacheFmp4Materialize, MaterializePartRange)
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
	ASSERT_GE(plan->segments.front().parts.size(), 1u);

	const auto &part = plan->segments.front().parts.front();
	auto result = segment_cache::Fmp4Materializer::MaterializeSampleRange(
		index, cmn::MediaType::Video, part.video_start, part.video_end);
	ASSERT_NE(result, nullptr);
	EXPECT_TRUE(HasBox(result->data, "moof"));
	EXPECT_TRUE(HasBox(result->data, "mdat"));
	EXPECT_EQ(result->sample_count, part.video_end - part.video_start);
}
