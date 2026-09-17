//==============================================================================
//
//  OvenMediaEngine - SampleIndex sidecar persistence
//
//==============================================================================
#include "sample_index_sidecar.h"

#include "mp4_sample_indexer.h"

#include <modules/bitstream/aac/audio_specific_config.h>
#include <modules/bitstream/h264/h264_decoder_configuration_record.h>

#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#define OV_LOG_TAG "SegmentCache.Sidecar"

namespace segment_cache
{
	namespace
	{
		constexpr char kMagic[8] = {'O', 'M', 'E', 'S', 'I', 'D', 'X', '\0'};
		constexpr uint32_t kFileFormatVersion = 2;

		bool WriteExact(FILE *fp, const void *data, size_t length)
		{
			return ::fwrite(data, 1, length, fp) == length;
		}

		bool ReadExact(FILE *fp, void *data, size_t length)
		{
			return ::fread(data, 1, length, fp) == length;
		}

		template <typename T>
		bool WritePod(FILE *fp, const T &value)
		{
			return WriteExact(fp, &value, sizeof(T));
		}

		template <typename T>
		bool ReadPod(FILE *fp, T &value)
		{
			return ReadExact(fp, &value, sizeof(T));
		}

		bool WriteString(FILE *fp, const ov::String &value)
		{
			uint32_t length = static_cast<uint32_t>(value.GetLength());
			if (WritePod(fp, length) == false)
			{
				return false;
			}
			if (length == 0)
			{
				return true;
			}
			return WriteExact(fp, value.CStr(), length);
		}

		bool ReadString(FILE *fp, ov::String &value)
		{
			uint32_t length = 0;
			if (ReadPod(fp, length) == false)
			{
				return false;
			}
			if (length == 0)
			{
				value = "";
				return true;
			}
			std::vector<char> buffer(length + 1, 0);
			if (ReadExact(fp, buffer.data(), length) == false)
			{
				return false;
			}
			value = ov::String(buffer.data(), length);
			return true;
		}

		bool WriteData(FILE *fp, const std::shared_ptr<const ov::Data> &data)
		{
			uint32_t length = (data != nullptr) ? static_cast<uint32_t>(data->GetLength()) : 0;
			if (WritePod(fp, length) == false)
			{
				return false;
			}
			if (length == 0)
			{
				return true;
			}
			return WriteExact(fp, data->GetData(), length);
		}

		bool ReadData(FILE *fp, std::shared_ptr<ov::Data> &data)
		{
			uint32_t length = 0;
			if (ReadPod(fp, length) == false)
			{
				return false;
			}
			if (length == 0)
			{
				data = nullptr;
				return true;
			}
			data = std::make_shared<ov::Data>(length);
			data->SetLength(length);
			return ReadExact(fp, data->GetWritableDataAs<uint8_t>(), length);
		}

		bool SourceMatches(const ov::String &path, int64_t expected_size, int64_t expected_mtime_ns)
		{
			struct stat st {};
			if (::stat(path.CStr(), &st) != 0)
			{
				return false;
			}
			const int64_t mtime_ns = static_cast<int64_t>(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec;
			return static_cast<int64_t>(st.st_size) == expected_size && mtime_ns == expected_mtime_ns;
		}
	}  // namespace

	ov::String SampleIndexSidecar::DefaultPathForSource(const ov::String &source_path,
														const PackagerFingerprint &fingerprint)
	{
		// Encode (LL)HLS packager params so distinct profiles do not share a sidecar.
		return ov::String::FormatString("%s.ome-segcache.f%u.td%u.cd%u.sv%u",
										source_path.CStr(),
										fingerprint.format_id,
										fingerprint.target_duration_ms,
										fingerprint.chunk_duration_ms,
										fingerprint.schema_version);
	}

	bool SampleIndexSidecar::Save(const ov::String &sidecar_path,
								  const SampleIndex &index,
								  const PackagerFingerprint &fingerprint)
	{
		ov::String tmp_path = sidecar_path + ".tmp";
		FILE *fp = ::fopen(tmp_path.CStr(), "wb");
		if (fp == nullptr)
		{
			logte("Failed to open sidecar for write: %s", tmp_path.CStr());
			return false;
		}

		auto fail = [&]() {
			::fclose(fp);
			::unlink(tmp_path.CStr());
			return false;
		};

		if (WriteExact(fp, kMagic, sizeof(kMagic)) == false ||
			WritePod(fp, kFileFormatVersion) == false ||
			WritePod(fp, fingerprint.format_id) == false ||
			WritePod(fp, fingerprint.target_duration_ms) == false ||
			WritePod(fp, fingerprint.chunk_duration_ms) == false ||
			WritePod(fp, fingerprint.schema_version) == false ||
			WriteString(fp, index.GetSourcePath()) == false ||
			WritePod(fp, index.GetSourceSize()) == false ||
			WritePod(fp, index.GetSourceMtimeNs()) == false)
		{
			return fail();
		}

		uint32_t track_count = static_cast<uint32_t>(index.GetTracks().size());
		if (WritePod(fp, track_count) == false)
		{
			return fail();
		}

		for (const auto &track : index.GetTracks())
		{
			if (track.media_track == nullptr)
			{
				return fail();
			}

			auto media_type = static_cast<uint32_t>(track.media_track->GetMediaType());
			auto codec_id = static_cast<uint32_t>(track.media_track->GetCodecId());
			int32_t tb_num = track.media_track->GetTimeBase().GetNum();
			int32_t tb_den = track.media_track->GetTimeBase().GetDen();

			std::shared_ptr<const ov::Data> dcr_data;
			auto dcr = track.media_track->GetDecoderConfigurationRecord();
			if (dcr != nullptr)
			{
				dcr_data = dcr->GetData();
			}

			uint32_t sample_count = static_cast<uint32_t>(track.samples.size());
			if (WritePod(fp, track.track_id) == false ||
				WritePod(fp, track.ffmpeg_stream_index) == false ||
				WritePod(fp, media_type) == false ||
				WritePod(fp, codec_id) == false ||
				WritePod(fp, tb_num) == false ||
				WritePod(fp, tb_den) == false ||
				WriteData(fp, dcr_data) == false ||
				WritePod(fp, sample_count) == false)
			{
				return fail();
			}

			for (const auto &sample : track.samples)
			{
				uint8_t keyframe = sample.keyframe ? 1 : 0;
				if (WritePod(fp, sample.file_offset) == false ||
					WritePod(fp, sample.size) == false ||
					WritePod(fp, sample.pts) == false ||
					WritePod(fp, sample.dts) == false ||
					WritePod(fp, sample.duration) == false ||
					WritePod(fp, keyframe) == false)
				{
					return fail();
				}
			}
		}

		::fclose(fp);
		if (::rename(tmp_path.CStr(), sidecar_path.CStr()) != 0)
		{
			logte("Failed to rename sidecar %s -> %s", tmp_path.CStr(), sidecar_path.CStr());
			::unlink(tmp_path.CStr());
			return false;
		}

		logti("Saved sample index sidecar %s (%u tracks)", sidecar_path.CStr(), track_count);
		return true;
	}

	std::shared_ptr<SampleIndex> SampleIndexSidecar::Load(const ov::String &sidecar_path,
														 const ov::String &expected_source_path,
														 int64_t expected_size,
														 int64_t expected_mtime_ns,
														 const PackagerFingerprint &fingerprint)
	{
		FILE *fp = ::fopen(sidecar_path.CStr(), "rb");
		if (fp == nullptr)
		{
			return nullptr;
		}

		auto fail = [&]() -> std::shared_ptr<SampleIndex> {
			::fclose(fp);
			return nullptr;
		};

		char magic[8] = {};
		uint32_t file_version = 0;
		PackagerFingerprint file_fp;
		ov::String source_path;
		int64_t source_size = 0;
		int64_t source_mtime_ns = 0;

		if (ReadExact(fp, magic, sizeof(magic)) == false ||
			std::memcmp(magic, kMagic, sizeof(kMagic)) != 0)
		{
			logtw("Sidecar magic mismatch: %s", sidecar_path.CStr());
			return fail();
		}
		if (ReadPod(fp, file_version) == false || file_version != kFileFormatVersion)
		{
			logtw("Sidecar version mismatch: %s (got %u)", sidecar_path.CStr(), file_version);
			return fail();
		}
		if (ReadPod(fp, file_fp.format_id) == false ||
			ReadPod(fp, file_fp.target_duration_ms) == false ||
			ReadPod(fp, file_fp.chunk_duration_ms) == false ||
			ReadPod(fp, file_fp.schema_version) == false ||
			!(file_fp == fingerprint))
		{
			logtw("Sidecar fingerprint mismatch: %s", sidecar_path.CStr());
			return fail();
		}
		if (ReadString(fp, source_path) == false ||
			ReadPod(fp, source_size) == false ||
			ReadPod(fp, source_mtime_ns) == false)
		{
			logtw("Sidecar identity read failed: %s", sidecar_path.CStr());
			return fail();
		}

		if (source_path != expected_source_path ||
			source_size != expected_size ||
			source_mtime_ns != expected_mtime_ns)
		{
			logti("Sidecar %s identity fields mismatch (path/size/mtime)", sidecar_path.CStr());
			return fail();
		}
		if (SourceMatches(expected_source_path, expected_size, expected_mtime_ns) == false)
		{
			logti("Sidecar %s invalidated (source file changed on disk)", sidecar_path.CStr());
			return fail();
		}

		uint32_t track_count = 0;
		if (ReadPod(fp, track_count) == false)
		{
			logtw("Sidecar track count read failed: %s", sidecar_path.CStr());
			return fail();
		}

		auto index = std::make_shared<SampleIndex>();
		index->SetSourceIdentity(source_path, source_size, source_mtime_ns);

		for (uint32_t t = 0; t < track_count; t++)
		{
			TrackIndex track;
			uint32_t media_type = 0;
			uint32_t codec_id = 0;
			int32_t tb_num = 0;
			int32_t tb_den = 0;
			std::shared_ptr<ov::Data> dcr_data;
			uint32_t sample_count = 0;

			if (ReadPod(fp, track.track_id) == false ||
				ReadPod(fp, track.ffmpeg_stream_index) == false ||
				ReadPod(fp, media_type) == false ||
				ReadPod(fp, codec_id) == false ||
				ReadPod(fp, tb_num) == false ||
				ReadPod(fp, tb_den) == false ||
				ReadData(fp, dcr_data) == false ||
				ReadPod(fp, sample_count) == false)
			{
				logtw("Sidecar track %u header read failed: %s", t, sidecar_path.CStr());
				return fail();
			}

			auto media_track = std::make_shared<MediaTrack>();
			media_track->SetId(track.track_id);
			media_track->SetMediaType(static_cast<cmn::MediaType>(media_type));
			media_track->SetCodecId(static_cast<cmn::MediaCodecId>(codec_id));
			media_track->SetTimeBase(tb_num, tb_den);

			if (dcr_data != nullptr)
			{
				if (media_track->GetCodecId() == cmn::MediaCodecId::H264)
				{
					auto avc = std::make_shared<AVCDecoderConfigurationRecord>();
					if (avc->Parse(dcr_data) == false)
					{
						logtw("Sidecar AVC DCR parse failed");
						return fail();
					}
					media_track->SetDecoderConfigurationRecord(avc);
					// Sidecar does not store VisualSampleEntry dimensions; MSE/Chrome
					// reject init segments with coded size [0,0]. Fill from SPS.
					if (avc->GetWidth() > 0 && avc->GetHeight() > 0)
					{
						media_track->SetResolution(avc->GetWidth(), avc->GetHeight());
					}
				}
				else if (media_track->GetCodecId() == cmn::MediaCodecId::Aac)
				{
					auto asc = std::make_shared<AudioSpecificConfig>();
					if (asc->Parse(dcr_data) == false)
					{
						logtw("Sidecar AAC ASC parse failed");
						return fail();
					}
					media_track->SetDecoderConfigurationRecord(asc);
					if (asc->Samplerate() > 0)
					{
						media_track->SetSampleRate(static_cast<int32_t>(asc->Samplerate()));
					}
					if (asc->Channel() > 0)
					{
						media_track->SetChannelCount(asc->Channel());
					}
				}
			}

			track.media_track = media_track;
			track.samples.reserve(sample_count);
			for (uint32_t s = 0; s < sample_count; s++)
			{
				SampleRef sample;
				uint8_t keyframe = 0;
				if (ReadPod(fp, sample.file_offset) == false ||
					ReadPod(fp, sample.size) == false ||
					ReadPod(fp, sample.pts) == false ||
					ReadPod(fp, sample.dts) == false ||
					ReadPod(fp, sample.duration) == false ||
					ReadPod(fp, keyframe) == false)
				{
					logtw("Sidecar track %u sample %u read failed: %s", t, s, sidecar_path.CStr());
					return fail();
				}
				sample.keyframe = (keyframe != 0);
				track.samples.push_back(sample);
			}

			index->GetTracks().push_back(std::move(track));
		}

		::fclose(fp);
		logti("Loaded sample index sidecar %s (%u tracks)", sidecar_path.CStr(), track_count);
		return index;
	}

	std::shared_ptr<SampleIndex> SampleIndexSidecar::LoadOrBuild(const ov::String &source_path,
																const PackagerFingerprint &fingerprint,
																bool persist_sidecar,
																double max_throughput_mbps,
																ov::String *sidecar_path_out)
	{
		struct stat st {};
		if (::stat(source_path.CStr(), &st) != 0)
		{
			logte("Cannot stat source %s", source_path.CStr());
			return nullptr;
		}

		const int64_t size = static_cast<int64_t>(st.st_size);
		const int64_t mtime_ns = static_cast<int64_t>(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec;
		ov::String sidecar_path = DefaultPathForSource(source_path, fingerprint);
		if (sidecar_path_out != nullptr)
		{
			*sidecar_path_out = sidecar_path;
		}

		if (persist_sidecar)
		{
			auto loaded = Load(sidecar_path, source_path, size, mtime_ns, fingerprint);
			if (loaded != nullptr)
			{
				return loaded;
			}

			// Plan/schema bumps (e.g. keyframe-safe parts) do not change the sample
			// index. Reuse an older sidecar, then rewrite under the current name so
			// we do not re-demux multi-GB fillers on every schema tweak.
			for (uint32_t old_sv = fingerprint.schema_version; old_sv > 0;)
			{
				old_sv--;
				PackagerFingerprint old_fp = fingerprint;
				old_fp.schema_version = old_sv;
				const ov::String old_path = DefaultPathForSource(source_path, old_fp);
				loaded = Load(old_path, source_path, size, mtime_ns, old_fp);
				if (loaded == nullptr)
				{
					continue;
				}
				logti("Migrating sidecar %s -> %s (schema %u -> %u)",
					  old_path.CStr(), sidecar_path.CStr(), old_sv, fingerprint.schema_version);
				if (Save(sidecar_path, *loaded, fingerprint) == false)
				{
					logtw("Loaded schema-%u index for %s but failed to write %s",
						  old_sv, source_path.CStr(), sidecar_path.CStr());
				}
				return loaded;
			}
		}

		auto built = Mp4SampleIndexer::Build(source_path, max_throughput_mbps);
		if (built == nullptr)
		{
			return nullptr;
		}

		if (persist_sidecar)
		{
			if (Save(sidecar_path, *built, fingerprint) == false)
			{
				logtw("Built index for %s but failed to persist sidecar %s", source_path.CStr(), sidecar_path.CStr());
			}
		}
		return built;
	}
}  // namespace segment_cache
