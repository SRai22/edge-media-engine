#include <gtest/gtest.h>

#include "eme/core/distributor.hpp"

using namespace eme;

namespace {

EncodedVideoFrame encoded(std::uint64_t seq, bool key) {
  return {.sequence = seq, .isKeyframe = key, .payload = std::make_shared<EncodedBuffer>()};
}

}  // namespace

TEST(Subscription, DropOldestKeepsFreshest) {
  Subscription<VideoFrame> sub(2, OverflowPolicy::DropOldest);
  for (std::uint64_t i = 0; i < 5; ++i) sub.offer(VideoFrame{.frameId = i});
  auto s = sub.stats();
  EXPECT_EQ(s.depth, 2u);
  EXPECT_EQ(s.dropped, 3u);
  EXPECT_EQ(s.highWatermark, 2u);
  EXPECT_EQ(sub.tryPop()->frameId, 3u);
  EXPECT_EQ(sub.tryPop()->frameId, 4u);
  EXPECT_FALSE(sub.tryPop());
}

TEST(Subscription, NewEncodedSubscriberStartsAtKeyframe) {
  Subscription<EncodedVideoFrame> sub(4, OverflowPolicy::DropUntilKeyframe);
  sub.offer(encoded(0, false));
  sub.offer(encoded(1, false));
  sub.offer(encoded(2, true));
  sub.offer(encoded(3, false));
  EXPECT_EQ(sub.tryPop()->sequence, 2u);
  EXPECT_EQ(sub.tryPop()->sequence, 3u);
  EXPECT_EQ(sub.stats().dropped, 2u);
}

TEST(Subscription, OverflowFlushesAndWaitsForKeyframe) {
  Subscription<EncodedVideoFrame> sub(2, OverflowPolicy::DropUntilKeyframe);
  EXPECT_FALSE(sub.offer(encoded(0, true)));
  EXPECT_FALSE(sub.offer(encoded(1, false)));
  EXPECT_TRUE(sub.offer(encoded(2, false)));   // overflow on a P-frame: flush, request keyframe
  EXPECT_FALSE(sub.offer(encoded(3, false)));  // still waiting: dropped, no repeated request
  EXPECT_TRUE(sub.stats().waitingForKeyframe);
  EXPECT_EQ(sub.stats().depth, 0u);
  EXPECT_FALSE(sub.offer(encoded(4, true)));
  EXPECT_EQ(sub.tryPop()->sequence, 4u);
  EXPECT_FALSE(sub.stats().waitingForKeyframe);
}

TEST(Subscription, OverflowOnKeyframeRestartsAtThatKeyframe) {
  Subscription<EncodedVideoFrame> sub(1, OverflowPolicy::DropUntilKeyframe);
  sub.offer(encoded(0, true));
  EXPECT_FALSE(sub.offer(encoded(1, true)));
  EXPECT_EQ(sub.tryPop()->sequence, 1u);
}

TEST(Subscription, PopReturnsOnStop) {
  Subscription<VideoFrame> sub(1, OverflowPolicy::DropOldest);
  std::stop_source stop;
  stop.request_stop();
  EXPECT_FALSE(sub.pop(stop.get_token()));
}

TEST(Distributor, FansOutSharedPayloadWithoutCopy) {
  Distributor<EncodedVideoFrame> d;
  auto a = d.subscribe(4, OverflowPolicy::DropUntilKeyframe);
  auto b = d.subscribe(4, OverflowPolicy::DropUntilKeyframe);
  auto c = d.subscribe(4, OverflowPolicy::DropUntilKeyframe);
  auto frame = encoded(0, true);
  d.publish(frame);
  for (auto* s : {&a, &b, &c}) EXPECT_EQ((*s)->tryPop()->payload.get(), frame.payload.get());
  EXPECT_EQ(frame.payload.use_count(), 1);  // queues released their references
}

TEST(Distributor, ReleasedSubscriptionIsPruned) {
  Distributor<VideoFrame> d;
  auto a = d.subscribe(1, OverflowPolicy::DropOldest);
  auto b = d.subscribe(1, OverflowPolicy::DropOldest);
  b.reset();
  EXPECT_EQ(d.subscriberCount(), 1u);
  d.publish(VideoFrame{});
  EXPECT_EQ(a->stats().depth, 1u);
}

TEST(Distributor, RequestsKeyframeOnJoinAndOnDependencyLoss) {
  Distributor<EncodedVideoFrame> d;
  int requests = 0;
  d.setKeyframeRequester([&] { ++requests; });
  auto raw = d.subscribe(1, OverflowPolicy::DropOldest);
  EXPECT_EQ(requests, 0);  // raw-style subscribers never need keyframes
  auto sub = d.subscribe(1, OverflowPolicy::DropUntilKeyframe);
  EXPECT_EQ(requests, 1);
  d.publish(encoded(0, true));
  d.publish(encoded(1, false));  // overflow -> loss
  EXPECT_EQ(requests, 2);
  d.publish(encoded(2, false));  // still waiting -> no new request
  EXPECT_EQ(requests, 2);
}
