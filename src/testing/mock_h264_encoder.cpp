#include "eme/testing/mock_h264_encoder.hpp"

#include <algorithm>
#include <cstring>
#include <initializer_list>

namespace eme::mock {
namespace {

void append(std::vector<std::byte>& out, std::initializer_list<unsigned> bytes) {
  for (auto b : bytes) out.push_back(static_cast<std::byte>(b));
}

}  // namespace

MockH264Encoder::MockH264Encoder(EncoderConfig config) : config_(config), bitrate_(config.bitrateBps) {
  config_.keyframeIntervalFrames = std::max(1, config_.keyframeIntervalFrames);
  config_.fps = std::max(1, config_.fps);
  auto ps = std::make_shared<std::vector<std::byte>>();
  append(*ps, {0, 0, 0, 1, 0x67, 0x42, 0xe0, 0x1f});  // SPS, Constrained Baseline 3.1 (fake body)
  append(*ps, {0, 0, 0, 1, 0x68, 0xce, 0x38, 0x80});  // PPS
  codecConfig_ = {.version = 1, .parameterSets = std::move(ps)};
}

EncoderConfig MockH264Encoder::config() const {
  auto c = config_;
  c.bitrateBps = bitrate_.load();
  return c;
}

bool MockH264Encoder::supportsInput(const VideoFormat& f) const {
  return f.kind == MediaKind::RawVideo &&
         (f.pixelFormat == PixelFormat::NV12 || f.pixelFormat == PixelFormat::I420) &&
         f.width == config_.width && f.height == config_.height;
}

Result MockH264Encoder::encode(const VideoFrame& frame, const EmitFn& emit) {
  if (!frame.buffer) return fail(ErrorCode::InvalidArgument, "frame has no buffer");
  ++framesIn_;
  const bool forced = keyframePending_.exchange(false);
  const bool key = forced || sequence_ % static_cast<std::uint64_t>(config_.keyframeIntervalFrames) == 0;
  if (forced) ++forced_;
  if (key) ++keyframes_;

  auto payload = std::make_shared<EncodedBuffer>();
  const std::size_t size = std::max<std::size_t>(16, bitrate_.load() / 8 / static_cast<unsigned>(config_.fps));
  payload->bytes.reserve(size);
  append(payload->bytes, {0, 0, 0, 1, key ? 0x65u : 0x41u});
  payload->bytes.resize(size);
  std::memcpy(payload->bytes.data() + 5, &sequence_, sizeof sequence_);

  emit(EncodedVideoFrame{
      .sequence = sequence_++,
      .sourceId = frame.sourceId,
      .codec = VideoCodec::H264,
      .captureTimestampNs = frame.captureTimestampNs,
      .presentationTimestampNs = frame.presentationTimestampNs,
      .isKeyframe = key,
      .payload = std::move(payload),
      .codecConfig = codecConfig_,
  });
  ++framesOut_;
  return {};
}

void MockH264Encoder::requestKeyframe() noexcept {
  ++requests_;
  keyframePending_.store(true);
}

Result MockH264Encoder::setBitrate(std::uint32_t bitrateBps) {
  if (bitrateBps == 0) return fail(ErrorCode::InvalidArgument, "bitrate must be positive");
  bitrate_.store(bitrateBps);
  return {};
}

EncoderStats MockH264Encoder::stats() const {
  return {framesIn_.load(), framesOut_.load(), keyframes_.load(), requests_.load(), forced_.load()};
}

}  // namespace eme::mock
