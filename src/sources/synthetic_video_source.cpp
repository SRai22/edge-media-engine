#include "eme/sources/synthetic_video_source.hpp"

#include <chrono>
#include <condition_variable>
#include <algorithm>
#include <cstring>
#include <span>
#include <mutex>
#include <random>

#include "eme/testing/barcode.hpp"

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

// NV12/I420 (identical with neutral chroma): dark luma with a bright vertical bar that moves 8 px per frame; neutral chroma. Frame id in the
// first 8 luma bytes so tests can identify frames.
void drawTestPattern(std::span<std::byte> nv12, int width, int height, std::uint64_t id) {
  const auto w = static_cast<std::size_t>(width);
  const auto h = static_cast<std::size_t>(height);
  std::memset(nv12.data(), 16, w * h);
  std::memset(nv12.data() + w * h, 128, w * h / 2);
  const std::size_t barX = (id * 8) % w;
  const std::size_t barW = std::min<std::size_t>(32, w - barX);
  for (std::size_t y = 0; y < h; ++y) std::memset(nv12.data() + y * w + barX, 235, barW);
  std::memcpy(nv12.data(), &id, sizeof id);
}

}  // namespace

SyntheticVideoSource::SyntheticVideoSource(std::string name, SyntheticSourceConfig config)
    : Node(std::move(name)), config_(config), pool_(nv12Bytes(config), config.poolSize) {}

SyntheticVideoSource::~SyntheticVideoSource() { stop(); }

bool SyntheticVideoSource::valid() const noexcept {
  const bool format = config_.pixelFormat == PixelFormat::NV12 || config_.pixelFormat == PixelFormat::I420;
  return format && config_.width % 2 == 0 && config_.height % 2 == 0 && nv12Bytes(config_) >= sizeof(std::uint64_t) &&
         config_.fps > 0 && config_.poolSize > 0;
}

std::optional<VideoFormat> SyntheticVideoSource::outputFormat() const {
  return VideoFormat{.kind = MediaKind::RawVideo,
                     .pixelFormat = config_.pixelFormat,
                     .width = config_.width,
                     .height = config_.height,
                     .fps = config_.fps};
}

Result SyntheticVideoSource::start() {
  if (!valid()) return fail(ErrorCode::InvalidArgument, name() + ": needs even positive width/height, fps > 0, poolSize > 0, NV12 or I420");
  if (config_.timestampBarcode && !mock::barcodeFits(config_.width, config_.height)) {
    return fail(ErrorCode::InvalidArgument, name() + ": timestamp barcode needs width >= 512 and height >= 128");
  }
  if (config_.pattern == SyntheticPattern::MovingTexture && texture_.empty()) {
    const auto w = static_cast<std::size_t>(config_.width), h = static_cast<std::size_t>(config_.height);
    texture_.resize(w * h);
    std::mt19937 rng(config_.sourceId + 1);
    std::uniform_int_distribution<int> luma(40, 220);
    for (std::size_t by = 0; by < h; by += 8)
      for (std::size_t bx = 0; bx < w; bx += 8) {
        const auto v = static_cast<std::uint8_t>(luma(rng));
        for (std::size_t y = by; y < std::min(by + 8, h); ++y)
          std::memset(texture_.data() + y * w + bx, v, std::min<std::size_t>(8, w - bx));
      }
  }
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
  const auto captureNs = steadyNowNs();
  if (config_.pattern == SyntheticPattern::MovingTexture && !texture_.empty()) {
    const auto w = static_cast<std::size_t>(config_.width), h = static_cast<std::size_t>(config_.height);
    auto* dst = reinterpret_cast<std::uint8_t*>(buffer->data().data());
    const std::size_t shift = (id * 4) % w;
    for (std::size_t y = 0; y < h; ++y) {  // pan left: row = texture[shift..w) + texture[0..shift)
      const std::uint8_t* src = texture_.data() + y * w;
      std::memcpy(dst + y * w, src + shift, w - shift);
      std::memcpy(dst + y * w + (w - shift), src, shift);
    }
    std::memset(dst + w * h, 128, w * h / 2);
  } else {
    drawTestPattern(buffer->data(), config_.width, config_.height, id);
  }
  if (config_.timestampBarcode) {
    mock::writeBarcode(reinterpret_cast<std::uint8_t*>(buffer->data().data()), config_.width, config_.width,
                       static_cast<std::uint64_t>(captureNs / 1000));
  }
  const VideoFrame frame{
      .frameId = id,
      .sourceId = config_.sourceId,
      .width = config_.width,
      .height = config_.height,
      .pixelFormat = config_.pixelFormat,
      .captureTimestampNs = captureNs,
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
