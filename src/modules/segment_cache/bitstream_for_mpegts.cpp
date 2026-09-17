//==============================================================================
//
//  OvenMediaEngine - Convert MP4 sample payloads to MPEG-TS-friendly bitstreams
//
//==============================================================================
#include "bitstream_for_mpegts.h"

#include <modules/bitstream/aac/aac_converter.h>
#include <modules/bitstream/aac/audio_specific_config.h>
#include <modules/bitstream/h264/h264_common.h>
#include <modules/bitstream/h264/h264_decoder_configuration_record.h>
#include <modules/bitstream/h264/h264_parser.h>
#include <modules/bitstream/nalu/nal_stream_converter.h>

#define OV_LOG_TAG "SegmentCache.Bitstream"

namespace segment_cache
{
	static const uint8_t kStartCode[4] = {0x00, 0x00, 0x00, 0x01};

	static std::shared_ptr<ov::Data> ConvertAvccToAnnexbWithParamSets(
		const std::shared_ptr<const ov::Data> &avcc,
		const std::shared_ptr<const AVCDecoderConfigurationRecord> &avc_config,
		bool keyframe)
	{
		auto annexb = NalStreamConverter::ConvertXvccToAnnexb(avcc);
		if (annexb == nullptr)
		{
			return nullptr;
		}

		if (keyframe == false || avc_config == nullptr)
		{
			return annexb;
		}

		bool has_sps = false;
		bool has_pps = false;
		bool has_idr = false;

		const auto *buf = annexb->GetDataAs<uint8_t>();
		size_t len = annexb->GetLength();
		size_t i = 0;
		while (i + 4 < len)
		{
			size_t sc = 0;
			if (buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 0 && buf[i + 3] == 1)
			{
				sc = 4;
			}
			else if (buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1)
			{
				sc = 3;
			}
			else
			{
				i++;
				continue;
			}

			size_t nal_start = i + sc;
			if (nal_start >= len)
			{
				break;
			}

			H264NalUnitHeader header;
			if (H264Parser::ParseNalUnitHeader(buf + nal_start, 1, header))
			{
				auto type = header.GetNalUnitType();
				if (type == H264NalUnitType::Sps)
				{
					has_sps = true;
				}
				else if (type == H264NalUnitType::Pps)
				{
					has_pps = true;
				}
				else if (type == H264NalUnitType::IdrSlice)
				{
					has_idr = true;
				}
			}

			i = nal_start + 1;
		}

		if (has_idr && (has_sps == false || has_pps == false))
		{
			auto sps = avc_config->GetSPSData(0);
			auto pps = avc_config->GetPPSData(0);
			if (sps == nullptr || pps == nullptr)
			{
				logte("Missing SPS/PPS in AVC DCR");
				return nullptr;
			}

			auto with_ps = std::make_shared<ov::Data>(annexb->GetLength() + sps->GetLength() + pps->GetLength() + 16);
			if (has_sps == false)
			{
				with_ps->Append(kStartCode, sizeof(kStartCode));
				with_ps->Append(sps);
			}
			if (has_pps == false)
			{
				with_ps->Append(kStartCode, sizeof(kStartCode));
				with_ps->Append(pps);
			}
			with_ps->Append(annexb);
			return with_ps;
		}

		return annexb;
	}

	std::shared_ptr<MediaPacket> ConvertSampleForMpegTs(
		const std::shared_ptr<const MediaTrack> &track,
		uint32_t track_id,
		const std::shared_ptr<const ov::Data> &payload,
		int64_t pts,
		int64_t dts,
		int64_t duration,
		bool keyframe)
	{
		if (track == nullptr || payload == nullptr)
		{
			return nullptr;
		}

		if (track->GetMediaType() == cmn::MediaType::Video && track->GetCodecId() == cmn::MediaCodecId::H264)
		{
			auto avc = track->GetDecoderConfigurationRecordAs<AVCDecoderConfigurationRecord>();
			auto annexb = ConvertAvccToAnnexbWithParamSets(payload, avc, keyframe);
			if (annexb == nullptr)
			{
				return nullptr;
			}

			auto flag = keyframe ? MediaPacketFlag::Key : MediaPacketFlag::NoFlag;
			return std::make_shared<MediaPacket>(cmn::MediaType::Video, track_id, annexb, pts, dts, duration,
												 flag, cmn::BitstreamFormat::H264_ANNEXB, cmn::PacketType::NALU);
		}

		if (track->GetMediaType() == cmn::MediaType::Audio && track->GetCodecId() == cmn::MediaCodecId::Aac)
		{
			auto asc = track->GetDecoderConfigurationRecordAs<AudioSpecificConfig>();
			if (asc == nullptr)
			{
				logte("Audio track has no AudioSpecificConfig");
				return nullptr;
			}

			auto adts = AacConverter::ConvertRawToAdts(payload, asc);
			if (adts == nullptr)
			{
				logte("AAC RAW->ADTS conversion failed");
				return nullptr;
			}

			return std::make_shared<MediaPacket>(cmn::MediaType::Audio, track_id, adts, pts, dts, duration,
												 MediaPacketFlag::Key, cmn::BitstreamFormat::AAC_ADTS, cmn::PacketType::RAW);
		}

		logte("Unsupported track type/codec for MPEG-TS materialize");
		return nullptr;
	}
}  // namespace segment_cache
