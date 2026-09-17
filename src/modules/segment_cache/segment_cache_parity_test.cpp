//==============================================================================
//
//  Verify: after N full loops, cache vs demux materialize are byte-identical,
//  and cache path only reads the segment's sample bytes (not the whole file).
//
//==============================================================================
#include <gtest/gtest.h>

#include "mp4_sample_indexer.h"
#include "mpegts_materializer.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
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

		const char *candidates[] = {
			"/opt/ovenmediaengine/media/480p/trains3.mp4",
			"/tmp/ome_segcache_fixture.mp4",
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
			"-t 4 -c:v libx264 -pix_fmt yuv420p -profile:v baseline "
			"-x264-params keyint=30:min-keyint=30:scenecut=0 "
			"-c:a aac -b:a 64k -movflags +faststart %s",
			path.CStr());
		return std::system(cmd.CStr()) == 0 && ::access(path.CStr(), R_OK) == 0;
	}

	int64_t VideoDurationMs(const segment_cache::SampleIndex &index)
	{
		const auto *video = index.FindTrackByMediaType(cmn::MediaType::Video);
		if (video == nullptr || video->samples.empty())
		{
			return 0;
		}
		const auto &last = video->samples.back();
		return (last.dts + last.duration) / 90;
	}

	bool DataEqual(const std::shared_ptr<const ov::Data> &a, const std::shared_ptr<const ov::Data> &b)
	{
		if (a == nullptr || b == nullptr)
		{
			return false;
		}
		if (a->GetLength() != b->GetLength())
		{
			return false;
		}
		return std::memcmp(a->GetDataAs<uint8_t>(), b->GetDataAs<uint8_t>(), a->GetLength()) == 0;
	}

	uint64_t ReadProcIo(const char *field)
	{
		FILE *fp = ::fopen("/proc/self/io", "r");
		if (fp == nullptr)
		{
			return 0;
		}
		char line[256];
		uint64_t value = 0;
		while (::fgets(line, sizeof(line), fp) != nullptr)
		{
			if (std::strncmp(line, field, std::strlen(field)) == 0)
			{
				value = std::strtoull(line + std::strlen(field), nullptr, 10);
				break;
			}
		}
		::fclose(fp);
		return value;
	}
}  // namespace

TEST(SegmentCacheParity, ByteIdenticalAfterMultipleLoops)
{
	// Prefer a short fixture so demux-from-start stays fast; still exercises
	// schedule-style join after several full loops (modulo position).
	ov::String fixture = "/tmp/ome_segcache_fixture.mp4";
	ASSERT_TRUE(EnsureTinyFixture(fixture));

	auto index = segment_cache::Mp4SampleIndexer::Build(fixture);
	ASSERT_NE(index, nullptr);

	const int64_t duration_ms = VideoDurationMs(*index);
	ASSERT_GT(duration_ms, 1000);

	constexpr uint32_t kTargetSegMs = 1000;
	const int64_t offsets_ms[] = {
		0,
		duration_ms / 4,
		duration_ms / 2};
	// Stay clear of EOF so a full target segment (+ next keyframe) still fits.


	for (int64_t offset_ms : offsets_ms)
	{
		const int64_t join_dts = segment_cache::JoinDtsAfterLoops(duration_ms, 3, offset_ms);

		auto cached = segment_cache::MpegTsMaterializer::Materialize(index, join_dts, kTargetSegMs);
		ASSERT_NE(cached, nullptr) << "cache materialize failed at offset_ms=" << offset_ms;

		auto demuxed = segment_cache::MpegTsMaterializer::MaterializeFromDemux(index, join_dts, kTargetSegMs);
		ASSERT_NE(demuxed, nullptr) << "demux materialize failed at offset_ms=" << offset_ms;

		EXPECT_EQ(cached->first_dts, demuxed->first_dts);
		EXPECT_EQ(cached->end_dts, demuxed->end_dts);
		EXPECT_EQ(cached->video_samples, demuxed->video_samples);
		EXPECT_EQ(cached->audio_samples, demuxed->audio_samples);
		ASSERT_TRUE(DataEqual(cached->data, demuxed->data))
			<< "TS bytes differ at offset_ms=" << offset_ms
			<< " cached=" << cached->data->GetLength()
			<< " demuxed=" << demuxed->data->GetLength();

		EXPECT_GT(cached->bytes_read, 0u);
		EXPECT_LT(cached->bytes_read, static_cast<uint64_t>(index->GetSourceSize()));
		EXPECT_EQ(cached->bytes_read, demuxed->bytes_read);
	}
}

TEST(SegmentCacheParity, ByteIdenticalOnRealFillerNearStart)
{
	const char *path = "/opt/ovenmediaengine/media/480p/trains3.mp4";
	if (::access(path, R_OK) != 0)
	{
		GTEST_SKIP() << "real filler not available";
	}

	auto index = segment_cache::Mp4SampleIndexer::Build(path);
	ASSERT_NE(index, nullptr);
	const int64_t duration_ms = VideoDurationMs(*index);
	// After 3 loops + 10s — same media position as 10s into the file.
	const int64_t join_dts = segment_cache::JoinDtsAfterLoops(duration_ms, 3, 10000);

	auto cached = segment_cache::MpegTsMaterializer::Materialize(index, join_dts, 1000);
	ASSERT_NE(cached, nullptr);
	auto demuxed = segment_cache::MpegTsMaterializer::MaterializeFromDemux(index, join_dts, 1000);
	ASSERT_NE(demuxed, nullptr);
	ASSERT_TRUE(DataEqual(cached->data, demuxed->data));
	EXPECT_EQ(cached->bytes_read, demuxed->bytes_read);
	EXPECT_LT(cached->bytes_read, static_cast<uint64_t>(index->GetSourceSize()) / 100);
}

TEST(SegmentCacheParity, CacheDeterministicAndBoundedIo)
{
	ov::String fixture = FindFixtureMp4();
	if (fixture.IsEmpty())
	{
		fixture = "/tmp/ome_segcache_fixture.mp4";
		ASSERT_TRUE(EnsureTinyFixture(fixture));
	}

	auto index = segment_cache::Mp4SampleIndexer::Build(fixture);
	ASSERT_NE(index, nullptr);

	const int64_t duration_ms = VideoDurationMs(*index);
	const int64_t join_dts = segment_cache::JoinDtsAfterLoops(duration_ms, 5, duration_ms / 3);

	// Drop page cache for this file if permitted (best-effort); still assert app-level bytes_read.
	int fd = ::open(fixture.CStr(), O_RDONLY);
	if (fd >= 0)
	{
		::posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
		::close(fd);
	}

	const uint64_t io_before = ReadProcIo("read_bytes: ");
	auto first = segment_cache::MpegTsMaterializer::Materialize(index, join_dts, 1000);
	const uint64_t io_after = ReadProcIo("read_bytes: ");
	ASSERT_NE(first, nullptr);

	auto second = segment_cache::MpegTsMaterializer::Materialize(index, join_dts, 1000);
	ASSERT_NE(second, nullptr);
	ASSERT_TRUE(DataEqual(first->data, second->data));

	EXPECT_EQ(first->bytes_read, second->bytes_read);
	EXPECT_LT(first->bytes_read, static_cast<uint64_t>(index->GetSourceSize()) / 10);

	// Kernel read_bytes may be 0 if already cached; when non-zero it should be near the segment.
	if (io_after > io_before)
	{
		const uint64_t delta = io_after - io_before;
		EXPECT_LT(delta, static_cast<uint64_t>(index->GetSourceSize()) / 2)
			<< "kernel read_bytes delta suggests scanning far more than one segment";
	}
}
