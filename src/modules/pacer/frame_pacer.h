//==============================================================================
//
//  OvenMediaEngine
//
//==============================================================================
#pragma once

#include <base/mediarouter/media_buffer.h>
#include <base/ovlibrary/delay_queue.h>
#include <base/ovlibrary/ovlibrary.h>

#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <vector>

#include "adaptive_delay_controller.h"

// Per-track sender-side frame pacer (WebRTC JitterBuffer path).
//
// Schedules each frame's dispatch on a shared ov::DelayQueue. Frames may
// arrive in bursts; without smoothing, players render at the burst rate.
// Release is a fixed hold from wall-clock arrival:
//   after_ms = current AdaptiveDelay (or fallback)
// DTS is still tracked so AdaptiveDelay can record lateness and so drift
// resets keep the anchor sane, but after_ms is NOT DTS-target pacing.
// DTS-ahead scheduling (after_ms = delay + N*frame_period during catch-up)
// produced Chrome freezeEvents with packetsLost=0 under 4K+ULPFEC.
//
// Frames always leave in push (decode) order via the pending queue, even if
// millisecond rounding would otherwise reorder targets. DTS (not PTS) is used
// for lateness so B-frame content stays decode-ordered.
//
// When attached to a shared AdaptiveDelayController, the delay is queried per
// frame (Min == Max pins a fixed hold — preferred for concurrent 4K WebRTC).
class FramePacer
{
public:
	using DispatchFn = std::function<void(const std::shared_ptr<MediaPacket> &)>;

	FramePacer(const ov::String &stream_id, int64_t timebase_num, int64_t timebase_den, uint32_t fallback_delay_ms);

	// Set the scheduler (shared DelayQueue) and the dispatcher callback that
	// will be invoked by the scheduler thread at the target time.
	void Init(std::shared_ptr<ov::DelayQueue> scheduler, DispatchFn dispatcher);

	// Optional: attach a stream-shared adaptive controller. When set, the
	// pacer queries it for the current delay (instead of the fallback fixed
	// value) and reports observed lateness back to it.
	void SetAdaptiveController(std::shared_ptr<AdaptiveDelayController> controller);

	// Schedule the frame for delayed dispatch.
	//
	// arrival_time is the wall-clock moment the frame entered the publisher
	// pipeline (captured at the top of RtcStream::SendVideoFrame / SendAudioFrame).
	// It must be passed in rather than read inside Push() so that any variable
	// processing delay between entry and Push (e.g. the initial buffer flush
	// during the Started→Prepared transition) does not leak into the anchor.
	void Push(const std::shared_ptr<MediaPacket> &packet,
			  std::chrono::steady_clock::time_point arrival_time);

private:
	ov::String _stream_id;
	int64_t _timebase_num;
	int64_t _timebase_den;
	uint32_t _fallback_delay_ms;

	std::shared_ptr<AdaptiveDelayController> _adaptive_controller;

	std::shared_ptr<ov::DelayQueue> _scheduler;
	DispatchFn _dispatcher;

	// Frames waiting for dispatch, in push order. Shared with the scheduled
	// tasks so they stay valid if the FramePacer is destroyed first.
	struct PendingQueue
	{
		ov::Mutex mutex;
		std::deque<std::shared_ptr<MediaPacket>> packets OV_GUARDED_BY(mutex);
	};
	std::shared_ptr<PendingQueue> _pending = std::make_shared<PendingQueue>();

	ov::Mutex _mu;
	bool _anchor_set OV_GUARDED_BY(_mu) = false;
	int64_t _anchor_dts_us OV_GUARDED_BY(_mu) = 0;
	std::chrono::steady_clock::time_point _anchor_arrival OV_GUARDED_BY(_mu);
	std::chrono::steady_clock::time_point _last_push OV_GUARDED_BY(_mu);
	std::chrono::steady_clock::time_point _last_drift_warn OV_GUARDED_BY(_mu);

	// Anchor calibration. The provisional anchor (first frame after reset)
	// may sit at an extreme of the path-delay distribution; calibration
	// collects N frames and shifts the anchor by their median so baseline
	// lateness centers on zero.
	bool _anchor_calibrated OV_GUARDED_BY(_mu) = false;
	std::vector<int64_t> _calibration_samples_ms OV_GUARDED_BY(_mu);
	// Total frames since last (re)anchor including outliers, used as a
	// timeout when valid samples don't accumulate.
	size_t _calibration_attempts OV_GUARDED_BY(_mu) = 0;
};
