#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

#include "eme/core/node.hpp"

namespace eme::mock {

struct SinkStats {
  std::uint64_t received = 0;
  std::uint64_t keyframes = 0;
  std::uint64_t dependencyViolations = 0;  // P-frame whose predecessor this sink never got
  const EncodedBuffer* lastPayload = nullptr;  // identity only (zero-copy checks); never dereference
  QueueStats queue;
};

// Mock transport consumer: verifies decodability, optionally simulates a slow network.
class CountingSink final : public Node, public Consumer<EncodedVideoFrame> {
 public:
  struct Config {
    std::size_t queueCapacity = 8;
    std::chrono::microseconds perFrameDelay{0};
    std::function<void()> beforeFrame;  // runs per frame before it is counted; lets tests stall the sink without timers
  };

  CountingSink(std::string name, Config config);
  ~CountingSink() override;

  bool accepts(const VideoFormat& format) const override;
  Result bindInput(Distributor<EncodedVideoFrame>& upstream) override;
  Result start() override;
  void stop() noexcept override;
  SinkStats stats() const;

 private:
  void run(std::stop_token st);

  Config config_;
  std::shared_ptr<Subscription<EncodedVideoFrame>> input_;
  mutable std::mutex m_;
  SinkStats stats_;
  std::optional<std::uint64_t> lastSequence_;
  std::jthread worker_;
};

}  // namespace eme::mock
