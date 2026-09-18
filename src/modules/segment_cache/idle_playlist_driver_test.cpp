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

	// After one full loop media sequence continues; window bridges the wrap
	// so the oldest entry is still from the previous loop when window>1.
	driver.SetEpochElapsedMs(item_ms + 100);
	EXPECT_EQ(driver.GetPlayhead().loop_count, 1u);
	EXPECT_EQ(driver.GetPlayhead().segment_ordinal, 0u);
	ASSERT_EQ(driver.GetWindow().size(), cfg.window_segments);
	EXPECT_EQ(driver.GetWindow().back().media_sequence,
			  static_cast<int64_t>(session->GetPlan().segments.size()));
	EXPECT_TRUE(driver.GetWindow().back().discontinuity);
	EXPECT_EQ(driver.GetMediaSequenceStart(),
			  static_cast<int64_t>(session->GetPlan().segments.size()) -
				  static_cast<int64_t>(cfg.window_segments) + 1);
	EXPECT_EQ(driver.GetWindow().front().media_sequence, driver.GetMediaSequenceStart());
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

TEST(SegmentCacheIdlePlaylist, WrapDiscontinuitySequenceBefore)
{
	EXPECT_EQ(segment_cache::WrapDiscontinuitySequenceBefore(0, 10), 0);
	EXPECT_EQ(segment_cache::WrapDiscontinuitySequenceBefore(9, 10), 0);
	// First listed segment is the wrap discontinuity itself → DISC-SEQ 0
	EXPECT_EQ(segment_cache::WrapDiscontinuitySequenceBefore(10, 10), 0);
	// Wrap disc at msn=10 has scrolled out
	EXPECT_EQ(segment_cache::WrapDiscontinuitySequenceBefore(11, 10), 1);
	EXPECT_EQ(segment_cache::WrapDiscontinuitySequenceBefore(20, 10), 1);
	EXPECT_EQ(segment_cache::WrapDiscontinuitySequenceBefore(21, 10), 2);
}

TEST(SegmentCacheIdlePlaylist, WindowBridgesLoopWrap)
{
	ov::String fixture = "/tmp/ome_segcache_fixture.mp4";
	ASSERT_TRUE(EnsureTinyFixture(fixture));

	segment_cache::PackagerFingerprint fp;
	fp.format_id = 2;
	fp.target_duration_ms = 1000;
	fp.chunk_duration_ms = 500;

	auto session = segment_cache::SourceSession::Open(fixture, fp);
	ASSERT_NE(session, nullptr);
	const size_t plan_size = session->GetPlan().segments.size();
	ASSERT_GE(plan_size, 2u);

	segment_cache::IdlePlaylistDriver::Config cfg;
	cfg.window_segments = std::min<size_t>(4, plan_size);
	segment_cache::IdlePlaylistDriver driver(session, cfg);

	const int64_t item_ms = session->GetItemDurationMs();
	ASSERT_GT(item_ms, 0);

	// Just after the first wrap: playhead on ordinal 0 of loop 1.
	driver.SetEpochElapsedMs(item_ms + 50);
	EXPECT_EQ(driver.GetPlayhead().loop_count, 1u);
	EXPECT_EQ(driver.GetPlayhead().segment_ordinal, 0u);

	const auto &window = driver.GetWindow();
	ASSERT_FALSE(window.empty());
	// Must not collapse to a single segment — previous loop tail is included.
	EXPECT_EQ(window.size(), cfg.window_segments);
	EXPECT_EQ(window.back().media_sequence, static_cast<int64_t>(plan_size));
	EXPECT_TRUE(window.back().discontinuity);
	// Oldest is still from loop 0
	EXPECT_LT(window.front().media_sequence, static_cast<int64_t>(plan_size));
	EXPECT_FALSE(window.front().discontinuity);

	// After the wrap segment scrolls out of a size-1 view of "first only"...
	// With full window, DISC-SEQ before first should stay 0 while wrap is listed.
	EXPECT_EQ(segment_cache::WrapDiscontinuitySequenceBefore(window.front().media_sequence, plan_size), 0);

	// Advance far enough that the wrap disc (msn=plan_size) leaves a window of `want`.
	// End at ordinal want-1 of loop 1 → first msn = plan_size + 0? 
	// end = plan_size + (want-1), first = plan_size + (want-1) - (want-1) = plan_size
	// Still includes disc. Need end ordinal such that first > plan_size.
	// first = end_msn - want + 1 > plan_size → end_msn > plan_size + want - 1
	// end_msn = plan_size + ord → ord > want - 1, so ord >= want
	if (plan_size > cfg.window_segments)
	{
		// Seek near end of second loop's early segments past the wrap.
		const size_t target_ord = cfg.window_segments;  // first msn = plan_size + 1
		const auto &seg = session->GetPlan().segments[target_ord];
		const int64_t origin = session->GetPlan().segments.front().start_dts;
		const int64_t pos_ms = (seg.start_dts - origin) / 90 + 10;
		driver.SetEpochElapsedMs(item_ms + pos_ms);
		ASSERT_GE(driver.GetPlayhead().segment_ordinal, target_ord);

		const auto &w2 = driver.GetWindow();
		ASSERT_FALSE(w2.empty());
		EXPECT_GT(w2.front().media_sequence, static_cast<int64_t>(plan_size));
		EXPECT_EQ(segment_cache::WrapDiscontinuitySequenceBefore(w2.front().media_sequence, plan_size), 1);
		// Wrap disc no longer listed
		for (const auto &e : w2)
		{
			EXPECT_FALSE(e.discontinuity) << "msn=" << e.media_sequence;
		}
	}
}

TEST(SegmentCacheIdlePlaylist, LookaheadExtendsPastPlayhead)
{
	ov::String fixture = "/tmp/ome_segcache_fixture.mp4";
	ASSERT_TRUE(EnsureTinyFixture(fixture));

	segment_cache::PackagerFingerprint fp;
	fp.format_id = 2;
	fp.target_duration_ms = 1000;
	fp.chunk_duration_ms = 500;

	auto session = segment_cache::SourceSession::Open(fixture, fp);
	ASSERT_NE(session, nullptr);
	const size_t plan_size = session->GetPlan().segments.size();
	ASSERT_GE(plan_size, 3u);

	segment_cache::IdlePlaylistDriver::Config cfg;
	cfg.window_segments = 2;
	cfg.lookahead_segments = 2;
	segment_cache::IdlePlaylistDriver driver(session, cfg);

	driver.SetEpochElapsedMs(0);
	const auto &window = driver.GetWindow();
	ASSERT_EQ(window.size(), 3u);  // playhead (ord 0) + 2 lookahead; no history yet
	EXPECT_EQ(window.front().plan_ordinal, 0u);
	EXPECT_EQ(window.front().media_sequence, 0);
	EXPECT_EQ(driver.GetPlayhead().segment_ordinal, 0u);
	EXPECT_EQ(window.back().plan_ordinal, 2u);
	EXPECT_EQ(window.back().media_sequence, 2);
	// Playhead segment is still listed before lookahead tip
	EXPECT_EQ(window[0].media_sequence, driver.MediaSequenceForOrdinal(0));
}

TEST(SegmentCacheIdlePlaylist, LookaheadWrapsIntoNextLoop)
{
	ov::String fixture = "/tmp/ome_segcache_fixture.mp4";
	ASSERT_TRUE(EnsureTinyFixture(fixture));

	segment_cache::PackagerFingerprint fp;
	fp.format_id = 2;
	fp.target_duration_ms = 1000;
	fp.chunk_duration_ms = 500;

	auto session = segment_cache::SourceSession::Open(fixture, fp);
	ASSERT_NE(session, nullptr);
	const size_t plan_size = session->GetPlan().segments.size();
	ASSERT_GE(plan_size, 2u);

	segment_cache::IdlePlaylistDriver::Config cfg;
	cfg.window_segments = 1;
	cfg.lookahead_segments = 1;
	segment_cache::IdlePlaylistDriver driver(session, cfg);

	// Park on the last segment of loop 0 so lookahead crosses the wrap.
	const auto &last = session->GetPlan().segments.back();
	const int64_t origin = session->GetPlan().segments.front().start_dts;
	const int64_t pos_ms = (last.start_dts - origin) / 90 + 10;
	driver.SetEpochElapsedMs(pos_ms);
	EXPECT_EQ(driver.GetPlayhead().loop_count, 0u);
	EXPECT_EQ(driver.GetPlayhead().segment_ordinal, plan_size - 1);

	const auto &window = driver.GetWindow();
	ASSERT_EQ(window.size(), 2u);
	EXPECT_EQ(window.front().media_sequence, static_cast<int64_t>(plan_size - 1));
	EXPECT_EQ(window.back().media_sequence, static_cast<int64_t>(plan_size));
	EXPECT_EQ(window.back().plan_ordinal, 0u);
	EXPECT_TRUE(window.back().discontinuity);
}
