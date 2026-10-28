//==============================================================================
//
//  OvenMediaEngine
//
//  Created by Getroot
//  Copyright (c) 2018 AirenSoft. All rights reserved.
//
//==============================================================================
#include "rtc_session.h"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <optional>

// In a RED-wrapped ULPFEC payload: [RED header (1)][E L P X CC (1)][M PT (1)][SN base (2)]...
static constexpr size_t kFecSnBaseOffset = 3;
#include <thread>
#include <utility>

#include "base/info/stream.h"
#include "modules/rtp_rtcp/rtcp_info/nack.h"
#include "modules/rtp_rtcp/rtcp_info/remb.h"
#include "modules/rtp_rtcp/rtcp_info/transport_cc.h"
#include "rtc_application.h"
#include "rtc_common_types.h"
#include "rtc_private.h"
#include "rtc_stream.h"
#include "webrtc_publisher.h"

std::shared_ptr<RtcSession> RtcSession::Create(const std::shared_ptr<WebRtcPublisher> &publisher,
											   const std::shared_ptr<pub::Application> &application,
											   const std::shared_ptr<pub::Stream> &stream,
											   const ov::String &file_name,
											   const std::shared_ptr<const SessionDescription> &offer_sdp,
											   const std::shared_ptr<const SessionDescription> &peer_sdp,
											   const std::shared_ptr<IcePort> &ice_port,
											   session_id_t ice_session_id,
											   const std::shared_ptr<http::svr::ws::WebSocketSession> &ws_session,
											   const ov::String &initial_rendition)
{
	// Session Id of the offer sdp is unique value
	auto session_info = info::Session(*std::static_pointer_cast<info::Stream>(stream), offer_sdp->GetSessionId());
	auto session	  = std::make_shared<RtcSession>(session_info, publisher, application, stream, file_name, offer_sdp, peer_sdp, ice_port, ice_session_id, ws_session, initial_rendition);
	if (!session->Start())
	{
		return nullptr;
	}
	return session;
}

RtcSession::RtcSession(const info::Session &session_info,
					   const std::shared_ptr<WebRtcPublisher> &publisher,
					   const std::shared_ptr<pub::Application> &application,
					   const std::shared_ptr<pub::Stream> &stream,
					   const ov::String &file_name,
					   const std::shared_ptr<const SessionDescription> &offer_sdp,
					   const std::shared_ptr<const SessionDescription> &peer_sdp,
					   const std::shared_ptr<IcePort> &ice_port,
					   session_id_t ice_session_id,
					   const std::shared_ptr<http::svr::ws::WebSocketSession> &ws_session,
					   const ov::String &initial_rendition)
	: Session(session_info, application, stream), Node(NodeType::Edge)
{
	_publisher		= publisher;
	_offer_sdp		= offer_sdp;
	_peer_sdp		= peer_sdp;
	_ice_port		= ice_port;
	_ice_session_id = ice_session_id;
	_ws_session		= ws_session;
	_file_name		= file_name;
	_initial_rendition = initial_rendition;
}

RtcSession::~RtcSession()
{
	Stop();
	logtt("RtcSession(%d) has been terminated finally", GetId());
}

bool RtcSession::Start()
{
	// start and stop must be called independently.
	std::lock_guard<std::shared_mutex> lock(_start_stop_lock);

	if (pub::Session::GetState() != SessionState::Ready)
	{
		return false;
	}

	logtd("[WebRTC Publisher] OfferSDP");
	logtd("%s\n", _offer_sdp->ToString().CStr());
	logtd("[WebRTC Publisher] AnswerSDP");
	logtd("%s", _peer_sdp->ToString().CStr());

	auto offer_media_desc_list = _offer_sdp->GetMediaList();
	auto peer_media_desc_list  = _peer_sdp->GetMediaList();

	if (offer_media_desc_list.size() != peer_media_desc_list.size())
	{
		logte("m= line of answer does not correspond with offer");
		return false;
	}

	// Create nodes

	_rtp_rtcp = std::make_shared<RtpRtcp>(RtpRtcpInterface::GetSharedPtr());
	_srtp_transport = std::make_shared<SrtpTransport>();

	_dtls_transport = std::make_shared<DtlsTransport>();
	std::shared_ptr<RtcApplication> application = std::static_pointer_cast<RtcApplication>(GetApplication());
	_dtls_transport->SetLocalCertificate(application->GetCertificate());

	// Bind the peer fingerprint advertised in the remote (player) SDP so the
	// DTLS handshake refuses any peer whose certificate digest does not match.
	ov::String peer_fingerprint_algorithm = _peer_sdp->GetFingerprintAlgorithm();
	ov::String peer_fingerprint_value = _peer_sdp->GetFingerprintValue();
	if (peer_fingerprint_algorithm.IsEmpty() || peer_fingerprint_value.IsEmpty())
	{
		logte("Peer SDP does not include a DTLS fingerprint; refusing to start DTLS");
		return false;
	}
	_dtls_transport->SetPeerFingerprint(peer_fingerprint_algorithm, peer_fingerprint_value);

	_dtls_transport->StartDTLS();

	bool transport_cc_enabled = false;
	bool remb_enabled = false;
	// RFC3264
	// For each "m=" line in the offer, there MUST be a corresponding "m=" line in the answer.
	for (size_t i = 0; i < peer_media_desc_list.size(); i++)
	{
		auto peer_media_desc  = peer_media_desc_list[i];
		auto offer_media_desc = offer_media_desc_list[i];

		// The first payload has the highest priority.
		auto first_payload	  = peer_media_desc->GetFirstPayload();
		if (first_payload == nullptr)
		{
			logte("Failed to get the first Payload type of peer sdp");
			return false;
		}

		bool media_transport_cc_enabled = peer_media_desc->GetExtmapItem(RTP_HEADER_EXTENSION_TRANSPORT_CC_ID).IsEmpty() == false;
		media_transport_cc_enabled = media_transport_cc_enabled || first_payload->IsRtcpFbEnabled(PayloadAttr::RtcpFbType::TransportCc);

		bool media_remb_enabled = first_payload->IsRtcpFbEnabled(PayloadAttr::RtcpFbType::GoogRemb);

		if (peer_media_desc->GetMediaType() == MediaDescription::MediaType::Audio)
		{
			_audio_payload_type = first_payload->GetId();
			_audio_ssrc			= offer_media_desc->GetSsrc().value_or(0);
			_rtp_rtcp->AddRtpSender(_audio_payload_type, _audio_ssrc, first_payload->GetCodecRate(), offer_media_desc->GetCname().value_or(""));
			_audio_transport_cc_enabled = media_transport_cc_enabled;
		}
		else
		{
			_video_payload_type = first_payload->GetId();
			_video_ssrc = offer_media_desc->GetSsrc().value_or(0);
			_rtp_rtcp->AddRtpSender(_video_payload_type, _video_ssrc, first_payload->GetCodecRate(), offer_media_desc->GetCname().value_or(""));

			auto payload = peer_media_desc->GetPayload(static_cast<uint8_t>(FixedRtcPayloadType::RED_PAYLOAD_TYPE));
			if (payload != nullptr)
			{
				if (payload->GetCodec() == PayloadAttr::SupportCodec::RED)
				{
					_red_enabled = true;
				}
			}

			// Retransmission, We always define the RTX payload as payload + 1
			payload = peer_media_desc->GetPayload(_video_payload_type + 1);
			if (payload != nullptr)
			{
				if (payload->GetCodec() == PayloadAttr::SupportCodec::RTX)
				{
					_rtx_enabled = true;
				}
			}

			_video_transport_cc_enabled = media_transport_cc_enabled;

			auto rtc_stream = std::static_pointer_cast<RtcStream>(GetStream());
			// <PlayoutDelay><Adaptive>: this session sets its own delay (see UpdateAdaptivePlayoutDelay)
			if (rtc_stream->IsAdaptivePlayoutDelayEnabled() && peer_media_desc->GetExtmapItem(RTP_HEADER_EXTENSION_PLAYOUT_DELAY_ID).IsEmpty() == false)
			{
				_playout_delay_negotiated = true;
				_config_playout_min_ms	  = rtc_stream->GetPlayoutDelayMin();
				_config_playout_max_ms	  = rtc_stream->GetPlayoutDelayMax();
				_playout_min_ms			  = _config_playout_min_ms;
				_playout_max_ms			  = _config_playout_max_ms;
			}
		}

		transport_cc_enabled = transport_cc_enabled || media_transport_cc_enabled;
		remb_enabled = remb_enabled || media_remb_enabled;
	}

	{
		auto rtc_stream			= std::static_pointer_cast<RtcStream>(GetStream());
		_send_pacing_enabled	= rtc_stream->IsSendPacingEnabled();
		_video_shedding_enabled = rtc_stream->IsVideoSheddingEnabled();
	}

	// Init bandwidth estimator
	if (transport_cc_enabled || remb_enabled)
	{
		_bandwidth_estimator = RtcBandwidthEstimator::Create(RtcBandwidthEstimatorObserver::GetSharedPtr());
	}
	
	// Get Playlist
	_playlist = std::static_pointer_cast<RtcStream>(GetStream())->GetRtcPlaylist(_file_name, CodecIdFromPayloadTypeNumber(_video_payload_type), CodecIdFromPayloadTypeNumber(_audio_payload_type));
	if (_playlist == nullptr)
	{
		logte("Failed to get the playlist (%s/%s/%s) because there is no available rendition", GetApplication()->GetVHostAppName().CStr(), GetStream()->GetName().CStr(), _file_name.CStr());
		return false;
	}

	_auto_abr = _playlist->IsWebRtcAutoAbr();
	_current_rendition = _playlist->GetFirstRendition();
	// Optional initial rendition (signalling URL ?rendition=Name, see
	// WebRtcPublisher::OnAddRemoteDescription). Without this, every auto-
	// playlist session starts on GetFirstRendition() — often Bypass — until a
	// later change_rendition+keyframe. N concurrent viewers briefly force N×
	// the top rung onto the wire (observed as simultaneous identical
	// RtcKfDrain lines and 4K+ULPFEC freezes under the all3 soak).
	if (_initial_rendition.IsEmpty() == false)
	{
		auto requested = _playlist->GetRendition(_initial_rendition);
		if (requested != nullptr)
		{
			_current_rendition = requested;
			// Explicit rung selection disables auto ABR for this session.
			_auto_abr = false;
		}
		else
		{
			logtw("Initial rendition %s not in playlist %s — using first rendition",
				  _initial_rendition.CStr(), _file_name.CStr());
		}
	}

	auto current_video_track = _current_rendition->GetVideoTrack();
	auto current_audio_track = _current_rendition->GetAudioTrack();

	PublishTrackDemand();

	SendPlaylistInfo(_playlist);
	SendRenditionChanged(_current_rendition);

	logtt("Video PT(%d) Audio PT(%d) Video TrackID(%u) Audio TrackID(%u)", _video_payload_type, _audio_payload_type,
		  current_video_track ? current_video_track->GetId() : -1,
		  current_audio_track ? current_audio_track->GetId() : -1);

	// Connect nodes

	// [RTP_RTCP] -> [SRTP] -> [DTLS] -> [RtcSession(Edge)]

	_rtp_rtcp->RegisterPrevNode(nullptr);
	_rtp_rtcp->RegisterNextNode(_srtp_transport);
	_rtp_rtcp->Start();
	_srtp_transport->RegisterPrevNode(_rtp_rtcp);
	_srtp_transport->RegisterNextNode(_dtls_transport);
	_srtp_transport->Start();
	_dtls_transport->RegisterPrevNode(_srtp_transport);
	_dtls_transport->RegisterNextNode(ov::Node::GetSharedPtr());
	_dtls_transport->Start();

	RegisterPrevNode(_dtls_transport);
	RegisterNextNode(nullptr);
	ov::Node::Start();

	if (Session::Start() == false)
	{
		return false;
	}

	StartSendThread();
	return true;
}

void RtcSession::StartSendThread()
{
	if (_send_running.exchange(true) == true)
	{
		return;
	}

	_send_queue.SetAlias(ov::String::FormatString("RtcSend-%u", GetId()).CStr());
	_send_thread = std::thread(&RtcSession::SendThreadProc, this);
}

void RtcSession::StopSendThread()
{
	if (_send_running.exchange(false) == false)
	{
		return;
	}

	_send_queue.Stop();
	if (_send_thread.joinable())
	{
		_send_thread.join();
	}
}

void RtcSession::FinishKeyframeDrain(const char *reason)
{
	if (_kf_drain_active == false)
	{
		return;
	}

	const auto drain_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
							  std::chrono::steady_clock::now() - _kf_drain_start)
							  .count();
	const double copy_ms = static_cast<double>(_kf_drain_copy_ns) / 1.0e6;
	const double send_ms = static_cast<double>(_kf_drain_send_ns) / 1.0e6;

	// Every drain at debug level. A large keyframe alone is normal (a 1080p/4K one is 150-300 packets, and
	// <SendPacing> spreads it on purpose); warn only when the send itself stalled: a packet waited long enough
	// to show as a freeze, or the queue is still deep after the keyframe. At most once per 10 s per session.
	const bool stalled = drain_ms >= 100 || _kf_drain_max_age_ms >= 100 || _send_queue.Size() >= 64;
	if (stalled)
	{
		_kf_drain_stalls++;
	}
	const auto now = std::chrono::steady_clock::now();
	const bool warn = stalled && now - _kf_drain_warned_at >= std::chrono::seconds(10);
	if (warn || ov_log_get_enabled(OV_LOG_TAG, OVLogLevelDebug))
	{
		const auto line = ov::String::FormatString(
			"RtcKfDrain session=%u reason=%s pkts=%u fec=%u bytes=%" PRIu64
			" drain_ms=%lld q0=%zu max_age_ms=%lld copy_ms=%.1f send_ms=%.1f queue_now=%zu stalls=%u",
			GetId(), reason, _kf_drain_packets, _kf_drain_fec_packets, _kf_drain_bytes,
			static_cast<long long>(drain_ms), _kf_drain_q_start,
			static_cast<long long>(_kf_drain_max_age_ms), copy_ms, send_ms, _send_queue.Size(), _kf_drain_stalls);
		if (warn)
		{
			logtw("%s", line.CStr());
		}
		else
		{
			logtd("%s", line.CStr());
		}
	}
	if (warn)
	{
		_kf_drain_warned_at = now;
		_kf_drain_stalls = 0;
	}

	// The largest keyframe (with its parity): what this session already sends at once
	_pacing_burst_bytes = std::max({_pacing_burst_bytes.load(), kMinPacingBurstBytes, _kf_drain_bytes});

	_kf_drain_active = false;
	_kf_drain_seen_marker = false;
}

void RtcSession::SendThreadProc()
{
	ov::logger::ThreadHelper thread_helper;

	// Drain as fast as the kernel accepts, up to the largest keyframe at once (PaceSend), retransmissions
	// first. App-level
	// bitrate caps were tried and caused silent freezes (RTP seq is assigned here, so queue delay ≠
	// packetsLost): PaceSend never holds back a burst the session already sends at its keyframes.
	// Kernel SO_SNDBUF + sendmsg EAGAIN retry absorb keyframe microbursts.
	auto last_log = std::chrono::steady_clock::now();

	while (true)
	{
		auto packet = _send_queue.Dequeue();
		if (packet.has_value() == false)
		{
			if (_kf_drain_active)
			{
				FinishKeyframeDrain("queue_stop");
			}
			break;
		}

		SendQueuedRtx();
		if (packet.value() == nullptr)
		{
			continue;  // woken for retransmissions only
		}

		ProcessOutgoingRtp(packet.value());
		_send_processed.fetch_add(1, std::memory_order_relaxed);

		const auto now = std::chrono::steady_clock::now();
		if (now - last_log > std::chrono::seconds(5))
		{
			last_log = now;
			const uint64_t drops = _send_queue_drops.load(std::memory_order_relaxed);
			const uint64_t enq = _send_enqueued.load(std::memory_order_relaxed);
			const uint64_t proc = _send_processed.load(std::memory_order_relaxed);
			if (drops > 0 || _send_queue.Size() > 256)
			{
				logtw("RtcSend session=%u queue=%zu drops=%" PRIu64 " enq=%" PRIu64 " proc=%" PRIu64,
					  GetId(), _send_queue.Size(), drops, enq, proc);
			}
		}
	}
}

bool RtcSession::Stop()
{
	// Drain/join send thread before taking _start_stop_lock (send path uses a shared lock).
	StopSendThread();

	// start and stop must be called independently.
	std::lock_guard<std::shared_mutex> lock(_start_stop_lock);

	logtt("Stop session. Peer sdp session id : %u", GetOfferSDP()->GetSessionId());

	if (pub::Session::GetState() != SessionState::Started && pub::Session::GetState() != SessionState::Stopping)
	{
		return true;
	}

	auto rtc_stream = std::static_pointer_cast<RtcStream>(GetStream());
	if (rtc_stream != nullptr)
	{
		rtc_stream->ClearSessionDemandedTracks(GetId());
	}

	if (_bandwidth_estimator != nullptr)
	{
		_bandwidth_estimator->Release();
		_bandwidth_estimator = nullptr;
	}

	if (_rtp_rtcp != nullptr)
	{
		_rtp_rtcp->Stop();
	}

	if (_dtls_transport != nullptr)
	{
		_dtls_transport->Stop();
	}

	if (_srtp_transport != nullptr)
	{
		_srtp_transport->Stop();
	}

	// TODO(Getroot): Doesn't need this?
	//_ws_session->Close();

	ov::Node::Stop();

	return Session::Stop();
}

void RtcSession::SetSessionExpiredTime(uint64_t expired_time)
{
	_session_expired_time = expired_time;
}

const std::shared_ptr<const SessionDescription> &RtcSession::GetOfferSDP() const
{
	return _offer_sdp;
}

const std::shared_ptr<const SessionDescription> &RtcSession::GetPeerSDP() const
{
	return _peer_sdp;
}

const std::shared_ptr<http::svr::ws::WebSocketSession> &RtcSession::GetWSClient()
{
	return _ws_session;
}

bool RtcSession::RequestChangeRendition(const ov::String &rendition_name)
{
	auto rendition = _playlist->GetRendition(rendition_name);
	if (rendition == nullptr)
	{
		logte("Failed to get the rendition (%s) because there is no available rendition", rendition_name.CStr());
		return false;
	}

	if (rendition == GetCurrentRendition())
	{
		logtt("The rendition (%s) is already selected", rendition_name.CStr());
		return true;
	}

	// Before any selected video has been sent, adopt immediately. Auto-playlist
	// sessions otherwise all start on GetFirstRendition() (often Bypass) until
	// the first keyframe after change_rendition — N viewers briefly force N×
	// the top rung onto the wire (4K+ULPFEC freezes under simultaneous soak).
	if (_video_rtp_sequence_number == 0 && IsNextRenditionAvailable() == false)
	{
		{
			std::unique_lock<std::shared_mutex> lock(_change_rendition_lock);
			_current_rendition = rendition;
			_next_rendition	  = nullptr;
		}
		PublishTrackDemand();
		SendRenditionChanged(_current_rendition);
		logti("Session %u adopted rendition %s before first video packet",
			  GetId(), rendition_name.CStr());
		return true;
	}

	SetNextRendition(rendition);

	return true;
}

bool RtcSession::RequestChangeRendition(SwitchOver switch_over)
{
	// is changing
	if (GetNextRendition() != nullptr)
	{
		return false;
	}

	std::shared_ptr<const RtcRendition> next_rendition = nullptr;

	if (switch_over == SwitchOver::HIGHER)
	{
		next_rendition = _playlist->GetNextHigherBitrateRendition(_current_rendition);
	}
	else
	{
		next_rendition = _playlist->GetNextLowerBitrateRendition(_current_rendition);
	}

	if (next_rendition != nullptr)
	{
		SetNextRendition(next_rendition);
	}
	else
	{
		// No more higher/lower rendition
		return false; 
	}

	return true;
}

bool RtcSession::SetAutoAbr(bool auto_abr)
{
	_auto_abr = auto_abr;
	SendRenditionChanged(_current_rendition);
	return true;
}

void RtcSession::OnSignal(const std::shared_ptr<RtcBandwidthEstimatorSignal> &signal)
{
	if (_auto_abr == false)
	{
		return;
	}

	if (signal->GetState() == RtcBandwidthEstimatorSignal::State::OverUse)
	{
		if (RequestChangeRendition(SwitchOver::LOWER) == true)
		{
			logtd("Bandwidth OverUse detected. Requested to lower the rendition.");
		}
		else
		{
			logtd("Bandwidth OverUse detected. But there is no lower rendition or changing state.");
			return;
		}
	}
	else if (signal->GetState() == RtcBandwidthEstimatorSignal::State::Stable)
	{
	}
	else if (signal->GetState() == RtcBandwidthEstimatorSignal::State::UnderUse)
	{
		if (RequestChangeRendition(SwitchOver::HIGHER) == true)
		{
			logtd("Bandwidth UnderUse detected. Requested to higher the rendition.");
		}
		else
		{
			logtd("Bandwidth UnderUse detected. But there is no higher rendition or changing state.");
			return;
		}
	}
}

uint64_t RtcSession::GetCurrentBitrateBps() const
{
	auto current_bitrate = GetCurrentRendition()->GetBitrates();
	return current_bitrate;
}

uint64_t RtcSession::GetNextHigherBitrateBps() const
{
	auto current_rendition = GetCurrentRendition();
	auto next_higher_rendition = _playlist->GetNextHigherBitrateRendition(current_rendition);
	if (next_higher_rendition != nullptr)
	{
		return next_higher_rendition->GetBitrates();
	}

	// No higher rendition
	return 0;
}

bool RtcSession::SendPlaylistInfo(const std::shared_ptr<const RtcPlaylist> &playlist) const
{
	auto ws_response = std::static_pointer_cast<http::svr::ws::WebSocketResponse>(_ws_session->GetResponse());
	if (ws_response == nullptr)
	{
		logte("Failed to get the websocket response");
		return false;
	}

	Json::Value json_response;

	json_response["command"] = "notification";
	json_response["type"]	 = "playlist";

	// Message
	json_response["message"] = playlist->ToJson(_auto_abr);

	return ws_response->Send(json_response) > 0;
}

bool RtcSession::SendRenditionChanged(const std::shared_ptr<const RtcRendition> &rendition) const
{
	auto ws_response = std::static_pointer_cast<http::svr::ws::WebSocketResponse>(_ws_session->GetResponse());
	if (ws_response == nullptr)
	{
		logte("Failed to get the websocket response");
		return false;
	}

	Json::Value json_response;

	json_response["command"] = "notification";
	json_response["type"]	 = "rendition_changed";

	// Message
	Json::Value json_message;

	json_message["rendition_name"] = rendition->GetName().CStr();
	json_message["auto"]		   = _auto_abr;

	json_response["message"]	   = json_message;

	return ws_response->Send(json_response) > 0;
}

void RtcSession::OnMessageReceived(const std::any &message)
{
	//It must not be called during start and stop.
	std::shared_lock<std::shared_mutex> lock(_start_stop_lock);

	std::shared_ptr<const ov::Data> data = nullptr;
	try
	{
		data = std::any_cast<std::shared_ptr<const ov::Data>>(message);
		if (data == nullptr)
		{
			return;
		}
	}
	catch (const std::bad_any_cast &e)
	{
		logtt("An incorrect type of packet was input from the stream.");
		return;
	}

	_received_bytes += data->GetLength();

	logtt("Received %" PRIu64 " bytes", _received_bytes);

	// RTP_RTCP -> SRTP -> DTLS -> Edge Node(RtcSession)
	SendDataToPrevNode(data);
}

void RtcSession::ChangeRendition()
{
	std::unique_lock<std::shared_mutex> lock(_change_rendition_lock);

	if (_next_rendition == nullptr)
	{
		return;
	}

	if (_next_rendition == _current_rendition)
	{
		logtt("The rendition is already selected");
		return;
	}

	_current_rendition = _next_rendition;
	_next_rendition = nullptr;

	lock.unlock();

	PublishTrackDemand();
	SendRenditionChanged(_current_rendition);
}

bool RtcSession::IsNextRenditionAvailable() const
{
	return GetNextRendition() != nullptr;
}

void RtcSession::SetNextRendition(const std::shared_ptr<const RtcRendition> &rendition)
{
	{
		std::unique_lock<std::shared_mutex> lock(_change_rendition_lock);
		_next_rendition = rendition;
	}
	// Demand the target rung immediately so it is packetized before the
	// keyframe switch (ChangeRendition) completes.
	PublishTrackDemand();
}

void RtcSession::PublishTrackDemand()
{
	auto rtc_stream = std::static_pointer_cast<RtcStream>(GetStream());
	if (rtc_stream == nullptr)
	{
		return;
	}

	std::unordered_set<uint32_t> current_track_ids;
	std::unordered_set<uint32_t> next_track_ids;
	auto add_rendition_tracks = [](const std::shared_ptr<const RtcRendition> &rendition,
								   std::unordered_set<uint32_t> &track_ids) {
		if (rendition == nullptr)
		{
			return;
		}
		if (auto video = rendition->GetVideoTrack(); video != nullptr)
		{
			track_ids.insert(video->GetId());
		}
		if (auto audio = rendition->GetAudioTrack(); audio != nullptr)
		{
			track_ids.insert(audio->GetId());
		}
	};

	add_rendition_tracks(GetCurrentRendition(), current_track_ids);
	add_rendition_tracks(GetNextRendition(), next_track_ids);

	rtc_stream->SetSessionDemandedTracks(pub::Session::GetSharedPtrAs<RtcSession>(), std::move(current_track_ids), std::move(next_track_ids), _red_enabled);
}

std::shared_ptr<const RtcRendition> RtcSession::GetCurrentRendition() const
{
	std::shared_lock<std::shared_mutex> change_lock(_change_rendition_lock);
	return _current_rendition;
}

std::shared_ptr<const RtcRendition> RtcSession::GetNextRendition() const
{
	std::shared_lock<std::shared_mutex> change_lock(_change_rendition_lock);
	return _next_rendition;
}

uint8_t RtcSession::GetOriginPayloadTypeFromRedRtpPacket(const std::shared_ptr<const RedRtpPacket> &red_rtp_packet)
{
	uint8_t rtp_payload_type = 0;

	// RED includes FEC packet or Media packet.
	if (red_rtp_packet->IsUlpfec())
	{
		rtp_payload_type = red_rtp_packet->OriginPayloadType();
	}
	else
	{
		rtp_payload_type = red_rtp_packet->BlockPT();
	}

	return rtp_payload_type;
}

bool RtcSession::IsSelectedPacket(const std::shared_ptr<const RtpPacket> &rtp_packet)
{
	auto reserved_rendition = GetNextRendition();

	// Check if need to change rendition
	if (reserved_rendition != nullptr)
	{
		// Try to change rendition
		// When a video packet of the next rendition is a keyframe
		if (reserved_rendition->GetVideoTrack() != nullptr && reserved_rendition->GetVideoTrack()->GetId() == rtp_packet->GetTrackId() && rtp_packet->IsKeyframe())
		{
			ChangeRendition();
		}
		// Can change rendition immediately if next rendition has only audio track
		else if (reserved_rendition->GetVideoTrack() == nullptr)
		{
			ChangeRendition();
		}
	}

	auto current_rendition = GetCurrentRendition();

	// Select Track for ABR
	auto selected_track = rtp_packet->IsVideoPacket() ? current_rendition->GetVideoTrack() : current_rendition->GetAudioTrack();
	if (selected_track == nullptr || rtp_packet->GetTrackId() != selected_track->GetId())
	{
		return false;
	}

	uint32_t rtp_payload_type = rtp_packet->PayloadType();

	if (rtp_payload_type == _audio_payload_type ||
		// if RED is disabled, origin RTP packet is selected
		(_red_enabled == false && rtp_payload_type == _video_payload_type) ||
		// if RED is enabled, RED packet is selected
		(_red_enabled == true && rtp_payload_type == static_cast<uint8_t>(FixedRtcPayloadType::RED_PAYLOAD_TYPE)))
	{
		return true;
	}

	return false;
}

void RtcSession::SendOutgoingData(const std::any &packet)
{
	if (_send_running.load(std::memory_order_acquire) == false)
	{
		return;
	}

	std::shared_ptr<RtpPacket> session_packet;
	try
	{
		session_packet = std::any_cast<std::shared_ptr<RtpPacket>>(packet);
	}
	catch (const std::bad_any_cast &e)
	{
		logtt("An incorrect type of packet was input from the stream.");
		return;
	}

	if (session_packet == nullptr)
	{
		return;
	}

	// Soft bound only: sequence numbers are assigned at send time, so dropping
	// here is invisible as packetsLost and manifests as freezes. Prefer growth
	// + EAGAIN backpressure; drop only under extreme backlog (~2s at 4K).
	static constexpr size_t kHardDropQueuePackets = 24000;
	while (_send_queue.Size() >= kHardDropQueuePackets)
	{
		_send_queue.Dequeue(0);
		_send_queue_drops.fetch_add(1, std::memory_order_relaxed);
	}

	_send_queue.Enqueue(std::move(session_packet));
	_send_enqueued.fetch_add(1, std::memory_order_relaxed);
}

void RtcSession::ProcessOutgoingRtp(const std::shared_ptr<RtpPacket> &session_packet)
{
	// It must not be called during start and stop.
	std::shared_lock<std::shared_mutex> lock(_start_stop_lock);

	if (pub::Session::GetState() != SessionState::Started)
	{
		return;
	}

	// Check expired time
	if (_session_expired_time != 0 && _session_expired_time < ov::Clock::NowMSec())
	{
		_ice_port->DisconnectSession(_ice_session_id);
		SetState(SessionState::Stopping);
		return;
	}

	if (session_packet == nullptr)
	{
		return;
	}

	// Check the packet is selected.
	if (IsSelectedPacket(session_packet) == false)
	{
		return;
	}

	const bool is_video = session_packet->IsVideoPacket();
	const bool is_fec = session_packet->IsUlpfec();
	const bool is_key_media = is_video && session_packet->IsKeyframe() && (is_fec == false);

	if (is_video && ShedVideo(session_packet, is_fec, is_key_media))
	{
		return;
	}

	// Start/extend keyframe drain window (media + trailing FEC of same timestamp).
	if (is_key_media && session_packet->IsFirstPacketOfFrame())
	{
		if (_kf_drain_active)
		{
			FinishKeyframeDrain("new_keyframe");
		}
		_kf_drain_active = true;
		_kf_drain_seen_marker = false;
		_kf_drain_timestamp = session_packet->Timestamp();
		_kf_drain_start = std::chrono::steady_clock::now();
		_kf_drain_packets = 0;
		_kf_drain_fec_packets = 0;
		_kf_drain_bytes = 0;
		_kf_drain_copy_ns = 0;
		_kf_drain_send_ns = 0;
		_kf_drain_q_start = _send_queue.Size();
		_kf_drain_max_age_ms = 0;
	}
	else if (_kf_drain_active && is_video &&
			 session_packet->Timestamp() != _kf_drain_timestamp)
	{
		FinishKeyframeDrain("ts_change");
	}

	if (_kf_drain_active)
	{
		const auto age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
								std::chrono::system_clock::now() - session_packet->GetCreatedTime())
								.count();
		if (age_ms > _kf_drain_max_age_ms)
		{
			_kf_drain_max_age_ms = age_ms;
		}
	}

	// Exclusive packets (typical: ULPFEC with no RTX history share) can SRTP
	// in place. Shared packets must be copied — SRTP mutates the buffer.
	const auto t_copy0 = std::chrono::steady_clock::now();
	std::shared_ptr<RtpPacket> send_packet;
	const bool exclusive = (session_packet.use_count() == 1);
	if (exclusive)
	{
		send_packet = session_packet;
	}
	else
	{
		send_packet = std::make_shared<RtpPacket>(*session_packet);
	}
	const auto copy_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
							 std::chrono::steady_clock::now() - t_copy0)
							 .count();

	auto &media_rtp_sequence_number = send_packet->IsVideoPacket() ? _video_rtp_sequence_number : _audio_rtp_sequence_number;
	const uint16_t origin_sequence_number = session_packet->SequenceNumber();
	if (is_video && is_fec && RemapFecSnBase(*send_packet, origin_sequence_number, media_rtp_sequence_number) == false)
	{
		// Its protected packets are not where its mask says in this session's numbering: a receiver
		// would recover the wrong packet. Not sent, so it takes no sequence number either.
		return;
	}
	if (is_video)
	{
		_video_sequence_map[origin_sequence_number % kSequenceMapSize] = {origin_sequence_number, media_rtp_sequence_number, true};
	}
	if (is_key_media && session_packet->IsFirstPacketOfFrame())
	{
		_newest_keyframe_sequence_number = media_rtp_sequence_number;
		_newest_keyframe_sent_ms		 = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
		_has_sent_keyframe				 = true;
	}
	send_packet->SetSequenceNumber(media_rtp_sequence_number++);

	PaceSend(send_packet->GetDataLength(), true);

	// Set transport-wide sequence number
	bool media_transport_cc_enabled = send_packet->IsVideoPacket() ? _video_transport_cc_enabled : _audio_transport_cc_enabled;

	// Only a packet that carries the extension takes a transport-wide number. ULPFEC parity has no
	// header extensions: numbering it anyway left a gap per parity packet, which the receiver's
	// feedback reports as lost, so the estimator saw 5-20% loss on a clean path and auto ABR fell to
	// the lowest rung and stayed there.
	if (media_transport_cc_enabled)
	{
		media_transport_cc_enabled = SetTransportWideSequenceNumber(send_packet, _wide_sequence_number);
	}

	SetAbsSendTime(send_packet, ov::Clock::NowMSec());
	if (_playout_delay_negotiated && is_video && _playout_min_ms != _config_playout_min_ms)
	{
		// The path stopped losing: back towards the configured delay, slowly enough for the receiver's
		// audio sync to follow
		const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
		if (now_ms - _last_nack_ms > kAdaptivePlayoutQuietMs && now_ms - _last_playout_step_ms >= kAdaptivePlayoutStepIntervalMs)
		{
			_last_playout_step_ms = now_ms;
			SetAdaptivePlayoutMin(_playout_min_ms - kAdaptivePlayoutStepMs, 0);
		}
		SetPlayoutDelay(send_packet, _playout_min_ms, _playout_max_ms);
	}

	const auto t_send0 = std::chrono::steady_clock::now();
	// rtp_rtcp -> srtp -> dtls -> Edge Node(RtcSession)
	_rtp_rtcp->SendRtpPacket(send_packet);
	const auto send_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
							 std::chrono::steady_clock::now() - t_send0)
							 .count();

	if (is_video && (is_fec == false))
	{
		const auto gap_ms = _have_sent_video
								? std::chrono::duration_cast<std::chrono::milliseconds>(
									  t_send0 - _last_video_sent)
									  .count()
								: 0;
		if (_have_sent_video && gap_ms >= 200 &&
			(t_send0 - _last_send_gap_warn) >= std::chrono::milliseconds(500))
		{
			_last_send_gap_warn = t_send0;
			logtw("RtcSendGap session=%u gap_ms=%lld queue=%zu key=%s age_ms=%lld",
				  GetId(), static_cast<long long>(gap_ms), _send_queue.Size(),
				  is_key_media ? "true" : "false",
				  static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
											 std::chrono::system_clock::now() -
											 session_packet->GetCreatedTime())
											 .count()));
		}
		_last_video_sent = t_send0;
		_have_sent_video = true;
	}

	RecordRtpSent(send_packet, origin_sequence_number, media_transport_cc_enabled ? _wide_sequence_number : 0);

	if (media_transport_cc_enabled)
	{
		if (_bandwidth_estimator != nullptr)
		{
			_bandwidth_estimator->OnRtpSent(_wide_sequence_number, send_packet);
		}

		_wide_sequence_number++;
	}

	MonitorInstance->IncreaseBytesOut(*GetStream(), PublisherType::Webrtc, send_packet->GetDataLength());

	if (_kf_drain_active)
	{
		_kf_drain_packets++;
		_kf_drain_bytes += send_packet->GetDataLength();
		_kf_drain_copy_ns += static_cast<uint64_t>(copy_ns);
		_kf_drain_send_ns += static_cast<uint64_t>(send_ns);
		if (is_fec)
		{
			_kf_drain_fec_packets++;
		}
		if (is_key_media && session_packet->Marker())
		{
			_kf_drain_seen_marker = true;
		}
		// Drain ends on timestamp change (next frame) — see top of this function.
	}
}

bool RtcSession::SetTransportWideSequenceNumber(const std::shared_ptr<RtpPacket> &rtp_packet, uint16_t wide_sequence_number)
{
	auto extension_buffer = rtp_packet->Extension(RTP_HEADER_EXTENSION_TRANSPORT_CC_ID);
	if (extension_buffer == nullptr)
	{
		return false;
	}

	auto payload_offset = rtp_packet->GetExtensionType() == RtpHeaderExtension::HeaderType::ONE_BYTE_HEADER ? 1 : 2;

	ByteWriter<uint16_t>::WriteBigEndian(extension_buffer + payload_offset, wide_sequence_number);

	return true;
}

void RtcSession::UpdateAdaptivePlayoutDelay()
{
	const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
	_last_nack_ms = now_ms;
	_nack_times.push_back(now_ms);
	while (now_ms - _nack_times.front() > kAdaptivePlayoutWindowMs)
	{
		_nack_times.pop_front();
	}

	// Sustained loss: NACKs in enough different seconds of the window. One outage or burst NACKs a
	// lot at once, and needs no deeper buffer afterwards.
	size_t seconds = 0;
	int64_t last_second = -1;
	for (const auto at_ms : _nack_times)
	{
		if (at_ms / 1000 != last_second)
		{
			last_second = at_ms / 1000;
			seconds++;
		}
	}
	const bool raised = _playout_min_ms != _config_playout_min_ms;
	if ((raised == false && seconds < kAdaptivePlayoutMinNackSeconds) || _bandwidth_estimator == nullptr)
	{
		return;
	}

	// A repair arrives about an RTT after the loss. The path's own RTT, not the NACKs' delay: that
	// includes whatever queue or outage caused the loss (a 1 s outage read as a 1 s repair time).
	const int64_t repair_ms = _bandwidth_estimator->GetBaseRttMs();
	if (repair_ms <= kAdaptivePlayoutMinRepairMs)
	{
		return;	 // within what the receiver buffers anyway
	}
	const int min_ms = std::clamp(static_cast<int>(repair_ms) + kAdaptivePlayoutMarginMs, _config_playout_min_ms, std::max(_config_playout_min_ms, kAdaptivePlayoutCapMs));
	SetAdaptivePlayoutMin(min_ms, _nack_times.size());
}

void RtcSession::SetAdaptivePlayoutMin(int min_ms, size_t nacks)
{
	// 10 ms granularity on the wire. Raised in one go (the viewer freezes until then); lowered only by
	// the send thread's steps (nacks == 0). Small raises are not worth a change in the receiver's target.
	min_ms = std::max(_config_playout_min_ms, (min_ms + 9) / 10 * 10);
	if (nacks > 0 && min_ms < _playout_min_ms + 20)
	{
		return;
	}
	const int max_ms = (min_ms == _config_playout_min_ms) ? _config_playout_max_ms : std::max(_config_playout_max_ms, min_ms + kAdaptivePlayoutMaxHeadroomMs);
	if (min_ms == _playout_min_ms && max_ms == _playout_max_ms)
	{
		return;
	}
	if (nacks > 0 || min_ms == _config_playout_min_ms)
	{
		logtd("RtcPlayoutDelay session=%u %d:%d ms -> %d:%d ms (%zu NACKs in %" PRId64 " ms)", GetId(), _playout_min_ms.load(), _playout_max_ms.load(), min_ms, max_ms, nacks, kAdaptivePlayoutWindowMs);
	}
	_playout_max_ms = max_ms;
	_playout_min_ms = min_ms;
}

bool RtcSession::SetPlayoutDelay(const std::shared_ptr<RtpPacket> &rtp_packet, int min_ms, int max_ms)
{
	auto extension_buffer = rtp_packet->Extension(RTP_HEADER_EXTENSION_PLAYOUT_DELAY_ID);
	if (extension_buffer == nullptr)
	{
		return false;
	}

	auto payload_offset = rtp_packet->GetExtensionType() == RtpHeaderExtension::HeaderType::ONE_BYTE_HEADER ? 1 : 2;

	// 12 bits each, in 10 ms units
	const uint32_t value = (static_cast<uint32_t>(std::min(min_ms / 10, 0xFFF)) << 12) | static_cast<uint32_t>(std::min(max_ms / 10, 0xFFF));
	ByteWriter<uint24_t>::WriteBigEndian(extension_buffer + payload_offset, value);

	return true;
}

bool RtcSession::SetAbsSendTime(const std::shared_ptr<RtpPacket> &rtp_packet, uint64_t time_ms)
{
	auto extension_buffer = rtp_packet->Extension(RTP_HEADER_EXTENSION_ABS_SEND_TIME_ID);
	if (extension_buffer == nullptr)
	{
		return false;
	}

	auto payload_offset = rtp_packet->GetExtensionType() == RtpHeaderExtension::HeaderType::ONE_BYTE_HEADER ? 1 : 2;

	auto abs_send_time	= RtpHeaderExtensionAbsSendTime::MsToAbsSendTime(time_ms);
	ByteWriter<uint24_t>::WriteBigEndian(extension_buffer + payload_offset, abs_send_time);

	return true;
}

bool RtcSession::RecordRtpSent(const std::shared_ptr<const RtpPacket> &rtp_packet, uint16_t origin_sequence_number, uint16_t wide_sequence_number)
{
	if (rtp_packet == nullptr)
	{
		return false;
	}

	auto sent_log = std::make_shared<RtpSentLog>();
	sent_log->_sequence_number = rtp_packet->SequenceNumber();
	sent_log->_wide_sequence_number = wide_sequence_number;
	sent_log->_track_id = rtp_packet->GetTrackId();
	sent_log->_payload_type = rtp_packet->PayloadType();
	sent_log->_origin_sequence_number = origin_sequence_number;
	sent_log->_timestamp = rtp_packet->Timestamp();
	sent_log->_marker = rtp_packet->Marker();
	sent_log->_is_fec = rtp_packet->IsUlpfec();
	sent_log->_ssrc = rtp_packet->Ssrc();

	sent_log->_sent_bytes = rtp_packet->GetDataLength();
	sent_log->_sent_time = std::chrono::system_clock::now();

	auto video_rtp_key = sent_log->_sequence_number % MAX_RTP_RECORDS;

	std::lock_guard<std::shared_mutex> lock(_rtp_record_map_lock);

	if (rtp_packet->IsVideoPacket())
	{
		_video_rtp_sent_record_map[video_rtp_key] = sent_log;
		_newest_video_sent_sequence_number = sent_log->_sequence_number;
		_has_sent_video_record = true;
	}

	return true;
}

std::shared_ptr<RtcSession::RtpSentLog> RtcSession::TraceRtpSentByVideoSeqNo(uint16_t sequence_number)
{
	std::shared_lock<std::shared_mutex> lock(_rtp_record_map_lock);

	auto key = sequence_number % MAX_RTP_RECORDS;
	auto it	 = _video_rtp_sent_record_map.find(key);
	if (it == _video_rtp_sent_record_map.end())
	{
		return nullptr;
	}

	// The slot is a ring: a NACK older than MAX_RTP_RECORDS packets finds a
	// newer packet here. Retransmitting that one never fills the receiver's
	// hole (it re-NACKs until its packet buffer overflows and it drops to the
	// next keyframe), so treat it as not found.
	if (it->second->_sequence_number != sequence_number)
	{
		return nullptr;
	}

	return it->second;
}

void RtcSession::OnRtpFrameReceived(const std::vector<std::shared_ptr<RtpPacket>> &rtp_packets)
{
	// No player sends RTP packet
}

void RtcSession::OnRtcpReceived(const std::shared_ptr<RtcpInfo> &rtcp_info)
{
	if (pub::Session::GetState() != SessionState::Started)
	{
		return;
	}

	if (rtcp_info->GetPacketType() == RtcpPacketType::RR)
	{
		// Process
		ProcessReceiverReport(rtcp_info);
	}
	else if (rtcp_info->GetPacketType() == RtcpPacketType::RTPFB)
	{
		if (rtcp_info->GetCountOrFmt() == static_cast<uint8_t>(RTPFBFMT::NACK))
		{
			// Process
			ProcessNACK(rtcp_info);
		}
		else if (rtcp_info->GetCountOrFmt() == static_cast<uint8_t>(RTPFBFMT::TRANSPORT_CC))
		{
			// Process
			ProcessTransportCc(rtcp_info);
		}
	}
	else if (rtcp_info->GetPacketType() == RtcpPacketType::PSFB)
	{
		if (rtcp_info->GetCountOrFmt() == static_cast<uint8_t>(PSFBFMT::AFB))  // REMB
		{
			// Process
			ProcessRemb(rtcp_info);
		}
	}

	//rtcp_info->DebugPrint();
}

bool RtcSession::ProcessReceiverReport(const std::shared_ptr<RtcpInfo> &rtcp_info)
{
	auto rr = std::static_pointer_cast<ReceiverReport>(rtcp_info);
	if (rr == nullptr)
	{
		logte("Could not parse ReceiverReport");
		return false;
	}

	//rr->DebugPrint();

	return true;
}

bool RtcSession::ProcessNACK(const std::shared_ptr<RtcpInfo> &rtcp_info)
{
	if (_rtx_enabled == false)
	{
		return true;
	}

	auto stream = std::static_pointer_cast<RtcStream>(GetStream());
	if (stream == nullptr)
	{
		return false;
	}

	auto nack = std::static_pointer_cast<NACK>(rtcp_info);
	if (nack->GetMediaSsrc() != _video_ssrc)
	{
		return false;
	}

	// Oldest packet of this NACK that was retransmitted
	std::optional<uint16_t> oldest_repaired;

	// Retransmission — do not return after the first hit (old bug left the rest
	// of the NACK list unretransmitted).
	for (size_t i = 0; i < nack->GetLostIdCount(); i++)
	{
		auto seq_no	  = nack->GetLostId(i);
		auto sent_log = TraceRtpSentByVideoSeqNo(seq_no);
		static const bool debug_rtx = std::getenv("OME_DEBUG_RTX") != nullptr;
		if (sent_log == nullptr)
		{
			if (debug_rtx)
			{
				logtw("RtxDebug session=%u nack_seq=%u miss=sent_log next_video_seq=%u", GetId(), seq_no, _video_rtp_sequence_number);
			}
			continue;
		}

		logtt("RTX requested(%d) - TrackID(%u) PayloadType(%d) OriginSeqNo(%u)", seq_no, sent_log->_track_id, sent_log->_payload_type, sent_log->_origin_sequence_number);

		if (_playout_delay_negotiated)
		{
			UpdateAdaptivePlayoutDelay();
		}

		auto rtx_packet = stream->GetRtxRtpPacket(sent_log->_track_id, sent_log->_payload_type, sent_log->_origin_sequence_number);
		if (rtx_packet != nullptr && RtxWanted(seq_no, rtx_packet->GetDataLength()) == false)
		{
			if (debug_rtx)
			{
				logtw("RtxDebug session=%u nack_seq=%u skipped (budget or superseded by a keyframe)", GetId(), seq_no);
			}
			continue;
		}
		if (rtx_packet == nullptr)
		{
			if (debug_rtx)
			{
				logtw("RtxDebug session=%u nack_seq=%u miss=history track=%u pt=%u origin_seq=%u fec=%d", GetId(), seq_no,
					  sent_log->_track_id, sent_log->_payload_type, sent_log->_origin_sequence_number, sent_log->_is_fec ? 1 : 0);
			}
			continue;
		}

		auto copy_rtx_packet = std::make_shared<RtxRtpPacket>(*rtx_packet);
		copy_rtx_packet->SetSequenceNumber(_rtx_sequence_number++);
		copy_rtx_packet->SetOriginalSequenceNumber(sent_log->_sequence_number);
		if (sent_log->_is_fec)
		{
			// The history holds the parity as the stream numbered it; move its SN base the way the
			// original send did (same offset: RemapFecSnBase only sends parity with a constant one)
			const uint16_t offset = sent_log->_sequence_number - sent_log->_origin_sequence_number;
			if (copy_rtx_packet->PayloadSize() >= kFecSnBaseOffset + 2)
			{
				uint8_t *sn_base = copy_rtx_packet->Payload() + kFecSnBaseOffset;
				ByteWriter<uint16_t>::WriteBigEndian(sn_base, ByteReader<uint16_t>::ReadBigEndian(sn_base) + offset);
			}
		}

		if (debug_rtx)
		{
			const uint8_t *osn = copy_rtx_packet->Payload() - RTX_HEADER_SIZE;
			logtw("RtxDebug session=%u nack_seq=%u log_seq=%u origin_seq=%u pt=%u hist_seq=%u hist_ts=%u log_ts=%u osn_bytes=%u rtx_seq=%u len=%zu",
				  GetId(), seq_no, sent_log->_sequence_number, sent_log->_origin_sequence_number, sent_log->_payload_type,
				  rtx_packet->GetOriginalSequenceNumber(), rtx_packet->Timestamp(), sent_log->_timestamp,
				  static_cast<unsigned>((osn[0] << 8) | osn[1]), copy_rtx_packet->SequenceNumber(), copy_rtx_packet->GetDataLength());
		}
		EnqueueRtx(copy_rtx_packet);

		if (oldest_repaired.has_value() == false || static_cast<int16_t>(seq_no - *oldest_repaired) < 0)
		{
			oldest_repaired = seq_no;
		}
	}

	// OME_TEST_NO_PADDING_REPLAY=1 turns the replay off, to measure what it is worth
	static const bool no_padding_replay = std::getenv("OME_TEST_NO_PADDING_REPLAY") != nullptr;
	if (oldest_repaired.has_value() && no_padding_replay == false)
	{
		ReannounceFecPadding(stream, *oldest_repaired);
	}

	return true;
}

bool RtcSession::RtxWanted(uint16_t sequence_number, size_t bytes)
{
	const auto now = std::chrono::steady_clock::now();

	// The viewer restarts from the newest keyframe: older packets only cost path capacity
	if (_has_sent_keyframe)
	{
		const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
		if (now_ms - _newest_keyframe_sent_ms.load() >= kKeyframeSupersedesMs &&
			static_cast<int16_t>(sequence_number - _newest_keyframe_sequence_number.load()) < 0)
		{
			return false;
		}
	}

	// Until the media rate is known (the first second), retransmissions are not limited; nor without
	// <SendPacing>, which is what sends them ahead of queued media
	const double media_rate = _media_bytes_per_second.load();
	if (media_rate == 0 || _send_pacing_enabled == false)
	{
		return true;
	}
	const double burst = static_cast<double>(std::max(_pacing_burst_bytes.load(), kMinPacingBurstBytes));
	if (_rtx_refilled_at == std::chrono::steady_clock::time_point{})
	{
		_rtx_tokens = burst;
	}
	else
	{
		_rtx_tokens = std::min(burst, _rtx_tokens + kRtxRateFactor * media_rate * std::chrono::duration<double>(now - _rtx_refilled_at).count());
	}
	_rtx_refilled_at = now;

	if (_rtx_tokens < static_cast<double>(bytes))
	{
		return false;  // the receiver asks again
	}
	_rtx_tokens -= static_cast<double>(bytes);
	return true;
}

void RtcSession::EnqueueRtx(const std::shared_ptr<RtpPacket> &rtx_packet)
{
	if (_send_running.load(std::memory_order_acquire) == false)
	{
		return;
	}

	{
		std::lock_guard<std::mutex> lock(_rtx_queue_lock);
		_rtx_queue.push_back(rtx_packet);
	}
	_send_queue.Enqueue(nullptr);
}

void RtcSession::SendQueuedRtx()
{
	while (true)
	{
		std::shared_ptr<RtpPacket> rtx_packet;
		{
			std::lock_guard<std::mutex> lock(_rtx_queue_lock);
			if (_rtx_queue.empty())
			{
				return;
			}
			rtx_packet = std::move(_rtx_queue.front());
			_rtx_queue.pop_front();
		}
		SendRtx(rtx_packet);
	}
}

void RtcSession::SendRtx(const std::shared_ptr<RtpPacket> &rtx_packet)
{
	std::shared_lock<std::shared_mutex> lock(_start_stop_lock);
	if (pub::Session::GetState() != SessionState::Started)
	{
		return;
	}

	PaceSend(rtx_packet->GetDataLength(), false);

	// A fresh transport-wide number, as libwebrtc does. The copy carried the original's: a lost packet then
	// read as received an RTT late, and its frame's packet train as hugely spread (a tiny capacity sample).
	if (_video_transport_cc_enabled && SetTransportWideSequenceNumber(rtx_packet, _wide_sequence_number))
	{
		if (_bandwidth_estimator != nullptr)
		{
			_bandwidth_estimator->OnRtpSent(_wide_sequence_number, rtx_packet, true);
		}
		_wide_sequence_number++;
	}

	_rtp_rtcp->SendRtpPacket(rtx_packet);
}

bool RtcSession::ShedVideo(const std::shared_ptr<const RtpPacket> &packet, bool is_fec, bool is_key_media)
{
	if (_video_shedding_enabled == false)
	{
		return false;
	}
	if (is_fec == false && packet->IsFirstPacketOfFrame())
	{
		if (is_key_media)
		{
			if (_shedding)
			{
				logtd("RtcShed session=%u resumed at a keyframe after %" PRIu64 " frame(s) (episode %" PRIu64 ")", GetId(), _shed_frames, _shed_episodes);
				_shed_resumed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
			}
			_shedding = false;
		}
		else if (_shedding == false && _bandwidth_estimator != nullptr && _has_sent_keyframe &&
				 _bandwidth_estimator->GetQueueSampleSendMs() >= _shed_resumed_ms)
		{
			const int64_t queue_ms = _bandwidth_estimator->GetQueueDelayMs();
			const double growth = _bandwidth_estimator->GetQueueGrowth();
			const double loss = _bandwidth_estimator->GetRecentLoss();
			const auto rendition = GetCurrentRendition();
			const bool can_switch_down = _auto_abr && GetNextRendition() == nullptr && rendition != nullptr &&
										 _playlist->GetNextLowerBitrateRendition(rendition) != nullptr;
			// OME_TEST_SHED_QUEUE_MS overrides the threshold, to measure it (0: never shed)
			static const int64_t shed_queue_ms = std::getenv("OME_TEST_SHED_QUEUE_MS") ? std::atoll(std::getenv("OME_TEST_SHED_QUEUE_MS")) : kShedQueueDelayMs;
			// A link that carries what this session sends drains the queue by itself (see kShedCapacity)
			const double capacity_bps = _bandwidth_estimator->GetLinkCapacityBps();
			const double media_bps = std::max(_unshed_media_bytes_per_second * 8.0, rendition != nullptr ? static_cast<double>(rendition->GetBitrates()) : 0.0);
			const bool link_narrower = capacity_bps <= 0 || media_bps <= 0 || capacity_bps < media_bps * kShedCapacity;
			if (shed_queue_ms > 0 && queue_ms > shed_queue_ms && (growth > kShedQueueGrowth || loss > kShedLoss) &&
				link_narrower && can_switch_down == false)
			{
				_shedding = true;
				_shed_in_media_window = true;
				_shed_frames = 0;
				_shed_episodes++;
				logtd("RtcShed session=%u queue=%" PRId64 " ms growth=%.2f loss=%.1f%% capacity=%.2f/%.2f Mbps: video waits for the next keyframe", GetId(), queue_ms, growth, loss * 100, capacity_bps / 1e6, media_bps / 1e6);
			}
		}

		if (_shedding == false)
		{
			_last_sent_video_timestamp = packet->Timestamp();
		}
		else
		{
			_shed_frames++;
		}
	}

	// The parity of the last frame sent still goes out; its media did
	return _shedding && (is_fec == false || packet->Timestamp() != _last_sent_video_timestamp);
}

void RtcSession::PaceSend(size_t bytes, bool is_media)
{
	auto now = std::chrono::steady_clock::now();

	// The media rate, measured over 1 s windows (smoothed over ~4): the stream's own bitrate, which
	// the path to this viewer carried so far
	if (is_media)
	{
		if (_media_bytes_in_window == 0 && _media_bytes_per_second == 0)
		{
			_media_window_start = now;
		}
		_media_bytes_in_window += bytes;
		const double window_s = std::chrono::duration<double>(now - _media_window_start).count();
		if (window_s >= 1.0)
		{
			const double sample = static_cast<double>(_media_bytes_in_window) / window_s;
			_media_bytes_per_second = (_media_bytes_per_second == 0) ? sample : (0.75 * _media_bytes_per_second + 0.25 * sample);
			if (_shed_in_media_window == false)
			{
				_unshed_media_bytes_per_second = (_unshed_media_bytes_per_second == 0) ? sample : (0.75 * _unshed_media_bytes_per_second + 0.25 * sample);
			}
			_shed_in_media_window = _shedding;
			_media_bytes_in_window = 0;
			_media_window_start = now;
		}
	}

	if (_send_pacing_enabled == false || _pacing_burst_bytes == 0 || _media_bytes_per_second == 0)
	{
		return;
	}

	// Not below the current rendition's own bitrate: just after an up-switch the measured rate is still the
	// old rung's, and pacing the new one at it throttled it (and the bandwidth estimator measured the pacer)
	const auto rendition = GetCurrentRendition();
	const double rendition_bytes_per_second = (rendition != nullptr) ? static_cast<double>(rendition->GetBitrates()) / 8.0 : 0.0;
	const double rate = kPacingFactor * std::max(_media_bytes_per_second.load(), rendition_bytes_per_second);
	if (_pacing_refilled_at == std::chrono::steady_clock::time_point{})
	{
		_pacing_tokens = static_cast<double>(_pacing_burst_bytes);
	}
	else
	{
		_pacing_tokens += rate * std::chrono::duration<double>(now - _pacing_refilled_at).count();
		_pacing_tokens = std::min(_pacing_tokens, static_cast<double>(_pacing_burst_bytes));
	}
	_pacing_refilled_at = now;

	if (_pacing_tokens < 0)
	{
		std::this_thread::sleep_for(std::chrono::duration<double>(-_pacing_tokens / rate));
		_pacing_tokens = 0;
		_pacing_refilled_at = std::chrono::steady_clock::now();
	}

	// The packet may overdraw: the next one waits for it
	_pacing_tokens -= static_cast<double>(bytes);
}

// With RED/ULPFEC the parity packets take video sequence numbers between frames. libwebrtc's H.264
// reference finder (RtpSeqNumOnlyRefFinder) bridges those numbers as padding, but forgets padding more
// than 100 sequence numbers older than the newest it has seen (kMaxPaddingAge). A frame repaired later
// than that finds no bridge to the next one, and every following frame is stashed until a keyframe:
// up to a GOP, as OME does not answer PLI. So once a retransmission repairs an old packet, the FEC
// sequence numbers the receiver has forgotten since then are announced again, right behind it: each
// as an RTX packet with an empty payload, which the receiver restores to an empty packet and takes as
// padding (~30 bytes). The receiver never asks for these (it saw the originals), so each is sent twice.
//
// Each one costs the receiver a frame search, so only the forgotten ones are replayed, and at most
// kMaxReplayed per NACK.
void RtcSession::ReannounceFecPadding(const std::shared_ptr<RtcStream> &stream, uint16_t repaired_sequence_number)
{
	// libwebrtc keeps padding up to 100 sequence numbers behind the newest; replay what is older than
	// (newest - kStillKnown), which leaves a margin for packets still on their way
	constexpr uint16_t kStillKnown = 80;
	// libwebrtc keeps at most 1000 missing packets in its NACK list
	constexpr uint16_t kMaxReplayRange = 1000;
	constexpr size_t kMaxReplayed = 64;
	constexpr int kCopies = 2;

	uint16_t newest;
	{
		std::shared_lock<std::shared_mutex> lock(_rtp_record_map_lock);
		if (_has_sent_video_record == false)
		{
			return;
		}
		newest = _newest_video_sent_sequence_number;
	}

	const uint16_t age = newest - repaired_sequence_number;
	if (age <= kStillKnown || age > kMaxReplayRange)
	{
		return;
	}

	// From just after the repaired packet up to (newest - kStillKnown)
	const uint16_t count = age - kStillKnown;
	size_t replayed = 0;
	for (uint16_t i = 1; i < count && replayed < kMaxReplayed; i++)
	{
		const uint16_t seq = repaired_sequence_number + i;
		auto sent_log = TraceRtpSentByVideoSeqNo(seq);
		if (sent_log == nullptr || sent_log->_is_fec == false)
		{
			continue;
		}

		auto rtx_packet = stream->GetRtxRtpPacket(sent_log->_track_id, sent_log->_payload_type, sent_log->_origin_sequence_number);
		if (rtx_packet == nullptr)
		{
			continue;
		}

		for (int copy = 0; copy < kCopies; copy++)
		{
			auto padding = std::make_shared<RtxRtpPacket>(*rtx_packet);
			padding->SetPayloadSize(0);
			padding->SetSequenceNumber(_rtx_sequence_number++);
			padding->SetOriginalSequenceNumber(seq);
			EnqueueRtx(padding);
		}
		replayed++;
	}
}

// ULPFEC's SN base (RFC 5109) names the first protected packet by sequence number, and the stream
// numbers its packets once for all sessions, while each session renumbers what it sends. Without
// moving the base into this session's numbering the mask names other packets: a receiver never
// recovers anything, or recovers the wrong packet. The packets from the base up to this parity must
// have been sent here with the same offset, so that the mask's positions still hold.
bool RtcSession::RemapFecSnBase(RtpPacket &fec_packet, uint16_t origin_sequence_number, uint16_t session_sequence_number)
{
	if (fec_packet.PayloadSize() < kFecSnBaseOffset + 2)
	{
		return false;
	}

	uint8_t *sn_base = fec_packet.Payload() + kFecSnBaseOffset;
	const uint16_t origin_base = ByteReader<uint16_t>::ReadBigEndian(sn_base);
	const uint16_t offset = session_sequence_number - origin_sequence_number;

	const auto &base = _video_sequence_map[origin_base % kSequenceMapSize];
	if (base.valid == false || base.origin != origin_base || static_cast<uint16_t>(base.session - base.origin) != offset)
	{
		return false;
	}

	ByteWriter<uint16_t>::WriteBigEndian(sn_base, base.session);
	return true;
}

bool RtcSession::ProcessTransportCc(const std::shared_ptr<RtcpInfo> &rtcp_info)
{
	if (_bandwidth_estimator == nullptr)
	{
		return false;
	}

	auto transport_cc = std::static_pointer_cast<TransportCc>(rtcp_info);
	_bandwidth_estimator->OnTransportCc(transport_cc);
	return true;
}

bool RtcSession::ProcessRemb(const std::shared_ptr<RtcpInfo> &rtcp_info)
{
	if (_bandwidth_estimator == nullptr)
	{
		return false;
	}

	auto remb = std::static_pointer_cast<REMB>(rtcp_info);
	_bandwidth_estimator->OnRemb(remb);	
	return true;
}

// ov::Node Interface
// RtpRtcp -> SRTP -> DTLS -> Edge(this)
bool RtcSession::OnDataReceivedFromPrevNode(NodeType from_node, const std::shared_ptr<ov::Data> &data)
{
	if (ov::Node::GetNodeState() != ov::Node::NodeState::Started)
	{
		logtt("Node has not started, so the received data has been canceled.");
		return false;
	}

	return _ice_port->Send(_ice_session_id, data);
}

// RtcSession Node has not a lower node so it will not be called
bool RtcSession::OnDataReceivedFromNextNode(NodeType from_node, const std::shared_ptr<const ov::Data> &data)
{
	return true;
}