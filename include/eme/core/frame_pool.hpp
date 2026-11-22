#pragma once

#include <cstddef>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

#include "eme/core/media_buffer.hpp"

namespace eme {

class CpuBuffer final : public MediaBuffer {
 public:
  explicit CpuBuffer(std::size_t bytes) : data_(bytes) {}
  MemoryDomain domain() const noexcept override { return MemoryDomain::Cpu; }
  std::span<const std::byte> cpuData() const noexcept override { return data_; }
  std::span<std::byte> data() noexcept { return data_; }  // producer fills before publishing

 private:
  std::vector<std::byte> data_;
};

// Fixed-capacity recycling pool. acquire() never blocks: when every buffer is in flight it returns
// nullptr and the caller drops the frame. Buffers may safely outlive the pool.
class FramePool {
 public:
  FramePool(std::size_t bufferBytes, std::size_t maxBuffers);

  std::shared_ptr<CpuBuffer> acquire();
  std::size_t outstanding() const;
  std::size_t allocated() const;
  std::size_t capacity() const noexcept { return state_->maxBuffers; }

 private:
  struct State {
    std::mutex m;
    std::vector<std::unique_ptr<CpuBuffer>> free;
    std::size_t bufferBytes = 0;
    std::size_t maxBuffers = 0;
    std::size_t allocated = 0;
    std::size_t outstanding = 0;
  };
  std::shared_ptr<State> state_;
};

}  // namespace eme
