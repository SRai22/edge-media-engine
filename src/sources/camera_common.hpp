#pragma once
// Backend-independent parts of eme::CameraSource, shared by the V4L2 and Media Foundation backends.

#include <algorithm>
#include <cmath>
#include <mutex>
#include <set>
#include <span>
#include <string_view>
#include <string>
#include <vector>

#include "eme/sources/camera.hpp"

namespace eme::camera_detail {

// Device ids currently owned by a CameraSource in this process, so reconnect never takes another source's camera.
class Claims {
 public:
  bool claim(const std::string& id) {
    std::lock_guard l(m_);
    return ids_.insert(id).second;
  }
  void release(const std::string& id) {
    std::lock_guard l(m_);
    ids_.erase(id);
  }
  bool claimed(const std::string& id) {
    std::lock_guard l(m_);
    return ids_.contains(id);
  }

 private:
  std::mutex m_;
  std::set<std::string> ids_;
};
inline Claims& claims() {
  static Claims c;
  return c;
}

struct ModeChoice {
  const CameraMode* mode = nullptr;
  double rate = 0;  // camera rate to request
};

// The size closest to the request (area, then aspect), then the first format in `preference` order that reaches the
// requested rate (PLAN §5.2: raw before MJPEG); then the lowest offered rate that still reaches the request (less USB
// bandwidth and work than running at max). `modes` must not be empty and only hold formats from `preference`.
inline ModeChoice chooseMode(const std::vector<CameraMode>& modes, std::span<const std::string_view> preference,
                             const CameraSourceConfig& config) {
  const auto area = [&](const CameraMode& m) { return std::abs(m.width * m.height - config.width * config.height); };
  const double wantAspect = static_cast<double>(config.width) / config.height;
  const auto aspectOff = [&](const CameraMode& m) { return std::abs(static_cast<double>(m.width) / m.height - wantAspect); };
  const auto bestArea = area(*std::ranges::min_element(modes, {}, area));
  const CameraMode* chosen = nullptr;
  double chosenFps = 0;
  for (auto f : preference) {
    for (const auto& m : modes) {
      if (area(m) != bestArea || m.fourcc != f) continue;
      const double maxFps = m.fps.empty() ? 0 : *std::ranges::max_element(m.fps);
      const bool betterShape = chosen && aspectOff(m) < aspectOff(*chosen) && maxFps >= std::min<double>(chosenFps, config.fps);
      if (!chosen || betterShape || (chosenFps < config.fps && maxFps > chosenFps)) {
        chosen = &m;
        chosenFps = maxFps;
      }
    }
    if (chosen && chosenFps >= config.fps) break;
  }
  double rate = chosenFps;
  for (double f : chosen->fps) if (f >= config.fps && f < rate) rate = f;
  return {chosen, rate};
}

// What downstream actually gets: the request, or less if the camera cannot go that fast.
inline int effectiveFps(int requested, double cameraFps) {
  return cameraFps > 0 ? std::min(requested, std::max(1, static_cast<int>(std::floor(cameraFps + 0.01)))) : requested;
}

inline bool validConfig(const CameraSourceConfig& c) {
  return c.width > 0 && c.height > 0 && c.width % 2 == 0 && c.height % 2 == 0 && c.fps > 0 && c.poolSize > 0 &&
         c.reconnectInterval.count() > 0;
}

}  // namespace eme::camera_detail
