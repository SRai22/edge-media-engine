#include "eme/transport/webrtc_peer_server.hpp"

#include "../webrtc_common/h264_track.hpp"

#include <rtc/rtc.hpp>
#include <spdlog/spdlog.h>

#include <chrono>
#include <nlohmann/json.hpp>

namespace eme {
namespace {

// Constant-time compare so the token cannot be recovered by timing.
bool tokenMatches(std::string_view given, std::string_view expected) {
  if (given.size() != expected.size()) return false;
  unsigned char diff = 0;
  for (std::size_t i = 0; i < given.size(); ++i) diff |= static_cast<unsigned char>(given[i] ^ expected[i]);
  return diff == 0;
}

// Accepts "/?token=abc" or "/path?x=1&token=abc". No URL-decoding: tokens are expected to be URL-safe.
std::string_view tokenFromPath(std::string_view path) {
  const auto q = path.find('?');
  if (q == std::string_view::npos) return {};
  for (auto rest = path.substr(q + 1); !rest.empty();) {
    const auto amp = rest.find('&');
    const auto pair = rest.substr(0, amp);
    if (pair.starts_with("token=")) return pair.substr(6);
    if (amp == std::string_view::npos) break;
    rest = rest.substr(amp + 1);
  }
  return {};
}

}  // namespace

struct WebRtcPeerServer::Peer {
  struct Track {
    std::shared_ptr<rtc::Track> track;
    std::shared_ptr<Subscription<EncodedVideoFrame>> input;  // set once the track opens (guarded by m_)
  };
  std::uint64_t id = 0;
  std::shared_ptr<rtc::WebSocket> ws;
  std::shared_ptr<rtc::PeerConnection> pc;
  std::vector<Track> tracks;
  bool closing = false;  // guarded by m_
  std::chrono::steady_clock::time_point deadline;  // must be connected by then (guarded by m_)
  std::atomic<bool> connected{false};
  std::atomic<std::uint64_t> framesSent{0};
  std::atomic<std::uint64_t> pli{0};
};

namespace {
template <class P>
void closePeer(P& p) {
  for (auto& t : p.tracks) t.track->resetCallbacks();
  p.pc->resetCallbacks();
  p.ws->resetCallbacks();
  p.pc->close();
  p.ws->close();
}
}  // namespace

WebRtcPeerServer::WebRtcPeerServer(std::string name, WebRtcPeerServerConfig config)
    : Node(std::move(name)), config_(std::move(config)) {}

WebRtcPeerServer::~WebRtcPeerServer() { stop(); }

bool WebRtcPeerServer::accepts(const VideoFormat& format) const {
  return format.kind == MediaKind::EncodedVideo && format.codec == VideoCodec::H264;
}

Result WebRtcPeerServer::bindInput(Distributor<EncodedVideoFrame>& upstream) {
  inputs_.push_back(&upstream);  // one track per connected encoder
  return {};
}

Result WebRtcPeerServer::start() {
  if (inputs_.empty()) return fail(ErrorCode::InvalidState, name() + " has no input");
  if (config_.accessToken.empty()) return fail(ErrorCode::InvalidArgument, name() + ": accessToken is required");
  if (config_.tlsCertificatePem.has_value() != config_.tlsKeyPem.has_value()) {
    return fail(ErrorCode::InvalidArgument, name() + ": set both TLS certificate and key, or neither");
  }
  rtc::WebSocketServerConfiguration ws;
  ws.port = config_.signalingPort;
  ws.bindAddress = config_.bindAddress;
  ws.enableTls = config_.tlsCertificatePem.has_value();
  ws.certificatePemFile = config_.tlsCertificatePem;
  ws.keyPemFile = config_.tlsKeyPem;
  ws.maxMessageSize = 64 * 1024;  // SDP answers are a few KB; bound untrusted input
  ws.connectionTimeout = std::chrono::duration_cast<std::chrono::milliseconds>(config_.handshakeTimeout);
  try {
    signaling_ = std::make_unique<rtc::WebSocketServer>(ws);
  } catch (const std::exception& e) {
    return fail(ErrorCode::Unsupported, name() + ": signaling server failed: " + e.what());
  }
  {
    std::lock_guard lock(m_);
    stopping_ = false;
  }
  dispatcher_ = std::jthread([this](std::stop_token st) { dispatch(st); });
  signaling_->onClient([this](std::shared_ptr<rtc::WebSocket> client) { onClient(std::move(client)); });
  if (!ws.enableTls) spdlog::warn("{}: signaling without TLS; use only on trusted networks", name());
  spdlog::info("{}: signaling on port {} ({} tracks)", name(), signaling_->port(), inputs_.size());
  return {};
}

void WebRtcPeerServer::stop() noexcept {
  {
    std::lock_guard lock(m_);
    stopping_ = true;  // an onOpen already past the token check must not add a peer after the drain below
  }
  if (signaling_) {
    signaling_->stop();
    signaling_.reset();
  }
  if (dispatcher_.joinable()) {
    dispatcher_.request_stop();
    dispatcher_.join();
  }
  std::vector<std::shared_ptr<Peer>> peers;
  std::vector<std::shared_ptr<rtc::WebSocket>> handshaking;
  {
    std::lock_guard lock(m_);
    peers.swap(peers_);
    handshaking.swap(handshaking_);
  }
  for (auto& ws : handshaking) {
    ws->resetCallbacks();
    ws->close();
  }
  for (auto& p : peers) {
    closePeer(*p);
  }
}

std::uint16_t WebRtcPeerServer::signalingPort() const { return signaling_ ? signaling_->port() : 0; }

void WebRtcPeerServer::onClient(std::shared_ptr<rtc::WebSocket> ws) {
  constexpr std::size_t kMaxHandshaking = 16;  // bound half-open connections
  {
    std::lock_guard lock(m_);
    if (stopping_ || handshaking_.size() >= kMaxHandshaking) {
      ++rejected_;
      ws->close();
      return;
    }
    handshaking_.push_back(ws);
  }
  std::weak_ptr<rtc::WebSocket> weak = ws;
  auto release = [this](const rtc::WebSocket* raw) {
    std::lock_guard lock(m_);
    std::erase_if(handshaking_, [raw](const auto& h) { return h.get() == raw; });
  };
  ws->onClosed([weak, release] {
    if (auto ws = weak.lock()) release(ws.get());
  });
  ws->onOpen([this, weak, release] {
    auto ws = weak.lock();
    if (!ws) return;
    release(ws.get());
    const auto path = ws->path().value_or("");
    std::size_t count = 0;
    {
      std::lock_guard lock(m_);
      count = peers_.size();
    }
    if (!tokenMatches(tokenFromPath(path), config_.accessToken) || count >= config_.maxPeers) {
      ++rejected_;
      spdlog::warn("{}: rejected signaling client {} ({})", name(), ws->remoteAddress().value_or("?"),
                   count >= config_.maxPeers ? "peer limit" : "bad token");  // never log the token
      ws->close();
      return;
    }
    createPeer(std::move(ws));
  });
}

void WebRtcPeerServer::createPeer(std::shared_ptr<rtc::WebSocket> ws) {
  rtc::Configuration rtcConfig;
  for (const auto& url : config_.iceServers) rtcConfig.iceServers.emplace_back(url);
  rtcConfig.portRangeBegin = config_.portRangeBegin;
  rtcConfig.portRangeEnd = config_.portRangeEnd;

  auto peer = std::make_shared<Peer>();
  peer->ws = ws;
  peer->pc = std::make_shared<rtc::PeerConnection>(rtcConfig);
  {
    std::lock_guard lock(m_);
    peer->id = nextPeerId_++;
  }
  const std::weak_ptr<Peer> weakPeer = peer;
  const std::string cname = "eme-" + std::to_string(peer->id);

  for (std::size_t i = 0; i < inputs_.size(); ++i) {
    const std::string mid = "video" + std::to_string(i);
    Distributor<EncodedVideoFrame>* input = inputs_[i];
    auto track = detail::addH264SendTrack(*peer->pc, mid, cname, [weakPeer, input] {
      if (auto p = weakPeer.lock()) ++p->pli;
      input->requestKeyframe();  // coalesced + rate-limited by the shared encoder
    });

    // Subscribe only once media can flow, so the first frame this peer gets is a keyframe.
    track->onOpen([this, weakPeer, input, i] {
      auto sub = input->subscribe(config_.perPeerQueueFrames, OverflowPolicy::DropUntilKeyframe,
                                  [wake = wake_] { wake->wake(); });
      if (auto p = weakPeer.lock()) {
        std::lock_guard lock(m_);
        p->tracks[i].input = std::move(sub);
      }
    });
    peer->tracks.push_back({std::move(track), nullptr});
  }

  const auto id = peer->id;
  peer->pc->onGatheringStateChange([weakPeer](rtc::PeerConnection::GatheringState state) {
    auto p = weakPeer.lock();
    if (!p || state != rtc::PeerConnection::GatheringState::Complete) return;
    // shortcut: non-trickle ICE (full SDP after gathering); add trickle if connection setup time matters.
    if (auto desc = p->pc->localDescription()) {
      p->ws->send(nlohmann::json{{"type", desc->typeString()}, {"sdp", std::string(*desc)}}.dump());
    }
  });
  peer->pc->onStateChange([this, weakPeer, id](rtc::PeerConnection::State state) {
    using S = rtc::PeerConnection::State;
    if (auto p = weakPeer.lock()) p->connected = state == S::Connected;
    if (state == S::Disconnected || state == S::Failed || state == S::Closed) removePeer(id);
  });
  ws->onMessage([this, weakPeer, id](rtc::message_variant msg) {
    auto p = weakPeer.lock();
    const auto* text = std::get_if<std::string>(&msg);
    if (!p || !text) return;
    try {
      const auto j = nlohmann::json::parse(*text);
      if (j.at("type").get<std::string>() == "answer") {
        p->pc->setRemoteDescription(rtc::Description(j.at("sdp").get<std::string>(), "answer"));
      }
    } catch (const std::exception& e) {
      spdlog::warn("{}: peer {} sent invalid signaling message: {}", name(), id, e.what());
      removePeer(id);
    }
  });
  ws->onClosed([this, id] { removePeer(id); });

  bool admitted = false;
  {
    std::lock_guard lock(m_);
    if (!stopping_ && peers_.size() < config_.maxPeers) {  // re-checked atomically with the insert
      peer->deadline = std::chrono::steady_clock::now() + config_.answerTimeout;
      peers_.push_back(peer);
      admitted = true;
    }
  }
  if (!admitted) {
    ++rejected_;
    closePeer(*peer);
    return;
  }
  ++accepted_;
  spdlog::info("{}: peer {} joined", name(), id);
  peer->pc->setLocalDescription();  // offer; sent when gathering completes
}

void WebRtcPeerServer::removePeer(std::uint64_t id) {
  {
    std::lock_guard lock(m_);
    for (auto& p : peers_) {
      if (p->id == id) p->closing = true;
    }
  }
  wake_->wake();  // the dispatcher closes it: libdatachannel objects must not be destroyed from their own callbacks
}

void WebRtcPeerServer::WakeState::wake() {
  {
    std::lock_guard lock(m);
    pending = true;
  }
  cv.notify_one();
}

void WebRtcPeerServer::dispatch(std::stop_token st) {
  struct Work {
    std::shared_ptr<Peer> peer;
    std::shared_ptr<rtc::Track> track;
    std::shared_ptr<Subscription<EncodedVideoFrame>> input;
  };
  std::vector<Work> work;
  std::vector<std::shared_ptr<Peer>> dead;
  // Closed peers are kept briefly so a callback that still holds a weak->strong reference never ends up
  // destroying its own PeerConnection/WebSocket from inside that callback.
  std::vector<std::pair<std::chrono::steady_clock::time_point, std::shared_ptr<Peer>>> graveyard;
  while (!st.stop_requested()) {
    {
      std::unique_lock lock(wake_->m);
      wake_->cv.wait_for(lock, st, std::chrono::seconds(1), [&] { return wake_->pending; });  // 1 s tick for deadlines
      if (st.stop_requested()) break;
      wake_->pending = false;
    }
    const auto now = std::chrono::steady_clock::now();
    std::erase_if(graveyard, [&](const auto& g) { return now - g.first > std::chrono::seconds(5); });
    work.clear();
    dead.clear();
    {
      std::lock_guard lock(m_);
      std::erase_if(peers_, [&](const std::shared_ptr<Peer>& p) {
        if (!p->connected && now > p->deadline) p->closing = true;  // authenticated but never answered/connected
        if (p->closing) dead.push_back(p);
        return p->closing;
      });
      for (const auto& p : peers_) {
        for (const auto& t : p->tracks) {
          if (t.input) work.push_back({p, t.track, t.input});
        }
      }
    }
    for (auto& p : dead) {
      closePeer(*p);
      spdlog::info("{}: peer {} left", name(), p->id);
      graveyard.emplace_back(now, p);
    }
    for (auto& w : work) {
      while (auto frame = w.input->tryPop()) {
        if (!w.track->isOpen()) continue;
        try {
          detail::sendAccessUnit(*w.track, *frame);
          ++w.peer->framesSent;
        } catch (const std::exception& e) {
          spdlog::warn("{}: peer {} send failed: {}", name(), w.peer->id, e.what());
          removePeer(w.peer->id);
          break;
        }
      }
    }
  }
}

WebRtcServerStats WebRtcPeerServer::stats() const {
  WebRtcServerStats s{.peersAccepted = accepted_.load(), .peersRejected = rejected_.load(), .peers = {}};
  std::lock_guard lock(m_);
  for (const auto& p : peers_) {
    WebRtcPeerStats ps{.id = p->id, .connected = p->connected.load(), .framesSent = p->framesSent.load(),
                       .pliReceived = p->pli.load(), .tracks = {}};
    for (const auto& t : p->tracks) ps.tracks.push_back(t.input ? t.input->stats() : QueueStats{});
    s.peers.push_back(std::move(ps));
  }
  return s;
}

}  // namespace eme
