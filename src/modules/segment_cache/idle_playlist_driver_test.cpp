//==============================================================================
//
//  Idle playlist driver tests
//
//==============================================================================
#include <gtest/gtest.h>

#include "idle_playlist_driver.h"
#include "source_session.h"

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

TEST(SegmentCacheIdlePlaylist, WindowAdvancesWithElapsedTime)
{
	ov::String fixture = "/tmp/ome_segcache_fixture.mp4";
	ASSERT_TRUE(EnsureTinyFixture(fixture));

	segment_cache::PackagerFingerprint fp;
	fp.format_id = 2;
	fp.target_duration_ms = 1000;
	fp.chunk_duration_ms = 500;

	auto session = segment_cache::SourceSession::Open(fixture, fp);
	ASSERT_NE(session, nullptr);
	ASSERT_GE(session->GetPlan().segments.size(), 2u);

	segment_cache::IdlePlaylistDriver::Config cfg;
	cfg.window_segments = 2;
	segment_cache::IdlePlaylistDriver driver(session, cfg);

	driver.SetEpochElapsedMs(0);
	ASSERT_FALSE(driver.GetWindow().empty());
	EXPECT_EQ(driver.GetWindow().back().plan_ordinal, 0u);
	EXPECT_EQ(driver.GetMediaSequenceStart(), 0);

	const int64_t item_ms = session->GetItemDurationMs();
	ASSERT_GT(item_ms, 1000);

	// Mid first loop — playhead should leave segment 0.
	driver.SetEpochElapsedMs(1500);
	EXPECT_GE(driver.GetPlayhead().segment_ordinal, 1u);
	EXPECT_EQ(driver.GetPlayhead().loop_count, 0u);
	EXPECT_LE(driver.GetWindow().size(), cfg.window_segments);
	EXPECT_EQ(driver.GetWindow().back().plan_ordinal, driver.GetPlayhead().segment_ordinal);

	// After one full loop media sequence continues.
	driver.SetEpochElapsedMs(item_ms + 100);
	EXPECT_EQ(driver.GetPlayhead().loop_count, 1u);
	EXPECT_EQ(driver.GetPlayhead().segment_ordinal, 0u);
	EXPECT_EQ(driver.GetMediaSequenceStart(),
			  static_cast<int64_t>(session->GetPlan().segments.size()));
	EXPECT_EQ(driver.GetWindow().front().media_sequence,
			  static_cast<int64_t>(session->GetPlan().segments.size()));
}

TEST(SegmentCacheIdlePlaylist, PartsCountedOnLlhlsPlan)
{
	ov::String fixture = "/tmp/ome_segcache_fixture.mp4";
	ASSERT_TRUE(EnsureTinyFixture(fixture));

	segment_cache::PackagerFingerprint fp;
	fp.format_id = 2;
	fp.target_duration_ms = 1000;
	fp.chunk_duration_ms = 500;

	auto session = segment_cache::SourceSession::Open(fixture, fp);
	ASSERT_NE(session, nullptr);

	segment_cache::IdlePlaylistDriver driver(session, {});
	driver.SetEpochElapsedMs(0);
	ASSERT_FALSE(driver.GetWindow().empty());
	EXPECT_GE(driver.GetWindow().front().part_count, 1u);
}
