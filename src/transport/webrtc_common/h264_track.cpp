#include "h264_track.hpp"

#include <chrono>
#include <random>

namespace eme::detail {
namespace {

constexpr int kH264PayloadType = 96;

// Browsers need SPS/PPS before an IDR. Encoders that only provide them out of band get them prepended.
bool startsWithSps(const std::vector<std::byte>& au) {
  std::size_t i = 0;
  if (au.size() > 4 && au[0] == std::byte{0} && au[1] == std::byte{0} && au[2] == std::byte{0} && au[3] == std::byte{1}) i = 4;
  else if (au.size() > 3 && au[0] == std::byte{0} && au[1] == std::byte{0} && au[2] == std::byte{1}) i = 3;
  else return false;
  return (std::to_integer<unsigned>(au[i]) & 0x1f) == 7;
}

std::uint32_t randomSsrc() {
  static thread_local std::mt19937 rng{std::random_device{}()};
  return std::uniform_int_distribution<std::uint32_t>(1, 0xfffffffe)(rng);
}

}  // namespace

std::shared_ptr<rtc::Track> addH264SendTrack(rtc::PeerConnection& pc, const std::string& mid, const std::string& cname,
                                             std::function<void()> onPli) {
  const auto ssrc = randomSsrc();
  rtc::Description::Video media(mid, rtc::Description::Direction::SendOnly);
  media.addH264Codec(kH264PayloadType);
  media.addSSRC(ssrc, cname, "eme", mid);
  auto track = pc.addTrack(media);

  auto rtp = std::make_shared<rtc::RtpPacketizationConfig>(ssrc, cname, kH264PayloadType, rtc::H264RtpPacketizer::ClockRate);
  auto packetizer = std::make_shared<rtc::H264RtpPacketizer>(rtc::NalUnit::Separator::StartSequence, rtp);
  packetizer->addToChain(std::make_shared<rtc::RtcpSrReporter>(rtp));
  packetizer->addToChain(std::make_shared<rtc::RtcpNackResponder>());
  packetizer->addToChain(std::make_shared<rtc::PliHandler>(std::move(onPli)));
  track->setMediaHandler(packetizer);
  return track;
}

void sendAccessUnit(rtc::Track& track, const EncodedVideoFrame& frame) {
  const auto& au = frame.payload->bytes;
  const rtc::FrameInfo info{std::chrono::duration<double>(static_cast<double>(frame.presentationTimestampNs) / 1e9)};
  if (frame.isKeyframe && frame.codecConfig.parameterSets && !startsWithSps(au)) {
    rtc::binary withConfig(*frame.codecConfig.parameterSets);
    withConfig.insert(withConfig.end(), au.begin(), au.end());
    track.sendFrame(std::move(withConfig), info);
  } else {
    track.sendFrame(au.data(), au.size(), info);  // packetizer copies into RTP packets
  }
}

}  // namespace eme::detail
