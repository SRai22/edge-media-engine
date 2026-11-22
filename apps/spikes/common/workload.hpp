#pragma once
// Identical workload for both SFU publisher spikes (EME-014/015): 2 x 720p15 textured cameras with a
// capture-time barcode -> 2 OpenH264 sessions at 1.2 Mbps. Only the sink differs between spikes.

#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>

#include "eme/core/graph.hpp"
#include "eme/processing/encoder_node.hpp"
#include "eme/processing/openh264_encoder.hpp"
#include "eme/sources/synthetic_video_source.hpp"

namespace spike {

inline volatile std::sig_atomic_t gStop = 0;

struct Workload {
  eme::SyntheticVideoSource* sources[2] = {};
  eme::EncoderNode* encoders[2] = {};
};

inline std::string env(const char* name) {
  const char* v = std::getenv(name);
  if (!v || !*v) {
    spdlog::error("missing environment variable {}", name);
    std::exit(2);
  }
  return v;
}

// gopFrames: periodic keyframe interval; long GOPs make late-join recovery depend on keyframe requests.
inline Workload buildWorkload(eme::MediaGraph& graph, int gopFrames) {
  Workload w;
  for (std::uint32_t cam = 0; cam < 2; ++cam) {
    const std::string tag = "cam" + std::to_string(cam);
    w.sources[cam] = &graph.add<eme::SyntheticVideoSource>(
        tag, eme::SyntheticSourceConfig{.sourceId = cam, .pixelFormat = eme::PixelFormat::I420,
                                        .pattern = eme::SyntheticPattern::MovingTexture, .timestampBarcode = true});
    auto enc = eme::OpenH264Encoder::create(eme::EncoderConfig{.keyframeIntervalFrames = gopFrames});
    if (!enc) {
      spdlog::error("{}", enc.error().message);
      std::exit(1);
    }
    w.encoders[cam] = &graph.add<eme::EncoderNode>(tag + "/h264", std::move(*enc));
    if (auto r = graph.connect(*w.sources[cam], *w.encoders[cam]); !r) {
      spdlog::error("connect: {}", r.error().message);
      std::exit(1);
    }
  }
  return w;
}

inline nlohmann::json workloadStats(const Workload& w) {
  nlohmann::json out = nlohmann::json::array();
  for (int i = 0; i < 2; ++i) {
    const auto es = w.encoders[i]->encoder().stats();
    out.push_back({{"captured", w.sources[i]->framesProduced()}, {"capture_dropped", w.sources[i]->framesDropped()},
                   {"encoded", es.framesOut}, {"encode_input_dropped", w.encoders[i]->inputStats().dropped},
                   {"keyframes", es.keyframes}, {"keyframe_requests", w.encoders[i]->keyframeRequestsReceived()},
                   {"forced_keyframes", es.forcedKeyframes}});
  }
  return out;
}

// Runs until SIGINT/SIGTERM or `seconds` elapse (0 = forever).
inline void runFor(int seconds) {
  std::signal(SIGINT, [](int) { gStop = 1; });
  std::signal(SIGTERM, [](int) { gStop = 1; });
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
  while (!gStop && (seconds == 0 || std::chrono::steady_clock::now() < deadline)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

}  // namespace spike
