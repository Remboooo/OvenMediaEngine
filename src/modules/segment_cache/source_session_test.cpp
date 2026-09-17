//==============================================================================
//
//  SourceSession + idle playhead + registry tests
//
//==============================================================================
#include <gtest/gtest.h>

#include "source_session.h"

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
		for (size_t i = 0; i + 8 <= data->GetLength(); i++)
		{
			if (std::memcmp(p + i + 4, fourcc, 4) == 0)
			{
				return true;
			}
		}
		return false;
	}
}  // namespace

TEST(SegmentCacheSourceSession, ServesHlsAndFmp4WithHotHit)
{
	ov::String fixture = "/tmp/ome_segcache_fixture.mp4";
	ASSERT_TRUE(EnsureTinyFixture(fixture));

	segment_cache::PackagerFingerprint fp;
	fp.format_id = 1;
	fp.target_duration_ms = 1000;

	auto session = segment_cache::SourceSession::Open(fixture, fp);
	ASSERT_NE(session, nullptr);
	ASSERT_GE(session->GetPlan().segments.size(), 2u);

	auto ts0 = session->GetHlsTsSegment(0);
	ASSERT_NE(ts0, nullptr);
	EXPECT_GE(ts0->GetLength(), 188u);
	EXPECT_EQ(session->GetHotCache().GetHitCount(), 0u);

	auto ts0_hit = session->GetHlsTsSegment(0);
	ASSERT_NE(ts0_hit, nullptr);
	EXPECT_EQ(ts0_hit->GetLength(), ts0->GetLength());
	EXPECT_EQ(session->GetHotCache().GetHitCount(), 1u);

	fp.format_id = 2;
	fp.chunk_duration_ms = 500;
	auto llhls = segment_cache::SourceSession::Open(fixture, fp);
	ASSERT_NE(llhls, nullptr);

	auto init = llhls->GetFmp4Init(cmn::MediaType::Video);
	ASSERT_NE(init, nullptr);
	EXPECT_TRUE(HasBox(init, "moov"));

	auto seg = llhls->GetFmp4Segment(cmn::MediaType::Video, 0);
	ASSERT_NE(seg, nullptr);
	EXPECT_TRUE(HasBox(seg, "moof"));

	ASSERT_FALSE(llhls->GetPlan().segments.front().parts.empty());
	auto part = llhls->GetFmp4Part(cmn::MediaType::Video, 0, 0);
	ASSERT_NE(part, nullptr);
	EXPECT_TRUE(HasBox(part, "moof"));
	EXPECT_LE(part->GetLength(), seg->GetLength());
}

TEST(SegmentCacheSourceSession, IdlePlayheadLoops)
{
	ov::String fixture = "/tmp/ome_segcache_fixture.mp4";
	ASSERT_TRUE(EnsureTinyFixture(fixture));

	segment_cache::PackagerFingerprint fp;
	fp.format_id = 2;
	fp.target_duration_ms = 1000;
	fp.chunk_duration_ms = 500;

	auto session = segment_cache::SourceSession::Open(fixture, fp);
	ASSERT_NE(session, nullptr);

	const int64_t item_ms = session->GetItemDurationMs();
	ASSERT_GT(item_ms, 0);

	auto head0 = session->ResolvePlayhead(0);
	EXPECT_EQ(head0.segment_ordinal, 0u);
	EXPECT_EQ(head0.loop_count, 0u);

	auto mid = session->ResolvePlayhead(item_ms / 2);
	EXPECT_EQ(mid.loop_count, 0u);
	EXPECT_GE(mid.segment_ordinal, 0u);

	auto looped = session->ResolvePlayhead(item_ms + 100);
	EXPECT_EQ(looped.loop_count, 1u);
	EXPECT_EQ(looped.segment_ordinal, 0u);
}

TEST(SegmentCacheSourceSession, GreedyHydrateFillsHotCache)
{
	ov::String fixture = "/tmp/ome_segcache_fixture.mp4";
	ASSERT_TRUE(EnsureTinyFixture(fixture));

	segment_cache::PackagerFingerprint fp;
	fp.format_id = 1;
	fp.target_duration_ms = 1000;

	auto session = segment_cache::SourceSession::Open(fixture, fp);
	ASSERT_NE(session, nullptr);
	ASSERT_GE(session->GetPlan().segments.size(), 1u);

	segment_cache::HydrateOptions hydrate;
	hydrate.mode = segment_cache::HydrateMode::Greedy;
	hydrate.max_threads = 2;
	hydrate.max_throughput_mbps = 0;

	const size_t hydrated = session->HydrateAll(hydrate);
	EXPECT_EQ(hydrated, session->GetPlan().segments.size());
	EXPECT_TRUE(session->IsHydrateDone());

	const auto misses_after_hydrate = session->GetHotCache().GetMissCount();
	const auto hits_before = session->GetHotCache().GetHitCount();

	auto ts0 = session->GetHlsTsSegment(0);
	ASSERT_NE(ts0, nullptr);
	EXPECT_EQ(session->GetHotCache().GetMissCount(), misses_after_hydrate);
	EXPECT_GT(session->GetHotCache().GetHitCount(), hits_before);
}

TEST(SegmentCacheSourceSession, RegistryGating)
{
	segment_cache::SessionRegistry::GetInstance().Clear();
	::unsetenv("OME_SEGMENT_CACHE");
	EXPECT_FALSE(segment_cache::SessionRegistry::IsEnvEnabled());

	::setenv("OME_SEGMENT_CACHE", "1", 1);
	EXPECT_TRUE(segment_cache::SessionRegistry::IsEnvEnabled());

	ov::String fixture = "/tmp/ome_segcache_fixture.mp4";
	ASSERT_TRUE(EnsureTinyFixture(fixture));
	segment_cache::PackagerFingerprint fp;
	fp.target_duration_ms = 1000;
	auto session = segment_cache::SourceSession::Open(fixture, fp);
	ASSERT_NE(session, nullptr);

	segment_cache::SessionRegistry::GetInstance().Put("filler", session);
	EXPECT_NE(segment_cache::SessionRegistry::GetInstance().Find("filler"), nullptr);
	EXPECT_EQ(segment_cache::SessionRegistry::GetInstance().Find("other"), nullptr);

	segment_cache::SessionRegistry::GetInstance().Clear();
	::unsetenv("OME_SEGMENT_CACHE");
}
