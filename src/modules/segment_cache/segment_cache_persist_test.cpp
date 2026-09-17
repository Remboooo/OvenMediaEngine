//==============================================================================
//
//  Sidecar persistence + hot segment byte cache tests
//
//==============================================================================
#include <gtest/gtest.h>

#include "hot_segment_cache.h"
#include "mp4_sample_indexer.h"
#include "mpegts_materializer.h"
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

	bool DataEqual(const std::shared_ptr<const ov::Data> &a, const std::shared_ptr<const ov::Data> &b)
	{
		if (a == nullptr || b == nullptr || a->GetLength() != b->GetLength())
		{
			return false;
		}
		return std::memcmp(a->GetDataAs<uint8_t>(), b->GetDataAs<uint8_t>(), a->GetLength()) == 0;
	}
}  // namespace

TEST(SegmentCacheSidecar, RoundTripAndInvalidation)
{
	ov::String fixture = "/tmp/ome_segcache_fixture.mp4";
	ASSERT_TRUE(EnsureTinyFixture(fixture));

	segment_cache::PackagerFingerprint fp;
	fp.format_id = 1;
	fp.target_duration_ms = 1000;

	ov::String sidecar = "/tmp/ome_segcache_fixture.mp4.ome-segcache-test";
	::unlink(sidecar.CStr());

	auto built = segment_cache::Mp4SampleIndexer::Build(fixture);
	ASSERT_NE(built, nullptr);
	ASSERT_TRUE(segment_cache::SampleIndexSidecar::Save(sidecar, *built, fp));

	auto loaded = segment_cache::SampleIndexSidecar::Load(
		sidecar, built->GetSourcePath(), built->GetSourceSize(), built->GetSourceMtimeNs(), fp);
	ASSERT_NE(loaded, nullptr);
	ASSERT_EQ(loaded->GetTracks().size(), built->GetTracks().size());

	const auto *bv = built->FindTrackByMediaType(cmn::MediaType::Video);
	const auto *lv = loaded->FindTrackByMediaType(cmn::MediaType::Video);
	ASSERT_NE(bv, nullptr);
	ASSERT_NE(lv, nullptr);
	ASSERT_EQ(lv->samples.size(), bv->samples.size());
	EXPECT_EQ(lv->samples.front().file_offset, bv->samples.front().file_offset);
	EXPECT_EQ(lv->samples.back().dts, bv->samples.back().dts);

	// Materialize from loaded sidecar index must match fresh index.
	auto from_built = segment_cache::MpegTsMaterializer::Materialize(built, 0, 1000);
	auto from_loaded = segment_cache::MpegTsMaterializer::Materialize(loaded, 0, 1000);
	ASSERT_NE(from_built, nullptr);
	ASSERT_NE(from_loaded, nullptr);
	ASSERT_TRUE(DataEqual(from_built->data, from_loaded->data));

	// Fingerprint mismatch -> reject
	segment_cache::PackagerFingerprint other = fp;
	other.target_duration_ms = 2000;
	EXPECT_EQ(segment_cache::SampleIndexSidecar::Load(
				  sidecar, built->GetSourcePath(), built->GetSourceSize(), built->GetSourceMtimeNs(), other),
			  nullptr);

	// LoadOrBuild should hit sidecar on second call
	ov::String path_out;
	auto first = segment_cache::SampleIndexSidecar::LoadOrBuild(fixture, fp, true, 0.0, &path_out);
	ASSERT_NE(first, nullptr);
	EXPECT_EQ(path_out, segment_cache::SampleIndexSidecar::DefaultPathForSource(fixture, fp));
	auto second = segment_cache::SampleIndexSidecar::LoadOrBuild(fixture, fp, true, 0.0, &path_out);
	ASSERT_NE(second, nullptr);
	EXPECT_EQ(second->FindTrackByMediaType(cmn::MediaType::Video)->samples.size(),
			  first->FindTrackByMediaType(cmn::MediaType::Video)->samples.size());

	// Distinct (LL)HLS params → distinct sidecar filenames
	segment_cache::PackagerFingerprint llhls = fp;
	llhls.format_id = 2;
	llhls.chunk_duration_ms = 500;
	EXPECT_NE(segment_cache::SampleIndexSidecar::DefaultPathForSource(fixture, fp),
			  segment_cache::SampleIndexSidecar::DefaultPathForSource(fixture, llhls));

	::unlink(sidecar.CStr());
	::unlink(path_out.CStr());
}

TEST(SegmentCacheHot, HitReturnsIdenticalBytesAndEvicts)
{
	ov::String fixture = "/tmp/ome_segcache_fixture.mp4";
	ASSERT_TRUE(EnsureTinyFixture(fixture));

	auto index = segment_cache::Mp4SampleIndexer::Build(fixture);
	ASSERT_NE(index, nullptr);

	auto seg = segment_cache::MpegTsMaterializer::Materialize(index, 0, 1000);
	ASSERT_NE(seg, nullptr);

	segment_cache::HotSegmentCache::Config cfg;
	cfg.max_entries = 2;
	cfg.max_bytes = 1024ull * 1024ull;
	segment_cache::HotSegmentCache cache(cfg);

	segment_cache::HotSegmentKey key;
	key.source_path = fixture;
	key.start_dts = seg->first_dts;
	key.target_duration_ms = 1000;
	key.format_id = 1;

	EXPECT_EQ(cache.Get(key), nullptr);
	EXPECT_EQ(cache.GetMissCount(), 1u);

	cache.Put(key, seg->data);
	auto hit = cache.Get(key);
	ASSERT_NE(hit, nullptr);
	ASSERT_TRUE(DataEqual(hit, seg->data));
	EXPECT_EQ(cache.GetHitCount(), 1u);
	EXPECT_EQ(cache.GetEntryCount(), 1u);

	// Fill beyond max_entries and ensure eviction keeps the newest.
	auto seg2 = segment_cache::MpegTsMaterializer::Materialize(index, 90 * 1000, 1000);
	auto seg3 = segment_cache::MpegTsMaterializer::Materialize(index, 90 * 2000, 1000);
	ASSERT_NE(seg2, nullptr);
	ASSERT_NE(seg3, nullptr);

	segment_cache::HotSegmentKey key2 = key;
	key2.start_dts = seg2->first_dts;
	segment_cache::HotSegmentKey key3 = key;
	key3.start_dts = seg3->first_dts;

	cache.Put(key2, seg2->data);
	cache.Put(key3, seg3->data);
	EXPECT_EQ(cache.GetEntryCount(), 2u);
	EXPECT_EQ(cache.Get(key), nullptr);  // oldest evicted
	EXPECT_NE(cache.Get(key2), nullptr);
	EXPECT_NE(cache.Get(key3), nullptr);
}
