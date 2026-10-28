//==============================================================================
//
//  OvenMediaEngine
//
//  Created by Hyunjun Jang
//  Copyright (c) 2019 AirenSoft. All rights reserved.
//
//==============================================================================
#pragma once

#include "publisher.h"

namespace cfg
{
	namespace vhost
	{
		namespace app
		{
			namespace pub
			{
				struct PlayoutDelay : public Item
				{
				protected:
					int _min = 0;
					int _max = 0;
					bool _adaptive = false;

				public:
					CFG_DECLARE_CONST_REF_GETTER_OF(GetMin, _min)
					CFG_DECLARE_CONST_REF_GETTER_OF(GetMax, _max)
					CFG_DECLARE_CONST_REF_GETTER_OF(IsAdaptive, _adaptive)

				protected:
					void MakeList() override
					{
						Register("Min", &_min);
						Register("Max", &_max);
						Register<Optional>("Adaptive", &_adaptive);
					}
				};

				// The hold range of <JitterBuffer> (ms): it holds each frame at least Min after it arrived and
				// adapts up to Max to the source's jitter
				struct JitterBufferDelay : public Item
				{
				protected:
					int _min = 20;
					int _max = 500;

				public:
					CFG_DECLARE_CONST_REF_GETTER_OF(GetMin, _min)
					CFG_DECLARE_CONST_REF_GETTER_OF(GetMax, _max)

				protected:
					std::shared_ptr<ConfigError> Validate() const
					{
						if (_min < 0 || _max < _min || _max > 5000)
						{
							return CreateConfigErrorPtr("JitterBufferDelay must have 0 <= Min <= Max <= 5000 (got: %d:%d)", _min, _max);
						}
						return nullptr;
					}

					void MakeList() override
					{
						Register<Optional>("Min", &_min, nullptr, [=]() { return Validate(); });
						Register<Optional>("Max", &_max, nullptr, [=]() { return Validate(); });
					}
				};

				struct WebrtcPublisher : public Publisher
				{
					PublisherType GetType() const override
					{
						return PublisherType::Webrtc;
					}

					CFG_DECLARE_CONST_REF_GETTER_OF(GetTimeout, _timeout)
					CFG_DECLARE_CONST_REF_GETTER_OF(IsRtxEnabled, _rtx)
					CFG_DECLARE_CONST_REF_GETTER_OF(IsUlpfecEnalbed, _ulpfec)
					CFG_DECLARE_CONST_REF_GETTER_OF(IsJitterBufferEnabled, _jitter_buffer)
					CFG_DECLARE_CONST_REF_GETTER_OF(GetJitterBufferDelay, _jitter_buffer_delay)
					CFG_DECLARE_CONST_REF_GETTER_OF(IsSendPacingEnabled, _send_pacing)
					CFG_DECLARE_CONST_REF_GETTER_OF(IsVideoSheddingEnabled, _video_shedding)
					CFG_DECLARE_CONST_REF_GETTER_OF(GetPlayoutDelay, _playout_delay)
					CFG_DECLARE_CONST_REF_GETTER_OF(GetBandwidthEstimationType, _bandwidth_estimation_type)
					CFG_DECLARE_CONST_REF_GETTER_OF(ShouldCreateDefaultPlaylist, _create_default_playlist)

				protected:
					void MakeList() override
					{
						Publisher::MakeList();

						Register<Optional>("Timeout", &_timeout);
						Register<Optional>("Rtx", &_rtx);
						Register<Optional>("Ulpfec", &_ulpfec);
						Register<Optional>("PlayoutDelay", &_playout_delay);
						Register<Optional>("JitterBuffer", &_jitter_buffer);
						Register<Optional>("JitterBufferDelay", &_jitter_buffer_delay);
						Register<Optional>("SendPacing", &_send_pacing);
						Register<Optional>("VideoShedding", &_video_shedding);
						Register<Optional>("CreateDefaultPlaylist", &_create_default_playlist);
						Register<Optional>("BandwidthEstimation", &_bwe, [=]() -> std::shared_ptr<ConfigError> { return nullptr; }, [=]() -> std::shared_ptr<ConfigError> {
								if (_bwe.UpperCaseString() == "REMB")
								{
									_bandwidth_estimation_type = RtcBWEType::REMB;
								}
								else if (_bwe.UpperCaseString() == "TRANSPORTCC")
								{
									_bandwidth_estimation_type = RtcBWEType::TransportCc;
								}
								else if (_bwe.UpperCaseString() == "ALL" || _bwe.IsEmpty())
								{
									_bandwidth_estimation_type = RtcBWEType::All;
								}
								else
								{
									return CreateConfigErrorPtr("Invalid value for BWE. Valid values are 'TransportCC' or 'REMB'");
								}

								return nullptr; });
					}

					int _timeout = 30000;
					bool _rtx	 = false;
					bool _ulpfec = false;
					ov::String _bwe;

					RtcBWEType _bandwidth_estimation_type = RtcBWEType::All;
					PlayoutDelay _playout_delay;
					bool _jitter_buffer			  = false;
					JitterBufferDelay _jitter_buffer_delay;
					bool _send_pacing			  = false;
					bool _video_shedding		  = false;
					bool _create_default_playlist = true;
				};
			}  // namespace pub
		}  // namespace app
	}  // namespace vhost
}  // namespace cfg