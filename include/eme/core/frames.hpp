#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "eme/core/media_buffer.hpp"
#include "eme/core/types.hpp"

namespace eme {

struct VideoFrame {
  std::uint64_t frameId = 0;
  std::uint32_t sourceId = 0;
  int width = 0;
  int height = 0;
  PixelFormat pixelFormat = PixelFormat::Unknown;
  std::int64_t captureTimestampNs = 0;       // steady clock at capture
  std::int64_t presentationTimestampNs = 0;  // media timeline, starts at 0
  MediaBufferHandle buffer;

  MemoryDomain memoryDomain() const noexcept { return buffer ? buffer->domain() : MemoryDomain::Cpu; }
};

// One complete access unit, Annex-B byte stream.
struct EncodedBuffer {
  std::vector<std::byte> bytes;
};

struct CodecConfiguration {
  std::uint32_t version = 0;  // bumped whenever parameter sets change
  std::shared_ptr<const std::vector<std::byte>> parameterSets;  // Annex-B SPS+PPS, shared, never copied per frame
};

struct EncodedVideoFrame {
  std::uint64_t sequence = 0;  // contiguous per encoder session; a gap means a dropped dependency
  std::uint32_t sourceId = 0;
  VideoCodec codec = VideoCodec::H264;
  std::int64_t captureTimestampNs = 0;
  std::int64_t presentationTimestampNs = 0;
  bool isKeyframe = false;
  std::shared_ptr<const EncodedBuffer> payload;
  CodecConfiguration codecConfig;
};

// Can a consumer start (or restart) decoding at this item? Used by keyframe-aware queues.
inline bool isRandomAccessPoint(const VideoFrame&) noexcept { return true; }
inline bool isRandomAccessPoint(const EncodedVideoFrame& f) noexcept { return f.isKeyframe; }

}  // namespace eme
