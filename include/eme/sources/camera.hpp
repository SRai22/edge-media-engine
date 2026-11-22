#pragma once
// Platform-neutral camera capture. Linux: V4L2 (src/sources/v4l2). Windows: Media Foundation (EME-004).

#include <atomic>
#include <chrono>
#include <stop_token>
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
  std::chrono::milliseconds reconnectInterval{1000};  // how often to look for the camera after it was unplugged
};

struct CameraStats {
  std::uint64_t framesCaptured = 0;  // from the device
  std::uint64_t framesPublished = 0;
  std::uint64_t framesDropped = 0;   // pool exhausted or conversion failed
  bool deviceLost = false;        // currently unplugged / failed; capture resumes when it comes back
  std::uint64_t reconnects = 0;   // successful recoveries
};

// Captures one camera into I420 CPU frames from a bounded pool. The capture thread never blocks on consumers.
// If the camera is unplugged, capture pauses (deviceLost) and resumes automatically when the same camera is
// plugged back in, with the same format, so encoders and viewers continue without renegotiation.
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
  // Test hook: makes the capture thread treat the device as lost, exercising the release/rediscover/reopen path
  // without physically unplugging it.
  void simulateDeviceLossForTesting();
  std::string negotiated() const;  // e.g. "MJPG 1280x720 @30 fps (emitting 15)"

  struct Impl;  // platform specific

 private:
  CameraSource(std::string name, std::unique_ptr<Impl> impl);
  void run(std::stop_token st);
  bool reconnect();
  std::unique_ptr<Impl> impl_;
  Distributor<VideoFrame> output_;
};

}  // namespace eme
