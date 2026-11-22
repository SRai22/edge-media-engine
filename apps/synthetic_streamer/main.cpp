// Reference topology on synthetic media: 2 sources -> 2 encoders -> {p2p, sfu, slow viewer} each.
// Usage: synthetic_streamer [seconds]
#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "eme/core/graph.hpp"
#include "eme/processing/encoder_node.hpp"
#include "eme/sources/synthetic_video_source.hpp"
#include "eme/testing/counting_sink.hpp"
#include "eme/testing/mock_h264_encoder.hpp"

using namespace eme;
using namespace std::chrono_literals;
using mock::CountingSink;

int main(int argc, char** argv) {
  const int seconds = argc > 1 ? std::atoi(argv[1]) : 5;
  if (seconds <= 0) {
    spdlog::error("usage: {} [seconds>0]", argv[0]);
    return 2;
  }

  MediaGraph graph;
  struct Pipeline {
    SyntheticVideoSource* source;
    EncoderNode* encoder;
    std::vector<CountingSink*> sinks;
  };
  std::vector<Pipeline> pipelines;
  for (std::uint32_t cam = 0; cam < 2; ++cam) {
    const std::string tag = "cam" + std::to_string(cam);
    Pipeline p;
    p.source = &graph.add<SyntheticVideoSource>(tag, SyntheticSourceConfig{.sourceId = cam});
    p.encoder = &graph.add<EncoderNode>(tag + "/h264", std::make_unique<mock::MockH264Encoder>(EncoderConfig{}));
    p.sinks = {&graph.add<CountingSink>(tag + "/p2p", CountingSink::Config{}),
               &graph.add<CountingSink>(tag + "/sfu", CountingSink::Config{}),
               &graph.add<CountingSink>(tag + "/slow", CountingSink::Config{.queueCapacity = 2, .perFrameDelay = 150ms})};
    if (auto r = graph.connect(*p.source, *p.encoder); !r) {
      spdlog::error("connect: {}", r.error().message);
      return 1;
    }
    for (auto* sink : p.sinks) {
      if (auto r = graph.connect(*p.encoder, *sink); !r) {
        spdlog::error("connect: {}", r.error().message);
        return 1;
      }
    }
    pipelines.push_back(p);
  }

  if (auto r = graph.start(); !r) {
    spdlog::error("start: {}", r.error().message);
    return 1;
  }
  std::this_thread::sleep_for(std::chrono::seconds(seconds));
  graph.stop();

  for (const auto& p : pipelines) {
    const auto es = p.encoder->encoder().stats();
    spdlog::info("{}: captured={} capture_dropped={} encoded={} keyframes={} (forced {} of {} requests)",
                 p.source->name(), p.source->framesProduced(), p.source->framesDropped(), es.framesOut,
                 es.keyframes, es.forcedKeyframes, p.encoder->keyframeRequestsReceived());
    for (const auto* sink : p.sinks) {
      const auto s = sink->stats();
      spdlog::info("  {}: received={} dropped={} max_depth={} dependency_violations={}", sink->name(), s.received,
                   s.queue.dropped, s.queue.highWatermark, s.dependencyViolations);
    }
  }
  return 0;
}
