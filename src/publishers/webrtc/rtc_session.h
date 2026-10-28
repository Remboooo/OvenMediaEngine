//==============================================================================
//
//  OvenMediaEngine
//
//  Created by Getroot
//  Copyright (c) 2018 AirenSoft. All rights reserved.
//
//==============================================================================
#pragma once

#include <array>
#include <modules/http/server/web_socket/web_socket_session.h>
#include <monitoring/monitoring.h>

#include <atomic>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_set>

#include "base/info/media_track.h"
#include "base/ovlibrary/queue.h"
#include "base/publisher/session.h"
#include "modules/dtls_srtp/dtls_transport.h"
#include "modules/ice/ice_port.h"
#include "modules/rtp_rtcp/rtp_packetizer_interface.h"
#include "modules/rtp_rtcp/rtp_rtcp.h"
#include "modules/sdp/session_description.h"
#include "rtc_playlist.h"
#include "rtc_bandwidth_estimator.h"

/*	Node Connection
 * [  RTP_RTCP ]
 * [SRTP] [SCTP]				
 * [    DTLS   ]
 * [  ICE/STUN ]
 * [ RtcSession](Edge Node)	 ----> Send packet
 *		  <----------------------- Recv packet
 *   
 */

class WebRtcPublisher;
class RtcApplication;
class RtcStream;

class RtcSession : public pub::Session, public RtpRtcpInterface, public ov::Node, public RtcBandwidthEstimatorObserver
{
public:
	static std::shared_ptr<RtcSession> Create(const std::shared_ptr<WebRtcPublisher> &publisher,
											  const std::shared_ptr<pub::Application> &application,
											  const std::shared_ptr<pub::Stream> &stream,
											  const ov::String &file_name,
											  const std::shared_ptr<const SessionDescription> &offer_sdp,
											  const std::shared_ptr<const SessionDescription> &peer_sdp,
											  const std::shared_ptr<IcePort> &ice_port,
											  session_id_t ice_session_id,
											  const std::shared_ptr<http::svr::ws::WebSocketSession> &ws_session,
											  const ov::String &initial_rendition = "");

	RtcSession(const info::Session &session_info,
			   const std::shared_ptr<WebRtcPublisher> &publisher,
			   const std::shared_ptr<pub::Application> &application,
			   const std::shared_ptr<pub::Stream> &stream,
			   const ov::String &file_name,
			   const std::shared_ptr<const SessionDescription> &offer_sdp,
			   const std::shared_ptr<const SessionDescription> &peer_sdp,
			   const std::shared_ptr<IcePort> &ice_port,
			   session_id_t ice_session_id,
			   const std::shared_ptr<http::svr::ws::WebSocketSession> &ws_session,
			   const ov::String &initial_rendition = "");
	~RtcSession() override;

	bool Start() override;
	bool Stop() override;

	void SetSessionExpiredTime(uint64_t expired_time);

	const std::shared_ptr<const SessionDescription> &GetPeerSDP() const;
	const std::shared_ptr<const SessionDescription> &GetOfferSDP() const;
	const std::shared_ptr<http::svr::ws::WebSocketSession> &GetWSClient();

	// pub::Session Interface
	void SendOutgoingData(const std::any &packet) override;
	void OnMessageReceived(const std::any &message) override;

	// RtpRtcp Interface
	void OnRtpFrameReceived(const std::vector<std::shared_ptr<RtpPacket>> &rtp_packets) override;
	void OnRtcpReceived(const std::shared_ptr<RtcpInfo> &rtcp_info) override;

	// ov::Node Interface
	bool OnDataReceivedFromPrevNode(NodeType from_node, const std::shared_ptr<ov::Data> &data) override;
	bool OnDataReceivedFromNextNode(NodeType from_node, const std::shared_ptr<const ov::Data> &data) override;

	bool RequestChangeRendition(const ov::String &rendition_name);

	enum class SwitchOver : uint8_t
	{
		HIGHER,
		LOWER
	};

	bool RequestChangeRendition(SwitchOver switch_over);
	bool SetAutoAbr(bool auto_abr);

	// RtcBandwidthEstimatorObserver Interface
	void OnSignal(const std::shared_ptr<RtcBandwidthEstimatorSignal> &signal) override;
	uint64_t GetCurrentBitrateBps() const override;
	uint64_t GetNextHigherBitrateBps() const override;

	session_id_t GetIceSessionId() const
	{
		return _ice_session_id;
	}

	bool IsRedEnabled() const
	{
		return _red_enabled;
	}

private:
	bool ProcessReceiverReport(const std::shared_ptr<RtcpInfo> &rtcp_info);
	bool ProcessNACK(const std::shared_ptr<RtcpInfo> &rtcp_info);
	void ReannounceFecPadding(const std::shared_ptr<RtcStream> &stream, uint16_t repaired_sequence_number);
	bool RemapFecSnBase(RtpPacket &fec_packet, uint16_t origin_sequence_number, uint16_t session_sequence_number);
	bool ProcessTransportCc(const std::shared_ptr<RtcpInfo> &rtcp_info);
	bool ProcessRemb(const std::shared_ptr<RtcpInfo> &rtcp_info);
	bool IsSelectedPacket(const std::shared_ptr<const RtpPacket> &rtp_packet);
	void ProcessOutgoingRtp(const std::shared_ptr<RtpPacket> &session_packet);
	void StartSendThread();
	void StopSendThread();
	void SendThreadProc();
	void FinishKeyframeDrain(const char *reason);
	// Hands an RTX packet to the send thread, which sends it ahead of queued media, paced with it
	void EnqueueRtx(const std::shared_ptr<RtpPacket> &rtx_packet);
	void SendRtx(const std::shared_ptr<RtpPacket> &rtx_packet);
	void PaceSend(size_t bytes, bool is_media);
	// Whether this video packet is shed (see kShedQueueDelayMs)
	bool ShedVideo(const std::shared_ptr<const RtpPacket> &packet, bool is_fec, bool is_key_media);
	// Whether a NACKed packet is still worth retransmitting (see kRtxRateFactor)
	bool RtxWanted(uint16_t sequence_number, size_t bytes);

	uint8_t GetOriginPayloadTypeFromRedRtpPacket(const std::shared_ptr<const RedRtpPacket> &red_rtp_packet);

	bool SendPlaylistInfo(const std::shared_ptr<const RtcPlaylist> &playlist) const;
	bool SendRenditionChanged(const std::shared_ptr<const RtcRendition> &rendition) const;

	std::shared_ptr<WebRtcPublisher> _publisher;

	std::shared_ptr<RtpRtcp> _rtp_rtcp;
	std::shared_ptr<SrtpTransport> _srtp_transport;
	std::shared_ptr<DtlsTransport> _dtls_transport;

	std::shared_ptr<const SessionDescription> _offer_sdp;
	std::shared_ptr<const SessionDescription> _peer_sdp;
	std::shared_ptr<IcePort> _ice_port;
	std::shared_ptr<http::svr::ws::WebSocketSession> _ws_session;  // Signalling

	uint8_t _video_payload_type = 0;
	uint32_t _video_ssrc = 0;

	uint8_t _audio_payload_type = 0;
	uint32_t _audio_ssrc = 0;

	bool _red_enabled = false;
	bool _rtx_enabled = false;
	bool _video_transport_cc_enabled = false;
	bool _audio_transport_cc_enabled = false;

	uint16_t _rtx_sequence_number = 1;
	uint64_t _session_expired_time = 0;

	std::shared_mutex _start_stop_lock;

	// Per-session send thread: pace/StreamWorker only enqueue; SRTP/ICE runs here
	// so 4K+ULPFEC cannot stall other rungs or the packetize DelayQueue.
	static constexpr size_t kMaxSendQueuePackets = 8000;
	ov::Queue<std::shared_ptr<RtpPacket>> _send_queue{nullptr, kMaxSendQueuePackets};
	std::thread _send_thread;
	std::atomic<bool> _send_running{false};
	std::atomic<uint64_t> _send_queue_drops{0};
	std::atomic<uint64_t> _send_enqueued{0};
	std::atomic<uint64_t> _send_processed{0};
	// Retransmissions, sent before any queued media (which is newer, and cannot be shown before the
	// repair anyway). A null entry in _send_queue wakes the send thread for them.
	std::mutex _rtx_queue_lock;
	std::deque<std::shared_ptr<RtpPacket>> _rtx_queue;
	void SendQueuedRtx();

	// Keyframe send-drain instrumentation (seq assigned here → HOL freezes).
	bool _kf_drain_active = false;
	bool _kf_drain_seen_marker = false;
	uint32_t _kf_drain_timestamp = 0;
	std::chrono::steady_clock::time_point _kf_drain_start{};
	uint32_t _kf_drain_packets = 0;
	uint32_t _kf_drain_fec_packets = 0;
	uint64_t _kf_drain_bytes = 0;
	uint64_t _kf_drain_copy_ns = 0;
	uint64_t _kf_drain_send_ns = 0;
	size_t _kf_drain_q_start = 0;
	int64_t _kf_drain_max_age_ms = 0;
	uint32_t _kf_drain_stalls = 0;  // since the last warning
	std::chrono::steady_clock::time_point _kf_drain_warned_at{};

	// Video send-gap detector: pacer/packetize stalls show as packetsLost=0 freezes.
	bool _have_sent_video = false;
	std::chrono::steady_clock::time_point _last_video_sent{};
	std::chrono::steady_clock::time_point _last_send_gap_warn{};

	// Send pacing, a token bucket over all this session sends (send thread only). A burst up to the
	// largest keyframe still goes out at once, as before. A bigger one is spread at kPacingFactor times
	// the session's media rate, so a bottleneck queue on the path does not drop it: the NACK answer
	// after an outage, or a backlog released after an ingest stall. Until a keyframe and a second of
	// media have been measured, nothing is paced.
	// Only with <SendPacing>.
	bool _send_pacing_enabled = false;
	static constexpr double kPacingFactor = 2.5;  // libwebrtc's pacing factor
	static constexpr uint64_t kMinPacingBurstBytes = 64 * 1024;
	std::atomic<uint64_t> _pacing_burst_bytes{0};  // also read by ProcessNACK (RTX budget)
	double _pacing_tokens = 0;
	std::chrono::steady_clock::time_point _pacing_refilled_at{};
	std::atomic<double> _media_bytes_per_second{0};
	// The same, from windows in which no video was shed: what the stream needs, for kShedCapacity. A shed
	// lowers the measured rate for seconds, and against it a link of 0.8x the stream looked wide enough
	// to stop shedding: it dropped packets of every frame for ~4 s per GOP.
	double _unshed_media_bytes_per_second = 0;
	bool _shed_in_media_window = false;
	uint64_t _media_bytes_in_window = 0;
	std::chrono::steady_clock::time_point _media_window_start{};

	// Frame shedding (send thread only), for a session with no lower rung to switch to: pinned with
	// ?rendition=, or auto ABR on its lowest rung. When the queue in front of the receiver
	// (RtcBandwidthEstimator::GetQueueDelayMs) exceeds kShedQueueDelayMs while the link carries less than
	// this session sends (the queue grows by more than kShedQueueGrowth, GetQueueGrowth, or over
	// kShedLoss of the packets are lost, GetRecentLoss: a full drop-tail queue stops growing), video stops
	// at the next delta frame and resumes at the next keyframe. A deep queue that neither grows nor drops
	// is the path's own (FIFO jitter of 60 ms σ held 150-250 ms without congestion, and over 2 s its
	// slope sometimes passed 0.1, hence a 4 s window); shedding cannot drain it, and it froze the viewer
	// a GOP at a time. Without shedding a link under the rung's bitrate froze the viewer
	// for the whole congestion (28 s of 30): the bottleneck dropped packets of every frame, the repairs
	// competed with new media, and each broken frame stalled all later ones of its GOP. Shed frames are
	// dropped before this session numbers them, so the receiver sees no gap and asks for nothing; it shows
	// each GOP from its keyframe as far as the link carries it.
	static constexpr int64_t kShedQueueDelayMs = 200;
	static constexpr double kShedQueueGrowth = 0.1;
	static constexpr double kShedLoss = 0.05;
	// Shed only while the link's capacity (RtcBandwidthEstimator::GetLinkCapacityBps) is under this times
	// the session's media rate, or unknown. A sudden longer path (FIFO jitter of 240 ms σ: ~0.9 s more
	// delay) looks like a growing queue, but its packets arrive bunched, not spread to a narrow link's
	// rate; shedding there cost a GOP for nothing.
	static constexpr double kShedCapacity = 1.0;
	// A shed sends nothing, so the estimator holds what made it shed until feedback on the keyframe that
	// ends it arrives, a one-way delay later. Checked on the frames right after that keyframe, the stale
	// state shed again at once: on a path ~1 s long (FIFO jitter of 240 ms σ) every GOP showed only its
	// keyframe, for as long as the delay lasted. So a session sheds again only on feedback for a frame
	// sent after it resumed (steady clock ms).
	int64_t _shed_resumed_ms = 0;
	// Only with <VideoShedding>.
	bool _video_shedding_enabled = false;
	bool _shedding = false;
	uint32_t _last_sent_video_timestamp = 0;
	uint64_t _shed_frames = 0;
	uint64_t _shed_episodes = 0;

	// Adaptive playout delay (with <PlayoutDelay>). A receiver shows a frame that waits on a
	// retransmission only if its buffer outlasts the repair, about one RTT; Chrome's own buffer stays
	// near 100-140 ms whatever the path. At 300 ms RTT with 1% loss the configured 50:200 froze the
	// viewer 15-21 times in 30 s, and a fixed 400:600 only 4 times, at ~350 ms more delay for everyone.
	// So a session that keeps NACKing (in kAdaptivePlayoutMinNackSeconds different seconds of
	// kAdaptivePlayoutWindowMs) on a path whose base RTT (RtcBandwidthEstimator::GetBaseRttMs) exceeds
	// kAdaptivePlayoutMinRepairMs raises the minimum for its own viewer to that RTT plus
	// kAdaptivePlayoutMarginMs. The maximum keeps kAdaptivePlayoutMaxHeadroomMs over it. Not from the
	// NACKs' own delay: that included the queue or outage behind the loss, and a 1 s outage or a
	// congested link left the viewer 400-1100 ms behind for a minute. Short or clean paths never change.
	// Back down only after kAdaptivePlayoutQuietMs without a NACK, kAdaptivePlayoutStepMs per
	// kAdaptivePlayoutStepIntervalMs: Chrome syncs audio to the video delay at ~10 ms/s, and a sudden
	// 300 ms drop left the audio that far behind for 20 s (a long path stays long, so flapping with
	// intermittent loss would be worse). The send thread rewrites the extension in this session's copy.
	static constexpr int64_t kAdaptivePlayoutWindowMs = 10000;
	static constexpr size_t kAdaptivePlayoutMinNackSeconds = 5;
	static constexpr int kAdaptivePlayoutMarginMs = 80;
	// Chrome buffers ~100-140 ms by itself, which already covers a repair this fast
	static constexpr int64_t kAdaptivePlayoutMinRepairMs = 100;
	static constexpr int kAdaptivePlayoutMaxHeadroomMs = 200;
	static constexpr int kAdaptivePlayoutCapMs = 1500;
	static constexpr int64_t kAdaptivePlayoutQuietMs = 60000;
	static constexpr int kAdaptivePlayoutStepMs = 10;
	static constexpr int64_t kAdaptivePlayoutStepIntervalMs = 500;
	void UpdateAdaptivePlayoutDelay();
	void SetAdaptivePlayoutMin(int min_ms, size_t nacks);
	bool SetPlayoutDelay(const std::shared_ptr<RtpPacket> &rtp_packet, int min_ms, int max_ms);
	// <PlayoutDelay><Adaptive> is on and the peer took the extension: the session writes its own delay
	bool _playout_delay_negotiated = false;
	int _config_playout_min_ms = 0;
	int _config_playout_max_ms = 0;
	std::deque<int64_t> _nack_times;  // NACK arrival ms (RTCP thread)
	std::atomic<int64_t> _last_nack_ms{0};
	int64_t _last_playout_step_ms = 0;  // send thread
	std::atomic<int> _playout_min_ms{0};
	std::atomic<int> _playout_max_ms{0};

	// Retransmission guards (ProcessNACK). Retransmissions go before media, so under congestion they
	// could fill the path on their own: lost again, NACKed again, while new media starved (a 3.6 Mbps
	// bottleneck under a 7 Mbps rung: 9-16 Mbps of RTX and no media for 20 s). They get at most
	// kRtxRateFactor x the media rate (libwebrtc caps them at its bandwidth estimate; OME has none it
	// trusts), and nothing older than the newest keyframe once that keyframe has had time to arrive.
	static constexpr double kRtxRateFactor = 1.0;
	static constexpr int64_t kKeyframeSupersedesMs = 200;
	double _rtx_tokens = 0;
	std::chrono::steady_clock::time_point _rtx_refilled_at{};
	std::atomic<bool> _has_sent_keyframe{false};
	std::atomic<uint16_t> _newest_keyframe_sequence_number{0};
	std::atomic<int64_t> _newest_keyframe_sent_ms{0};  // steady clock

	ov::String _file_name;
	ov::String _initial_rendition;
	std::shared_ptr<const RtcPlaylist> _playlist;

	uint16_t _video_rtp_sequence_number = 0;
	// Stream -> session video sequence numbers of recently sent packets (send thread only), so that
	// ULPFEC's SN base can be moved into this session's numbering
	struct SequenceMapping
	{
		uint16_t origin = 0;
		uint16_t session = 0;
		bool valid = false;
	};
	static constexpr size_t kSequenceMapSize = 1024;
	std::array<SequenceMapping, kSequenceMapSize> _video_sequence_map{};
	uint16_t _audio_rtp_sequence_number = 0;
	uint16_t _wide_sequence_number = 0;

	///////////////////////////////////////////////
	// For ABR
	void ChangeRendition();
	std::shared_ptr<const RtcRendition> GetCurrentRendition() const;
	std::shared_ptr<const RtcRendition> GetNextRendition() const;
	void SetNextRendition(const std::shared_ptr<const RtcRendition> &rendition);
	bool IsNextRenditionAvailable() const;
	// Tell RtcStream which tracks this session needs (current + pending next).
	void PublishTrackDemand();

	std::shared_ptr<const RtcRendition> _current_rendition = nullptr;
	std::shared_ptr<const RtcRendition> _next_rendition = nullptr;
	mutable std::shared_mutex _change_rendition_lock;

	// Auto switch rendition
	bool _auto_abr = true;
	std::shared_ptr<RtcBandwidthEstimator> _bandwidth_estimator;
	
	///////////////////////////////
	// For NACK
	struct RtpSentLog
	{
		uint16_t _wide_sequence_number = 0;
		uint16_t _sequence_number = 0;

		uint32_t _track_id = 0;
		uint8_t _payload_type = 0;
		uint16_t _origin_sequence_number = 0;

		uint32_t _ssrc = 0;
		uint32_t _timestamp = 0;
		bool _marker = false;
		bool _is_fec = false;

		uint32_t _sent_bytes = 0;
		std::chrono::system_clock::time_point _sent_time;

		ov::String ToString()
		{
			return ov::String::FormatString("WideSeq(%d) SSRC(%u) Seq(%d) Track(%d) PT(%d) Timestamp(%u) Marker(%s) OriginSeq(%d) SentBytes(%u)",
											_wide_sequence_number, _ssrc, _sequence_number, _track_id, _payload_type, _timestamp, _marker == true ? "O" : "X", _origin_sequence_number, _sent_bytes);
		}
	};
	// video sequence number % MAX_RTP_RECORDS : RtpSentRecord	
	std::unordered_map<uint16_t, std::shared_ptr<RtpSentLog>> _video_rtp_sent_record_map;
	std::shared_mutex _rtp_record_map_lock;
	// Newest video sequence number sent (guarded by _rtp_record_map_lock)
	uint16_t _newest_video_sent_sequence_number = 0;
	bool _has_sent_video_record = false;
	bool RecordRtpSent(const std::shared_ptr<const RtpPacket> &rtp_packet, uint16_t origin_sequence_number, uint16_t wide_sequence_number);
	std::shared_ptr<RtpSentLog> TraceRtpSentByVideoSeqNo(uint16_t sequence_number);
	/////////////////////////////// For NACK

	// RTP Header Extension Setters
	bool SetTransportWideSequenceNumber(const std::shared_ptr<RtpPacket> &rtp_packet, uint16_t wide_sequence_number);
	bool SetAbsSendTime(const std::shared_ptr<RtpPacket> &rtp_packet, uint64_t time_ms);

	session_id_t _ice_session_id;
};