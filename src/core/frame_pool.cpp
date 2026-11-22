#include "eme/core/frame_pool.hpp"

namespace eme {

FramePool::FramePool(std::size_t bufferBytes, std::size_t maxBuffers) : state_(std::make_shared<State>()) {
  state_->bufferBytes = bufferBytes;
  state_->maxBuffers = maxBuffers;
}

std::shared_ptr<CpuBuffer> FramePool::acquire() {
  std::unique_ptr<CpuBuffer> buffer;
  {
    std::lock_guard lock(state_->m);
    if (!state_->free.empty()) {
      buffer = std::move(state_->free.back());
      state_->free.pop_back();
    } else if (state_->allocated < state_->maxBuffers) {
      buffer = std::make_unique<CpuBuffer>(state_->bufferBytes);
      ++state_->allocated;
    } else {
      return nullptr;
    }
    ++state_->outstanding;
  }
  std::weak_ptr<State> weak = state_;
  return std::shared_ptr<CpuBuffer>(buffer.release(), [weak](CpuBuffer* b) {
    if (auto s = weak.lock()) {
      std::lock_guard lock(s->m);
      --s->outstanding;
      s->free.emplace_back(b);
    } else {
      delete b;
    }
  });
}

std::size_t FramePool::outstanding() const {
  std::lock_guard lock(state_->m);
  return state_->outstanding;
}

std::size_t FramePool::allocated() const {
  std::lock_guard lock(state_->m);
  return state_->allocated;
}

}  // namespace eme
