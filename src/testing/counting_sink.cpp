#include "eme/testing/counting_sink.hpp"

namespace eme::mock {

CountingSink::CountingSink(std::string name, Config config) : Node(std::move(name)), config_(config) {}

CountingSink::~CountingSink() { stop(); }

bool CountingSink::accepts(const VideoFormat& format) const {
  return format.kind == MediaKind::EncodedVideo && format.codec == VideoCodec::H264;
}

Result CountingSink::bindInput(Distributor<EncodedVideoFrame>& upstream) {
  if (input_) return fail(ErrorCode::InvalidState, name() + " already has an input");
  input_ = upstream.subscribe(config_.queueCapacity, OverflowPolicy::DropUntilKeyframe);
  return {};
}

Result CountingSink::start() {
  if (!input_) return fail(ErrorCode::InvalidState, name() + " has no input");
  worker_ = std::jthread([this](std::stop_token st) { run(st); });
  return {};
}

void CountingSink::stop() noexcept {
  if (worker_.joinable()) {
    worker_.request_stop();
    worker_.join();
  }
}

SinkStats CountingSink::stats() const {
  std::lock_guard lock(m_);
  SinkStats s = stats_;
  if (input_) s.queue = input_->stats();
  return s;
}

void CountingSink::run(std::stop_token st) {
  while (auto frame = input_->pop(st)) {
    if (config_.beforeFrame) config_.beforeFrame();
    if (config_.perFrameDelay.count() > 0) std::this_thread::sleep_for(config_.perFrameDelay);
    std::lock_guard lock(m_);
    ++stats_.received;
    if (frame->isKeyframe) ++stats_.keyframes;
    if (!frame->isKeyframe && (!lastSequence_ || frame->sequence != *lastSequence_ + 1)) {
      ++stats_.dependencyViolations;
    }
    lastSequence_ = frame->sequence;
    stats_.lastPayload = frame->payload.get();
  }
}

}  // namespace eme::mock
