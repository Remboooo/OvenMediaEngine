//==============================================================================
//
//  OvenMediaEngine - Unit Tests
//
//  Covers: UlpfecEncodeWorker + RtpPacketizer ULPFEC emission
//          - one worker, results in submit order, bounded queues
//          - FEC is RED-wrapped and never interleaved into a frame's media
//          - each frame's media RED sequence numbers stay contiguous
//          - ULPFEC parity is computed over the media packet after its fixed header:
//            extensions and payload, RED header excluded; every protected packet recovers
//          - RTX of a RED packet carries [OSN][RED header][media payload]
//
//==============================================================================
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "red_rtp_packet.h"
#include "rtx_rtp_packet.h"
#include "rtp_packetizer.h"
#include "rtp_packetizer_interface.h"
#include "ulpfec_encode_worker.h"

namespace
{
	constexpr uint8_t kMediaPt = 98;
	constexpr uint8_t kRedPt = 120;
	constexpr uint8_t kUlpfecPt = 122;
	constexpr uint32_t kTrackId = 7;

	std::vector<std::shared_ptr<RedRtpPacket>> MakeFrame(uint16_t first_seq, size_t count, size_t payload_len = 1000)
	{
		std::vector<std::shared_ptr<RedRtpPacket>> media;
		std::vector<uint8_t> payload(payload_len, 0xAB);
		for (size_t i = 0; i < count; i++)
		{
			RtpPacket rtp;
			rtp.SetPayloadType(kMediaPt);
			rtp.SetSequenceNumber(static_cast<uint16_t>(first_seq + i));
			rtp.SetTimestamp(1000);
			rtp.SetMarker(i + 1 == count);
			rtp.SetPayload(payload.data(), payload.size());
			media.push_back(std::make_shared<RedRtpPacket>(kRedPt, rtp));
		}
		return media;
	}

	// Drain until `expected` frames are accounted for (emitted or dropped).
	std::vector<UlpfecEncodeWorker::Result> DrainAll(UlpfecEncodeWorker &worker, size_t expected)
	{
		std::vector<UlpfecEncodeWorker::Result> all;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (all.size() + worker.GetDroppedCount() < expected && std::chrono::steady_clock::now() < deadline)
		{
			for (auto &r : worker.TakeResults())
			{
				all.push_back(std::move(r));
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		return all;
	}

	// Packetize `frames` 4K-sized H.264 keyframes with ULPFEC on; returns every
	// packet the packetizer emitted, in order.
	std::vector<std::shared_ptr<RtpPacket>> PacketizeWithFec(int frames);

	class CapturingStream : public RtpPacketizerInterface
	{
	public:
		bool OnRtpPacketized(std::shared_ptr<RtpPacket> packet) override
		{
			std::lock_guard<std::mutex> lock(mutex);
			packets.push_back(packet);
			return true;
		}

		std::mutex mutex;
		std::vector<std::shared_ptr<RtpPacket>> packets;
	};
}  // namespace

TEST(UlpfecEncodeWorker, ResultsInSubmitOrder)
{
	UlpfecEncodeWorker worker("test-fec");
	for (uint32_t ts = 1; ts <= 5; ts++)
	{
		worker.Submit(ts, kTrackId, MakeFrame(static_cast<uint16_t>(ts * 100), 14));
	}

	auto results = DrainAll(worker, 5);
	ASSERT_EQ(results.size(), 5u);
	EXPECT_EQ(worker.GetDroppedCount(), 0u);
	for (size_t i = 0; i < results.size(); i++)
	{
		EXPECT_EQ(results[i].timestamp, i + 1);
		EXPECT_EQ(results[i].track_id, kTrackId);
		// 14 media packets / 7 per FEC packet -> 2 parity packets.
		EXPECT_TRUE(results[i].generator->IsAvailableFecPackets());
	}
}

TEST(UlpfecEncodeWorker, ReadyQueueIsBoundedWhenNotDrained)
{
	UlpfecEncodeWorker worker("test-fec");
	constexpr size_t kFrames = 50;
	for (uint32_t ts = 1; ts <= kFrames; ts++)
	{
		worker.Submit(ts, kTrackId, MakeFrame(static_cast<uint16_t>(ts * 100), 7, 100));
	}

	// Let the worker go idle without anyone draining.
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (worker.GetDroppedCount() < kFrames - UlpfecEncodeWorker::kMaxReadyResults &&
		   std::chrono::steady_clock::now() < deadline)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}

	auto results = worker.TakeResults();
	EXPECT_LE(results.size(), UlpfecEncodeWorker::kMaxReadyResults);
	EXPECT_EQ(results.size() + worker.GetDroppedCount(), kFrames);
	// Whatever survives is the newest parity, still in order.
	for (size_t i = 1; i < results.size(); i++)
	{
		EXPECT_LT(results[i - 1].timestamp, results[i].timestamp);
	}
	EXPECT_EQ(results.back().timestamp, kFrames);
}

TEST(UlpfecEncodeWorker, DestroyWithPendingJobs)
{
	auto worker = std::make_unique<UlpfecEncodeWorker>("test-fec");
	for (uint32_t ts = 1; ts <= 20; ts++)
	{
		worker->Submit(ts, kTrackId, MakeFrame(static_cast<uint16_t>(ts * 300), 240));
	}
	worker.reset();	 // must join promptly, not hang or crash
	SUCCEED();
}

// End-to-end through RtpPacketizer: FEC must be RED-wrapped, emitted between
// frames, and never take RED sequence numbers inside a frame's media run.
TEST(RtpPacketizerUlpfec, FecIsRedWrappedAndFramesStayContiguous)
{
	auto stream = std::make_shared<CapturingStream>();
	{
		RtpPacketizer packetizer(stream);
		packetizer.SetCodec(cmn::MediaCodecId::H264);
		packetizer.SetPayloadType(kMediaPt);
		packetizer.SetTrackId(kTrackId);
		packetizer.SetSSRC(0x1234);
		packetizer.SetUlpfec(kRedPt, kUlpfecPt);

		RTPVideoHeader video_header{};
		video_header.codec = cmn::MediaCodecId::H264;
		video_header.codec_header.h26X.packetization_mode = H26XPacketizationMode::NonInterleaved;

		// ~40KB IDR-sized NAL -> ~35 FU-A packets -> several FEC groups per frame.
		std::vector<uint8_t> nal(40000, 0x11);
		nal[0] = 0x65;
		FragmentationHeader frag;
		frag.fragmentation_offset.push_back(0);
		frag.fragmentation_length.push_back(nal.size());

		constexpr int kFrames = 30;
		for (int f = 0; f <= kFrames; f++)
		{
			if (f == kFrames)
			{
				// Give the worker time to finish the last frames; this extra
				// frame's Packetize drains their parity.
				std::this_thread::sleep_for(std::chrono::milliseconds(200));
			}
			ASSERT_TRUE(packetizer.Packetize(FrameType::VideoFrameKey, static_cast<uint32_t>(f * 1500), 0,
											 nal.data(), nal.size(), &frag, &video_header));
		}
	}

	std::vector<std::shared_ptr<RtpPacket>> red_stream;
	size_t fec_count = 0;
	for (const auto &p : stream->packets)
	{
		// ULPFEC must never go out bare (RtcStream routes bare video to non-RED peers only).
		ASSERT_NE(p->PayloadType(), kUlpfecPt);
		if (p->PayloadType() == kRedPt)
		{
			red_stream.push_back(p);
			fec_count += p->IsUlpfec() ? 1 : 0;
		}
	}
	ASSERT_FALSE(red_stream.empty());
	EXPECT_GT(fec_count, 0u);

	for (size_t i = 1; i < red_stream.size(); i++)
	{
		const auto &prev = red_stream[i - 1];
		const auto &cur = red_stream[i];
		// All RED-stream sequence numbers are assigned on one thread, in emit order.
		EXPECT_EQ(static_cast<uint16_t>(prev->SequenceNumber() + 1), cur->SequenceNumber()) << "at index " << i;
		// A frame's media run is never split by parity.
		if (cur->IsUlpfec() == false && prev->IsUlpfec() == true)
		{
			EXPECT_TRUE(cur->IsFirstPacketOfFrame()) << "FEC interleaved into a frame at index " << i;
		}
	}
}

// Parity goes out right behind its frame, not when the next frame arrives (a frame period later): a
// frame's FEC packets carry its own, older timestamp, and beside the next frame's media they read as a
// frame of jitter.
TEST(RtpPacketizerUlpfec, ParityFollowsItsFrame)
{
	auto stream = std::make_shared<CapturingStream>();
	RtpPacketizer packetizer(stream);
	packetizer.SetCodec(cmn::MediaCodecId::H264);
	packetizer.SetPayloadType(kMediaPt);
	packetizer.SetTrackId(kTrackId);
	packetizer.SetSSRC(0x1234);
	packetizer.SetUlpfec(kRedPt, kUlpfecPt);

	RTPVideoHeader video_header{};
	video_header.codec = cmn::MediaCodecId::H264;
	video_header.codec_header.h26X.packetization_mode = H26XPacketizationMode::NonInterleaved;

	std::vector<uint8_t> nal(12000, 0x11);
	nal[0] = 0x41;
	FragmentationHeader frag;
	frag.fragmentation_offset.push_back(0);
	frag.fragmentation_length.push_back(nal.size());
	for (uint32_t f = 0; f < 20; f++)
	{
		const uint32_t timestamp = f * 3000;
		ASSERT_TRUE(packetizer.Packetize(FrameType::VideoFrameDelta, timestamp, 0, nal.data(), nal.size(), &frag, &video_header));
		// Without packetizing anything else: this frame's parity is already out
		const bool has_parity = std::any_of(stream->packets.begin(), stream->packets.end(), [&](const auto &p) {
			return p->PayloadType() == kRedPt && p->IsUlpfec() && p->Timestamp() == timestamp;
		});
		EXPECT_TRUE(has_parity) << "frame " << f << ": parity not sent behind it";
	}
}

namespace
{
	std::vector<std::shared_ptr<RtpPacket>> PacketizeWithFec(int frames)
	{
		auto stream = std::make_shared<CapturingStream>();
		{
			RtpPacketizer packetizer(stream);
			packetizer.SetCodec(cmn::MediaCodecId::H264);
			packetizer.SetPayloadType(kMediaPt);
			packetizer.SetTrackId(kTrackId);
			packetizer.SetSSRC(0x1234);
			packetizer.SetUlpfec(kRedPt, kUlpfecPt);
			// As the WebRTC publisher: the parity must cover these extensions too
			packetizer.EnableTransportCc(0);
			packetizer.EnableAbsSendTime();

			RTPVideoHeader video_header{};
			video_header.codec = cmn::MediaCodecId::H264;
			video_header.codec_header.h26X.packetization_mode = H26XPacketizationMode::NonInterleaved;

			// Distinct bytes per frame/position so a misaligned XOR cannot pass by accident.
			std::vector<uint8_t> nal(40000);
			FragmentationHeader frag;
			frag.fragmentation_offset.push_back(0);
			frag.fragmentation_length.push_back(nal.size());
			for (int f = 0; f <= frames; f++)
			{
				for (size_t i = 0; i < nal.size(); i++)
				{
					nal[i] = static_cast<uint8_t>((i * 31 + f * 7) ^ (i >> 8));
				}
				nal[0] = 0x65;
				if (f == frames)
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(200));
				}
				packetizer.Packetize(FrameType::VideoFrameKey, static_cast<uint32_t>(f * 1500), 0,
									 nal.data(), nal.size(), &frag, &video_header);
			}
		}
		return stream->packets;
	}

	// Media payload carried inside a RED packet: [RED header][media payload].
	std::vector<uint8_t> RedInner(const RtpPacket &p)
	{
		return std::vector<uint8_t>(p.Payload() + 1, p.Payload() + p.PayloadSize());
	}

	// What a receiver protects and recovers of a RED media packet (RFC 5109, libwebrtc's
	// ForwardErrorCorrection): the media packet without its RED header, from after the fixed
	// 12-byte RTP header: CSRCs, header extensions, payload.
	std::vector<uint8_t> ProtectedBytes(const RtpPacket &p)
	{
		std::vector<uint8_t> out(p.Header() + 12, p.Header() + p.HeadersSize());
		const auto inner = RedInner(p);
		out.insert(out.end(), inner.begin(), inner.end());
		return out;
	}

	struct ParsedFec
	{
		const uint8_t *header;	// ULPFEC header, after the RED header
		size_t header_len;
		uint16_t sn_base;
		const uint8_t *mask;
		size_t mask_len;
		uint16_t protection_len;
	};

	ParsedFec ParseFec(const RtpPacket &f)
	{
		ParsedFec out{};
		out.header = f.Payload() + 1;
		out.mask_len = (out.header[0] & 0x40) ? 6 : 2;
		out.header_len = 10 + 2 + out.mask_len;
		out.sn_base = static_cast<uint16_t>((out.header[2] << 8) | out.header[3]);
		out.protection_len = static_cast<uint16_t>((out.header[10] << 8) | out.header[11]);
		out.mask = out.header + 12;
		return out;
	}
}  // namespace

// RFC 5109 over RED: parity covers the media packets themselves (PT 98 payload),
// not their RED encapsulation. A receiver recovers a lost packet by XORing the
// FEC payload with the other protected packets — so XOR of all protected media
// payloads (zero-padded) must equal the FEC payload, and PT recovery must be 98.
TEST(RtpPacketizerUlpfec, ParityCoversMediaPayloadNotRedHeader)
{
	auto packets = PacketizeWithFec(10);

	std::map<uint16_t, std::shared_ptr<RtpPacket>> media_by_seq;
	std::vector<std::shared_ptr<RtpPacket>> fec;
	for (const auto &p : packets)
	{
		if (p->PayloadType() != kRedPt)
		{
			continue;
		}
		if (p->IsUlpfec())
		{
			fec.push_back(p);
		}
		else
		{
			media_by_seq[p->SequenceNumber()] = p;
		}
	}
	ASSERT_FALSE(fec.empty());

	size_t checked = 0;
	for (const auto &f : fec)
	{
		const uint8_t *red = f->Payload();
		ASSERT_EQ(red[0] & 0x7F, kUlpfecPt);		 // RED block PT
		const uint8_t *fh = red + 1;				 // ULPFEC header
		const bool long_mask = (fh[0] & 0x40) != 0;
		const size_t mask_len = long_mask ? 6 : 2;
		const size_t header_len = 10 + 2 + mask_len;
		const uint16_t sn_base = static_cast<uint16_t>((fh[2] << 8) | fh[3]);
		const uint16_t protection_len = static_cast<uint16_t>((fh[10] << 8) | fh[11]);
		const uint8_t *mask = fh + 12;
		const size_t fec_payload_len = f->PayloadSize() - 1 - header_len;
		ASSERT_EQ(protection_len, fec_payload_len);

		std::vector<uint8_t> x(protection_len, 0);
		uint8_t pt_recovery = 0;
		uint16_t len_recovery = 0;
		int count = 0;
		for (size_t bit = 0; bit < mask_len * 8; bit++)
		{
			if ((mask[bit / 8] & (0x80 >> (bit % 8))) == 0)
			{
				continue;
			}
			auto it = media_by_seq.find(static_cast<uint16_t>(sn_base + bit));
			ASSERT_NE(it, media_by_seq.end()) << "mask protects a packet that is not media";
			ASSERT_TRUE(it->second->HasExtension()) << "test packets must carry header extensions";
			const auto inner = ProtectedBytes(*it->second);
			ASSERT_LE(inner.size(), x.size());
			for (size_t i = 0; i < inner.size(); i++)
			{
				x[i] ^= inner[i];
			}
			pt_recovery ^= kMediaPt;
			len_recovery ^= static_cast<uint16_t>(inner.size());
			count++;
		}
		ASSERT_GT(count, 0);
		EXPECT_EQ(fh[1] & 0x7F, pt_recovery) << "PT recovery must be the media PT, FEC #" << checked;
		EXPECT_EQ(static_cast<uint16_t>((fh[8] << 8) | fh[9]), len_recovery) << "length recovery, FEC #" << checked;
		EXPECT_TRUE(std::equal(x.begin(), x.end(), fh + header_len)) << "parity payload mismatch, FEC #" << checked;
		checked++;
	}
	EXPECT_EQ(checked, fec.size());
}

// A receiver that lost exactly one protected packet rebuilds it the way libwebrtc does
// (ForwardErrorCorrection::RecoverPacket): the FEC header's recovery fields and payload XORed with
// every other protected packet. The result must be the lost media packet byte for byte.
TEST(RtpPacketizerUlpfec, EveryProtectedPacketRecovers)
{
	auto packets = PacketizeWithFec(10);

	std::map<uint16_t, std::shared_ptr<RtpPacket>> media_by_seq;
	std::vector<std::shared_ptr<RtpPacket>> fec;
	for (const auto &p : packets)
	{
		if (p->PayloadType() != kRedPt)
		{
			continue;
		}
		(p->IsUlpfec() ? fec.push_back(p) : void(media_by_seq[p->SequenceNumber()] = p));
	}
	ASSERT_FALSE(fec.empty());

	size_t recovered = 0;
	for (const auto &f : fec)
	{
		const auto h = ParseFec(*f);
		std::vector<uint16_t> protected_seqs;
		for (size_t bit = 0; bit < h.mask_len * 8; bit++)
		{
			if (h.mask[bit / 8] & (0x80 >> (bit % 8)))
			{
				protected_seqs.push_back(static_cast<uint16_t>(h.sn_base + bit));
			}
		}

		for (auto lost : protected_seqs)
		{
			uint8_t b0 = h.header[0], b1 = h.header[1];
			uint8_t ts[4] = {h.header[4], h.header[5], h.header[6], h.header[7]};
			uint16_t length = static_cast<uint16_t>((h.header[8] << 8) | h.header[9]);
			std::vector<uint8_t> data(h.header + h.header_len, h.header + h.header_len + h.protection_len);
			for (auto seq : protected_seqs)
			{
				if (seq == lost)
				{
					continue;
				}
				const auto &m = *media_by_seq.at(seq);
				const auto bytes = ProtectedBytes(m);
				b0 ^= m.Header()[0];
				b1 ^= static_cast<uint8_t>((m.Marker() ? 0x80 : 0) | kMediaPt);
				for (int i = 0; i < 4; i++)
				{
					ts[i] ^= m.Header()[4 + i];
				}
				length ^= static_cast<uint16_t>(bytes.size());
				if (data.size() < bytes.size())
				{
					data.resize(bytes.size(), 0);
				}
				for (size_t i = 0; i < bytes.size(); i++)
				{
					data[i] ^= bytes[i];
				}
			}
			b0 = static_cast<uint8_t>((b0 | 0x80) & 0xBF);
			data.resize(length, 0);

			const auto &orig = *media_by_seq.at(lost);
			EXPECT_EQ(b0, orig.Header()[0]) << "V/P/X/CC of seq " << lost;
			EXPECT_EQ(b1, static_cast<uint8_t>((orig.Marker() ? 0x80 : 0) | kMediaPt)) << "M/PT of seq " << lost;
			EXPECT_TRUE(std::equal(ts, ts + 4, orig.Header() + 4)) << "timestamp of seq " << lost;
			EXPECT_EQ(data, ProtectedBytes(orig)) << "extensions and payload of seq " << lost;
			recovered++;
		}
	}
	EXPECT_GT(recovered, fec.size());
}

// RFC 4588: RTX payload = OSN + the original packet's payload. For RED media
// that payload starts with the RED header, so a receiver gets the RED packet back.
TEST(RtpPacketizerUlpfec, RtxOfRedPacketKeepsRedHeaderAfterOsn)
{
	auto packets = PacketizeWithFec(2);
	size_t checked = 0;
	for (const auto &p : packets)
	{
		if (p->PayloadType() != kRedPt || p->IsUlpfec())
		{
			continue;
		}
		RtxRtpPacket rtx(0x5678, 121, *p);
		// Parse the wire bytes as a receiver would: RTP header, then payload.
		const uint8_t *wire = rtx.GetData()->GetDataAs<uint8_t>();
		const size_t wire_len = rtx.GetDataLength();
		size_t header_len = 12 + 4 * (wire[0] & 0x0F);
		if (wire[0] & 0x10)
		{
			header_len += 4 + 4 * ((wire[header_len + 2] << 8) | wire[header_len + 3]);
		}
		ASSERT_EQ(wire[1] & 0x7F, 121);
		const uint8_t *pl = wire + header_len;
		ASSERT_EQ(wire_len - header_len, p->PayloadSize() + 2);
		EXPECT_EQ(static_cast<uint16_t>((pl[0] << 8) | pl[1]), p->SequenceNumber());
		EXPECT_EQ(pl[2] & 0x7F, kMediaPt);	// RED header (block PT) right after OSN
		EXPECT_TRUE(std::equal(p->Payload(), p->Payload() + p->PayloadSize(), pl + 2));
		checked++;
	}
	EXPECT_GT(checked, 0u);
}
