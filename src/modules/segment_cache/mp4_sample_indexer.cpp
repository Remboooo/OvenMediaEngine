//==============================================================================
//
//  OvenMediaEngine - Build SampleIndex by scanning an MP4 with FFmpeg
//
//==============================================================================
#include "mp4_sample_indexer.h"

#include "throughput_limiter.h"

#include <modules/bitstream/aac/audio_specific_config.h>
#include <modules/bitstream/h264/h264_decoder_configuration_record.h>
#include <modules/ffmpeg/compat.h>

#include <cstring>
#include <cinttypes>
#include <sys/stat.h>

#define OV_LOG_TAG "SegmentCache.Indexer"

namespace segment_cache
{
	static constexpr int64_t kTimebase = 90000;

	// CreateMediaTrack() builds DCRs from FFmpeg extradata with reference-only
	// ov::Data. ov::Data::Clone() also keeps that reference. Copy bytes into
	// owned storage before AVFormatContext is closed.
	static bool SealDecoderConfigurationRecord(const std::shared_ptr<MediaTrack> &media_track)
	{
		auto dcr = media_track->GetDecoderConfigurationRecord();
		if (dcr == nullptr)
		{
			return true;
		}

		auto data = dcr->GetData();
		if (data == nullptr || data->GetLength() == 0)
		{
			return true;
		}

		auto owned = std::make_shared<ov::Data>(data->GetData(), data->GetLength(), false);

		switch (media_track->GetCodecId())
		{
			case cmn::MediaCodecId::H264: {
				auto avc = std::make_shared<AVCDecoderConfigurationRecord>();
				if (avc->Parse(owned) == false)
				{
					logte("Failed to seal H264 DCR");
					return false;
				}
				media_track->SetDecoderConfigurationRecord(avc);
				break;
			}
			case cmn::MediaCodecId::Aac: {
				auto asc = std::make_shared<AudioSpecificConfig>();
				if (asc->Parse(owned) == false)
				{
					logte("Failed to seal AAC ASC");
					return false;
				}
				media_track->SetDecoderConfigurationRecord(asc);
				break;
			}
			default:
				break;
		}

		return true;
	}

	static bool IsMp4FamilyFormat(const AVFormatContext *format_context)
	{
		if (format_context == nullptr || format_context->iformat == nullptr ||
			format_context->iformat->name == nullptr)
		{
			return false;
		}
		const char *name = format_context->iformat->name;
		// Typical FFmpeg names: "mov,mp4,m4a,3gp,3g2,mj2"
		return (::strstr(name, "mp4") != nullptr) || (::strstr(name, "mov") != nullptr) ||
			   (::strstr(name, "isom") != nullptr);
	}

	static const char *CodecName(AVCodecID id)
	{
		const char *name = ::avcodec_get_name(id);
		return (name != nullptr) ? name : "unknown";
	}

	CompatibilityResult Mp4SampleIndexer::Probe(const ov::String &file_path, bool require_audio)
	{
		CompatibilityResult result;

		AVFormatContext *fmt = nullptr;
		int err = ::avformat_open_input(&fmt, file_path.CStr(), nullptr, nullptr);
		if (err < 0)
		{
			char errbuf[AV_ERROR_MAX_STRING_SIZE] = {};
			::av_strerror(err, errbuf, sizeof(errbuf));
			result.error = ov::String::FormatString(
				"cannot open media file '%s' (%s)", file_path.CStr(), errbuf);
			logte("%s", result.error.CStr());
			return result;
		}

		std::shared_ptr<AVFormatContext> format_context(fmt, [](AVFormatContext *ctx) {
			if (ctx != nullptr)
			{
				::avformat_close_input(&ctx);
			}
		});

		err = ::avformat_find_stream_info(format_context.get(), nullptr);
		if (err < 0)
		{
			char errbuf[AV_ERROR_MAX_STRING_SIZE] = {};
			::av_strerror(err, errbuf, sizeof(errbuf));
			result.error = ov::String::FormatString(
				"cannot read stream info for '%s' (%s)", file_path.CStr(), errbuf);
			logte("%s", result.error.CStr());
			return result;
		}

		if (IsMp4FamilyFormat(format_context.get()) == false)
		{
			const char *fmt_name = (format_context->iformat != nullptr && format_context->iformat->name != nullptr)
									   ? format_context->iformat->name
									   : "unknown";
			result.error = ov::String::FormatString(
				"SegmentCache requires MP4/MOV (ISOBMFF) sources; '%s' is format '%s' "
				"(TS/MKV/MP3 and other containers are not supported)",
				file_path.CStr(), fmt_name);
			logte("%s", result.error.CStr());
			return result;
		}

		bool saw_video = false;
		bool saw_unsupported_video = false;
		bool saw_unsupported_audio = false;
		ov::String unsupported_video;
		ov::String unsupported_audio;

		for (unsigned i = 0; i < format_context->nb_streams; i++)
		{
			AVStream *stream = format_context->streams[i];
			if (stream == nullptr || stream->codecpar == nullptr)
			{
				continue;
			}

			if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
			{
				saw_video = true;
				if (stream->codecpar->codec_id == AV_CODEC_ID_H264)
				{
					result.has_h264_video = true;
				}
				else
				{
					saw_unsupported_video = true;
					unsupported_video = CodecName(stream->codecpar->codec_id);
				}
			}
			else if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
			{
				if (stream->codecpar->codec_id == AV_CODEC_ID_AAC)
				{
					result.has_aac_audio = true;
				}
				else
				{
					saw_unsupported_audio = true;
					unsupported_audio = CodecName(stream->codecpar->codec_id);
				}
			}
		}

		if (saw_video == false || result.has_h264_video == false)
		{
			if (saw_unsupported_video)
			{
				result.error = ov::String::FormatString(
					"SegmentCache requires H.264 video; '%s' has unsupported video codec '%s'",
					file_path.CStr(), unsupported_video.CStr());
			}
			else
			{
				result.error = ov::String::FormatString(
					"SegmentCache requires an H.264 video track; none found in '%s'",
					file_path.CStr());
			}
			logte("%s", result.error.CStr());
			return result;
		}

		if (saw_unsupported_video)
		{
			result.error = ov::String::FormatString(
				"SegmentCache requires H.264-only video; '%s' also contains unsupported video codec '%s'",
				file_path.CStr(), unsupported_video.CStr());
			logte("%s", result.error.CStr());
			return result;
		}

		if (require_audio)
		{
			if (result.has_aac_audio == false)
			{
				if (saw_unsupported_audio)
				{
					result.error = ov::String::FormatString(
						"SegmentCache requires AAC audio when AudioTrack=true; '%s' has unsupported audio codec '%s'",
						file_path.CStr(), unsupported_audio.CStr());
				}
				else
				{
					result.error = ov::String::FormatString(
						"SegmentCache requires AAC audio when AudioTrack=true; none found in '%s'",
						file_path.CStr());
				}
				logte("%s", result.error.CStr());
				return result;
			}
		}

		if (saw_unsupported_audio && result.has_aac_audio == false)
		{
			result.error = ov::String::FormatString(
				"SegmentCache requires AAC audio; '%s' has unsupported audio codec '%s'",
				file_path.CStr(), unsupported_audio.CStr());
			logte("%s", result.error.CStr());
			return result;
		}

		if (saw_unsupported_audio && result.has_aac_audio)
		{
			// Extra non-AAC tracks are ignored by the indexer historically; reject so
			// schedules cannot silently drop tracks and serve incomplete A/V.
			result.error = ov::String::FormatString(
				"SegmentCache requires AAC-only audio; '%s' also contains unsupported audio codec '%s'",
				file_path.CStr(), unsupported_audio.CStr());
			logte("%s", result.error.CStr());
			return result;
		}

		result.ok = true;
		return result;
	}

	std::shared_ptr<SampleIndex> Mp4SampleIndexer::Build(const ov::String &file_path,
														 double max_throughput_mbps)
	{
		// Same hard requirements as Probe (H264 + MP4; AAC when present).
		auto probe = Probe(file_path, false);
		if (probe.ok == false)
		{
			return nullptr;
		}

		AVFormatContext *fmt = nullptr;
		int err = ::avformat_open_input(&fmt, file_path.CStr(), nullptr, nullptr);
		if (err < 0)
		{
			logte("Failed to open %s (%d)", file_path.CStr(), err);
			return nullptr;
		}

		if (max_throughput_mbps > 0.0)
		{
			logti("Indexing %s with throughput cap %.1f Mbps", file_path.CStr(), max_throughput_mbps);
		}

		std::shared_ptr<AVFormatContext> format_context(fmt, [](AVFormatContext *ctx) {
			if (ctx != nullptr)
			{
				::avformat_close_input(&ctx);
			}
		});

		err = ::avformat_find_stream_info(format_context.get(), nullptr);
		if (err < 0)
		{
			logte("Failed to find stream info for %s (%d)", file_path.CStr(), err);
			return nullptr;
		}

		struct stat st {};
		if (::stat(file_path.CStr(), &st) != 0)
		{
			logte("Failed to stat %s", file_path.CStr());
			return nullptr;
		}

		auto index = std::make_shared<SampleIndex>();
		index->SetSourceIdentity(file_path, static_cast<int64_t>(st.st_size),
								 static_cast<int64_t>(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec);

		uint32_t next_track_id = 0;
		for (unsigned i = 0; i < format_context->nb_streams; i++)
		{
			AVStream *stream = format_context->streams[i];
			if (stream == nullptr || stream->codecpar == nullptr)
			{
				continue;
			}

			if (stream->codecpar->codec_type != AVMEDIA_TYPE_VIDEO &&
				stream->codecpar->codec_type != AVMEDIA_TYPE_AUDIO)
			{
				continue;
			}

			if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
				stream->codecpar->codec_id != AV_CODEC_ID_H264)
			{
				logte("Unsupported video codec '%s' in %s (SegmentCache requires H.264)",
					  CodecName(stream->codecpar->codec_id), file_path.CStr());
				return nullptr;
			}

			if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO &&
				stream->codecpar->codec_id != AV_CODEC_ID_AAC)
			{
				logte("Unsupported audio codec '%s' in %s (SegmentCache requires AAC)",
					  CodecName(stream->codecpar->codec_id), file_path.CStr());
				return nullptr;
			}

			auto media_track = ffmpeg::compat::CreateMediaTrack(stream);
			if (media_track == nullptr)
			{
				logte("CreateMediaTrack failed for stream %u in %s", i, file_path.CStr());
				return nullptr;
			}

			media_track->SetId(next_track_id);
			media_track->SetTimeBase(1, kTimebase);

			if (SealDecoderConfigurationRecord(media_track) == false)
			{
				logte("Failed to own decoder config for stream %u in %s", i, file_path.CStr());
				return nullptr;
			}

			TrackIndex track_index;
			track_index.track_id = next_track_id++;
			track_index.ffmpeg_stream_index = static_cast<int>(i);
			track_index.media_track = media_track;
			index->GetTracks().push_back(std::move(track_index));
		}

		if (index->FindTrackByMediaType(cmn::MediaType::Video) == nullptr)
		{
			logte("No usable video track in %s", file_path.CStr());
			return nullptr;
		}

		AVPacket *packet = ::av_packet_alloc();
		if (packet == nullptr)
		{
			logte("av_packet_alloc failed");
			return nullptr;
		}

		ThroughputLimiter limiter(max_throughput_mbps);
		uint64_t since_pace = 0;
		uint64_t video_packets_seen = 0;
		uint64_t video_packets_skipped_no_pos = 0;

		while (true)
		{
			err = ::av_read_frame(format_context.get(), packet);
			if (err == AVERROR_EOF)
			{
				break;
			}
			if (err < 0)
			{
				logte("av_read_frame failed on %s (%d)", file_path.CStr(), err);
				::av_packet_free(&packet);
				return nullptr;
			}

			auto *track_index = index->FindTrackByFfmpegIndex(packet->stream_index);
			if (track_index == nullptr)
			{
				::av_packet_unref(packet);
				continue;
			}

			const bool is_video =
				track_index->media_track != nullptr &&
				track_index->media_track->GetMediaType() == cmn::MediaType::Video;
			if (is_video)
			{
				video_packets_seen++;
			}

			if (packet->pos < 0 || packet->size <= 0)
			{
				if (is_video)
				{
					video_packets_skipped_no_pos++;
				}
				::av_packet_unref(packet);
				continue;
			}

			AVStream *stream = format_context->streams[packet->stream_index];
			AVRational src_tb = stream->time_base;
			AVRational dst_tb{1, static_cast<int>(kTimebase)};

			SampleRef sample;
			sample.file_offset = packet->pos;
			sample.size = static_cast<uint32_t>(packet->size);
			sample.pts = (packet->pts != AV_NOPTS_VALUE)
							 ? ::av_rescale_q(packet->pts, src_tb, dst_tb)
							 : ::av_rescale_q(packet->dts, src_tb, dst_tb);
			sample.dts = (packet->dts != AV_NOPTS_VALUE)
							 ? ::av_rescale_q(packet->dts, src_tb, dst_tb)
							 : sample.pts;
			sample.duration = (packet->duration > 0)
								  ? ::av_rescale_q(packet->duration, src_tb, dst_tb)
								  : 0;
			sample.keyframe = (packet->flags & AV_PKT_FLAG_KEY) != 0;

			track_index->samples.push_back(sample);

			since_pace += static_cast<uint64_t>(packet->size);
			// Pace every ~256 KiB so we don't sleep per tiny packet.
			if (since_pace >= (256ull * 1024ull))
			{
				limiter.Account(since_pace);
				since_pace = 0;
			}

			::av_packet_unref(packet);
		}

		if (since_pace > 0)
		{
			limiter.Account(since_pace);
		}

		::av_packet_free(&packet);

		const auto *video = index->FindTrackByMediaType(cmn::MediaType::Video);
		if (video == nullptr || video->samples.empty())
		{
			if (video_packets_seen > 0 &&
				video_packets_skipped_no_pos * 2 >= video_packets_seen)
			{
				logte("No video samples indexed in %s: %" PRIu64 "/%" PRIu64
					  " video packets lacked byte offsets (SegmentCache requires seekable MP4 with reliable packet positions)",
					  file_path.CStr(), video_packets_skipped_no_pos, video_packets_seen);
			}
			else
			{
				logte("No video samples indexed in %s", file_path.CStr());
			}
			return nullptr;
		}

		if (video_packets_seen > 0 &&
			video_packets_skipped_no_pos * 2 >= video_packets_seen)
		{
			logte("Unreliable sample offsets in %s: skipped %" PRIu64 "/%" PRIu64
				  " video packets without byte positions (re-mux to MP4 with +faststart)",
				  file_path.CStr(), video_packets_skipped_no_pos, video_packets_seen);
			return nullptr;
		}

		logti("Indexed %s: %zu tracks, %zu video samples",
			  file_path.CStr(), index->GetTracks().size(), video->samples.size());
		return index;
	}
}  // namespace segment_cache
