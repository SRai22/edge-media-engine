// Reference topology on synthetic media: 2 sources -> 2 encoders -> {p2p, sfu, slow viewer} each,
// or any topology from a JSON file (configs/synthetic_streamer.json shows the format).
// Usage: synthetic_streamer [seconds] [--config file.json]
#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "eme/config/engine_config.hpp"
#include "eme/core/graph.hpp"
#include "eme/processing/encoder_node.hpp"
#include "eme/sources/synthetic_video_source.hpp"
#include "eme/testing/counting_sink.hpp"
#include "eme/testing/mock_h264_encoder.hpp"

using namespace eme;
using namespace std::chrono_literals;
using mock::CountingSink;

namespace {

EngineConfig referenceTopology() {
  EngineConfig cfg;
  for (std::uint32_t cam = 0; cam < 2; ++cam) {
    PipelineConfig p{.name = "cam" + std::to_string(cam), .source = {.sourceId = cam}};
    p.sinks = {{"p2p", {}}, {"sfu", {}}, {"slow", {.queueCapacity = 2, .perFrameDelay = 150ms}}};
    cfg.pipelines.push_back(std::move(p));
  }
  return cfg;
}

}  // namespace

int main(int argc, char** argv) {
  int seconds = 5;
  EngineConfig config = referenceTopology();
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--config" && i + 1 < argc) {
#ifdef EME_HAVE_CONFIG_LOADER
      auto loaded = loadConfig(argv[++i]);
      if (!loaded) {
        spdlog::error("config: {}", loaded.error().message);
        return 2;
      }
      config = std::move(*loaded);
#else
      spdlog::error("--config needs a build with nlohmann/json (EME_WITH_CONFIG)");
      return 2;
#endif
    } else if (seconds = std::atoi(a.c_str()); seconds <= 0) {
      spdlog::error("usage: {} [seconds>0] [--config file.json]", argv[0]);
      return 2;
    }
  }

  MediaGraph graph;
  struct Pipeline {
    SyntheticVideoSource* source;
    EncoderNode* encoder;
    std::vector<CountingSink*> sinks;
  };
  std::vector<Pipeline> pipelines;
  for (const auto& pc : config.pipelines) {
    Pipeline p;
    p.source = &graph.add<SyntheticVideoSource>(pc.name, pc.source);
    p.encoder = &graph.add<EncoderNode>(pc.name + "/h264", std::make_unique<mock::MockH264Encoder>(pc.encoder), pc.encoderNode);
    for (const auto& s : pc.sinks) p.sinks.push_back(&graph.add<CountingSink>(pc.name + "/" + s.name, s.config));
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
