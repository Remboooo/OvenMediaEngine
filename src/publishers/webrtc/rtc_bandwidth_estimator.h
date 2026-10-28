//==============================================================================
//
//  OvenMediaEngine
//
//  Created by Getroot
//  Copyright (c) 2025 AirenSoft. All rights reserved.
//
//==============================================================================
#pragma once

#include <base/ovlibrary/ovlibrary.h>

#include <atomic>
#include <deque>
#include <optional>
#include <modules/rtp_rtcp/rtcp_info/remb.h>
#include <modules/rtp_rtcp/rtcp_info/transport_cc.h>
#include <modules/rtp_rtcp/rtp_packet.h>

class RtcBandwidthEstimatorSignal
{
public:
	enum class State
	{
		UnderUse,
		Stable,
		OverUse
	};

	void SetState(State state)
	{
		_state = state;
	}

	State GetState() const
	{
		return _state;
	}

private:
	State _state = State::Stable;
};

class RtcBandwidthEstimatorObserver : public ov::EnableSharedFromThis<RtcBandwidthEstimatorObserver>
{
public:
	virtual void OnSignal(const std::shared_ptr<RtcBandwidthEstimatorSignal> &signal) = 0;

	virtual uint64_t GetCurrentBitrateBps() const								= 0;
	virtual uint64_t GetNextHigherBitrateBps() const							= 0;  // For UnderUse state
};

static constexpr uint32_t kMaxRtpPacketHistory = 8192;
class RtcBandwidthEstimator
{
public:
	enum class Mechanism
	{
		TransportCc,
		Remb
	};

	static std::shared_ptr<RtcBandwidthEstimator> Create(const std::shared_ptr<RtcBandwidthEstimatorObserver> &observer);
	void Release();

	explicit RtcBandwidthEstimator(const std::shared_ptr<RtcBandwidthEstimatorObserver> &observer);
	virtual ~RtcBandwidthEstimator();

	// is_rtx: a retransmission (or padding): counted in packet trains, not in its frame's statistics
	void OnRtpSent(uint16_t wide_seq_no, const std::shared_ptr<const RtpPacket> &rtp_packet, bool is_rtx = false);
	void OnTransportCc(const std::shared_ptr<const TransportCc> &transport_cc);
	// The queue in front of the receiver, as of the newest feedback: a video frame's first packet's one-way
	// delay over the lowest of the last kQueueBaseWindowMs (the clock offset cancels). 0 until known.
	// Any thread.
	int64_t GetQueueDelayMs() const
	{
		return _queue_delay_ms.load(std::memory_order_relaxed);
	}
	// How fast that queue grows: the least-squares slope of the one-way delay over send time across the
	// last kQueueGrowthWindowMs (ms of delay per ms sent). A link narrower than the session's wire rate
	// keeps it positive (1.2x over the link: 0.2); a jittery path that carries the rate swings it around
	// 0. 0 until known. Any thread.
	double GetQueueGrowth() const
	{
		return _queue_growth.load(std::memory_order_relaxed);
	}
	// The share of video packets lost over the same window. A full drop-tail queue stops growing and
	// drops instead (1.2x over the link: ~17%). Any thread.
	double GetRecentLoss() const
	{
		return _recent_loss.load(std::memory_order_relaxed);
	}
	// When the newest frame behind those three was sent (steady clock ms): they describe the path as of
	// then, a one-way delay plus a feedback interval ago. 0 until known. Any thread.
	int64_t GetQueueSampleSendMs() const
	{
		return _queue_sample_send_ms.load(std::memory_order_relaxed);
	}
	// The link's capacity as the receiver's packet trains show it (median over kCapacityWindowMs as of the
	// newest train; kUnlimitedBps where the link did not spread a frame). 0 until known. Any thread.
	double GetLinkCapacityBps() const
	{
		return _link_capacity_bps.load(std::memory_order_relaxed);
	}
	// The path's round trip without its queues: the lowest of the last kBaseRttWindowMs of (a
	// transport-wide feedback's arrival - the send of the newest packet it reports received). 0 until
	// known. Any thread.
	int64_t GetBaseRttMs() const
	{
		return _base_rtt_ms.load(std::memory_order_relaxed);
	}
	void OnRemb(const std::shared_ptr<const REMB> &remb);

	std::shared_ptr<RtcBandwidthEstimatorSignal> GetLastSignal() const;

	bool ProcessTransportCc();

private:
	std::shared_ptr<RtcBandwidthEstimatorObserver> _observer;
	std::shared_ptr<RtcBandwidthEstimatorSignal> _last_signal;

	void Signal(RtcBandwidthEstimatorSignal::State state);


	struct RtpHistory
	{
		uint16_t wide_sequence_number = 0;
		uint8_t payload_type		  = 0;
		uint32_t timestamp			  = 0;
		size_t packet_size			  = 0;
		bool is_video				  = false;
		uint32_t track_id			  = 0;
		bool is_rtx					  = false;
		std::chrono::steady_clock::time_point sent_time;
	};
	// wide_sequence_number -> RtpHistory
	std::vector<RtpHistory> _rtp_history;
	std::shared_mutex _rtp_history_guard;

	uint32_t GetRtpHistoryIndex(uint16_t wide_seq_no) const;

	// Base RTT (GetBaseRttMs): feedback ms, RTT ms; increasing from front to back (windowed minimum)
	static constexpr int64_t kBaseRttWindowMs = 30000;
	std::deque<std::pair<int64_t, int64_t>> _rtt_samples;  // under _tcc_feedbacks_guard
	std::atomic<int64_t> _base_rtt_ms{0};
	void UpdateBaseRtt(uint16_t newest_received_wide_seq_no);

	struct TransportCCFeedback
	{
		uint16_t wide_seq_no = 0;
		bool received = false;
		int64_t arrival_time_ms = 0;
	};
	std::deque<TransportCCFeedback> _tcc_feedbacks;
	std::shared_mutex _tcc_feedbacks_guard;

	struct FrameStats
	{
		size_t total_bytes = 0;
		int packet_total_count = 0;
		int packet_lost_count = 0;

		int64_t first_send_ms = std::numeric_limits<int64_t>::max();
		int64_t last_send_ms  = std::numeric_limits<int64_t>::min();
		int64_t first_recv_ms = std::numeric_limits<int64_t>::max();
		int64_t last_recv_ms  = std::numeric_limits<int64_t>::min();
		// For the packet-train capacity estimate: received packets, and the bytes of all but the first
		int packet_received_count = 0;
		size_t first_recv_bytes = 0;
		size_t received_bytes = 0;
	};
	// (track id << 32 | RTP timestamp) -> FrameStats. Not the timestamp alone: at a rung switch the old and
	// new rung's keyframes share one, and their packets merged into a frame with a huge spread.
	std::map<uint64_t, FrameStats> _frame_stats;
	// The track the delay trend and loss follow: the one being sent. A switch starts them afresh, since the
	// old rung's queue and losses say nothing about the new one.
	std::optional<uint32_t> _trend_track_id;
	void StartTrend(uint32_t track_id);
	
	// TrendLine
	struct FrameSample 
	{
		int64_t send_time_ms;
		int64_t arrival_time_ms;
		int64_t delay_variation_ms;
	};
	std::deque<FrameSample> _trendline_window;
	FrameStats _prev_frame_stats;
	double _trendline_slope = 0.0;
	double _ema_trendline_slope = 0.0;
	double UpdateTrendLine(const FrameStats &current_frame_stats);

	bool IsNewerRtpTimestamp(uint32_t base, uint32_t comp) const;
	double CalculateTrendlineSlope() const;

	// Packet loss tracking
	double _ema_packet_loss_ratio = 0.0;
	double UpdatePacketLoss(const FrameStats& stat);
	

	// Path capacity from packet trains: a frame's packets leave back to back, so behind a bottleneck they
	// arrive spread out at its rate. Median over a few seconds of frames with enough packets; a lower
	// bound when the path is faster than the receiver's 1 ms timestamps can show.
	static constexpr int kMinTrainPackets = 4;
	static constexpr int64_t kMinTrainSpreadMs = 3;
	static constexpr double kUnlimitedBps = 1e10;
	static constexpr int64_t kCapacityWindowMs = 4000;
	static constexpr size_t kMinCapacitySamples = 2;
	std::deque<std::pair<std::chrono::steady_clock::time_point, double>> _capacity_samples;
	// Every packet the receiver reported (any track, audio, retransmissions): arrival ms, bytes. A train's
	// rate counts all that arrived during it; other traffic interleaved into a frame's packets would
	// otherwise stretch it and understate the link.
	std::deque<std::pair<int64_t, size_t>> _arrivals;
	// REMB is the fallback for peers without transport-wide feedback. Chrome sends both when both are
	// negotiated; its REMB then read 0.1-0.4 Mbps on a 3.7 Mbps link and forced down-switches.
	std::chrono::steady_clock::time_point _last_transport_cc{};
	static constexpr int64_t kTransportCcFreshMs = 2000;
	static constexpr int64_t kArrivalsKeepMs = 3000;
	void AddCapacitySample(const FrameStats &stat);

	// Queueing delay (GetQueueDelayMs): one-way delays with a windowed minimum (send ms, delay ms; the
	// delays increase from front to back)
	static constexpr int64_t kQueueBaseWindowMs = 10000;
	std::deque<std::pair<int64_t, int64_t>> _one_way_delays;
	std::atomic<int64_t> _queue_delay_ms{0};
	std::atomic<int64_t> _queue_sample_send_ms{0};
	std::atomic<double> _link_capacity_bps{0};
	static constexpr int64_t kQueueGrowthWindowMs = 4000;
	static constexpr size_t kMinQueueGrowthSamples = 10;
	struct RecentFrame
	{
		int64_t send_ms;
		int64_t delay_ms;  // one-way
		int packets;
		int lost;
	};
	std::deque<RecentFrame> _recent_frames;
	std::atomic<double> _queue_growth{0};
	std::atomic<double> _recent_loss{0};
	void UpdateQueueDelay(const FrameStats &stat);
	double GetCapacityBps(int64_t window_ms = kCapacityWindowMs) const;
	// A rung that no longer fits shows in the capacity before the delay or loss does: a 7 Mbps rung on
	// a 5.8 Mbps link grows the queue by only 0.25 ms per ms, under the slope threshold, until the
	// queue overflows. So a recent capacity below kStayHeadroom x the rung counts as over-use too.
	static constexpr int64_t kRecentCapacityWindowMs = 1000;

	// Make decision. Down: delay trend or loss over threshold for kOverUseMinMs (one step, then a
	// cooldown). Up: straight to the next rung once the capacity estimate has kUpgradeHeadroom over its
	// bitrate and nothing overused for kUpgradeQuietMs. An upgrade followed by overuse within
	// kFailedUpgradeWindowMs blocks the next one for a back-off that doubles (kInitial..kMax).
	static constexpr double kOverUseSlopeThreshold = 0.4;
	static constexpr double kOverUseLossThreshold = 0.05;  // 5%
	// Longer than a keyframe's queueing behind a link the rung fits (50 kB at 3.7 Mbps: ~110 ms); real
	// congestion keeps the delay growing and still shows within this
	static constexpr int64_t kOverUseMinMs = 500;
	// After a downgrade the queue the old rung built drains, and the smoothed loss still carries its drops:
	// over-use then means nothing, as long as the capacity still has kStayHeadroom over the new rung
	static constexpr int64_t kDrainMs = 3000;
	static constexpr double kStayHeadroom = 1.5;
	static constexpr int64_t kOverUseCoolDownMs = 2000;
	static constexpr double kUnderUseLossThreshold = 0.01;  // 1%
	// Over a rung's configured bitrate: its wire rate is ~1.2x that (ULPFEC, audio, RTX, headers), and it
	// plays without dropped frames while that is at most ~80% of the capacity (above, each keyframe's
	// burst delays the frames behind it: 1080p at 83% of a link dropped 8% of its frames). Upgrades
	// need more than staying, so a rung at the edge does not flip back and forth.
	static constexpr double kUpgradeHeadroom = 1.7;
	static constexpr int64_t kUpgradeQuietMs = 3000;
	static constexpr int64_t kFailedUpgradeWindowMs = 5000;
	static constexpr int64_t kInitialUpgradeBackoffMs = 10000;
	static constexpr int64_t kMaxUpgradeBackoffMs = 40000;

	std::optional<std::chrono::steady_clock::time_point> _overuse_since;
	std::chrono::steady_clock::time_point _last_overuse{};
	std::optional<std::chrono::steady_clock::time_point> _last_upgrade;
	std::optional<std::chrono::steady_clock::time_point> _last_downgrade;
	int64_t _upgrade_backoff_ms = 0;
	std::chrono::steady_clock::time_point _upgrade_blocked_until{};

	enum class InternalState
	{
		Neutral,
		Cooldown,  // OverUse recovery period
	};
	InternalState _state = InternalState::Neutral;
	std::chrono::steady_clock::time_point _state_entry_time = std::chrono::steady_clock::now();

	InternalState GetState() const;
	std::chrono::steady_clock::time_point GetStateEntryTime() const;
	int64_t GetTimeInStateMs() const;

	void SetState(InternalState state);

	void DetermineNetworkState(double trend_slope, double loss_ratio, double capacity_bps);
};
