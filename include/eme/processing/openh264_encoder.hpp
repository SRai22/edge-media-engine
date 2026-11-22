#pragma once

#include <atomic>
#include <cstdint>
#include <memory>

#include "eme/processing/video_encoder.hpp"

class ISVCEncoder;

namespace eme {

// Software H.264 (Cisco OpenH264, BSD-2): Constrained Baseline, CAVLC, single slice, no B-frames.
// The fallback when no hardware encoder is available (PLAN §14.1), and the browser-decodable
// encoder for development on machines without the target hardware. Input: I420 CPU frames.
class OpenH264Encoder final : public IVideoEncoder {
 public:
  static std::expected<std::unique_ptr<OpenH264Encoder>, Error> create(EncoderConfig config);
  ~OpenH264Encoder() override;

  VideoCodec codec() const noexcept override { return VideoCodec::H264; }
  EncoderConfig config() const override;
  bool supportsInput(const VideoFormat& format) const override;
  Result encode(const VideoFrame& frame, const EmitFn& emit) override;
  void requestKeyframe() noexcept override;
  Result setBitrate(std::uint32_t bitrateBps) override;
  EncoderStats stats() const override;

 private:
  OpenH264Encoder(EncoderConfig config, ISVCEncoder* encoder);

  EncoderConfig config_;
  ISVCEncoder* encoder_;
  std::atomic<std::uint32_t> bitrate_;
  std::atomic<bool> bitrateChanged_{false};
  std::atomic<bool> keyframePending_{false};
  std::uint64_t sequence_ = 0;  // encode() thread only
  std::atomic<std::uint64_t> framesIn_{0}, framesOut_{0}, keyframes_{0}, requests_{0}, forced_{0};
};

}  // namespace eme
