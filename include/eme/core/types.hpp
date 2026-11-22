#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <utility>

namespace eme {

enum class PixelFormat : std::uint8_t { Unknown, NV12, I420, YUY2, MJPEG };
enum class MemoryDomain : std::uint8_t { Cpu, D3D11Texture, DxgiSurface, Cuda };
enum class VideoCodec : std::uint8_t { H264 };
enum class MediaKind : std::uint8_t { RawVideo, EncodedVideo };

enum class ErrorCode : std::uint8_t {
  InvalidArgument,
  InvalidState,
  Incompatible,
  Cycle,
  Unsupported,
  ResourceExhausted,
  DeviceLost,
  Internal,
};

struct Error {
  ErrorCode code;
  std::string message;
};

using Result = std::expected<void, Error>;

inline std::unexpected<Error> fail(ErrorCode code, std::string message) {
  return std::unexpected(Error{code, std::move(message)});
}

// What a node produces or consumes; used by MediaGraph::connect to validate edges.
struct VideoFormat {
  MediaKind kind = MediaKind::RawVideo;
  PixelFormat pixelFormat = PixelFormat::Unknown;  // raw only
  VideoCodec codec = VideoCodec::H264;             // encoded only
  int width = 0;
  int height = 0;
  int fps = 0;
};

}  // namespace eme
