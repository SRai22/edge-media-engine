// EME-015 spike: workload -> WhipPublisher (libdatachannel) -> LiveKit Ingress (WHIP, no transcoding).
// Env: WHIP_URL, WHIP_TOKEN. Args: [seconds=60] [gopFrames=150]. Prints one JSON result line on exit.
#include <iostream>

#include "../common/workload.hpp"
#include "eme/transport/whip_publisher.hpp"

int main(int argc, char** argv) {
  const int seconds = argc > 1 ? std::atoi(argv[1]) : 60;
  const int gop = argc > 2 ? std::atoi(argv[2]) : 150;
  eme::MediaGraph graph;
  auto& whip = graph.add<eme::WhipPublisher>(
      "whip", eme::WhipPublisherConfig{.endpointUrl = spike::env("WHIP_URL"), .bearerToken = spike::env("WHIP_TOKEN")});
  auto w = spike::buildWorkload(graph, gop);
  for (auto* e : w.encoders) {
    if (auto r = graph.connect(*e, whip); !r) return spdlog::error("{}", r.error().message), 1;
  }
  if (auto r = graph.start(); !r) return spdlog::error("start: {}", r.error().message), 1;
  spike::runFor(seconds);
  const auto s = whip.stats();
  graph.stop();
  nlohmann::json out{{"mode", "whip"},
                     {"setup_ms", s.setupTime ? s.setupTime->count() : -1},
                     {"frames_sent", s.framesSent},
                     {"pli_received", s.pliReceived},
                     {"cameras", spike::workloadStats(w)}};
  std::cout << out.dump() << std::endl;
}
