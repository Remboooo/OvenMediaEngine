//==============================================================================
//
//  OvenMediaEngine - Materialize LLHLS fMP4 init/segment from a SampleIndex
//
//==============================================================================
#include "fmp4_materializer.h"

#include <modules/containers/bmff/bmff_packager.h>
#include <modules/containers/bmff/cenc.h>
#include <modules/containers/bmff/sample.h>

#include <chrono>
#include <cstdio>

#define OV_LOG_TAG "SegmentCache.Fmp4"

namespace segment_cache
{
	namespace
	{
		// Expose protected Packager writers for offline batch materialize.
		class FragmentWriter : public bmff::Packager
		{
		public:
			FragmentWriter(const std::shared_ptr<const MediaTrack> &media_track)
				: Packager(media_track, nullptr, bmff::CencProperty{})
			{
			}

			bool WriteInit(ov::ByteStream &stream)
			{
				return WriteFtypBox(stream) && WriteMoovBox(stream);
			}

			bool WriteFragment(ov::ByteStream &stream, const std::shared_ptr<const bmff::Samples> &samples)
			{
				return WriteMoofBox(stream, samples) && WriteMdatBox(stream, samples);
			}

		protected:
			// Match live LLHLS brands when possible.
			bool WriteFtypBox(ov::ByteStream &data_stream) override
			{
				ov::ByteStream body(128);
				body.WriteText("iso6");
				body.WriteBE32(0);
				body.WriteText("iso6mp42");
				switch (GetMediaTrack()->GetCodecId())
				{
					case cmn::MediaCodecId::H264:
						body.WriteText("avc1");
						break;
					case cmn::MediaCodecId::H265:
						body.WriteText("hvc1");
						break;
					default:
						break;
				}
				body.WriteText("dashhlsfaid3");
				return WriteBox(data_stream, "ftyp", *body.GetData());
			}
		};

		std::shared_ptr<ov::Data> ReadSampleBytes(FILE *fp, const SampleRef &sample)
		{
			if (fp == nullptr || sample.file_offset < 0 || sample.size == 0)
			{
				return nullptr;
			}
			if (::fseeko(fp, sample.file_offset, SEEK_SET) != 0)
			{
				return nullptr;
			}
			auto data = std::make_shared<ov::Data>(sample.size);
			data->SetLength(sample.size);
			if (::fread(data->GetWritableDataAs<uint8_t>(), 1, sample.size, fp) != sample.size)
			{
				return nullptr;
			}
			return data;
		}

		int64_t EffectiveDuration(const TrackIndex &track, size_t index)
		{
			const auto &sample = track.samples[index];
			if (sample.duration > 0)
			{
				return sample.duration;
			}
			if (index + 1 < track.samples.size())
			{
				const int64_t delta = track.samples[index + 1].dts - sample.dts;
				return delta > 0 ? delta : 0;
			}
			return 0;
		}

		std::shared_ptr<MediaPacket> MakePacket(const TrackIndex &track,
												const SampleRef &sample,
												int64_t duration,
												const std::shared_ptr<const ov::Data> &bytes)
		{
			const auto media_type = track.media_track->GetMediaType();
			const bool keyframe = sample.keyframe || media_type == cmn::MediaType::Audio;
			cmn::BitstreamFormat format = cmn::BitstreamFormat::Unknown;
			cmn::PacketType packet_type = cmn::PacketType::Unknown;

			if (media_type == cmn::MediaType::Video)
			{
				format = cmn::BitstreamFormat::H264_AVCC;
				packet_type = cmn::PacketType::NALU;
			}
			else if (media_type == cmn::MediaType::Audio)
			{
				format = cmn::BitstreamFormat::AAC_RAW;
				packet_type = cmn::PacketType::RAW;
			}
			else
			{
				return nullptr;
			}

			return std::make_shared<MediaPacket>(
				media_type, track.track_id, bytes,
				sample.pts, sample.dts, duration,
				keyframe ? MediaPacketFlag::Key : MediaPacketFlag::NoFlag,
				format, packet_type);
		}

		bool AppendIndexedSample(bmff::Samples &samples,
								 const TrackIndex &track,
								 size_t index,
								 const std::shared_ptr<const ov::Data> &bytes)
		{
			const auto &sample_ref = track.samples[index];
			const int64_t duration = EffectiveDuration(track, index);
			auto packet = MakePacket(track, sample_ref, duration, bytes);
			if (packet == nullptr)
			{
				return false;
			}

			bmff::Sample sample(packet);
			const double timescale = track.media_track->GetTimeBase().GetTimescale();
			sample.timing.dts_us = bmff::TicksToUs(packet->GetDts(), timescale);
			sample.timing.pts_us = bmff::TicksToUs(packet->GetPts(), timescale);
			sample.timing.duration_us =
				bmff::TicksToUs(packet->GetDts() + packet->GetDuration(), timescale) - sample.timing.dts_us;
			sample.timing.independent =
				(packet->GetFlag() == MediaPacketFlag::Key) ||
				(track.media_track->GetMediaType() == cmn::MediaType::Audio);
			return samples.AppendSample(sample);
		}
	}  // namespace

	std::shared_ptr<ov::Data> Fmp4Materializer::MaterializeInit(const std::shared_ptr<const MediaTrack> &media_track)
	{
		if (media_track == nullptr || media_track->GetDecoderConfigurationRecord() == nullptr)
		{
			return nullptr;
		}

		FragmentWriter writer(media_track);
		ov::ByteStream stream(4096);
		if (writer.WriteInit(stream) == false)
		{
			logte("Failed to write fMP4 init for track %u", media_track->GetId());
			return nullptr;
		}
		return stream.GetDataPointer();
	}

	std::shared_ptr<Fmp4MaterializeResult> Fmp4Materializer::Materialize(
		const std::shared_ptr<const SampleIndex> &index,
		cmn::MediaType media_type,
		int64_t start_dts_90k,
		uint32_t target_duration_ms)
	{
		if (index == nullptr)
		{
			return nullptr;
		}

		FILE *fp = ::fopen(index->GetSourcePath().CStr(), "rb");
		if (fp == nullptr)
		{
			logte("Failed to open source %s", index->GetSourcePath().CStr());
			return nullptr;
		}

		auto result = Fmp4Materializer::MaterializeWithReader(
			index, media_type, start_dts_90k, target_duration_ms,
			[fp](const TrackIndex &, const SampleRef &sample) {
				return ReadSampleBytes(fp, sample);
			});

		::fclose(fp);
		return result;
	}

	std::shared_ptr<Fmp4MaterializeResult> Fmp4Materializer::MaterializeWithReader(
		const std::shared_ptr<const SampleIndex> &index,
		cmn::MediaType media_type,
		int64_t start_dts_90k,
		uint32_t target_duration_ms,
		const PayloadReader &reader)
	{
		if (index == nullptr)
		{
			return nullptr;
		}

		SegmentWindow window;
		if (ResolveSegmentWindow(*index, start_dts_90k, target_duration_ms, window) == false)
		{
			logte("Could not resolve segment window for dts %" PRId64, start_dts_90k);
			return nullptr;
		}

		return Fmp4Materializer::MaterializeSampleRangeWithReader(index, media_type, window.video_start, window.video_end, reader);
	}

	std::shared_ptr<Fmp4MaterializeResult> Fmp4Materializer::MaterializeSampleRange(
		const std::shared_ptr<const SampleIndex> &index,
		cmn::MediaType media_type,
		size_t video_begin,
		size_t video_end)
	{
		if (index == nullptr)
		{
			return nullptr;
		}

		FILE *fp = ::fopen(index->GetSourcePath().CStr(), "rb");
		if (fp == nullptr)
		{
			logte("Failed to open source %s", index->GetSourcePath().CStr());
			return nullptr;
		}

		auto result = Fmp4Materializer::MaterializeSampleRangeWithReader(
			index, media_type, video_begin, video_end,
			[fp](const TrackIndex &, const SampleRef &sample) {
				return ReadSampleBytes(fp, sample);
			});

		::fclose(fp);
		return result;
	}

	std::shared_ptr<Fmp4MaterializeResult> Fmp4Materializer::MaterializeSampleRangeWithReader(
		const std::shared_ptr<const SampleIndex> &index,
		cmn::MediaType media_type,
		size_t video_begin,
		size_t video_end,
		const PayloadReader &reader)
	{
		auto t0 = std::chrono::steady_clock::now();

		if (index == nullptr || reader == nullptr)
		{
			return nullptr;
		}

		const auto *video = index->FindTrackByMediaType(cmn::MediaType::Video);
		if (video == nullptr || video->samples.empty() || video_begin >= video_end ||
			video_end > video->samples.size())
		{
			logte("Invalid video sample range [%zu, %zu)", video_begin, video_end);
			return nullptr;
		}

		const TrackIndex *track = index->FindTrackByMediaType(media_type);
		if (track == nullptr || track->media_track == nullptr || track->samples.empty())
		{
			logte("Index has no %s track", media_type == cmn::MediaType::Video ? "video" : "audio");
			return nullptr;
		}

		const int64_t window_start_dts = video->samples[video_begin].dts;
		const int64_t range_end_dts = (video_end < video->samples.size())
										  ? video->samples[video_end].dts
										  : (video->samples.back().dts + video->samples.back().duration);

		size_t begin = 0;
		size_t end = 0;
		if (media_type == cmn::MediaType::Video)
		{
			begin = video_begin;
			end = video_end;
			// Offline parts must start on an IDR — extend/skip to the next keyframe
			// rather than emit a non-decodable fragment.
			if (video->samples[begin].keyframe == false)
			{
				while (begin < end && video->samples[begin].keyframe == false)
				{
					begin++;
				}
				if (begin >= end)
				{
					logte("Video range [%zu, %zu) has no keyframe — refusing materialize",
						  video_begin, video_end);
					return nullptr;
				}
				logtw("Snapped video materialize start %zu → %zu (keyframe)", video_begin, begin);
			}
		}
		else
		{
			while (begin < track->samples.size() && track->samples[begin].dts < window_start_dts)
			{
				begin++;
			}
			end = begin;
			while (end < track->samples.size() && track->samples[end].dts < range_end_dts)
			{
				end++;
			}
		}

		if (begin >= end)
		{
			logte("Empty %s sample window", media_type == cmn::MediaType::Video ? "video" : "audio");
			return nullptr;
		}

		const int64_t range_start_dts = (media_type == cmn::MediaType::Video)
											? video->samples[begin].dts
											: track->samples[begin].dts;

		auto samples = std::make_shared<bmff::Samples>();
		uint64_t bytes_read = 0;
		for (size_t i = begin; i < end; i++)
		{
			auto bytes = reader(*track, track->samples[i]);
			if (bytes == nullptr)
			{
				logte("Failed to read sample at offset %" PRId64, track->samples[i].file_offset);
				return nullptr;
			}
			bytes_read += bytes->GetLength();
			if (AppendIndexedSample(*samples, *track, i, bytes) == false)
			{
				logte("Failed to append sample %zu", i);
				return nullptr;
			}
		}

		FragmentWriter writer(track->media_track);
		ov::ByteStream stream(static_cast<uint32_t>(bytes_read + 4096));
		if (writer.WriteFragment(stream, samples) == false)
		{
			logte("Failed to write moof/mdat");
			return nullptr;
		}

		auto result = std::make_shared<Fmp4MaterializeResult>();
		result->data = stream.GetDataPointer();
		result->first_dts = range_start_dts;
		result->end_dts = range_end_dts;
		result->duration_ms = static_cast<double>(range_end_dts - range_start_dts) / 90.0;
		result->sample_count = end - begin;
		result->bytes_read = bytes_read;
		result->cpu_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

		logti("Materialized fMP4 %s range: %.1f ms media, %zu bytes out, %" PRIu64 " bytes in, %zu samples, cpu=%.1f ms",
			  media_type == cmn::MediaType::Video ? "video" : "audio",
			  result->duration_ms, result->data->GetLength(), bytes_read, result->sample_count, result->cpu_ms);
		return result;
	}
}  // namespace segment_cache
