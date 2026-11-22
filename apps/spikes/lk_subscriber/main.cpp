// EME-016 measurement subscriber (identical for both publisher paths): joins the room, decodes every video
// track, reads the capture-time barcode and reports time-to-first-frame and capture->decode latency.
// Env: LIVEKIT_URL, LIVEKIT_TOKEN. Args: [seconds=20]. Prints one JSON result line.
#include <livekit/livekit.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <thread>
#include <vector>

#include "eme/testing/barcode.hpp"

namespace {

std::int64_t nowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct TrackResult {
  std::string name;
  std::int64_t firstFrameUs = -1;  // since process start
  std::uint64_t frames = 0, unreadable = 0;
  std::vector<double> latencyMs;
};

class Subscriber final : public livekit::RoomDelegate {
 public:
  explicit Subscriber(std::int64_t t0) : t0_(t0) {}
  void onTrackSubscribed(livekit::Room&, const livekit::TrackSubscribedEvent& ev) override {
    if (!ev.track || ev.track->kind() != livekit::TrackKind::KIND_VIDEO) return;
    livekit::VideoStream::Options o;
    o.capacity = 4;
    o.format = livekit::VideoBufferType::I420;
    auto stream = livekit::VideoStream::fromTrack(ev.track, o);
    std::lock_guard l(m_);
    auto& r = results_.emplace_back(std::make_unique<TrackResult>());
    r->name = ev.track->name();
    TrackResult* res = r.get();
    readers_.emplace_back([this, stream, res](std::stop_token st) {
      livekit::VideoFrameEvent ev;
      while (!st.stop_requested() && stream->read(ev)) {
        const auto t = nowUs();
        std::lock_guard l(m_);
        if (res->firstFrameUs < 0) res->firstFrameUs = t - t0_;
        ++res->frames;
        const auto& f = ev.frame;
        const auto stamp = f.type() == livekit::VideoBufferType::I420 ? eme::mock::readBarcode(f.data(), f.width(), f.width())
                                                                      : std::nullopt;
        if (stamp && static_cast<std::int64_t>(*stamp) <= t) res->latencyMs.push_back((t - static_cast<std::int64_t>(*stamp)) / 1000.0);
        else ++res->unreadable;
      }
    });
    streams_.push_back(stream);
  }
  nlohmann::json report() {
    for (auto& s : streams_) s->close();
    readers_.clear();
    std::lock_guard l(m_);
    nlohmann::json tracks = nlohmann::json::array();
    for (auto& r : results_) {
      auto lat = r->latencyMs;
      std::ranges::sort(lat);
      auto pct = [&](double p) { return lat.empty() ? -1.0 : lat[static_cast<std::size_t>(std::max(1.0, std::ceil(p * static_cast<double>(lat.size())))) - 1]; };  // nearest rank
      tracks.push_back({{"name", r->name}, {"first_frame_ms", r->firstFrameUs / 1000.0}, {"frames", r->frames},
                        {"unreadable", r->unreadable}, {"latency_p50_ms", pct(0.5)}, {"latency_p95_ms", pct(0.95)},
                        {"latency_p99_ms", pct(0.99)}});
    }
    return tracks;
  }

 private:
  std::int64_t t0_;
  std::mutex m_;
  std::vector<std::unique_ptr<TrackResult>> results_;
  std::vector<std::shared_ptr<livekit::VideoStream>> streams_;
  std::vector<std::jthread> readers_;
};

}  // namespace

int main(int argc, char** argv) {
  const auto t0 = nowUs();
  const int seconds = argc > 1 ? std::atoi(argv[1]) : 20;
  const char* url = std::getenv("LIVEKIT_URL");
  const char* token = std::getenv("LIVEKIT_TOKEN");
  if (!url || !token) return spdlog::error("set LIVEKIT_URL and LIVEKIT_TOKEN"), 2;
  livekit::initialize(livekit::LogLevel::Warn);
  Subscriber sub(t0);
  {
    livekit::Room room;
    room.setDelegate(&sub);
    livekit::RoomOptions opts;
    opts.auto_subscribe = true;
    opts.adaptive_stream = false;  // full resolution, no server-side layer games
    if (!room.connect(url, token, opts)) return spdlog::error("connect failed"), 1;
    const double connectedMs = (nowUs() - t0) / 1000.0;  // first_frame - connected = subscribe + keyframe wait
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    nlohmann::json out{{"connected_ms", connectedMs}, {"tracks", sub.report()}};
    room.setDelegate(nullptr);
    room.disconnect();
    std::cout << out.dump() << std::endl;
  }
  livekit::shutdown();
}
