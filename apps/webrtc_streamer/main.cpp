// Two synthetic cameras -> OpenH264 (one session each) -> direct WebRTC peers.
// Usage: EME_TOKEN=<secret> webrtc_streamer [port=8443] [seconds=0 (run until Ctrl-C)]
// Open apps/webrtc_streamer/viewer.html?port=<port>&token=<secret> in a browser.
#include <spdlog/spdlog.h>

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <string>
#include <thread>

#include "eme/core/graph.hpp"
#include "eme/processing/encoder_node.hpp"
#include "eme/processing/openh264_encoder.hpp"
#include "eme/sources/synthetic_video_source.hpp"
#include "eme/transport/webrtc_peer_server.hpp"

using namespace eme;

namespace {
volatile std::sig_atomic_t gStop = 0;
}

int main(int argc, char** argv) {
  const char* token = std::getenv("EME_TOKEN");
  if (!token || std::string(token).size() < 8) {
    spdlog::error("set EME_TOKEN to a secret of at least 8 URL-safe characters");
    return 2;
  }
  const int port = argc > 1 ? std::atoi(argv[1]) : 8443;
  const int seconds = argc > 2 ? std::atoi(argv[2]) : 0;
  if (port <= 0 || port > 65535 || seconds < 0) {
    spdlog::error("usage: {} [port] [seconds]", argv[0]);
    return 2;
  }
  std::signal(SIGINT, [](int) { gStop = 1; });
  std::signal(SIGTERM, [](int) { gStop = 1; });

  MediaGraph graph;
  auto& server = graph.add<WebRtcPeerServer>(
      "p2p", WebRtcPeerServerConfig{.signalingPort = static_cast<std::uint16_t>(port), .accessToken = token});
  EncoderNode* encoders[2];
  for (std::uint32_t cam = 0; cam < 2; ++cam) {
    const std::string tag = "cam" + std::to_string(cam);
    auto& src = graph.add<SyntheticVideoSource>(
        tag, SyntheticSourceConfig{.sourceId = cam, .pixelFormat = PixelFormat::I420});
    auto encoder = OpenH264Encoder::create(EncoderConfig{});  // 720p15, 1.2 Mbps, 2 s GOP
    if (!encoder) {
      spdlog::error("{}", encoder.error().message);
      return 1;
    }
    encoders[cam] = &graph.add<EncoderNode>(tag + "/h264", std::move(*encoder));
    for (auto r : {graph.connect(src, *encoders[cam]), graph.connect(*encoders[cam], server)}) {
      if (!r) {
        spdlog::error("connect: {}", r.error().message);
        return 1;
      }
    }
  }
  if (auto r = graph.start(); !r) {
    spdlog::error("start: {}", r.error().message);
    return 1;
  }
  spdlog::info("viewer: apps/webrtc_streamer/viewer.html?port={}&token=<EME_TOKEN>", server.signalingPort());

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
  while (!gStop && (seconds == 0 || std::chrono::steady_clock::now() < deadline)) {
    std::this_thread::sleep_for(std::chrono::seconds(5));
    const auto s = server.stats();
    for (auto* e : encoders) {
      const auto es = e->encoder().stats();
      spdlog::info("{}: encoded={} keyframes={} forced={}", e->name(), es.framesOut, es.keyframes, es.forcedKeyframes);
    }
    for (const auto& p : s.peers) {
      spdlog::info("  peer {}: connected={} sent={} pli={}", p.id, p.connected, p.framesSent, p.pliReceived);
    }
  }
  graph.stop();
  return 0;
}
