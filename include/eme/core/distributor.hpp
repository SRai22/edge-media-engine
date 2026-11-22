#pragma once

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <vector>

#include "eme/core/frames.hpp"

namespace eme {

enum class OverflowPolicy {
  DropOldest,         // independent items (raw frames): keep the freshest
  DropUntilKeyframe,  // inter-dependent items (encoded video): flush and resume at the next keyframe
};

struct QueueStats {
  std::uint64_t offered = 0;
  std::uint64_t delivered = 0;
  std::uint64_t dropped = 0;
  std::size_t depth = 0;
  std::size_t highWatermark = 0;
  bool waitingForKeyframe = false;
};

// One consumer's bounded queue. The producer side never waits on the consumer.
template <class T>
class Subscription {
 public:
  Subscription(std::size_t capacity, OverflowPolicy policy)
      : capacity_(std::max<std::size_t>(1, capacity)),
        policy_(policy),
        waitingForKeyframe_(policy == OverflowPolicy::DropUntilKeyframe) {}

  // Returns true when this consumer just lost a dependency and needs a keyframe.
  bool offer(const T& item) {
    {
      std::lock_guard lock(m_);
      ++stats_.offered;
      if (waitingForKeyframe_) {
        if (!isRandomAccessPoint(item)) {
          ++stats_.dropped;
          return false;  // already asked once; the encoder coalesces requests
        }
        waitingForKeyframe_ = false;
      }
      if (q_.size() >= capacity_) {
        if (policy_ == OverflowPolicy::DropOldest) {
          q_.pop_front();
          ++stats_.dropped;
        } else {
          // Consumer is a full queue behind: flush stale frames and resume fresh at a keyframe.
          stats_.dropped += q_.size();
          q_.clear();
          if (!isRandomAccessPoint(item)) {
            ++stats_.dropped;
            waitingForKeyframe_ = true;
            return true;
          }
        }
      }
      q_.push_back(item);
      stats_.highWatermark = std::max(stats_.highWatermark, q_.size());
    }
    cv_.notify_one();
    return false;
  }

  // Blocks until an item arrives or stop is requested.
  std::optional<T> pop(std::stop_token st) {
    std::unique_lock lock(m_);
    if (!cv_.wait(lock, st, [&] { return !q_.empty(); })) return std::nullopt;
    return takeFront();
  }

  std::optional<T> tryPop() {
    std::lock_guard lock(m_);
    if (q_.empty()) return std::nullopt;
    return takeFront();
  }

  QueueStats stats() const {
    std::lock_guard lock(m_);
    QueueStats s = stats_;
    s.depth = q_.size();
    s.waitingForKeyframe = waitingForKeyframe_;
    return s;
  }

  std::size_t capacity() const noexcept { return capacity_; }

 private:
  T takeFront() {
    T item = std::move(q_.front());
    q_.pop_front();
    ++stats_.delivered;
    return item;
  }

  const std::size_t capacity_;
  const OverflowPolicy policy_;
  mutable std::mutex m_;
  std::condition_variable_any cv_;
  std::deque<T> q_;
  bool waitingForKeyframe_;
  QueueStats stats_;
};

// Non-blocking fan-out of one producer's output to any number of subscriptions. Items are shared
// (shared_ptr payloads), never deep-copied. A consumer unsubscribes by releasing its Subscription.
template <class T>
class Distributor {
 public:
  void setKeyframeRequester(std::function<void()> fn) {
    std::lock_guard lock(m_);
    requester_ = std::move(fn);
  }

  std::shared_ptr<Subscription<T>> subscribe(std::size_t capacity, OverflowPolicy policy) {
    auto sub = std::make_shared<Subscription<T>>(capacity, policy);
    std::function<void()> request;
    {
      std::lock_guard lock(m_);
      subs_.push_back(sub);
      if (policy == OverflowPolicy::DropUntilKeyframe) request = requester_;
    }
    if (request) request();  // late joiner must start at a keyframe
    return sub;
  }

  void publish(const T& item) {
    bool needKeyframe = false;
    std::function<void()> request;
    {
      std::lock_guard lock(m_);
      for (auto it = subs_.begin(); it != subs_.end();) {
        if (auto sub = it->lock()) {
          needKeyframe |= sub->offer(item);
          ++it;
        } else {
          it = subs_.erase(it);
        }
      }
      if (needKeyframe) request = requester_;
    }
    if (request) request();
  }

  std::size_t subscriberCount() const {
    std::lock_guard lock(m_);
    return static_cast<std::size_t>(
        std::ranges::count_if(subs_, [](const auto& w) { return !w.expired(); }));
  }

 private:
  mutable std::mutex m_;
  std::vector<std::weak_ptr<Subscription<T>>> subs_;
  std::function<void()> requester_;
};

}  // namespace eme
