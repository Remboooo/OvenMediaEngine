//==============================================================================
//
//  OvenMediaEngine - TS materialize test (MP4 index -> one HLS TS segment)
//
//==============================================================================
#include <gtest/gtest.h>

#include "bitstream_for_mpegts.h"
#include "mp4_sample_indexer.h"
#include "mpegts_materializer.h"

#include <base/ovlibrary/files.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

namespace
{
	ov::String FindFixtureMp4()
	{
		const char *env = std::getenv("OME_SEGCACHE_TEST_MP4");
		if (env != nullptr && env[0] != '\0' && ::access(env, R_OK) == 0)
		{
			return env;
		}

		// Tiny fixture generated next to this test binary's source tree, or /tmp.
		const char *candidates[] = {
			"/tmp/ome_segcache_fixture.mp4",
			"/opt/ovenmediaengine/media/480p/trains3.mp4",
			nullptr};

		for (int i = 0; candidates[i] != nullptr; i++)
		{
			if (::access(candidates[i], R_OK) == 0)
			{
				return candidates[i];
			}
		}
		return "";
	}

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
			"-t 2 -c:v libx264 -pix_fmt yuv420p -profile:v baseline "
			"-x264-params keyint=30:min-keyint=30:scenecut=0 "
			"-c:a aac -b:a 64k -movflags +faststart %s",
			path.CStr());

		int rc = std::system(cmd.CStr());
		return rc == 0 && ::access(path.CStr(), R_OK) == 0;
	}

	bool LooksLikeMpegTs(const std::shared_ptr<const ov::Data> &data)
	{
		if (data == nullptr || data->GetLength() < 188)
		{
			return false;
		}
		const auto *p = data->GetDataAs<uint8_t>();
		// Every 188 bytes should start with sync 0x47 for a clean segment.
		size_t packets = std::min<size_t>(data->GetLength() / 188, 20);
		for (size_t i = 0; i < packets; i++)
		{
			if (p[i * 188] != 0x47)
			{
				return false;
			}
		}
		return true;
	}
}  // namespace

TEST(SegmentCacheMpegTsMaterialize, IndexAndMaterializeOneSegment)
{
	ov::String fixture = FindFixtureMp4();
	if (fixture.IsEmpty())
	{
		fixture = "/tmp/ome_segcache_fixture.mp4";
		ASSERT_TRUE(EnsureTinyFixture(fixture));
	}
	ASSERT_FALSE(fixture.IsEmpty()) << "No MP4 fixture available (install ffmpeg or set OME_SEGCACHE_TEST_MP4)";

	auto index = segment_cache::Mp4SampleIndexer::Build(fixture);
	ASSERT_NE(index, nullptr);
	ASSERT_NE(index->FindTrackByMediaType(cmn::MediaType::Video), nullptr);
	ASSERT_FALSE(index->FindTrackByMediaType(cmn::MediaType::Video)->samples.empty());

	auto result = segment_cache::MpegTsMaterializer::Materialize(index, 0, 1000);
	ASSERT_NE(result, nullptr);
	ASSERT_NE(result->data, nullptr);
	EXPECT_GT(result->video_samples, 0u);
	EXPECT_GT(result->duration_ms, 0.0);
	EXPECT_TRUE(LooksLikeMpegTs(result->data));
	EXPECT_LT(result->cpu_ms, 2000.0) << "Materialize took unexpectedly long: " << result->cpu_ms << " ms";

	// Optional: write artifact for manual ffplay when debugging
	const char *dump = std::getenv("OME_SEGCACHE_DUMP_TS");
	if (dump != nullptr && dump[0] != '\0')
	{
		FILE *out = ::fopen(dump, "wb");
		ASSERT_NE(out, nullptr);
		::fwrite(result->data->GetDataAs<uint8_t>(), 1, result->data->GetLength(), out);
		::fclose(out);
	}
}

TEST(SegmentCacheMpegTsMaterialize, SecondMaterializeUsesSameIndex)
{
	ov::String fixture = "/tmp/ome_segcache_fixture.mp4";
	ASSERT_TRUE(EnsureTinyFixture(fixture));

	auto index = segment_cache::Mp4SampleIndexer::Build(fixture);
	ASSERT_NE(index, nullptr);

	auto first = segment_cache::MpegTsMaterializer::Materialize(index, 0, 1000);
	ASSERT_NE(first, nullptr);

	// Second window further along the timeline (still within 2s fixture if possible)
	auto second = segment_cache::MpegTsMaterializer::Materialize(index, first->first_dts + static_cast<int64_t>(first->duration_ms * 90), 1000);
	// May be nullptr near EOF on a 2s file — only assert when enough media remains
	if (second != nullptr)
	{
		EXPECT_TRUE(LooksLikeMpegTs(second->data));
		EXPECT_GT(second->data->GetLength(), 188u);
	}
}

TEST(SegmentCacheMpegTsMaterialize, MarksDiscontinuityOnFirstMediaPackets)
{
	ov::String fixture = "/tmp/ome_segcache_fixture.mp4";
	ASSERT_TRUE(EnsureTinyFixture(fixture));

	auto index = segment_cache::Mp4SampleIndexer::Build(fixture);
	ASSERT_NE(index, nullptr);

	auto first = segment_cache::MpegTsMaterializer::Materialize(index, 0, 1000);
	ASSERT_NE(first, nullptr);
	ASSERT_NE(first->data, nullptr);

	auto count_disc_pids = [](const std::shared_ptr<const ov::Data> &data) {
		const auto *p = data->GetDataAs<uint8_t>();
		const size_t n = data->GetLength() / 188;
		bool seen[0x2000] = {};
		size_t disc_pids = 0;
		for (size_t i = 0; i < n; i++)
		{
			const uint8_t *b = p + i * 188;
			if (b[0] != 0x47)
			{
				continue;
			}
			const uint16_t pid = static_cast<uint16_t>(((b[1] & 0x1f) << 8) | b[2]);
			const uint8_t afc = (b[3] >> 4) & 0x03;
			if (afc < 2 || b[4] == 0)
			{
				continue;
			}
			if ((b[5] & 0x80) && seen[pid] == false)
			{
				seen[pid] = true;
				disc_pids++;
			}
		}
		return disc_pids;
	};

	// Video (and usually audio) PIDs carry an AF on the first PES packet.
	EXPECT_GE(count_disc_pids(first->data), 1u);

	auto second = segment_cache::MpegTsMaterializer::Materialize(
		index, first->first_dts + static_cast<int64_t>(first->duration_ms * 90), 1000);
	if (second != nullptr)
	{
		EXPECT_GE(count_disc_pids(second->data), 1u);
	}
}

TEST(SegmentCacheIndexer, ProbeAcceptsH264AacMp4)
{
	const ov::String fixture = "/tmp/ome_segcache_fixture.mp4";
	ASSERT_TRUE(EnsureTinyFixture(fixture));
	auto probe = segment_cache::Mp4SampleIndexer::Probe(fixture, true);
	EXPECT_TRUE(probe.ok) << probe.error.CStr();
	EXPECT_TRUE(probe.has_h264_video);
	EXPECT_TRUE(probe.has_aac_audio);
}

TEST(SegmentCacheIndexer, ProbeRejectsMissingFile)
{
	auto probe = segment_cache::Mp4SampleIndexer::Probe("/tmp/ome_segcache_does_not_exist.mp4", true);
	EXPECT_FALSE(probe.ok);
	EXPECT_FALSE(probe.error.IsEmpty());
}

TEST(SegmentCacheIndexer, ProbeRejectsMpegTsContainer)
{
	const char *path = "/tmp/ome_segcache_probe_reject.ts";
	const int rc = ::system(
		"ffmpeg -y -hide_banner -loglevel error -f lavfi -i testsrc=size=160x120:rate=10 "
		"-f lavfi -i sine=frequency=440:sample_rate=48000 -t 1 "
		"-c:v libx264 -pix_fmt yuv420p -c:a aac -f mpegts /tmp/ome_segcache_probe_reject.ts");
	ASSERT_EQ(rc, 0);
	ASSERT_EQ(::access(path, R_OK), 0);
	auto probe = segment_cache::Mp4SampleIndexer::Probe(path, true);
	EXPECT_FALSE(probe.ok);
	EXPECT_TRUE(ov::String(probe.error).LowerCaseString().IndexOf("mp4") >= 0 ||
				ov::String(probe.error).LowerCaseString().IndexOf("mov") >= 0 ||
				ov::String(probe.error).LowerCaseString().IndexOf("format") >= 0)
		<< probe.error.CStr();
	::unlink(path);
}
