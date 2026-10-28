//==============================================================================
//
//  OvenMediaEngine - Unit Tests
//
//  Covers: FramePacer dispatches frames in decode (push) order
//
//==============================================================================
#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include "frame_pacer.h"

namespace
{
constexpr int64_t kTimebaseDen = 90000;
constexpr int64_t kFrameDuration = 1500;  // 60fps @ 90kHz

std::shared_ptr<MediaPacket> MakeVideoPacket(int64_t pts, int64_t dts, bool key)
{
	uint8_t payload[4] = {0x00, 0x00, 0x00, 0x01};
	return std::make_shared<MediaPacket>(cmn::MediaType::Video, 0, payload, sizeof(payload),
										 pts, dts, kFrameDuration,
										 key ? MediaPacketFlag::Key : MediaPacketFlag::NoFlag,
										 cmn::BitstreamFormat::H264_ANNEXB, cmn::PacketType::NALU);
}

class Collector
{
public:
	void Add(const std::shared_ptr<MediaPacket> &packet)
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_dts.push_back(packet->GetDts());
		_cv.notify_all();
	}

	std::vector<int64_t> WaitFor(size_t count)
	{
		std::unique_lock<std::mutex> lock(_mutex);
		_cv.wait_for(lock, std::chrono::seconds(5), [&] { return _dts.size() >= count; });
		return _dts;
	}

private:
	std::mutex _mutex;
	std::condition_variable _cv;
	std::vector<int64_t> _dts;
};

// IPBB... in decode order, as an x264/OBS source with B-frames delivers it.
// pts = display index, dts = decode index (shifted so dts <= pts).
std::vector<std::shared_ptr<MediaPacket>> MakeIPBBSequence(int groups)
{
	std::vector<std::shared_ptr<MediaPacket>> packets;
	int64_t decode_index = 0;
	// I0
	packets.push_back(MakeVideoPacket(2 * kFrameDuration, decode_index++ * kFrameDuration, true));
	for (int g = 0; g < groups; g++)
	{
		int64_t base = (g * 3 + 1);
		// P(base+2), B(base), B(base+1)
		packets.push_back(MakeVideoPacket((base + 2 + 2) * kFrameDuration, decode_index++ * kFrameDuration, false));
		packets.push_back(MakeVideoPacket((base + 0 + 2) * kFrameDuration, decode_index++ * kFrameDuration, false));
		packets.push_back(MakeVideoPacket((base + 1 + 2) * kFrameDuration, decode_index++ * kFrameDuration, false));
	}
	return packets;
}
}  // namespace

class FramePacerTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_scheduler = std::make_shared<ov::DelayQueue>("FramePacerTest");
		_scheduler->Start();
	}

	void TearDown() override
	{
		_scheduler->Stop();
	}

	std::shared_ptr<ov::DelayQueue> _scheduler;
};

// A burst of B-frame content (all pushed at once) must come out in decode order.
TEST_F(FramePacerTest, BFramesBurstDispatchInDecodeOrder)
{
	Collector collector;
	FramePacer pacer("test/burst", 1, kTimebaseDen, 20);
	pacer.Init(_scheduler, [&](const std::shared_ptr<MediaPacket> &p) { collector.Add(p); });

	auto packets = MakeIPBBSequence(10);
	std::vector<int64_t> expected;
	for (auto &p : packets)
	{
		expected.push_back(p->GetDts());
		pacer.Push(p, std::chrono::steady_clock::now());
	}

	EXPECT_EQ(collector.WaitFor(packets.size()), expected);
}

// Frames arriving in real time at their cadence must also stay in decode order.
TEST_F(FramePacerTest, BFramesRealtimeDispatchInDecodeOrder)
{
	Collector collector;
	FramePacer pacer("test/realtime", 1, kTimebaseDen, 20);
	pacer.Init(_scheduler, [&](const std::shared_ptr<MediaPacket> &p) { collector.Add(p); });

	auto packets = MakeIPBBSequence(10);
	std::vector<int64_t> expected;
	auto start = std::chrono::steady_clock::now();
	for (size_t i = 0; i < packets.size(); i++)
	{
		std::this_thread::sleep_until(start + std::chrono::microseconds(i * kFrameDuration * 1000000 / kTimebaseDen));
		expected.push_back(packets[i]->GetDts());
		pacer.Push(packets[i], std::chrono::steady_clock::now());
	}

	EXPECT_EQ(collector.WaitFor(packets.size()), expected);
}

// Without B-frames (pts == dts) pacing still delays by roughly the configured amount.
TEST_F(FramePacerTest, DispatchIsDelayedByPacerDelay)
{
	std::mutex mutex;
	std::condition_variable cv;
	std::chrono::steady_clock::time_point dispatched_at;
	bool dispatched = false;

	FramePacer pacer("test/delay", 1, kTimebaseDen, 50);
	pacer.Init(_scheduler, [&](const std::shared_ptr<MediaPacket> &) {
		std::lock_guard<std::mutex> lock(mutex);
		dispatched_at = std::chrono::steady_clock::now();
		dispatched	  = true;
		cv.notify_all();
	});

	auto pushed_at = std::chrono::steady_clock::now();
	pacer.Push(MakeVideoPacket(0, 0, true), pushed_at);

	std::unique_lock<std::mutex> lock(mutex);
	ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(2), [&] { return dispatched; }));
	auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(dispatched_at - pushed_at).count();
	EXPECT_GE(elapsed_ms, 40);
	EXPECT_LT(elapsed_ms, 500);
}

// A frame that arrives late (and the frames queued behind it) is held only for
// what is left of its delay, so it leaves on its DTS cadence: the source's
// arrival jitter is absorbed rather than passed on to the viewer.
TEST_F(FramePacerTest, LateFramesLeaveOnDtsCadence)
{
	constexpr int kFrames	   = 120;
	constexpr int kSteadyFrames = 40;  // on cadence, so the anchor calibrates first
	std::mutex mutex;
	std::vector<std::chrono::steady_clock::time_point> sent(kFrames);
	int sent_count = 0;
	std::condition_variable cv;

	FramePacer pacer("test/jitter", 1, kTimebaseDen, 50);
	pacer.Init(_scheduler, [&](const std::shared_ptr<MediaPacket> &p) {
		std::lock_guard<std::mutex> lock(mutex);
		sent[p->GetDts() / kFrameDuration] = std::chrono::steady_clock::now();
		sent_count++;
		cv.notify_all();
	});

	auto frame_time = [](int i) { return std::chrono::microseconds(i * kFrameDuration * 1000000 / kTimebaseDen); };
	auto start		= std::chrono::steady_clock::now();
	auto arrival	= start;
	for (int i = 0; i < kFrames; i++)
	{
		// Every 8th frame after the steady part is 30 ms late; the frames behind
		// it can't overtake it, so they arrive in a burst with it.
		auto late = (i >= kSteadyFrames && i % 8 == 0) ? std::chrono::milliseconds(30) : std::chrono::milliseconds(0);
		arrival	  = std::max(arrival, start + frame_time(i) + late);
		std::this_thread::sleep_until(arrival);
		pacer.Push(MakeVideoPacket(i * kFrameDuration, i * kFrameDuration, i == 0), std::chrono::steady_clock::now());
	}

	std::unique_lock<std::mutex> lock(mutex);
	ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(5), [&] { return sent_count == kFrames; }));
	int64_t lo = INT64_MAX, hi = INT64_MIN;
	for (int i = kSteadyFrames; i < kFrames; i++)
	{
		auto offset_ms = std::chrono::duration_cast<std::chrono::milliseconds>(sent[i] - (start + frame_time(i))).count();
		lo			   = std::min(lo, offset_ms);
		hi			   = std::max(hi, offset_ms);
	}
	// Released at arrival + 50 ms, the late frames would trail by 30 ms
	EXPECT_LT(hi - lo, 15) << "send offset vs DTS ranges " << lo << ".." << hi << " ms";
}
