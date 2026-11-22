#include "eme/transport/whip_publisher.hpp"

#include <rtc/rtc.hpp>
#include <spdlog/spdlog.h>

#include <future>

#include "../webrtc_common/h264_track.hpp"
#include "http.hpp"

namespace eme {

WhipPublisher::WhipPublisher(std::string name, WhipPublisherConfig config)
    : Node(std::move(name)), config_(std::move(config)) {}

WhipPublisher::~WhipPublisher() { stop(); }

bool WhipPublisher::accepts(const VideoFormat& format) const {
  return format.kind == MediaKind::EncodedVideo && format.codec == VideoCodec::H264;
}

Result WhipPublisher::bindInput(Distributor<EncodedVideoFrame>& upstream) {
  inputs_.push_back(&upstream);
  return {};
}

void WhipPublisher::Wake::wake() {
  {
    std::lock_guard lock(m);
    pending = true;
  }
  cv.notify_one();
}

Result WhipPublisher::start() {
  if (inputs_.empty()) return fail(ErrorCode::InvalidState, name() + " has no input");
  if (!config_.endpointUrl.starts_with("http://") && !config_.endpointUrl.starts_with("https://")) {
    return fail(ErrorCode::InvalidArgument, name() + ": endpointUrl must be http(s)");
  }
  if (config_.bearerToken.empty()) return fail(ErrorCode::InvalidArgument, name() + ": bearerToken is required");
  const auto t0 = std::chrono::steady_clock::now();

  rtc::Configuration rtcConfig;
  for (const auto& url : config_.iceServers) rtcConfig.iceServers.emplace_back(url);
  pc_ = std::make_shared<rtc::PeerConnection>(rtcConfig);

  {
    std::lock_guard lock(m_);
    tracks_.clear();
  }
  for (std::size_t i = 0; i < inputs_.size(); ++i) {
    Distributor<EncodedVideoFrame>* input = inputs_[i];
    auto track = detail::addH264SendTrack(*pc_, "video" + std::to_string(i), "eme-whip", [pli = pli_, input] {
      ++*pli;
      input->requestKeyframe();
    });
    track->onOpen([this, input, i] {
      auto sub = input->subscribe(config_.queueFrames, OverflowPolicy::DropUntilKeyframe, [wake = wake_] { wake->wake(); });
      std::lock_guard lock(m_);
      if (i < tracks_.size()) tracks_[i].input = std::move(sub);
    });
    std::lock_guard lock(m_);
    tracks_.push_back({std::move(track), nullptr});
  }
  pc_->onStateChange([this](rtc::PeerConnection::State s) {
    connected_ = s == rtc::PeerConnection::State::Connected;
    if (s == rtc::PeerConnection::State::Failed || s == rtc::PeerConnection::State::Disconnected) {
      spdlog::warn("{}: WHIP session lost", name());  // reconnect: EME-010
    }
  });

  // shortcut: non-trickle (single POST with all candidates); add trickle PATCH if setup time matters.
  auto gathered = std::make_shared<std::promise<void>>();
  auto done = gathered->get_future();
  pc_->onGatheringStateChange([gathered](rtc::PeerConnection::GatheringState s) {
    if (s == rtc::PeerConnection::GatheringState::Complete) {
      try {
        gathered->set_value();
      } catch (const std::future_error&) {
      }
    }
  });
  pc_->setLocalDescription();
  if (done.wait_for(config_.setupTimeout) != std::future_status::ready) {
    stop();
    return fail(ErrorCode::Unsupported, name() + ": ICE gathering timed out");
  }
  const auto offer = pc_->localDescription();
  auto res = detail::httpRequest("POST", config_.endpointUrl, config_.bearerToken, std::string(*offer),
                                 "application/sdp", config_.setupTimeout);
  if (!res || res->status != 201) {
    const auto why = res ? "HTTP status " + std::to_string(res->status) : res.error().message;
    stop();
    return fail(ErrorCode::Unsupported, name() + ": WHIP offer rejected: " + why);
  }
  resourceUrl_ = res->location.empty() ? "" : detail::resolveUrl(config_.endpointUrl, res->location);
  if (!resourceUrl_.empty() && detail::origin(resourceUrl_) != detail::origin(config_.endpointUrl)) {
    // Never send the bearer token to a server-chosen other origin (or an https->http downgrade).
    spdlog::warn("{}: WHIP resource is on another origin; session will not be DELETEd on stop", name());
    resourceUrl_.clear();
  }
  try {
    pc_->setRemoteDescription(rtc::Description(res->body, "answer"));
  } catch (const std::exception& e) {
    stop();
    return fail(ErrorCode::Unsupported, name() + ": invalid WHIP answer: " + e.what());
  }
  {
    std::lock_guard lock(m_);
    setupTime_ = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0);
  }
  dispatcher_ = std::jthread([this](std::stop_token st) { dispatch(st); });
  spdlog::info("{}: WHIP session established ({} tracks)", name(), inputs_.size());
  return {};
}

// Not safe against concurrent calls; MediaGraph stops producers before this sink (inputs_ are raw pointers).
void WhipPublisher::stop() noexcept {
  if (dispatcher_.joinable()) {
    dispatcher_.request_stop();
    dispatcher_.join();
  }
  std::vector<Track> tracks;
  {
    std::lock_guard lock(m_);
    tracks.swap(tracks_);
  }
  for (auto& t : tracks) t.track->resetCallbacks();
  if (pc_) {
    pc_->resetCallbacks();
    pc_->close();
    pc_.reset();
  }
  if (!resourceUrl_.empty()) {
    // Best effort: tell the server the session is over so it frees resources immediately.
    if (auto r = detail::httpRequest("DELETE", resourceUrl_, config_.bearerToken, "", "", std::chrono::seconds(2)); !r) {
      spdlog::warn("{}: WHIP DELETE failed: {}", name(), r.error().message);
    }
    resourceUrl_.clear();
  }
  connected_ = false;
}

void WhipPublisher::dispatch(std::stop_token st) {
  std::vector<std::pair<std::shared_ptr<rtc::Track>, std::shared_ptr<Subscription<EncodedVideoFrame>>>> work;
  while (!st.stop_requested()) {
    {
      std::unique_lock lock(wake_->m);
      if (!wake_->cv.wait(lock, st, [&] { return wake_->pending; })) break;
      wake_->pending = false;
    }
    work.clear();
    {
      std::lock_guard lock(m_);
      for (const auto& t : tracks_) {
        if (t.input) work.emplace_back(t.track, t.input);
      }
    }
    for (auto& [track, input] : work) {
      while (auto frame = input->tryPop()) {
        if (!track->isOpen()) continue;
        try {
          detail::sendAccessUnit(*track, *frame);
          ++framesSent_;
        } catch (const std::exception& e) {
          spdlog::warn("{}: send failed: {}", name(), e.what());
          break;
        }
      }
    }
  }
}

WhipPublisherStats WhipPublisher::stats() const {
  std::lock_guard lock(m_);
  WhipPublisherStats s{.connected = connected_.load(), .framesSent = framesSent_.load(), .pliReceived = pli_->load(),
                       .setupTime = setupTime_, .tracks = {}};
  for (const auto& t : tracks_) s.tracks.push_back(t.input ? t.input->stats() : QueueStats{});
  return s;
}

}  // namespace eme
