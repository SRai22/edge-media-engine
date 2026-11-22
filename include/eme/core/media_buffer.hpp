#pragma once

#include <cstddef>
#include <memory>
#include <span>

#include "eme/core/types.hpp"

namespace eme {

// Immutable once published. Consumers that need GPU handles downcast to the domain-specific type;
// everyone else only sees the domain and (for CPU memory) the bytes.
class MediaBuffer {
 public:
  virtual ~MediaBuffer() = default;
  virtual MemoryDomain domain() const noexcept = 0;
  // Empty when the data is not CPU-visible (e.g. a D3D11 texture). Mapping/copying is an explicit,
  // counted operation of the GPU buffer type, never implicit here.
  virtual std::span<const std::byte> cpuData() const noexcept { return {}; }
};

using MediaBufferHandle = std::shared_ptr<const MediaBuffer>;

}  // namespace eme
