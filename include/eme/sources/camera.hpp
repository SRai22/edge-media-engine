#pragma once
// Platform-neutral camera capture. Linux: V4L2 (src/sources/v4l2). Windows: Media Foundation (EME-004).

#include <atomic>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <vector>

#include "eme/core/node.hpp"

namespace eme {

struct CameraMode {
  std::string fourcc;  // as reported by the device, e.g. "MJPG", "YUYV", "NV12"
  int width = 0;
  int height = 0;
  std::vector<double> fps;  // discrete rates offered at this size
};

struct CameraInfo {
  std::string id;    // open() key: device path on Linux, symbolic link on Windows
  std::string name;  // human readable
  std::string bus;   // stable-ish physical location (USB path), for persistent identity
  std::vector<CameraMode> modes;
};

// Lists video capture devices that offer at least one format we can convert to I420.
std::vector<CameraInfo> enumerateCameras();

struct CameraSourceConfig {
  std::string id;  // from CameraInfo::id
  std::uint32_t sourceId = 0;
  int width = 1280;
  int height = 720;
  int fps = 15;  // camera frames above this rate are dropped evenly
  std::size_t poolSize = 4;
};

struct CameraStats {
  std::uint64_t framesCaptured = 0;  // from the device
  std::uint64_t framesPublished = 0;
  std::uint64_t framesDropped = 0;   // pool exhausted or conversion failed
  bool deviceLost = false;
};

// Captures one camera into I420 CPU frames from a bounded pool. The capture thread never blocks on
// consumers. Device removal stops capture and sets deviceLost (reconnect: EME-010).
class CameraSource final : public Node, public Producer<VideoFrame> {
 public:
  // Opens the device and negotiates the closest mode; the negotiated format is fixed afterwards.
  static std::expected<std::unique_ptr<CameraSource>, Error> open(std::string name, CameraSourceConfig config);
  ~CameraSource() override;

  std::optional<VideoFormat> outputFormat() const override;
  Distributor<VideoFrame>& output() override { return output_; }
  Result start() override;
  void stop() noexcept override;

  CameraStats stats() const;
  std::string negotiated() const;  // e.g. "MJPG 1280x720 @30 fps (emitting 15)"

  struct Impl;  // platform specific

 private:
  CameraSource(std::string name, std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
  Distributor<VideoFrame> output_;
};

}  // namespace eme
