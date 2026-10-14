//==============================================================================
//
//  OvenMediaEngine
//
//  Created by Getroot
//  Copyright (c) 2018 AirenSoft. All rights reserved.
//
//==============================================================================
#pragma once

#include <base/common_types.h>
#include <base/info/stream.h>
#include <base/ovcrypto/certificate.h>
#include <base/publisher/stream.h>
#include <modules/ice/ice_port.h>
#include <modules/pacer/adaptive_delay_controller.h>
#include <modules/pacer/frame_pacer.h>
#include <modules/pacer/pace_group_map.h>
#include <modules/rtp_rtcp/rtp_history.h>
#include <modules/rtp_rtcp/rtp_rtcp_defines.h>
#include <modules/sdp/session_description.h>

#include <atomic>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "rtc_playlist.h"
#include "rtc_session.h"

// max initial media packet buffer size, for OOM protection
#define MAX_INITIAL_MEDIA_PACKET_BUFFER_SIZE 10000

class RtcStream final : public pub::Stream, public RtpPacketizerInterface
{
public:
	static std::shared_ptr<RtcStream> Create(const std::shared_ptr<pub::Application> application,
											 const info::Stream &info,
											 uint32_t worker_count);

	explicit RtcStream(const std::shared_ptr<pub::Application> application,
					   const info::Stream &info,
					   uint32_t worker_count);
	~RtcStream() final;

	//--------------------------------------------------------------------
	// Implementation of info::Stream
	//--------------------------------------------------------------------
	std::shared_ptr<const pub::Stream::DefaultPlaylistInfo> GetDefaultPlaylistInfo() const override;
	//--------------------------------------------------------------------

	std::shared_ptr<const SessionDescription> GetSessionDescription(const ov::String &file_name);
	std::shared_ptr<const RtcPlaylist> GetRtcPlaylist(const ov::String &file_name, cmn::MediaCodecId video_codec_id, cmn::MediaCodecId audio_codec_id);

	void SendVideoFrame(const std::shared_ptr<MediaPacket> &media_packet) override;
	void SendAudioFrame(const std::shared_ptr<MediaPacket> &media_packet) override;
	void SendDataFrame(const std::shared_ptr<MediaPacket> &media_packet) override {}  // Not supported

	std::shared_ptr<RtxRtpPacket> GetRtxRtpPacket(uint32_t track_id, uint8_t origin_payload_type, uint16_t origin_sequence_number);

	// Demand-based packetize/pace: only tracks selected by ≥1 session enter
	// the pacer / packetizer. Current-rendition tracks are fully packetized;
	// pending-next tracks are keyframe-only until the ABR cut (sessions drop
	// non-keyframes of next via IsSelectedPacket anyway).
	void SetSessionDemandedTracks(const std::shared_ptr<RtcSession> &session,
								  std::unordered_set<uint32_t> current_track_ids,
								  std::unordered_set<uint32_t> next_track_ids,
								  bool red_enabled);
	void ClearSessionDemandedTracks(session_id_t session_id);
	bool IsTrackDemanded(uint32_t track_id) const;
	bool IsTrackFullyDemanded(uint32_t track_id) const;

	// RtpRtcpPacketizerInterface Implementation
	bool OnRtpPacketized(std::shared_ptr<RtpPacket> packet) override;
	bool NeedsRawRtpFanout(uint32_t track_id) const override;

private:
	void RebuildDemandedTracksUnlocked();
	void LogDemandedTracksUnlocked() const;
	bool Start() override;
	bool Stop() override;
	// TODO(Getroot): a running session cannot adopt a codec change today; renegotiating the SDP on a codec change would allow it
	void OnTrackChanged(int32_t track_id, const std::shared_ptr<const MediaTrack> &old_track, const std::shared_ptr<const MediaTrack> &new_track) override;

	bool IsSupportedCodec(cmn::MediaCodecId codec_id);

	std::shared_ptr<SessionDescription> CreateSessionDescription(const ov::String &file_name = "");

	std::shared_ptr<const RtcMasterPlaylist> GetRtcMasterPlaylist(const ov::String &file_name);
	std::shared_ptr<RtcMasterPlaylist> CreateRtcMasterPlaylist(const ov::String &file_name);

	std::shared_ptr<MediaDescription> MakeVideoDescription() const;
	std::shared_ptr<MediaDescription> MakeAudioDescription() const;

	std::shared_ptr<PayloadAttr> MakePayloadAttr(const std::shared_ptr<const MediaTrack> &track) const;
	std::shared_ptr<PayloadAttr> MakeRtxPayloadAttr(const std::shared_ptr<const MediaTrack> &track) const;

	void MakeRtpVideoHeader(uint32_t track_id, const CodecSpecificInfo *info, RTPVideoHeader *rtp_video_header);
	uint16_t AllocateVP8PictureID(uint32_t track_id);

	bool StorePacketForRTX(std::shared_ptr<RtpPacket> &packet);

	bool PushToPacer(const std::shared_ptr<MediaPacket> &media_packet,
					 std::chrono::steady_clock::time_point arrival_time);
	void BufferMediaPacketUntilReadyToPlay(const std::shared_ptr<MediaPacket> &media_packet);
	bool SendBufferedPackets();
	void PacketizeVideoFrame(const std::shared_ptr<MediaPacket> &media_packet);
	void PacketizeAudioFrame(const std::shared_ptr<MediaPacket> &media_packet);

	void AddPacketizer(const std::shared_ptr<const MediaTrack> &track);
	std::shared_ptr<RtpPacketizer> GetPacketizer(uint32_t track_id);

	ov::String GetRtpHistoryKey(uint32_t track_id, uint8_t payload_type);
	void AddRtpHistory(const std::shared_ptr<const MediaTrack> &track);
	std::shared_ptr<RtpHistory> GetHistory(uint32_t track_id, uint8_t origin_payload_type);

	uint32_t GetSsrc(cmn::MediaType media_type);

	// SDP related info
	ov::String _msid;
	ov::String _cname;

	// VP8 Picture ID (per track)
	std::map<uint32_t, uint16_t> _vp8_picture_id_map;

	std::shared_ptr<Certificate> _certificate;

	// Track ID, Packetizer
	std::shared_mutex _packetizers_lock;
	std::map<uint32_t, std::shared_ptr<RtpPacketizer>> _packetizers;

	// RtpHistoryKey string, RtpHistory
	std::map<ov::String, std::shared_ptr<RtpHistory>> _rtp_history_map;

	uint32_t _video_ssrc		= 0;
	uint32_t _video_rtx_ssrc	= 0;
	uint32_t _audio_ssrc		= 0;

	bool _rtx_enabled			= true;
	bool _ulpfec_enabled		= true;
	bool _playout_delay_enabled = false;
	int _playout_delay_min		= 0;
	int _playout_delay_max		= 0;

	// Always true once pace groups are created: packetize runs on per-rung
	// DelayQueue threads. <JitterBuffer> only enables AdaptiveDelay hold.
	bool _pacer_enabled = false;
	bool _jitter_buffer_enabled = false;

	bool _transport_cc_enabled = false;
	bool _remb_enabled		   = false;

	uint32_t _worker_count = 0;

	// Per-pace-group resources (one DelayQueue for release timing + one
	// packetize worker thread + AdaptiveDelay per video rung; audio joins a
	// video group only when uniquely paired — see BuildPaceGroupMap).
	// Packetize (esp. RED/ULPFEC on 4K) must not run on the DelayQueue thread
	// or a single keyframe stalls every subsequently-due frame in the group.
	struct PaceGroup
	{
		std::shared_ptr<ov::DelayQueue> scheduler;
		std::shared_ptr<AdaptiveDelayController> adaptive;
		ov::Queue<std::shared_ptr<MediaPacket>> packetize_queue{nullptr, 0};
		std::thread packetize_thread;
		std::atomic<bool> packetize_running{false};
	};
	std::map<uint32_t, std::shared_ptr<PaceGroup>> _pace_groups;
	std::map<uint32_t, std::shared_ptr<FramePacer>> _pacers;
	std::shared_mutex _pacers_lock;

	ov::Queue<std::shared_ptr<MediaPacket>> _initial_media_packet_buffer;

	struct SessionTrackDemand
	{
		std::weak_ptr<RtcSession> session;
		std::unordered_set<uint32_t> current_track_ids;
		std::unordered_set<uint32_t> next_track_ids;
		bool red_enabled = false;
	};
	std::unordered_map<session_id_t, SessionTrackDemand> _session_demanded_tracks;
	// Union of every session's current-rendition tracks (full packetize).
	std::unordered_set<uint32_t> _fully_demanded_tracks;
	// Union of current + next (anything that may be packetized at all).
	std::unordered_set<uint32_t> _demanded_tracks;
	// track_id → sessions that currently or pending-next need that track
	std::unordered_map<uint32_t, std::unordered_set<session_id_t>> _track_interested_sessions;
	// Tracks where ≥1 interested session has red_enabled=false (needs raw RTP
	// fan-out alongside RED when Ulpfec is on).
	std::unordered_set<uint32_t> _tracks_needing_raw_rtp;
	mutable std::shared_mutex _demand_lock;

	ov::String _default_playlist_name;

	// Playlist File Name : SessionDescription
	std::map<ov::String, std::shared_ptr<const SessionDescription>> _offer_sdp_map;
	std::shared_mutex _offer_sdp_lock;

	// Playlist File Name : RtcPlaylist
	std::map<ov::String, std::shared_ptr<const RtcMasterPlaylist>> _rtc_master_playlist_map;
	std::shared_mutex _rtc_master_playlist_map_lock;
};
