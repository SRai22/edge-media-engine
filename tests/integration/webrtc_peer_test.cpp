// EME-008: direct WebRTC peers over real libdatachannel loopback connections (receiver = C++ client).
#include <gtest/gtest.h>

#include <rtc/rtc.hpp>

#include <atomic>
#include <chrono>
#include <mutex>
#include <nlohmann/json.hpp>
#include <thread>

#include "eme/core/graph.hpp"
#include "eme/processing/encoder_node.hpp"
#include "eme/sources/synthetic_video_source.hpp"
#include "eme/testing/mock_h264_encoder.hpp"
#include "eme/transport/webrtc_peer_server.hpp"
#include "test_util.hpp"

using namespace eme;
using namespace std::chrono_literals;

namespace {

constexpr auto kToken = "test-token-123";
[[maybe_unused]] const bool kLog = [] { if (std::getenv("EME_RTC_LOG")) rtc::InitLogger(rtc::LogLevel::Debug); return true; }();

// Minimal receiver: answers the server's offer and collects depacketized access units per track.
class Receiver {
 public:
  Receiver(std::uint16_t port, const std::string& token) {
    pc_ = std::make_shared<rtc::PeerConnection>();
    pc_->onTrack([this](std::shared_ptr<rtc::Track> track) {
      auto depacketizer = std::make_shared<rtc::H264RtpDepacketizer>();
      depacketizer->addToChain(std::make_shared<rtc::RtcpReceivingSession>());
      track->setMediaHandler(depacketizer);
      track->onFrame([this, mid = track->mid()](rtc::binary data, rtc::FrameInfo) {
        std::lock_guard lock(m_);
        frames_[mid].push_back(std::move(data));
      });
      std::lock_guard lock(m_);
      tracks_.push_back(track);
    });
    pc_->onGatheringStateChange([this](rtc::PeerConnection::GatheringState s) {
      if (s != rtc::PeerConnection::GatheringState::Complete) return;
      auto d = pc_->localDescription();
      ws_->send(nlohmann::json{{"type", d->typeString()}, {"sdp", std::string(*d)}}.dump());
    });
    ws_ = std::make_shared<rtc::WebSocket>();
    ws_->onMessage([this](rtc::message_variant msg) {
      const auto j = nlohmann::json::parse(std::get<std::string>(msg));
      pc_->setRemoteDescription(rtc::Description(j.at("sdp").get<std::string>(), j.at("type").get<std::string>()));
    });
    ws_->onClosed([this] { closed_ = true; });
    ws_->open("ws://127.0.0.1:" + std::to_string(port) + "/?token=" + token);
  }
  ~Receiver() {
    // Callbacks capture `this`: clear them before libdatachannel threads can fire them again.
    {
      std::lock_guard lock(m_);
      for (auto& t : tracks_) t->resetCallbacks();
    }
    pc_->resetCallbacks();
    ws_->resetCallbacks();
    pc_->close();
    ws_->close();
  }

  std::size_t frameCount(const std::string& mid) {
    std::lock_guard lock(m_);
    return frames_[mid].size();
  }
  std::vector<rtc::binary> frames(const std::string& mid) {
    std::lock_guard lock(m_);
    return frames_[mid];
  }
  bool requestKeyframe() {
    std::lock_guard lock(m_);
    return !tracks_.empty() && tracks_[0]->requestKeyframe();
  }
  bool closed() const { return closed_; }

 private:
  std::shared_ptr<rtc::PeerConnection> pc_;
  std::shared_ptr<rtc::WebSocket> ws_;
  std::mutex m_;
  std::vector<std::shared_ptr<rtc::Track>> tracks_;
  std::map<std::string, std::vector<rtc::binary>> frames_;
  std::atomic<bool> closed_{false};
};

struct Rig {
  MediaGraph graph;
  SyntheticVideoSource* cams[2];
  EncoderNode* encoders[2];
  WebRtcPeerServer* server;
  std::atomic<bool> pumping{true};
  std::jthread pump;

  explicit Rig(std::size_t maxPeers) {
    server = &graph.add<WebRtcPeerServer>(
        "p2p", WebRtcPeerServerConfig{.signalingPort = 0, .bindAddress = "127.0.0.1", .accessToken = kToken,
                                      .maxPeers = maxPeers});
    for (int i = 0; i < 2; ++i) {
      cams[i] = &graph.add<SyntheticVideoSource>(
          "cam" + std::to_string(i), SyntheticSourceConfig{.sourceId = static_cast<std::uint32_t>(i), .width = 64,
                                                           .height = 32, .fps = 30, .manualClock = true});
      encoders[i] = &graph.add<EncoderNode>(
          "enc" + std::to_string(i),
          std::make_unique<mock::MockH264Encoder>(EncoderConfig{.width = 64, .height = 32, .fps = 30, .keyframeIntervalFrames = 1000}),
          EncoderNode::Config{.minKeyframeInterval = 0ms});
      EXPECT_TRUE(graph.connect(*cams[i], *encoders[i]));
      EXPECT_TRUE(graph.connect(*encoders[i], *server));
    }
    EXPECT_TRUE(graph.start());
    pump = std::jthread([this](std::stop_token st) {
      while (!st.stop_requested()) {
        if (pumping) for (auto* c : cams) c->emitFrame();
        std::this_thread::sleep_for(10ms);
      }
    });
  }
};

}  // namespace

TEST(WebRtcPeerServer, TwoPeersShareOneEncodePerCamera) {
  Rig rig(3);
  const auto port = rig.server->signalingPort();
  ASSERT_GT(port, 0);
  Receiver a(port, kToken), b(port, kToken);
  ASSERT_TRUE(test::waitFor([&] {
    return a.frameCount("video0") >= 20 && a.frameCount("video1") >= 20 && b.frameCount("video0") >= 20 &&
           b.frameCount("video1") >= 20;
  }, 15s)) << "a=" << a.frameCount("video0") << " b=" << b.frameCount("video0");

  // Every peer starts at a keyframe with SPS/PPS prepended (mock encoder sends them out of band).
  for (auto* r : {&a, &b}) {
    for (const char* mid : {"video0", "video1"}) {
      const auto types = test::nalTypes(r->frames(mid).front());
      ASSERT_GE(types.size(), 3u) << mid;
      EXPECT_EQ(types[0], 7);
      EXPECT_EQ(types[1], 8);
      EXPECT_EQ(types[2], 5);
    }
  }
  rig.pumping = false;
  // Shared encode: with two peers attached, each captured frame is still encoded at most once
  // (per-peer encoding would give framesIn ~= 2x captured), and every peer's sends come from that one stream.
  std::uint64_t encodedTotal = 0;
  for (int i = 0; i < 2; ++i) {
    const auto es = rig.encoders[i]->encoder().stats();
    EXPECT_LE(es.framesIn, rig.cams[i]->framesProduced());
    EXPECT_EQ(es.framesOut, es.framesIn);
    encodedTotal += es.framesOut;
  }
  const auto s = rig.server->stats();
  for (const auto& p : s.peers) EXPECT_LE(p.framesSent, encodedTotal);
  EXPECT_EQ(s.peersAccepted, 2u);
  ASSERT_EQ(s.peers.size(), 2u);
  for (const auto& p : s.peers) {
    EXPECT_TRUE(p.connected);
    ASSERT_EQ(p.tracks.size(), 2u);
    for (const auto& q : p.tracks) EXPECT_LE(q.highWatermark, 8u);
  }
}

TEST(WebRtcPeerServer, PliFromPeerForcesSharedKeyframe) {
  Rig rig(3);
  Receiver a(rig.server->signalingPort(), kToken);
  ASSERT_TRUE(test::waitFor([&] { return a.frameCount("video0") >= 10; }, 15s));
  const auto before = rig.encoders[0]->encoder().stats().forcedKeyframes;
  ASSERT_TRUE(a.requestKeyframe());  // RTCP PLI on video0
  ASSERT_TRUE(test::waitFor([&] { return rig.encoders[0]->encoder().stats().forcedKeyframes > before; }, 5s));
  EXPECT_GE(rig.server->stats().peers.at(0).pliReceived, 1u);
}

TEST(WebRtcPeerServer, RejectsBadTokenAndExtraPeers) {
  Rig rig(1);
  const auto port = rig.server->signalingPort();
  Receiver bad(port, "wrong-token");
  ASSERT_TRUE(test::waitFor([&] { return bad.closed(); }, 5s));
  Receiver ok(port, kToken);
  ASSERT_TRUE(test::waitFor([&] { return ok.frameCount("video0") >= 1; }, 15s));
  Receiver extra(port, kToken);
  ASSERT_TRUE(test::waitFor([&] { return extra.closed(); }, 5s));
  const auto s = rig.server->stats();
  EXPECT_EQ(s.peersAccepted, 1u);
  EXPECT_EQ(s.peersRejected, 2u);
}

TEST(WebRtcPeerServer, DisconnectedPeerReleasesItsQueues) {
  Rig rig(3);
  const auto port = rig.server->signalingPort();
  {
    Receiver a(port, kToken);
    ASSERT_TRUE(test::waitFor([&] { return a.frameCount("video0") >= 5; }, 15s));
    EXPECT_EQ(rig.encoders[0]->output().subscriberCount(), 1u);
  }
  // Review finding (EME-008 notes): a gone peer must not keep a subscription that requests keyframes.
  EXPECT_TRUE(test::waitFor([&] { return rig.encoders[0]->output().subscriberCount() == 0; }, 10s));
  EXPECT_TRUE(rig.server->stats().peers.empty());
}

TEST(WebRtcPeerServer, MalformedSignalingClosesPeer) {
  Rig rig(3);
  auto ws = std::make_shared<rtc::WebSocket>();
  std::atomic<bool> open{false}, closed{false};
  ws->onOpen([&] { open = true; });
  ws->onClosed([&] { closed = true; });
  ws->open("ws://127.0.0.1:" + std::to_string(rig.server->signalingPort()) + "/?token=" + kToken);
  ASSERT_TRUE(test::waitFor([&] { return open.load() && rig.server->stats().peers.size() == 1; }, 5s));
  ws->send(std::string("{not json"));
  EXPECT_TRUE(test::waitFor([&] { return closed.load() && rig.server->stats().peers.empty(); }, 5s));
  EXPECT_EQ(rig.encoders[0]->output().subscriberCount(), 0u);
  ws->resetCallbacks();
}

TEST(WebRtcPeerServer, StartValidatesConfig) {
  MediaGraph g;
  auto& s = g.add<WebRtcPeerServer>("p2p", WebRtcPeerServerConfig{.signalingPort = 0});
  EXPECT_EQ(s.start().error().code, ErrorCode::InvalidState);  // no input
  auto& enc = g.add<EncoderNode>("enc", std::make_unique<mock::MockH264Encoder>(EncoderConfig{.width = 64, .height = 32}));
  ASSERT_TRUE(g.connect(enc, s));
  EXPECT_EQ(s.start().error().code, ErrorCode::InvalidArgument);  // no token
}
