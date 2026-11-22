#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <thread>

#include "eme/core/node.hpp"
#include "eme/processing/video_encoder.hpp"

namespace eme {

struct EncoderNodeConfig {
  std::size_t inputQueueCapacity = 2;
  // One slow consumer must not turn the shared stream into all-keyframes; later requests are deferred.
  std::chrono::milliseconds minKeyframeInterval{500};
};

// Owns one encode session and its worker. Raw input is a small drop-oldest queue (freshness over
// completeness); encoded output fans out to every downstream consumer without re-encoding.
class EncoderNode final : public Node, public Consumer<VideoFrame>, public Producer<EncodedVideoFrame> {
 public:
  using Config = EncoderNodeConfig;

  EncoderNode(std::string name, std::unique_ptr<IVideoEncoder> encoder, Config config = {});
  ~EncoderNode() override;

  std::optional<VideoFormat> outputFormat() const override;
  bool accepts(const VideoFormat& format) const override;
  Result bindInput(Distributor<VideoFrame>& upstream) override;
  Distributor<EncodedVideoFrame>& output() override { return output_; }
  Result start() override;
  void stop() noexcept override;

  IVideoEncoder& encoder() noexcept { return *encoder_; }
  QueueStats inputStats() const;
  std::uint64_t encodeErrors() const noexcept { return errors_.load(); }
  std::uint64_t keyframeRequestsReceived() const noexcept { return keyframeRequests_.load(); }

 private:
  void run(std::stop_token st);

  std::unique_ptr<IVideoEncoder> encoder_;
  Config config_;
  Distributor<EncodedVideoFrame> output_;
  std::shared_ptr<Subscription<VideoFrame>> input_;
  std::atomic<std::uint64_t> errors_{0};
  std::atomic<std::uint64_t> keyframeRequests_{0};
  std::atomic<bool> keyframeWanted_{false};
  std::optional<std::chrono::steady_clock::time_point> lastKeyframeForward_;  // worker thread only
  std::jthread worker_;  // last: joined before the members it uses are destroyed
};

}  // namespace eme
