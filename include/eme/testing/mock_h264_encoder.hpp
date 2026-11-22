#pragma once

#include <atomic>
#include <cstdint>

#include "eme/processing/video_encoder.hpp"

namespace eme::mock {

// Produces Annex-B-shaped access units (IDR / non-IDR NAL headers, fake SPS/PPS) sized to the
// configured bitrate. Every non-key frame depends on the previous one (P-frames, no B-frames).
class MockH264Encoder final : public IVideoEncoder {
 public:
  explicit MockH264Encoder(EncoderConfig config);

  VideoCodec codec() const noexcept override { return VideoCodec::H264; }
  EncoderConfig config() const override;
  bool supportsInput(const VideoFormat& format) const override;
  Result encode(const VideoFrame& frame, const EmitFn& emit) override;
  void requestKeyframe() noexcept override;
  Result setBitrate(std::uint32_t bitrateBps) override;
  EncoderStats stats() const override;

 private:
  EncoderConfig config_;
  std::atomic<std::uint32_t> bitrate_;
  CodecConfiguration codecConfig_;
  std::uint64_t sequence_ = 0;  // encode() thread only
  std::atomic<bool> keyframePending_{false};
  std::atomic<std::uint64_t> framesIn_{0}, framesOut_{0}, keyframes_{0}, requests_{0}, forced_{0};
};

}  // namespace eme::mock
