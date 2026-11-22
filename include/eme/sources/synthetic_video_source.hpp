#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

#include "eme/core/frame_pool.hpp"
#include "eme/core/node.hpp"

namespace eme {

enum class SyntheticPattern {
  MovingBar,      // nearly free to encode; good for functional tests
  MovingTexture,  // random 8x8 blocks panning 4 px/frame: realistic encoder load and bitrate
};

struct SyntheticSourceConfig {
  std::uint32_t sourceId = 0;
  int width = 1280;
  int height = 720;
  int fps = 15;
  PixelFormat pixelFormat = PixelFormat::NV12;  // NV12 or I420 (same size, same neutral pattern)
  std::size_t poolSize = 4;
  bool manualClock = false;  // true: no thread, caller drives emitFrame() (deterministic tests)
  SyntheticPattern pattern = SyntheticPattern::MovingBar;
  bool timestampBarcode = false;  // stamp capture time (monotonic µs) for latency measurement, see eme/testing/barcode.hpp
};

// Stand-in for a camera: NV12 CPU frames from a bounded pool at a fixed rate.
class SyntheticVideoSource final : public Node, public Producer<VideoFrame> {
 public:
  SyntheticVideoSource(std::string name, SyntheticSourceConfig config);
  ~SyntheticVideoSource() override;

  std::optional<VideoFormat> outputFormat() const override;
  Distributor<VideoFrame>& output() override { return output_; }
  Result start() override;
  void stop() noexcept override;

  // Produces one frame. Never blocks; returns false (and counts a drop) if the pool is exhausted.
  bool emitFrame();

  std::uint64_t framesProduced() const noexcept { return produced_.load(); }
  std::uint64_t framesDropped() const noexcept { return dropped_.load(); }
  const FramePool& pool() const noexcept { return pool_; }

 private:
  bool valid() const noexcept;
  void run(std::stop_token st);

  SyntheticSourceConfig config_;
  FramePool pool_;
  std::vector<std::uint8_t> texture_;  // MovingTexture only, built in start()
  Distributor<VideoFrame> output_;
  std::atomic<std::uint64_t> nextId_{0};
  std::atomic<std::uint64_t> produced_{0};
  std::atomic<std::uint64_t> dropped_{0};
  std::jthread worker_;
};

}  // namespace eme
