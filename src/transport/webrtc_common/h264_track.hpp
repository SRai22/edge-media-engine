#pragma once
// Internal: shared H.264 send-track setup for libdatachannel transports (direct peers, WHIP).

#include <rtc/rtc.hpp>

#include <functional>
#include <memory>
#include <string>

#include "eme/core/frames.hpp"

namespace eme::detail {

// Adds a sendonly H.264 track with packetizer -> SR reporter -> NACK responder -> PLI handler.
// onPli runs on a libdatachannel thread.
std::shared_ptr<rtc::Track> addH264SendTrack(rtc::PeerConnection& pc, const std::string& mid, const std::string& cname,
                                             std::function<void()> onPli);

// Sends one access unit; prepends out-of-band SPS/PPS to keyframes that lack them. Throws on transport errors.
void sendAccessUnit(rtc::Track& track, const EncodedVideoFrame& frame);

}  // namespace eme::detail
