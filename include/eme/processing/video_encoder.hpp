#pragma once

#include <cstdint>
#include <functional>

#include "eme/core/frames.hpp"
#include "eme/core/types.hpp"

namespace eme {

struct EncoderConfig {
  int width = 1280;
  int height = 720;
  int fps = 15;
  std::uint32_t bitrateBps = 1'200'000;
  int keyframeIntervalFrames = 30;  // 2 s at 15 fps
};

struct EncoderStats {
  std::uint64_t framesIn = 0;
  std::uint64_t framesOut = 0;
  std::uint64_t keyframes = 0;
  std::uint64_t keyframeRequests = 0;  // raw requests from all consumers
  std::uint64_t forcedKeyframes = 0;   // keyframes actually produced because of requests
};

// One encode session (one rendition). Backends: mock today, Media Foundation MFT next (EME-006).
class IVideoEncoder {
 public:
  using EmitFn = std::function<void(EncodedVideoFrame&&)>;

  virtual ~IVideoEncoder() = default;
  virtual VideoCodec codec() const noexcept = 0;
  virtual EncoderConfig config() const = 0;
  virtual bool supportsInput(const VideoFormat& format) const = 0;
  // Called from one worker thread. May emit zero or more access units (async hardware MFTs lag).
  virtual Result encode(const VideoFrame& frame, const EmitFn& emit) = 0;
  // Thread-safe; any number of requests before the next encoded frame yield one keyframe.
  virtual void requestKeyframe() noexcept = 0;
  virtual Result setBitrate(std::uint32_t bitrateBps) = 0;
  virtual EncoderStats stats() const = 0;
};

}  // namespace eme
