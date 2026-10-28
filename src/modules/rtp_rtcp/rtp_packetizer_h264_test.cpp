//==============================================================================
//
//  OvenMediaEngine - Unit Tests
//
//  Covers: RtpPacketizerH264
//          - AUD and HRD-timing-only SEI are not sent (IsDispensableNalUnit)
//          - other SEI (user data) and slices are
//          - a delta frame then takes only its slice's packets
//
//==============================================================================
#include <gtest/gtest.h>

#include "rtp_packetizer_h264.h"

namespace
{
	// As NVENC writes them in front of every frame (the SEI ends with a 4-byte start code's first byte)
	const std::vector<uint8_t> kAudNal{0x09, 0xF0};
	const std::vector<uint8_t> kPicTimingSeiNal{0x06, 0x01, 0x04, 0x00, 0x78, 0x08, 0x10, 0x80, 0x00};
	// buffering_period (0) and pic_timing (1) in one SEI NAL unit
	const std::vector<uint8_t> kHrdSeiNal{0x06, 0x00, 0x02, 0xAA, 0xBB, 0x01, 0x01, 0xCC, 0x80};
	// user_data_registered_itu_t_t35 (4): captions
	const std::vector<uint8_t> kCaptionSeiNal{0x06, 0x04, 0x03, 0xB5, 0x00, 0x31, 0x80};
	// pic_timing followed by user data: not only timing
	const std::vector<uint8_t> kMixedSeiNal{0x06, 0x01, 0x01, 0x10, 0x04, 0x02, 0xB5, 0x00, 0x80};

	bool Dispensable(const std::vector<uint8_t> &nal)
	{
		return RtpPacketizerH264::IsDispensableNalUnit(nal.data(), nal.size());
	}
}  // namespace

TEST(RtpPacketizerH264, AudAndTimingSeiAreDispensable)
{
	EXPECT_TRUE(Dispensable(kAudNal));
	EXPECT_TRUE(Dispensable(kPicTimingSeiNal));
	EXPECT_TRUE(Dispensable(kHrdSeiNal));
}

TEST(RtpPacketizerH264, OtherNalUnitsAreKept)
{
	EXPECT_FALSE(Dispensable(kCaptionSeiNal));
	EXPECT_FALSE(Dispensable(kMixedSeiNal));
	EXPECT_FALSE(Dispensable({0x67, 0x42, 0x00, 0x1F}));  // SPS
	EXPECT_FALSE(Dispensable({0x68, 0xCE, 0x3C, 0x80}));  // PPS
	EXPECT_FALSE(Dispensable({0x41, 0x9A, 0x00}));		  // non-IDR slice
	EXPECT_FALSE(Dispensable({0x65, 0x88, 0x84}));		  // IDR slice
	// Truncated or malformed SEI is kept
	EXPECT_FALSE(Dispensable({0x06, 0x01, 0x08, 0x00}));
	EXPECT_FALSE(Dispensable({0x06, 0x01, 0x01, 0x00, 0x7F}));
	EXPECT_FALSE(Dispensable({0x06}));
}

TEST(RtpPacketizerH264, DeltaFrameTakesOnlyItsSlicePackets)
{
	std::vector<uint8_t> slice_nal(3000, 0x5A);
	slice_nal[0] = 0x41;
	const std::vector<uint8_t> &slice = slice_nal;

	std::vector<uint8_t> frame;
	FragmentationHeader fragmentation;
	for (const auto *nal : {&kAudNal, &kPicTimingSeiNal, &slice})
	{
		fragmentation.fragmentation_offset.push_back(frame.size());
		fragmentation.fragmentation_length.push_back(nal->size());
		frame.insert(frame.end(), nal->begin(), nal->end());
	}

	RTPVideoTypeHeader header{};
	header.h26X.packetization_mode = H26XPacketizationMode::NonInterleaved;

	RtpPacketizerH264 packetizer;
	const size_t packets = packetizer.SetPayloadData(1200, 0, &header, FrameType::VideoFrameDelta, frame.data(), frame.size(), &fragmentation);

	// 3000 bytes in FU-A at 1200: 3 packets, none for the AUD and SEI
	EXPECT_EQ(packets, 3u);
}
