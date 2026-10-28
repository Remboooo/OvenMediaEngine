//==============================================================================
//
//  OvenMediaEngine
//
//  Created by Getroot
//  Copyright (c) 2025 AirenSoft. All rights reserved.
//
//==============================================================================

#include "rtc_bandwidth_estimator.h"

#include <optional>
#include "rtc_private.h"

#include <algorithm>
#include <vector>

std::shared_ptr<RtcBandwidthEstimator> RtcBandwidthEstimator::Create(const std::shared_ptr<RtcBandwidthEstimatorObserver> &observer)
{
	return std::make_shared<RtcBandwidthEstimator>(observer);
}

void RtcBandwidthEstimator::Release()
{
	_observer.reset();
	_last_signal.reset();
}

RtcBandwidthEstimator::RtcBandwidthEstimator(const std::shared_ptr<RtcBandwidthEstimatorObserver> &observer)
{
	_observer  = observer;

	_rtp_history.resize(kMaxRtpPacketHistory);
}

RtcBandwidthEstimator::~RtcBandwidthEstimator()
{
}

void RtcBandwidthEstimator::Signal(RtcBandwidthEstimatorSignal::State state)
{
	if (_observer == nullptr)
	{
		return;
	}

	auto signal = std::make_shared<RtcBandwidthEstimatorSignal>();
	signal->SetState(state);

	_last_signal = signal;

	_observer->OnSignal(signal);
}

void RtcBandwidthEstimator::OnRtpSent(uint16_t wide_seq_no, const std::shared_ptr<const RtpPacket> &rtp_packet, bool is_rtx)
{
	std::lock_guard<std::shared_mutex> lock(_rtp_history_guard);
	_rtp_history[wide_seq_no % kMaxRtpPacketHistory] = RtpHistory{
		.wide_sequence_number = wide_seq_no,
		.payload_type = rtp_packet->PayloadType(),
		.timestamp = rtp_packet->Timestamp(),
		.packet_size = rtp_packet->GetDataLength(),
		.is_video = rtp_packet->IsVideoPacket(),
		.track_id = rtp_packet->GetTrackId(),
		.is_rtx = is_rtx,
		.sent_time = std::chrono::steady_clock::now()
	};
}

void RtcBandwidthEstimator::OnTransportCc(const std::shared_ptr<const TransportCc> &transport_cc)
{
	if (_observer == nullptr)
	{
		return;
	}
	_last_transport_cc = std::chrono::steady_clock::now();

	{
		std::lock_guard<std::shared_mutex> lock(_tcc_feedbacks_guard);

		if (transport_cc == nullptr || transport_cc->GetPacketStatusCount() == 0)
		{
			return;
		}

		// Process each PacketFeedbackInfo
		std::optional<uint16_t> newest_received;
		for (uint32_t i = 0; i < transport_cc->GetPacketStatusCount(); i++)
		{
			auto packet_feedback = transport_cc->GetPacketFeedbackInfo(i);
			if (packet_feedback == nullptr)
			{
				continue;
			}

			TransportCCFeedback feedback;
			feedback.wide_seq_no = packet_feedback->_wide_sequence_number;
			feedback.received = packet_feedback->_received;
			feedback.arrival_time_ms = packet_feedback->_arrival_time_ms;
			_tcc_feedbacks.push_back(feedback);
			if (feedback.received)
			{
				newest_received = feedback.wide_seq_no;  // in sequence order
			}
		}
		if (newest_received.has_value())
		{
			UpdateBaseRtt(newest_received.value());
		}
	}

	// TODO(Getroot) : Need to consider whether to run ProcessTransportCc in a separate thread pool
	ProcessTransportCc();
}

void RtcBandwidthEstimator::OnRemb(const std::shared_ptr<const REMB> &remb)
{
	if (_observer == nullptr || remb == nullptr)
	{
		return;
	}
	if (std::chrono::steady_clock::now() - _last_transport_cc < std::chrono::milliseconds(kTransportCcFreshMs))
	{
		return;	 // transport-wide feedback is in use
	}

	double remb_bitrate_bps = static_cast<double>(remb->GetBitrateBps());
	double current_bitrate_bps = static_cast<double>(_observer->GetCurrentBitrateBps());
	[[maybe_unused]] double next_higher_bitrate_bps = static_cast<double>(_observer->GetNextHigherBitrateBps());

	// The receiver's own estimate: over-use below the current rung, and the capacity for upgrades
	const double simulated_slope = (remb_bitrate_bps < current_bitrate_bps * 0.97) ? 1.0 : 0.0;

    double current_loss_ratio = 0.0;

	logtt("REMB received: Estimated Bandwidth=%.2f Mbps, CurrentBitrate=%.2f Mbps, NextHigherBitrate=%.2f Mbps, SimulatedSlope=%.6f", remb_bitrate_bps / 1e6, current_bitrate_bps / 1e6, next_higher_bitrate_bps / 1e6, simulated_slope);

    DetermineNetworkState(simulated_slope, current_loss_ratio, remb_bitrate_bps);
}

std::shared_ptr<RtcBandwidthEstimatorSignal> RtcBandwidthEstimator::GetLastSignal() const
{
	return _last_signal;
}

bool RtcBandwidthEstimator::ProcessTransportCc()
{
	std::deque<TransportCCFeedback> batch_feedbacks;

	{
		std::lock_guard<std::shared_mutex> lock(_tcc_feedbacks_guard);
		batch_feedbacks.swap(_tcc_feedbacks);
	}

	if (batch_feedbacks.empty())
	{
		return true;
	}

	bool has_update = false;
	uint64_t latest_key = 0;
	for (const auto &feedback : batch_feedbacks)
	{
		// Find RTP history
		RtpHistory rtp_history;
		{
			std::shared_lock<std::shared_mutex> lock(_rtp_history_guard);
			const auto &item = _rtp_history[GetRtpHistoryIndex(feedback.wide_seq_no)];
			if (item.wide_sequence_number != feedback.wide_seq_no ||
				item.sent_time.time_since_epoch().count() == 0)
			{
				logtd("Cannot find RTP history for wide_seq_no(%u)", feedback.wide_seq_no);
				continue;
			}

			rtp_history = item;
		}

		if (feedback.received)
		{
			_arrivals.emplace_back(feedback.arrival_time_ms, rtp_history.packet_size);
		}

		// Video media only: audio shares the transport-wide numbers, but its timestamps are another clock,
		// and its packets in the same table made unrelated "frames" for the delay trend. Retransmissions
		// belong to an old frame and arrive an RTT later.
		if (rtp_history.is_video == false || rtp_history.is_rtx)
		{
			continue;
		}

		const uint64_t key = (static_cast<uint64_t>(rtp_history.track_id) << 32) | rtp_history.timestamp;
		FrameStats &frame_stats = _frame_stats[key];
		frame_stats.total_bytes += rtp_history.packet_size;
		frame_stats.packet_total_count += 1;

		if (feedback.received == false)
		{
			frame_stats.packet_lost_count += 1;
		}
		else
		{
			frame_stats.packet_received_count += 1;
			frame_stats.received_bytes += rtp_history.packet_size;
			if (feedback.arrival_time_ms < frame_stats.first_recv_ms)
			{
				frame_stats.first_recv_ms = feedback.arrival_time_ms;
				frame_stats.first_recv_bytes = rtp_history.packet_size;
			}
			if (feedback.arrival_time_ms > frame_stats.last_recv_ms)
			{
				frame_stats.last_recv_ms = feedback.arrival_time_ms;
			}
		}

		auto sent_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(rtp_history.sent_time.time_since_epoch()).count();
		if (sent_time_ms < frame_stats.first_send_ms)
		{
			frame_stats.first_send_ms = sent_time_ms;
		}
		if (sent_time_ms > frame_stats.last_send_ms)
		{
			frame_stats.last_send_ms = sent_time_ms;
		}
		
		// Feedback runs in send order: the last packet's frame is the newest one sent
		latest_key = key;
		has_update = true;
	}

	if (_arrivals.empty() == false)
	{
		const int64_t newest = _arrivals.back().first;
		while (_arrivals.empty() == false && newest - _arrivals.front().first > kArrivalsKeepMs)
		{
			_arrivals.pop_front();
		}
	}

	if (has_update == false)
	{
		return true;
	}

	auto it = _frame_stats.begin();
	while (it != _frame_stats.end())
	{
		// Keep the latest frame stats since it may be still updated
		if (it->first == latest_key)
		{
			++it;
		}
		else
		{
			// A train is a train on any rung; the trend and loss follow the rung being sent
			AddCapacitySample(it->second);
			UpdateQueueDelay(it->second);
			const uint32_t track_id = static_cast<uint32_t>(it->first >> 32);
			const uint32_t sending_track_id = static_cast<uint32_t>(latest_key >> 32);
			if (track_id != sending_track_id)
			{
				it = _frame_stats.erase(it);
				continue;
			}
			if (_trend_track_id != track_id)
			{
				StartTrend(track_id);
			}

			auto current_slope = UpdateTrendLine(it->second);
			auto current_loss_ratio = UpdatePacketLoss(it->second);
			auto capacity = GetCapacityBps();

			logtt("Frame timestamp: %u, TotalBytes: %zu, PacketCount: %d, LostPacketCount: %d, FirstSend: %" PRId64 " ms, FirstRecv: %" PRId64 " ms, LastRecv: %" PRId64 " ms, TrendSlope: %.6f, LossRatio: %.2f%%, Capacity: %.2f Mbps",
				it->first, it->second.total_bytes, it->second.packet_total_count, it->second.packet_lost_count,
				it->second.first_send_ms, it->second.first_recv_ms, it->second.last_recv_ms,
				current_slope, current_loss_ratio * 100.0, capacity / 1e6);

			DetermineNetworkState(current_slope, current_loss_ratio, capacity);

			it =  _frame_stats.erase(it);
		}
	}

	return true;
}

void RtcBandwidthEstimator::StartTrend(uint32_t track_id)
{
	_trend_track_id = track_id;
	_trendline_window.clear();
	_prev_frame_stats = FrameStats{};
	_trendline_slope = 0.0;
	_ema_trendline_slope = 0.0;
	_ema_packet_loss_ratio = 0.0;
	_overuse_since.reset();
}

void RtcBandwidthEstimator::UpdateBaseRtt(uint16_t newest_received_wide_seq_no)
{
	std::chrono::steady_clock::time_point sent_time;
	{
		std::shared_lock<std::shared_mutex> lock(_rtp_history_guard);
		const auto &item = _rtp_history[GetRtpHistoryIndex(newest_received_wide_seq_no)];
		if (item.wide_sequence_number != newest_received_wide_seq_no || item.sent_time.time_since_epoch().count() == 0)
		{
			return;
		}
		sent_time = item.sent_time;
	}

	// The receiver reports a packet within its feedback interval of receiving it: this feedback's
	// arrival minus that packet's send is an RTT plus at most that interval
	const auto now = std::chrono::steady_clock::now();
	const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
	const int64_t rtt_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - sent_time).count();
	while (_rtt_samples.empty() == false && _rtt_samples.back().second >= rtt_ms)
	{
		_rtt_samples.pop_back();
	}
	_rtt_samples.emplace_back(now_ms, rtt_ms);
	while (now_ms - _rtt_samples.front().first > kBaseRttWindowMs)
	{
		_rtt_samples.pop_front();
	}
	_base_rtt_ms.store(_rtt_samples.front().second, std::memory_order_relaxed);
}

uint32_t RtcBandwidthEstimator::GetRtpHistoryIndex(uint16_t wide_seq_no) const
{
	return wide_seq_no % kMaxRtpPacketHistory;
}

bool RtcBandwidthEstimator::IsNewerRtpTimestamp(uint32_t timestamp, uint32_t prev_timestamp) const
{
	return (timestamp != prev_timestamp) && (static_cast<uint32_t>(timestamp - prev_timestamp) < 0x80000000);
}

double RtcBandwidthEstimator::UpdateTrendLine(const FrameStats &current_frame_stats)
{
	static constexpr int64_t kTrendWindowSize = 30;
	static constexpr int64_t kAbsMaxDelayMs = 500; // 500 ms
	if (_prev_frame_stats.packet_total_count == 0)
	{
		_prev_frame_stats = current_frame_stats;
		return _trendline_slope;
	}

	int64_t send_delta_ms = current_frame_stats.first_send_ms - _prev_frame_stats.first_send_ms;
	int64_t recv_delta_ms = current_frame_stats.first_recv_ms - _prev_frame_stats.first_recv_ms;
	int64_t delay_ms = recv_delta_ms - send_delta_ms;

	_prev_frame_stats = current_frame_stats;

	// Accumulate delay
	int64_t acc_delay_ms = delay_ms;
	if (_trendline_window.empty() == false)
	{
		acc_delay_ms = _trendline_window.back().delay_variation_ms + delay_ms;
	}

	_trendline_window.push_back(FrameSample{
		.send_time_ms = current_frame_stats.first_send_ms,
		.arrival_time_ms = current_frame_stats.last_recv_ms,
		.delay_variation_ms =  acc_delay_ms
	});

	if (_trendline_window.size() > kTrendWindowSize)
	{
		_trendline_window.pop_front();
	}

	// To detect cases that steadily increase with a low slope: the standing queue, i.e. the accumulated
	// delay over the lowest in the window. (The accumulated delay itself keeps every past error and offset,
	// so it could stay over the limit on a drained path.)
	int64_t min_delay_ms = acc_delay_ms;
	for (const auto &sample : _trendline_window)
	{
		min_delay_ms = std::min(min_delay_ms, sample.delay_variation_ms);
	}
	if (acc_delay_ms - min_delay_ms > kAbsMaxDelayMs)
	{
		return 1.0; // Indicate over-use
	}

	if (_trendline_window.size() < kTrendWindowSize / 2)
	{
		return _trendline_slope;
	}

	_trendline_slope = CalculateTrendlineSlope();
	
	logtt("Trendline curr_delay: %" PRId64 " ms, acc_delay: %" PRId64 " ms, slope: %.6f", delay_ms, acc_delay_ms, _trendline_slope);

	return _trendline_slope;
}

double RtcBandwidthEstimator::CalculateTrendlineSlope() const
{
	size_t N = _trendline_window.size();
	if (N < 2)
	{
		return 0.0;
	}

	double sum_x = 0.0;
	double sum_y = 0.0;
	double sum_xy = 0.0;
	double sum_x2 = 0.0;

	int64_t start_x = _trendline_window.front().arrival_time_ms;
	for (const auto &sample : _trendline_window)
	{
		double x = static_cast<double>(sample.arrival_time_ms - start_x);
		double y = static_cast<double>(sample.delay_variation_ms);

		sum_x += x;
		sum_y += y;
		sum_xy += x * y;
		sum_x2 += x * x;
	}

	double numerator = (N * sum_xy) - (sum_x * sum_y);
	double denominator = (N * sum_x2) - (sum_x * sum_x);

	if (std::abs(denominator) < 1e-9)
	{
		return 0.0;
	}

	return numerator / denominator;
}

double RtcBandwidthEstimator::UpdatePacketLoss(const FrameStats& stat)
{
	static constexpr double kAlpha = 0.8;
	
	if (stat.packet_total_count == 0)
	{
		return _ema_packet_loss_ratio;
	}

	double current_loss = static_cast<double>(stat.packet_lost_count) / static_cast<double>(stat.packet_total_count);

	// Calc EMA packet loss ratio
	if (_ema_packet_loss_ratio == 0.0)
	{
		_ema_packet_loss_ratio = current_loss;
	}
	else
	{
		_ema_packet_loss_ratio = (kAlpha * _ema_packet_loss_ratio) + ((1.0 - kAlpha) * current_loss);
	}

	logtt("Packet Loss: current=%.2f%%, ema=%.2f%%", current_loss * 100.0, _ema_packet_loss_ratio * 100.0);

	return _ema_packet_loss_ratio;
}

void RtcBandwidthEstimator::AddCapacitySample(const FrameStats &stat)
{
	if (stat.packet_received_count < kMinTrainPackets || stat.received_bytes <= stat.first_recv_bytes)
	{
		return;
	}

	// The first packet's arrival starts the train; everything that arrived after it, up to the frame's last
	// packet, took the spread to cross the bottleneck
	size_t bytes = stat.received_bytes - stat.first_recv_bytes;
	size_t during = 0;
	for (const auto &arrival : _arrivals)
	{
		if (arrival.first > stat.first_recv_ms && arrival.first <= stat.last_recv_ms)
		{
			during += arrival.second;
		}
	}
	bytes = std::max(bytes, during);
	const int64_t measured_spread_ms = stat.last_recv_ms - stat.first_recv_ms;
	const int64_t spread_ms = std::max(measured_spread_ms, kMinTrainSpreadMs);
	double bps = static_cast<double>(bytes) * 8.0 * 1000.0 / static_cast<double>(spread_ms);
	// A train measures the link only if the link spread it out: OME's own pacer can send a frame more slowly
	// than the link carries it (just after an up-switch, at the old rung's rate), and the receive spread then
	// shows the pacer. Then it is a lower bound as well.
	const int64_t send_spread_ms = stat.last_send_ms - stat.first_send_ms;
	if (measured_spread_ms <= kMinTrainSpreadMs || measured_spread_ms <= send_spread_ms + kMinTrainSpreadMs)
	{
		// The train arrived faster than the receiver's timestamps resolve: only a lower bound, which a small
		// frame keeps low on any fast path. It must not argue for a down-switch: count it as unlimited.
		bps = kUnlimitedBps;
	}

	logtd("Capacity train: packets=%d bytes=%zu spread=%" PRId64 " ms -> %.2f Mbps", stat.packet_received_count, bytes, measured_spread_ms, bps / 1e6);

	const auto now = std::chrono::steady_clock::now();
	_capacity_samples.emplace_back(now, bps);
	while (_capacity_samples.empty() == false && now - _capacity_samples.front().first > std::chrono::milliseconds(kCapacityWindowMs))
	{
		_capacity_samples.pop_front();
	}
	_link_capacity_bps.store(GetCapacityBps(), std::memory_order_relaxed);
}

void RtcBandwidthEstimator::UpdateQueueDelay(const FrameStats &stat)
{
	if (stat.packet_received_count == 0 || stat.first_recv_ms == std::numeric_limits<int64_t>::max())
	{
		return;
	}

	// Receive clock minus send clock: the path's delay plus a constant offset, which the minimum removes
	const int64_t delay_ms = stat.first_recv_ms - stat.first_send_ms;
	while (_one_way_delays.empty() == false && _one_way_delays.back().second >= delay_ms)
	{
		_one_way_delays.pop_back();
	}
	_one_way_delays.emplace_back(stat.first_send_ms, delay_ms);
	while (stat.first_send_ms - _one_way_delays.front().first > kQueueBaseWindowMs)
	{
		_one_way_delays.pop_front();
	}

	_queue_delay_ms.store(delay_ms - _one_way_delays.front().second, std::memory_order_relaxed);
	_queue_sample_send_ms.store(stat.first_send_ms, std::memory_order_relaxed);

	_recent_frames.push_back({stat.first_send_ms, delay_ms, stat.packet_total_count, stat.packet_lost_count});
	while (stat.first_send_ms - _recent_frames.front().send_ms > kQueueGrowthWindowMs)
	{
		_recent_frames.pop_front();
	}
	int packets = 0, lost = 0;
	for (const auto &frame : _recent_frames)
	{
		packets += frame.packets;
		lost += frame.lost;
	}
	_recent_loss.store(packets > 0 ? static_cast<double>(lost) / packets : 0, std::memory_order_relaxed);
	if (_recent_frames.size() < kMinQueueGrowthSamples)
	{
		_queue_growth.store(0, std::memory_order_relaxed);
		return;
	}
	// Relative to the first sample, so the products stay small
	const auto &first = _recent_frames.front();
	double sx = 0, sy = 0, sxx = 0, sxy = 0;
	for (const auto &frame : _recent_frames)
	{
		const double x = static_cast<double>(frame.send_ms - first.send_ms);
		const double y = static_cast<double>(frame.delay_ms - first.delay_ms);
		sx += x;
		sy += y;
		sxx += x * x;
		sxy += x * y;
	}
	const double n = static_cast<double>(_recent_frames.size());
	const double denominator = n * sxx - sx * sx;
	_queue_growth.store(denominator > 0 ? (n * sxy - sx * sy) / denominator : 0, std::memory_order_relaxed);
}

double RtcBandwidthEstimator::GetCapacityBps(int64_t window_ms) const
{
	const auto since = std::chrono::steady_clock::now() - std::chrono::milliseconds(window_ms);
	std::vector<double> values;
	values.reserve(_capacity_samples.size());
	for (const auto &sample : _capacity_samples)
	{
		if (sample.first >= since)
		{
			values.push_back(sample.second);
		}
	}
	if (values.size() < kMinCapacitySamples)
	{
		return 0.0;	 // unknown
	}
	std::nth_element(values.begin(), values.begin() + values.size() / 2, values.end());
	return values[values.size() / 2];
}

void RtcBandwidthEstimator::DetermineNetworkState(double trend_slope, double loss_ratio, double capacity_bps)
{
	static constexpr double kAlpha = 0.8;
	if (_ema_trendline_slope == 0.0)
	{
		_ema_trendline_slope = trend_slope;
	}
	else
	{
		_ema_trendline_slope = (kAlpha * _ema_trendline_slope) + ((1.0 - kAlpha) * trend_slope);
	}

	logtt("DetermineNetworkState: slope=%.6f, emaSlope=%.6f, loss=%.2f%%, capacity=%.2f Mbps", trend_slope, _ema_trendline_slope, loss_ratio * 100.0, capacity_bps / 1e6);

	const auto now = std::chrono::steady_clock::now();
	const auto ms_since = [&now](std::chrono::steady_clock::time_point t) {
		return std::chrono::duration_cast<std::chrono::milliseconds>(now - t).count();
	};

	// Over-use must last kOverUseMinMs (not a few frames: at 60 fps three frames are 50 ms of noise);
	// heavy loss counts at once
	const double current_bitrate_bps = (_observer) ? static_cast<double>(_observer->GetCurrentBitrateBps()) : 0.0;
	const double recent_capacity_bps = GetCapacityBps(kRecentCapacityWindowMs);
	const bool does_not_fit = recent_capacity_bps > 0 && current_bitrate_bps > 0 && recent_capacity_bps < current_bitrate_bps * kStayHeadroom;
	const bool overusing = trend_slope > kOverUseSlopeThreshold || _ema_trendline_slope > kOverUseSlopeThreshold || loss_ratio > kOverUseLossThreshold || does_not_fit;
	if (overusing == false)
	{
		_overuse_since.reset();
	}
	else if (_overuse_since.has_value() == false)
	{
		_overuse_since = now;
	}
	bool congested = loss_ratio > 0.1 || (_overuse_since.has_value() && ms_since(*_overuse_since) >= kOverUseMinMs);
	// Loss too: the smoothed loss still carries the old rung's drops for a while after the switch
	if (congested && _last_downgrade.has_value() && ms_since(*_last_downgrade) < kDrainMs)
	{
		if (capacity_bps > 0 && capacity_bps >= current_bitrate_bps * kStayHeadroom)
		{
			congested = false;	// the previous rung's queue draining
		}
	}

	if (GetState() == InternalState::Cooldown)
	{
		if (GetTimeInStateMs() < kOverUseCoolDownMs)
		{
			return;
		}
		SetState(InternalState::Neutral);
	}

	if (congested)
	{
		_overuse_since.reset();
		_last_overuse = now;
		if (_last_upgrade.has_value() && ms_since(*_last_upgrade) < kFailedUpgradeWindowMs)
		{
			// The rung just reached did not fit: do not try again soon
			_upgrade_backoff_ms = std::min(std::max(_upgrade_backoff_ms * 2, kInitialUpgradeBackoffMs), kMaxUpgradeBackoffMs);
			_upgrade_blocked_until = now + std::chrono::milliseconds(_upgrade_backoff_ms);
			_last_upgrade.reset();
			logtd("Network State: upgrade failed, next try in %" PRId64 " ms", _upgrade_backoff_ms);
		}

		_last_downgrade = now;
		SetState(InternalState::Cooldown);
		logtd("Network State(%p): OverUse detected. Slope=%.6f, emaSlope=%.6f, Loss=%.2f%%, Capacity=%.2f Mbps (last 1 s %.2f, rung %.2f)", static_cast<void *>(this), trend_slope, _ema_trendline_slope, loss_ratio * 100.0, capacity_bps / 1e6, recent_capacity_bps / 1e6, current_bitrate_bps / 1e6);
		Signal(RtcBandwidthEstimatorSignal::State::OverUse);
		return;
	}

	if (_last_upgrade.has_value() && ms_since(*_last_upgrade) >= kFailedUpgradeWindowMs)
	{
		_upgrade_backoff_ms = 0;  // that upgrade held
		_last_upgrade.reset();
	}

	const double next_higher_bitrate_bps = (_observer) ? static_cast<double>(_observer->GetNextHigherBitrateBps()) : 0.0;
	const bool quiet = ms_since(_last_overuse) >= kUpgradeQuietMs &&
					   (_last_upgrade.has_value() == false || ms_since(*_last_upgrade) >= kUpgradeQuietMs);
	const bool fits = next_higher_bitrate_bps > 0 && capacity_bps >= next_higher_bitrate_bps * kUpgradeHeadroom;
	if (quiet && fits && loss_ratio < kUnderUseLossThreshold && now >= _upgrade_blocked_until)
	{
		_last_upgrade = now;
		logtd("Network State(%p): upgrade. Capacity=%.2f Mbps >= %.1f x next rung %.2f Mbps, Loss=%.2f%%", static_cast<void *>(this), capacity_bps / 1e6, kUpgradeHeadroom, next_higher_bitrate_bps / 1e6, loss_ratio * 100.0);
		Signal(RtcBandwidthEstimatorSignal::State::UnderUse);
		return;
	}

	Signal(RtcBandwidthEstimatorSignal::State::Stable);
}

RtcBandwidthEstimator::InternalState RtcBandwidthEstimator::GetState() const
{
	return _state;
}

std::chrono::steady_clock::time_point RtcBandwidthEstimator::GetStateEntryTime() const
{
	return _state_entry_time;
}

int64_t RtcBandwidthEstimator::GetTimeInStateMs() const
{
	auto now = std::chrono::steady_clock::now();
	return std::chrono::duration_cast<std::chrono::milliseconds>(now - _state_entry_time).count();
}

void RtcBandwidthEstimator::SetState(RtcBandwidthEstimator::InternalState state)
{
	if (_state == state)
	{
		return;
	}
	_state = state;
	_state_entry_time = std::chrono::steady_clock::now();
}