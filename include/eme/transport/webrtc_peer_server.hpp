#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "eme/core/node.hpp"

namespace rtc {
class WebSocket;
class WebSocketServer;
}  // namespace rtc

namespace eme {

struct WebRtcPeerServerConfig {
  std::uint16_t signalingPort = 8443;             // 0 = pick a free port (tests)
  std::optional<std::string> bindAddress;         // default: all interfaces
  std::string accessToken;                        // required; clients connect to ws(s)://host:port/?token=...
  std::optional<std::string> tlsCertificatePem;   // set both for wss:// (required outside localhost)
  std::optional<std::string> tlsKeyPem;
  std::vector<std::string> iceServers;            // e.g. "stun:stun.example.org:3478", "turn:user:pass@host:3478"
  std::size_t maxPeers = 3;                       // PLAN §14.1 max_direct_peers
  std::size_t perPeerQueueFrames = 8;             // ~0.5 s at 15 fps, per track
  std::chrono::seconds handshakeTimeout{5};       // WebSocket upgrade must finish within this
  std::chrono::seconds answerTimeout{15};         // authenticated peer must connect within this
  std::uint16_t portRangeBegin = 1024;
  std::uint16_t portRangeEnd = 65535;
};

struct WebRtcPeerStats {
  std::uint64_t id = 0;
  bool connected = false;
  std::uint64_t framesSent = 0;
  std::uint64_t pliReceived = 0;
  std::vector<QueueStats> tracks;
};

struct WebRtcServerStats {
  std::uint64_t peersAccepted = 0;
  std::uint64_t peersRejected = 0;  // bad token or over maxPeers
  std::vector<WebRtcPeerStats> peers;
};

// Direct WebRTC fan-out (libdatachannel). Each connected encoder becomes one sendonly H.264 track,
// mid "video0", "video1", ... in connect order. Every peer gets its own PeerConnection (SSRC,
// sequence numbers, SRTP, NACK history) and its own keyframe-gated queue per track; encoded payloads
// are shared, never re-encoded. One dispatcher thread serves all peers.
class WebRtcPeerServer final : public Node, public Consumer<EncodedVideoFrame> {
 public:
  WebRtcPeerServer(std::string name, WebRtcPeerServerConfig config);
  ~WebRtcPeerServer() override;

  bool accepts(const VideoFormat& format) const override;
  Result bindInput(Distributor<EncodedVideoFrame>& upstream) override;
  Result start() override;
  void stop() noexcept override;

  std::uint16_t signalingPort() const;  // actual port once started
  WebRtcServerStats stats() const;

 private:
  struct Peer;
  void onClient(std::shared_ptr<rtc::WebSocket> ws);
  void createPeer(std::shared_ptr<rtc::WebSocket> ws);
  void removePeer(std::uint64_t id);
  void dispatch(std::stop_token st);

  // Shared with subscription notify callbacks, which may outlive this object (they run on producer threads).
  struct WakeState {
    std::mutex m;
    std::condition_variable_any cv;
    bool pending = false;
    void wake();
  };

  WebRtcPeerServerConfig config_;
  std::vector<Distributor<EncodedVideoFrame>*> inputs_;
  std::unique_ptr<rtc::WebSocketServer> signaling_;

  mutable std::mutex m_;  // guards peers_; never held while calling into a Distributor
  std::vector<std::shared_ptr<Peer>> peers_;
  std::vector<std::shared_ptr<rtc::WebSocket>> handshaking_;  // kept alive until onOpen decides
  bool stopping_ = false;                                       // guarded by m_; no new peers once set
  std::uint64_t nextPeerId_ = 1;
  std::atomic<std::uint64_t> accepted_{0}, rejected_{0};

  std::shared_ptr<WakeState> wake_ = std::make_shared<WakeState>();
  std::jthread dispatcher_;
};

}  // namespace eme
