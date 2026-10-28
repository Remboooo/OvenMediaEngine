//==============================================================================
//
//  OvenMediaEngine - Unit Tests
//
//  Covers: RtcpTransportCcFeedbackGenerator (atomic check-and-generate)
//
//==============================================================================
#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

#include "rtcp_info/rtcp_transport_cc_feedback_generator.h"
#include "rtcp_info/transport_cc.h"
#include "rtcp_packet.h"
#include "rtp_header_extension/rtp_header_extension_transport_cc.h"
#include "rtp_header_extension/rtp_header_extensions.h"
#include "rtp_packet.h"

namespace
{
constexpr uint8_t kExtId = 5;
constexpr uint32_t kSenderSsrc = 0x11111111;
constexpr uint32_t kMediaSsrc = 0xDEADBEEF;

// Build a received RTP packet carrying a transport-wide sequence number.
// SetExtensions writes only the wire buffer (GetExtension reads the parsed
// map), so serialize and re-parse to populate it the way the receive path does.
std::shared_ptr<RtpPacket> MakeTwccPacket(uint16_t tw_seq, uint16_t rtp_seq)
{
	auto src = std::make_shared<RtpPacket>();
	src->SetPayloadType(96);
	src->SetSequenceNumber(rtp_seq);
	src->SetSsrc(kMediaSsrc);
	src->SetTimestamp(static_cast<uint32_t>(rtp_seq) * 90);

	auto ext = std::make_shared<RtpHeaderExtensionTransportCc>(kExtId);
	ext->SetSequenceNumber(tw_seq);

	RtpHeaderExtensions extensions;
	extensions.AddExtention(ext);
	src->SetExtensions(extensions);

	uint8_t payload[16] = {0};
	src->SetPayload(payload, sizeof(payload));

	return std::make_shared<RtpPacket>(src->GetData());
}
}  // namespace

// Sanity: the helper produces a packet whose transport-wide seq round-trips.
// If this fails, the generator tests below are testing the wrong thing.
TEST(RtcpTransportCcFeedbackGenerator, HelperRoundTripsExtension)
{
	auto packet = MakeTwccPacket(1234, 1000);
	auto value = packet->GetExtension<uint16_t>(kExtId);
	ASSERT_TRUE(value.has_value());
	EXPECT_EQ(*value, 1234);
}

// No packets accumulated -> nothing to send regardless of elapsed time.
TEST(RtcpTransportCcFeedbackGenerator, EmptyReturnsNull)
{
	RtcpTransportCcFeedbackGenerator gen(kExtId, kSenderSsrc);
	EXPECT_EQ(gen.GenerateTransportCcMessageIfElapsed(0), nullptr);
}

// The fix: check-and-consume happen under one lock. After a message is
// generated the batch is consumed, so an immediate second call returns null
// even though the 0ms interval check would otherwise pass again.
TEST(RtcpTransportCcFeedbackGenerator, GeneratesThenResets)
{
	RtcpTransportCcFeedbackGenerator gen(kExtId, kSenderSsrc);
	ASSERT_TRUE(gen.AddReceivedRtpPacket(MakeTwccPacket(1, 1000)));
	ASSERT_TRUE(gen.AddReceivedRtpPacket(MakeTwccPacket(2, 1001)));

	ASSERT_NE(gen.GenerateTransportCcMessageIfElapsed(0), nullptr);
	EXPECT_EQ(gen.GenerateTransportCcMessageIfElapsed(0), nullptr);
}

// When the interval has not elapsed the batch must be kept, not consumed.
TEST(RtcpTransportCcFeedbackGenerator, NotElapsedKeepsBatch)
{
	RtcpTransportCcFeedbackGenerator gen(kExtId, kSenderSsrc);
	ASSERT_TRUE(gen.AddReceivedRtpPacket(MakeTwccPacket(1, 1000)));

	EXPECT_EQ(gen.GenerateTransportCcMessageIfElapsed(100000), nullptr);
	EXPECT_NE(gen.GenerateTransportCcMessageIfElapsed(0), nullptr);
}

// Many threads add and generate against one generator: must not crash or
// deadlock, and must still produce feedback. Run under ThreadSanitizer to
// catch data races on the generator's internal state.
TEST(RtcpTransportCcFeedbackGenerator, ConcurrentAddAndGenerate)
{
	RtcpTransportCcFeedbackGenerator gen(kExtId, kSenderSsrc);

	std::vector<std::shared_ptr<RtpPacket>> pool;
	for (int i = 0; i < 256; i++)
	{
		pool.push_back(MakeTwccPacket(static_cast<uint16_t>(i), static_cast<uint16_t>(1000 + i)));
	}

	constexpr int kThreads = 8;
	constexpr int kIters = 500;
	std::atomic<int> generated{0};

	std::vector<std::thread> threads;
	for (int t = 0; t < kThreads; t++)
	{
		threads.emplace_back([&, t]() {
			for (int i = 0; i < kIters; i++)
			{
				gen.AddReceivedRtpPacket(pool[(t * kIters + i) % pool.size()]);
				if (gen.GenerateTransportCcMessageIfElapsed(0) != nullptr)
				{
					generated.fetch_add(1);
				}
			}
		});
	}
	for (auto &thread : threads)
	{
		thread.join();
	}

	EXPECT_GT(generated.load(), 0);
}

// Receive deltas are relative to the previous received packet (the first to the reference time), so
// arrival times accumulate: packets 2.75-3 ms apart stay that far apart after parsing. The feedback is
// built by hand as a browser sends it (draft-holmer-rmcat-transport-wide-cc-extensions-01).
TEST(TransportCcParse, ArrivalTimesAccumulateDeltas)
{
	const uint8_t ticks[] = {4, 12, 11, 12, 12, 11, 12, 12, 11, 12, 12, 11};  // 250 us each
	std::vector<uint8_t> payload = {
		0x11, 0x11, 0x11, 0x11,	 // sender SSRC
		0xDE, 0xAD, 0xBE, 0xEF,	 // media SSRC
		0x00, 0x64,				 // base sequence number 100
		0x00, 0x0C,				 // packet status count 12
		0x00, 0x00, 0x0A, 0x00,	 // reference time 10 (x 64 ms), feedback packet count 0
		0x20, 0x0C,				 // run-length chunk: 12 x "received, small delta"
	};
	payload.insert(payload.end(), std::begin(ticks), std::end(ticks));
	const uint8_t padding = static_cast<uint8_t>(4 - payload.size() % 4);
	for (uint8_t i = 1; i < padding; i++)
	{
		payload.push_back(0);
	}
	payload.push_back(padding);
	std::vector<uint8_t> rtcp = {0xA0 | 15, 205, 0, static_cast<uint8_t>(payload.size() / 4)};  // V=2 P=1 FMT=15, RTPFB
	rtcp.insert(rtcp.end(), payload.begin(), payload.end());

	RtcpPacket packet;
	size_t used = 0;
	ASSERT_TRUE(packet.Parse(rtcp.data(), rtcp.size(), used));
	auto info = std::make_shared<TransportCc>();
	ASSERT_TRUE(info->Parse(packet));
	ASSERT_EQ(info->GetPacketStatusCount(), 12u);

	int64_t expected_ticks = 640 * 4;
	for (uint32_t i = 0; i < 12; i++)
	{
		expected_ticks += ticks[i];
		EXPECT_EQ(info->GetPacketFeedbackInfo(i)->_arrival_time_ms, expected_ticks / 4) << "packet " << i;
	}
}
