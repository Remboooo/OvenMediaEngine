#pragma once

#include "rtp_packet.h"
#include "rtcp_packet.h"

class RtpPacketizerInterface : public ov::EnableSharedFromThis<RtpPacketizerInterface>
{
public:
    // RTP Packet을 전송한다.
    virtual bool        OnRtpPacketized(std::shared_ptr<RtpPacket> packet) = 0;

	// When Ulpfec/RED is on, media is normally emitted as RED only. Return true
	// if any interested session for this track still needs the unwrapped RTP
	// (peers that did not negotiate RED). Default false keeps the RED-only
	// hot path when every viewer can accept RED.
	virtual bool NeedsRawRtpFanout(uint32_t track_id) const
	{
		(void)track_id;
		return false;
	}
};
