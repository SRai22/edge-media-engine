// EME-014 spike: workload -> LiveKit C++ SDK EncodedVideoSource (pre-encoded passthrough).
// Env: LIVEKIT_URL, LIVEKIT_TOKEN. Args: [seconds=60] [gopFrames=150]. Prints one JSON result line on exit.
#include <livekit/livekit.h>

#include <iostream>
#include <mutex>

#include "../common/workload.hpp"

namespace {

// Spike-quality sink: one SDK room connection, one EncodedVideoSource + track per encoder.
class LiveKitSdkSink final : public eme::Node, public eme::Consumer<eme::EncodedVideoFrame> {
 public:
  LiveKitSdkSink(std::string url, std::string token) : Node("livekit-sdk"), url_(std::move(url)), token_(std::move(token)) {}
  ~LiveKitSdkSink() override { stop(); }

  bool accepts(const eme::VideoFormat& f) const override { return f.kind == eme::MediaKind::EncodedVideo; }
  eme::Result bindInput(eme::Distributor<eme::EncodedVideoFrame>& up) override {
    inputs_.push_back(&up);
    return {};
  }

  eme::Result start() override {
    const auto t0 = std::chrono::steady_clock::now();
    livekit::initialize(livekit::LogLevel::Warn);
    room_ = std::make_unique<livekit::Room>();
    livekit::RoomOptions opts;
    opts.auto_subscribe = false;
    if (!room_->connect(url_, token_, opts)) return eme::fail(eme::ErrorCode::Unsupported, "room connect failed");
    auto lp = room_->localParticipant().lock();
    if (!lp) return eme::fail(eme::ErrorCode::Internal, "no local participant");
    for (std::size_t i = 0; i < inputs_.size(); ++i) {
      auto src = std::make_shared<livekit::EncodedVideoSource>(livekit::VideoCodec::H264, 1280, 720);
      auto track = livekit::LocalVideoTrack::createLocalVideoTrack("cam" + std::to_string(i), src);
      livekit::TrackPublishOptions o;
      o.video_codec = livekit::VideoCodec::H264;
      o.simulcast = false;
      o.video_encoder = livekit::VideoEncoderBackend::PreEncoded;  // required for EncodedVideoSource
      o.source = livekit::TrackSource::SOURCE_CAMERA;
      o.video_encoding = livekit::VideoEncodingOptions{.max_bitrate = 1'200'000, .max_framerate = 15.0};
      try {
        lp->publishTrack(track, o);
      } catch (const std::exception& e) {
        return eme::fail(eme::ErrorCode::Unsupported, std::string("publishTrack: ") + e.what());
      }
      sources_.push_back(src);
      tracks_.push_back(track);
      subs_.push_back(inputs_[i]->subscribe(8, eme::OverflowPolicy::DropUntilKeyframe, [this] { wake(); }));
    }
    setupMs_ = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    worker_ = std::jthread([this](std::stop_token st) { run(st); });
    feedback_ = std::jthread([this](std::stop_token st) { pollFeedback(st); });
    return {};
  }

  void stop() noexcept override {
    feedback_ = {};
    worker_ = {};
    subs_.clear();
    if (room_) {
      room_->disconnect();
      room_.reset();
      tracks_.clear();
      sources_.clear();
      livekit::shutdown();
    }
  }

  nlohmann::json stats() const {
    return {{"setup_ms", setupMs_}, {"frames_sent", sent_.load()}, {"capture_rejected", rejected_.load()},
            {"feedback_keyframes", fbKeyframes_.load()}, {"rate_control_target_bps", lastTarget_.load()}};
  }

 private:
  void wake() {
    {
      std::lock_guard l(m_);
      pending_ = true;
    }
    cv_.notify_one();
  }
  void run(std::stop_token st) {
    while (!st.stop_requested()) {
      {
        std::unique_lock l(m_);
        if (!cv_.wait(l, st, [&] { return pending_; })) break;
        pending_ = false;
      }
      for (std::size_t i = 0; i < subs_.size(); ++i) {
        while (auto f = subs_[i]->tryPop()) {
          livekit::EncodedVideoSource::Frame frame;
          frame.is_keyframe = f->isKeyframe;
          // SPIKE_SDK_TS=pts gives the SDK the same regular media timeline the WHIP path uses for RTP.
          frame.timestamp_us = ptsMode_ ? f->presentationTimestampNs / 1000 : f->captureTimestampNs / 1000;
          const auto* b = reinterpret_cast<const std::uint8_t*>(f->payload->bytes.data());
          frame.data.assign(b, b + f->payload->bytes.size());  // SDK copies again internally
          try {
            if (sources_[i]->captureFrame(frame)) ++sent_; else ++rejected_;
          } catch (const std::exception& e) {
            ++rejected_;
            spdlog::warn("captureFrame: {}", e.what());
          }
        }
      }
    }
  }
  void pollFeedback(std::stop_token st) {
    while (!st.stop_requested()) {
      for (std::size_t i = 0; i < sources_.size(); ++i) {
        try {
          const auto fb = sources_[i]->takeFeedback();
          if (fb.keyframe_requested) {
            ++fbKeyframes_;
            inputs_[i]->requestKeyframe();
          }
          if (fb.rate_control) lastTarget_ = fb.rate_control->target_bitrate_bps;  // logged, not applied (spike)
        } catch (const std::exception& e) {
          spdlog::warn("takeFeedback: {}", e.what());
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }

  std::string url_, token_;
  const bool ptsMode_ = [] { const char* v = std::getenv("SPIKE_SDK_TS"); return v && std::string(v) == "pts"; }();
  std::vector<eme::Distributor<eme::EncodedVideoFrame>*> inputs_;
  std::unique_ptr<livekit::Room> room_;
  std::vector<std::shared_ptr<livekit::EncodedVideoSource>> sources_;
  std::vector<std::shared_ptr<livekit::LocalVideoTrack>> tracks_;
  std::vector<std::shared_ptr<eme::Subscription<eme::EncodedVideoFrame>>> subs_;
  std::mutex m_;
  std::condition_variable_any cv_;
  bool pending_ = false;
  long long setupMs_ = -1;
  std::atomic<std::uint64_t> sent_{0}, rejected_{0}, fbKeyframes_{0}, lastTarget_{0};
  std::jthread worker_, feedback_;
};

}  // namespace

int main(int argc, char** argv) {
  const int seconds = argc > 1 ? std::atoi(argv[1]) : 60;
  const int gop = argc > 2 ? std::atoi(argv[2]) : 150;
  eme::MediaGraph graph;
  auto& sink = graph.add<LiveKitSdkSink>(spike::env("LIVEKIT_URL"), spike::env("LIVEKIT_TOKEN"));
  auto w = spike::buildWorkload(graph, gop);
  for (auto* e : w.encoders) {
    if (auto r = graph.connect(*e, sink); !r) return spdlog::error("{}", r.error().message), 1;
  }
  if (auto r = graph.start(); !r) return spdlog::error("start: {}", r.error().message), 1;
  spike::runFor(seconds);
  auto s = sink.stats();
  graph.stop();
  s["mode"] = std::getenv("SPIKE_SDK_TS") && std::string(std::getenv("SPIKE_SDK_TS")) == "pts" ? "sdk-pts" : "sdk";
  s["cameras"] = spike::workloadStats(w);
  std::cout << s.dump() << std::endl;
}
