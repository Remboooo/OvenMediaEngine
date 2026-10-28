//==============================================================================
//
//  OvenMediaEngine - Unit Tests
//
//  Covers: RtpHistory
//
//==============================================================================
#include <gtest/gtest.h>

#include "rtp_history.h"

namespace
{
constexpr uint8_t kPayloadType = 120;
constexpr uint8_t kRtxPayloadType = 121;
constexpr uint32_t kRtxSsrc = 0xCAFEF00D;
constexpr uint32_t kCapacity = 8192;

std::shared_ptr<RtpPacket> MakePacket(uint16_t seq, uint32_t timestamp, uint8_t fill)
{
	auto packet = std::make_shared<RtpPacket>();
	packet->SetPayloadType(kPayloadType);
	packet->SetSequenceNumber(seq);
	packet->SetTimestamp(timestamp);
	packet->SetSsrc(0xDEADBEEF);
	uint8_t payload[100];
	memset(payload, fill, sizeof(payload));
	packet->SetPayload(payload, sizeof(payload));
	return packet;
}
}  // namespace

TEST(RtpHistory, ReturnsStoredPacket)
{
	RtpHistory history(kPayloadType, kRtxPayloadType, kRtxSsrc, kCapacity);
	history.StoreRtpPacket(MakePacket(100, 9000, 0xAA));

	auto rtx = history.GetRtxRtpPacket(100);
	ASSERT_NE(rtx, nullptr);
	EXPECT_EQ(rtx->GetOriginalSequenceNumber(), 100);
	EXPECT_EQ(rtx->Timestamp(), 9000u);
	EXPECT_EQ(rtx->Ssrc(), kRtxSsrc);
}

// A slot overwritten by a packet with another sequence number no longer answers for the old one
TEST(RtpHistory, OverwrittenSlotIsNotFound)
{
	RtpHistory history(kPayloadType, kRtxPayloadType, kRtxSsrc, kCapacity);
	history.StoreRtpPacket(MakePacket(100, 9000, 0xAA));
	ASSERT_NE(history.GetRtxRtpPacket(100), nullptr);

	history.StoreRtpPacket(MakePacket(100 + kCapacity, 18000, 0xBB));
	EXPECT_EQ(history.GetRtxRtpPacket(100), nullptr);
}

// After the 16-bit sequence number wraps, the same number names a new packet: a NACK for it must
// get the new packet, not the RTX copy cached for the old one
TEST(RtpHistory, SequenceWrapReturnsNewPacket)
{
	RtpHistory history(kPayloadType, kRtxPayloadType, kRtxSsrc, kCapacity);
	uint32_t timestamp = 0;
	for (uint32_t i = 0; i <= 65536; i++, timestamp += 300)
	{
		auto seq = static_cast<uint16_t>(i);
		history.StoreRtpPacket(MakePacket(seq, timestamp, i < 65536 ? 0xAA : 0xBB));
		if (i == 0)
		{
			// NACKed in the first wrap: the RTX copy is cached
			ASSERT_NE(history.GetRtxRtpPacket(0), nullptr);
		}
	}

	auto rtx = history.GetRtxRtpPacket(0);
	ASSERT_NE(rtx, nullptr);
	EXPECT_EQ(rtx->Timestamp(), 65536u * 300);
	EXPECT_EQ(rtx->Payload()[0], 0xBB);
}
