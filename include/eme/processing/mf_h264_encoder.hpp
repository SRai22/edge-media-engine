#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "eme/processing/video_encoder.hpp"

namespace eme {

// What the hardware MFT actually accepted, read back after configuration (PLAN §9.1: settings are not assumed honoured).
struct MfEncoderInfo {
  std::string name;        // MFT friendly name, e.g. "Intel® Quick Sync Video H.264 Encoder MFT"
  std::uint32_t profile = 0;  // 66 Baseline, 77 Main, 100 High
  bool lowLatency = false;
  bool cbr = false;
  std::uint32_t bitrateBps = 0;
  std::uint32_t gop = 0;
  std::uint32_t bFrames = 0;
  bool forceKeyframeSupported = false;
};

// Hardware H.264 through a Media Foundation encoder MFT (Windows; Intel Quick Sync on the target). Hardware MFTs are
// async: encode() feeds one frame when the MFT asks for input and emits whatever output it has signalled, so output
// lags input by a frame or two. Input: I420 or NV12 CPU frames (converted to NV12 in system memory; D3D11 is EME-005).
// create() returns Unsupported when Media Foundation or a hardware H.264 MFT is unavailable: callers fall back to
// OpenH264.
class MfH264Encoder final : public IVideoEncoder {
 public:
  static std::expected<std::unique_ptr<MfH264Encoder>, Error> create(EncoderConfig config);
  ~MfH264Encoder() override;

  VideoCodec codec() const noexcept override { return VideoCodec::H264; }
  EncoderConfig config() const override;
  bool supportsInput(const VideoFormat& format) const override;
  Result encode(const VideoFrame& frame, const EmitFn& emit) override;
  void requestKeyframe() noexcept override;
  Result setBitrate(std::uint32_t bitrateBps) override;
  EncoderStats stats() const override;

  const MfEncoderInfo& info() const noexcept;

  struct Impl;

 private:
  explicit MfH264Encoder(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace eme
