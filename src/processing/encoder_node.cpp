#include "eme/processing/encoder_node.hpp"

#include <spdlog/spdlog.h>

namespace eme {

EncoderNode::EncoderNode(std::string name, std::unique_ptr<IVideoEncoder> encoder, Config config)
    : Node(std::move(name)), encoder_(std::move(encoder)), config_(config) {
  output_.setKeyframeRequester([this] {
    ++keyframeRequests_;
    keyframeWanted_.store(true);
  });
}

EncoderNode::~EncoderNode() { stop(); }

std::optional<VideoFormat> EncoderNode::outputFormat() const {
  const auto c = encoder_->config();
  return VideoFormat{.kind = MediaKind::EncodedVideo,
                     .codec = encoder_->codec(),
                     .width = c.width,
                     .height = c.height,
                     .fps = c.fps};
}

bool EncoderNode::accepts(const VideoFormat& format) const {
  return format.kind == MediaKind::RawVideo && encoder_->supportsInput(format);
}

Result EncoderNode::bindInput(Distributor<VideoFrame>& upstream) {
  if (input_) return fail(ErrorCode::InvalidState, name() + " already has an input");
  input_ = upstream.subscribe(config_.inputQueueCapacity, OverflowPolicy::DropOldest);
  return {};
}

Result EncoderNode::start() {
  if (!input_) return fail(ErrorCode::InvalidState, name() + " has no input");
  worker_ = std::jthread([this](std::stop_token st) { run(st); });
  return {};
}

void EncoderNode::stop() noexcept {
  if (worker_.joinable()) {
    worker_.request_stop();
    worker_.join();
  }
}

QueueStats EncoderNode::inputStats() const { return input_ ? input_->stats() : QueueStats{}; }

void EncoderNode::run(std::stop_token st) {
  const IVideoEncoder::EmitFn emit = [this](EncodedVideoFrame&& f) { output_.publish(f); };
  while (auto frame = input_->pop(st)) {
    const auto now = std::chrono::steady_clock::now();
    const bool allowed = !lastKeyframeForward_ || now - *lastKeyframeForward_ >= config_.minKeyframeInterval;
    if (allowed && keyframeWanted_.exchange(false)) {
      lastKeyframeForward_ = now;
      encoder_->requestKeyframe();
    }
    if (auto r = encoder_->encode(*frame, emit); !r) {
      ++errors_;
      spdlog::warn("{}: encode failed: {}", name(), r.error().message);
    }
  }
}

}  // namespace eme
