#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "red_rtp_packet.h"
#include "ulpfec_generator.h"

// One long-lived ULPFEC Encode thread per RtpPacketizer.
//
// Encode/XOR of a 4K keyframe is too slow to run inline on the packetize
// thread, but the resulting FEC packets must still be sequence-numbered and
// emitted by the packetizer, between frames: UlpfecGenerator's protection mask
// assumes each frame's media RED sequence numbers are contiguous, so FEC must
// never take RED sequence numbers while the next frame is being numbered.
// The worker therefore only computes parity; the packetizer drains results.
//
// Both queues are bounded. When the worker falls behind, the oldest frame's
// parity is dropped — media is already on the wire and FEC is optional.
class UlpfecEncodeWorker
{
public:
	struct Result
	{
		uint32_t timestamp = 0;
		uint32_t track_id = 0;
		std::shared_ptr<UlpfecGenerator> generator;	 // holds the generated FEC payloads
	};

	static constexpr size_t kMaxPendingJobs = 8;
	static constexpr size_t kMaxReadyResults = 8;

	explicit UlpfecEncodeWorker(const std::string &thread_name);
	~UlpfecEncodeWorker();

	// Queue one frame's media RED packets (already copied) for Encode.
	void Submit(uint32_t timestamp, uint32_t track_id, std::vector<std::shared_ptr<RedRtpPacket>> &&media);

	// Encoded frames ready for emission, in submit order.
	std::vector<Result> TakeResults();

	// Waits until every submitted frame is encoded, at most `timeout`. False on timeout.
	bool WaitIdle(std::chrono::microseconds timeout);

	// Frames whose parity was dropped because a queue was full.
	uint64_t GetDroppedCount() const;

private:
	struct Job
	{
		uint32_t timestamp = 0;
		uint32_t track_id = 0;
		std::vector<std::shared_ptr<RedRtpPacket>> media;
	};

	void Run(std::string thread_name);

	mutable std::mutex _mutex;
	std::condition_variable _cv;
	std::condition_variable _idle_cv;
	bool _busy = false;
	std::deque<Job> _jobs;
	std::deque<Result> _ready;
	uint64_t _dropped = 0;
	bool _stop = false;
	std::thread _thread;
};
