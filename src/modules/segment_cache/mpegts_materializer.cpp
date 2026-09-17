//==============================================================================
//
//  OvenMediaEngine - Materialize one MPEG-TS HLS segment from a SampleIndex
//
//==============================================================================
#include "mpegts_materializer.h"

#include "bitstream_for_mpegts.h"

#include <modules/containers/mpegts/mpegts_packager.h>
#include <modules/containers/mpegts/mpegts_packetizer.h>
#include <modules/ffmpeg/compat.h>

#include <chrono>
#include <cstdio>
#include <map>
#include <utility>

#define OV_LOG_TAG "SegmentCache.MpegTs"

namespace segment_cache
{
	namespace
	{
		constexpr int64_t kTimebase = 90000;

		class SegmentCollector : public mpegts::PackagerSink
		{
		public:
			void OnSegmentCreated(const ov::String &packager_id, const std::shared_ptr<base::modules::Segment> &segment) override
			{
				_segments.push_back(segment);
			}

			void OnSegmentDeleted(const ov::String &packager_id, const std::shared_ptr<base::modules::Segment> &segment) override
			{
			}

			std::vector<std::shared_ptr<base::modules::Segment>> _segments;
		};

		std::shared_ptr<ov::Data> ReadSampleBytes(FILE *fp, const SampleRef &sample, uint64_t &bytes_read)
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
			size_t n = ::fread(data->GetWritableDataAs<uint8_t>(), 1, sample.size, fp);
			if (n != sample.size)
			{
				return nullptr;
			}
			bytes_read += n;
			return data;
		}

		size_t FindKeyframeAtOrAfter(const TrackIndex &video, int64_t start_dts)
		{
			for (size_t i = 0; i < video.samples.size(); i++)
			{
				if (video.samples[i].keyframe && video.samples[i].dts >= start_dts)
				{
					return i;
				}
			}
			return video.samples.size();
		}

		size_t FindEndKeyframe(const TrackIndex &video, size_t start_index, int64_t end_dts)
		{
			for (size_t i = start_index + 1; i < video.samples.size(); i++)
			{
				if (video.samples[i].keyframe && video.samples[i].dts >= end_dts)
				{
					return i;
				}
			}
			return video.samples.size();
		}

		using DemuxKey = int64_t;  // sample file_offset from the index

		// Independently packaged HLS segments restart continuity counters at 0.
		// Mark discontinuity_indicator on the first adaptation-field packet of each
		// PID so demuxers (ffmpeg) do not treat the CC reset as corruption.
		size_t MarkTsDiscontinuityIndicators(ov::Data &ts)
		{
			uint8_t *data = ts.GetWritableDataAs<uint8_t>();
			const size_t len = ts.GetLength();
			if (data == nullptr || len < 188)
			{
				return 0;
			}

			bool seen[0x2000] = {};
			size_t marked = 0;

			for (size_t off = 0; off + 188 <= len; off += 188)
			{
				if (data[off] != 0x47)
				{
					continue;
				}

				const uint16_t pid = static_cast<uint16_t>(((data[off + 1] & 0x1f) << 8) | data[off + 2]);
				if (pid >= 0x2000 || seen[pid])
				{
					continue;
				}

				const uint8_t afc = (data[off + 3] >> 4) & 0x03;
				if (afc != 2 && afc != 3)
				{
					// Payload-only (typical PAT/PMT): leave for a later AF packet on this PID.
					continue;
				}

				const uint8_t af_len = data[off + 4];
				if (af_len == 0)
				{
					continue;
				}

				// Bit 7 of the adaptation flags byte: discontinuity_indicator.
				data[off + 5] = static_cast<uint8_t>(data[off + 5] | 0x80);
				seen[pid] = true;
				marked++;
			}

			return marked;
		}

		// Demux from the start; assign payloads to indexed samples by ordinal within
		// each track. Avoids unreliable AVPacket.pos after mid-file operations.
		std::map<DemuxKey, std::shared_ptr<ov::Data>> LoadDemuxPayloadsForWindow(
			const ov::String &path,
			const SampleIndex &index,
			const SegmentWindow &window,
			uint64_t &bytes_read)
		{
			std::map<DemuxKey, std::shared_ptr<ov::Data>> out;

			const auto *video = index.FindTrackByMediaType(cmn::MediaType::Video);
			const auto *audio = index.FindTrackByMediaType(cmn::MediaType::Audio);
			if (video == nullptr)
			{
				return {};
			}

			AVFormatContext *fmt = nullptr;
			if (::avformat_open_input(&fmt, path.CStr(), nullptr, nullptr) < 0)
			{
				return {};
			}
			std::shared_ptr<AVFormatContext> format_context(fmt, [](AVFormatContext *ctx) {
				if (ctx != nullptr)
				{
					::avformat_close_input(&ctx);
				}
			});

			if (::avformat_find_stream_info(format_context.get(), nullptr) < 0)
			{
				return {};
			}

			AVPacket *packet = ::av_packet_alloc();
			if (packet == nullptr)
			{
				return {};
			}

			size_t video_ordinal = 0;
			size_t audio_ordinal = 0;
			const size_t video_needed = window.video_end;
			size_t audio_needed = 0;
			if (audio != nullptr)
			{
				while (audio_needed < audio->samples.size() && audio->samples[audio_needed].dts < window.end_dts)
				{
					audio_needed++;
				}
			}

			while (::av_read_frame(format_context.get(), packet) >= 0)
			{
				auto *track = index.FindTrackByFfmpegIndex(packet->stream_index);
				// Keep ordinals aligned with Mp4SampleIndexer (which skips pos < 0).
				if (track == nullptr || packet->size <= 0 || packet->pos < 0)
				{
					::av_packet_unref(packet);
					continue;
				}

				if (track->media_track->GetMediaType() == cmn::MediaType::Video)
				{
					if (video_ordinal < video->samples.size())
					{
						if (static_cast<uint32_t>(packet->size) != video->samples[video_ordinal].size)
						{
							logte("Demux/index size mismatch video ordinal %zu: demux=%d index=%u",
								  video_ordinal, packet->size, video->samples[video_ordinal].size);
							::av_packet_free(&packet);
							return {};
						}
						if (video_ordinal >= window.video_start && video_ordinal < window.video_end)
						{
							auto data = std::make_shared<ov::Data>(packet->data, packet->size);  // copy
							bytes_read += static_cast<uint64_t>(packet->size);
							out[video->samples[video_ordinal].file_offset] = data;
						}
						video_ordinal++;
					}
				}
				else if (audio != nullptr && track->media_track->GetMediaType() == cmn::MediaType::Audio)
				{
					if (audio_ordinal < audio->samples.size())
					{
						if (static_cast<uint32_t>(packet->size) != audio->samples[audio_ordinal].size)
						{
							logte("Demux/index size mismatch audio ordinal %zu: demux=%d index=%u",
								  audio_ordinal, packet->size, audio->samples[audio_ordinal].size);
							::av_packet_free(&packet);
							return {};
						}
						if (audio->samples[audio_ordinal].dts >= window.start_dts &&
							audio->samples[audio_ordinal].dts < window.end_dts)
						{
							auto data = std::make_shared<ov::Data>(packet->data, packet->size);  // copy
							bytes_read += static_cast<uint64_t>(packet->size);
							out[audio->samples[audio_ordinal].file_offset] = data;
						}
						audio_ordinal++;
					}
				}

				::av_packet_unref(packet);

				if (video_ordinal >= video_needed && (audio == nullptr || audio_ordinal >= audio_needed))
				{
					break;
				}
			}

			::av_packet_free(&packet);
			return out;
		}
	}  // namespace

	int64_t JoinDtsAfterLoops(int64_t item_duration_ms, uint32_t full_loops, int64_t offset_ms)
	{
		if (item_duration_ms <= 0)
		{
			return 0;
		}
		int64_t elapsed = static_cast<int64_t>(full_loops) * item_duration_ms + offset_ms;
		if (elapsed < 0)
		{
			elapsed = 0;
		}
		int64_t position_ms = elapsed % item_duration_ms;
		return position_ms * 90;  // 90 kHz
	}

	bool ResolveSegmentWindow(const SampleIndex &index, int64_t start_dts_90k, uint32_t target_duration_ms, SegmentWindow &out)
	{
		const auto *video = index.FindTrackByMediaType(cmn::MediaType::Video);
		if (video == nullptr || video->samples.empty())
		{
			return false;
		}

		size_t v_start = FindKeyframeAtOrAfter(*video, start_dts_90k);
		if (v_start >= video->samples.size())
		{
			return false;
		}

		int64_t target_end_dts = video->samples[v_start].dts + (static_cast<int64_t>(target_duration_ms) * 90);
		size_t v_end = FindEndKeyframe(*video, v_start, target_end_dts);
		if (v_end <= v_start)
		{
			return false;
		}

		out.video_start = v_start;
		out.video_end = v_end;
		out.start_dts = video->samples[v_start].dts;
		out.end_dts = (v_end < video->samples.size())
						  ? video->samples[v_end].dts
						  : (video->samples.back().dts + video->samples.back().duration);
		return true;
	}

	std::shared_ptr<MpegTsMaterializeResult> MpegTsMaterializer::MaterializeWithReader(
		const std::shared_ptr<const SampleIndex> &index,
		int64_t start_dts_90k,
		uint32_t target_duration_ms,
		const PayloadReader &reader)
	{
		auto t0 = std::chrono::steady_clock::now();

		if (index == nullptr || reader == nullptr)
		{
			return nullptr;
		}

		const auto *video = index->FindTrackByMediaType(cmn::MediaType::Video);
		const auto *audio = index->FindTrackByMediaType(cmn::MediaType::Audio);
		if (video == nullptr || video->media_track == nullptr || video->samples.empty())
		{
			logte("Index has no video samples");
			return nullptr;
		}

		SegmentWindow window;
		if (ResolveSegmentWindow(*index, start_dts_90k, target_duration_ms, window) == false)
		{
			logte("Could not resolve segment window for dts %" PRId64, start_dts_90k);
			return nullptr;
		}

		mpegts::Packager::Config packager_config;
		packager_config.target_duration_ms = target_duration_ms;
		packager_config.max_segment_count = 4;
		packager_config.stream_id_meta = "segcache";

		auto packager = std::make_shared<mpegts::Packager>("segcache", packager_config);
		auto collector = std::make_shared<SegmentCollector>();
		packager->AddSink(collector);

		mpegts::Packetizer packetizer;
		if (packetizer.AddTrack(video->media_track) == false)
		{
			logte("AddTrack video failed");
			return nullptr;
		}
		if (audio != nullptr && audio->media_track != nullptr)
		{
			if (packetizer.AddTrack(audio->media_track) == false)
			{
				logte("AddTrack audio failed");
				return nullptr;
			}
		}

		packetizer.AddSink(packager);
		if (packetizer.Start() == false)
		{
			logte("Packetizer Start failed");
			return nullptr;
		}

		size_t video_count = 0;
		size_t audio_count = 0;
		uint64_t bytes_read = 0;

		size_t vi = window.video_start;
		size_t ai = 0;
		if (audio != nullptr)
		{
			while (ai < audio->samples.size() && audio->samples[ai].dts < window.start_dts)
			{
				ai++;
			}
		}

		auto emit_video = [&](size_t idx) -> bool {
			const auto &sample = video->samples[idx];
			auto bytes = reader(*video, sample);
			if (bytes == nullptr)
			{
				logte("Failed to read video sample at offset %" PRId64, sample.file_offset);
				return false;
			}
			bytes_read += bytes->GetLength();
			auto media_packet = ConvertSampleForMpegTs(video->media_track, video->track_id, bytes,
													   sample.pts, sample.dts, sample.duration, sample.keyframe);
			if (media_packet == nullptr)
			{
				return false;
			}
			if (packetizer.AppendFrame(media_packet) == false && sample.keyframe)
			{
				logte("AppendFrame failed for video keyframe");
				return false;
			}
			video_count++;
			return true;
		};

		auto emit_audio = [&](size_t idx) -> bool {
			const auto &sample = audio->samples[idx];
			auto bytes = reader(*audio, sample);
			if (bytes == nullptr)
			{
				logte("Failed to read audio sample at offset %" PRId64, sample.file_offset);
				return false;
			}
			bytes_read += bytes->GetLength();
			auto media_packet = ConvertSampleForMpegTs(audio->media_track, audio->track_id, bytes,
													   sample.pts, sample.dts, sample.duration, true);
			if (media_packet == nullptr)
			{
				return false;
			}
			packetizer.AppendFrame(media_packet);
			audio_count++;
			return true;
		};

		while (vi < window.video_end ||
			   (audio != nullptr && ai < audio->samples.size() && audio->samples[ai].dts < window.end_dts))
		{
			bool take_video = vi < window.video_end;
			bool take_audio = audio != nullptr && ai < audio->samples.size() && audio->samples[ai].dts < window.end_dts;

			if (take_video && take_audio)
			{
				if (audio->samples[ai].dts <= video->samples[vi].dts)
				{
					if (emit_audio(ai++) == false)
					{
						return nullptr;
					}
				}
				else if (emit_video(vi++) == false)
				{
					return nullptr;
				}
			}
			else if (take_video)
			{
				if (emit_video(vi++) == false)
				{
					return nullptr;
				}
			}
			else if (take_audio)
			{
				if (emit_audio(ai++) == false)
				{
					return nullptr;
				}
			}
			else
			{
				break;
			}
		}

		packager->Flush();
		packetizer.Stop();

		if (collector->_segments.empty())
		{
			auto last = packager->GetLastSegment();
			if (last == nullptr || last->GetData() == nullptr)
			{
				logte("No TS segment produced (video_samples=%zu audio_samples=%zu)", video_count, audio_count);
				return nullptr;
			}
			collector->_segments.push_back(last);
		}

		auto segment = collector->_segments.front();
		auto data = segment->GetData();
		if (data == nullptr || data->GetLength() < 188)
		{
			logte("Segment data missing or too small");
			return nullptr;
		}

		// Clone before mutating — packager may still hold the original buffer.
		auto mutable_data = data->Clone();
		if (mutable_data == nullptr)
		{
			mutable_data = std::const_pointer_cast<ov::Data>(data);
		}
		const size_t disc_marked = MarkTsDiscontinuityIndicators(*mutable_data);

		auto result = std::make_shared<MpegTsMaterializeResult>();
		result->data = mutable_data;
		result->duration_ms = static_cast<double>(window.end_dts - window.start_dts) / 90.0;
		result->first_dts = window.start_dts;
		result->end_dts = window.end_dts;
		result->video_samples = video_count;
		result->audio_samples = audio_count;
		result->bytes_read = bytes_read;
		result->cpu_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

		logti("Materialized TS segment: %.1f ms media, %zu bytes out, %" PRIu64 " bytes in, %zuV/%zuA, disc_pids=%zu, cpu=%.1f ms",
			  result->duration_ms, mutable_data->GetLength(), bytes_read, video_count, audio_count, disc_marked, result->cpu_ms);
		return result;
	}

	std::shared_ptr<MpegTsMaterializeResult> MpegTsMaterializer::Materialize(
		const std::shared_ptr<const SampleIndex> &index,
		int64_t start_dts_90k,
		uint32_t target_duration_ms)
	{
		FILE *fp = ::fopen(index->GetSourcePath().CStr(), "rb");
		if (fp == nullptr)
		{
			logte("Failed to open source %s", index->GetSourcePath().CStr());
			return nullptr;
		}

		uint64_t ignored = 0;
		auto result = MaterializeWithReader(
			index, start_dts_90k, target_duration_ms,
			[fp, &ignored](const TrackIndex &, const SampleRef &sample) {
				return ReadSampleBytes(fp, sample, ignored);
			});

		// bytes_read is counted inside MaterializeWithReader from payload lengths;
		// keep that. Close file.
		::fclose(fp);
		return result;
	}

	std::shared_ptr<MpegTsMaterializeResult> MpegTsMaterializer::MaterializeFromDemux(
		const std::shared_ptr<const SampleIndex> &index,
		int64_t start_dts_90k,
		uint32_t target_duration_ms)
	{
		SegmentWindow window;
		if (ResolveSegmentWindow(*index, start_dts_90k, target_duration_ms, window) == false)
		{
			return nullptr;
		}

		uint64_t demux_bytes = 0;
		auto payloads = LoadDemuxPayloadsForWindow(index->GetSourcePath(), *index, window, demux_bytes);
		if (payloads.empty())
		{
			logte("Demux produced no payloads for window");
			return nullptr;
		}

		auto result = MaterializeWithReader(
			index, start_dts_90k, target_duration_ms,
			[&payloads](const TrackIndex &track, const SampleRef &sample) -> std::shared_ptr<ov::Data> {
				auto it = payloads.find(sample.file_offset);
				if (it == payloads.end())
				{
					return nullptr;
				}
				return it->second;
			});

		if (result != nullptr)
		{
			// Report demux-side reads (may be slightly higher due to seek preroll).
			result->bytes_read = demux_bytes;
		}
		return result;
	}
}  // namespace segment_cache
