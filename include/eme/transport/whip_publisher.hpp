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
class PeerConnection;
class Track;
}  // namespace rtc

namespace eme {

struct WhipPublisherConfig {
  std::string endpointUrl;               // e.g. http://127.0.0.1:8085/w (use https outside localhost)
  std::string bearerToken;               // WHIP auth (LiveKit: ingress stream key); never logged
  std::vector<std::string> iceServers;
  std::size_t queueFrames = 8;           // per track
  std::chrono::seconds setupTimeout{10};  // ICE gathering + HTTP exchange
};

struct WhipPublisherStats {
  bool connected = false;
  std::uint64_t framesSent = 0;
  std::uint64_t pliReceived = 0;
  std::optional<std::chrono::milliseconds> setupTime;  // start() -> answer applied
  std::vector<QueueStats> tracks;
};

// Publishes every connected encoder as one sendonly H.264 track of a single WHIP session (RFC 9725) on
// libdatachannel: no second WebRTC engine, no re-encoding. RTCP PLI from the server is relayed to the
// shared encoder. shortcut: no reconnect / ICE restart yet (EME-010); a failed session stays down.
class WhipPublisher final : public Node, public Consumer<EncodedVideoFrame> {
 public:
  WhipPublisher(std::string name, WhipPublisherConfig config);
  ~WhipPublisher() override;

  bool accepts(const VideoFormat& format) const override;
  Result bindInput(Distributor<EncodedVideoFrame>& upstream) override;
  Result start() override;
  void stop() noexcept override;
  WhipPublisherStats stats() const;

 private:
  struct Wake {
    std::mutex m;
    std::condition_variable_any cv;
    bool pending = false;
    void wake();
  };
  struct Track {
    std::shared_ptr<rtc::Track> track;
    std::shared_ptr<Subscription<EncodedVideoFrame>> input;  // guarded by m_
  };
  void dispatch(std::stop_token st);

  WhipPublisherConfig config_;
  std::vector<Distributor<EncodedVideoFrame>*> inputs_;
  std::shared_ptr<rtc::PeerConnection> pc_;
  std::string resourceUrl_;  // from the Location header; DELETEd on stop

  mutable std::mutex m_;
  std::vector<Track> tracks_;
  std::optional<std::chrono::milliseconds> setupTime_;
  std::atomic<bool> connected_{false};
  std::atomic<std::uint64_t> framesSent_{0};
  std::shared_ptr<std::atomic<std::uint64_t>> pli_ = std::make_shared<std::atomic<std::uint64_t>>(0);  // PLI handlers outlive callbacks
  std::shared_ptr<Wake> wake_ = std::make_shared<Wake>();
  std::jthread dispatcher_;
};

}  // namespace eme
