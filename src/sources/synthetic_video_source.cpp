#include "eme/sources/synthetic_video_source.hpp"

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>

namespace eme {
namespace {

std::int64_t steadyNowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::size_t nv12Bytes(const SyntheticSourceConfig& c) {
  if (c.width <= 0 || c.height <= 0) return 0;
  return static_cast<std::size_t>(c.width) * static_cast<std::size_t>(c.height) * 3 / 2;
}

}  // namespace

SyntheticVideoSource::SyntheticVideoSource(std::string name, SyntheticSourceConfig config)
    : Node(std::move(name)), config_(config), pool_(nv12Bytes(config), config.poolSize) {}

SyntheticVideoSource::~SyntheticVideoSource() { stop(); }

bool SyntheticVideoSource::valid() const noexcept {
  return nv12Bytes(config_) >= sizeof(std::uint64_t) && config_.fps > 0 && config_.poolSize > 0;
}

std::optional<VideoFormat> SyntheticVideoSource::outputFormat() const {
  return VideoFormat{.kind = MediaKind::RawVideo,
                     .pixelFormat = PixelFormat::NV12,
                     .width = config_.width,
                     .height = config_.height,
                     .fps = config_.fps};
}

Result SyntheticVideoSource::start() {
  if (!valid()) return fail(ErrorCode::InvalidArgument, name() + ": width/height/fps/poolSize must be positive");
  if (!config_.manualClock) worker_ = std::jthread([this](std::stop_token st) { run(st); });
  return {};
}

void SyntheticVideoSource::stop() noexcept {
  if (worker_.joinable()) {
    worker_.request_stop();
    worker_.join();
  }
}

bool SyntheticVideoSource::emitFrame() {
  if (!valid()) return false;  // manual-clock callers may skip start()
  auto buffer = pool_.acquire();
  if (!buffer) {
    ++dropped_;
    return false;
  }
  const std::uint64_t id = nextId_++;
  // shortcut: stamps only the frame id into the luma plane; add real test patterns if a consumer inspects pixels.
  std::memcpy(buffer->data().data(), &id, sizeof id);
  const VideoFrame frame{
      .frameId = id,
      .sourceId = config_.sourceId,
      .width = config_.width,
      .height = config_.height,
      .pixelFormat = PixelFormat::NV12,
      .captureTimestampNs = steadyNowNs(),
      .presentationTimestampNs = static_cast<std::int64_t>(id * 1'000'000'000ULL / static_cast<unsigned>(config_.fps)),
      .buffer = std::move(buffer),
  };
  ++produced_;
  output_.publish(frame);
  return true;
}

void SyntheticVideoSource::run(std::stop_token st) {
  using clock = std::chrono::steady_clock;
  const auto period = std::chrono::nanoseconds(1'000'000'000 / config_.fps);
  std::mutex m;
  std::condition_variable_any cv;
  auto next = clock::now();
  while (!st.stop_requested()) {
    emitFrame();
    next += period;
    if (clock::now() - next > period) next = clock::now();  // fell behind: don't burst to catch up
    std::unique_lock lock(m);
    cv.wait_until(lock, st, next, [] { return false; });
  }
}

}  // namespace eme
