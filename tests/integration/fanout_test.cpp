// Milestone 1 exit criterion: one encoded source fans out to multiple independent consumers
// without repeated encoding or unbounded memory growth, and slow/stalled consumers never block capture.
#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "eme/core/graph.hpp"
#include "eme/processing/encoder_node.hpp"
#include "eme/sources/synthetic_video_source.hpp"
#include "eme/testing/counting_sink.hpp"
#include "eme/testing/mock_h264_encoder.hpp"
#include "test_util.hpp"

using namespace eme;
using namespace std::chrono_literals;
using mock::CountingSink;

TEST(FanOut, SharedEncodeToManyConsumers) {
  constexpr int kFrames = 300;
  MediaGraph g;
  auto& src = g.add<SyntheticVideoSource>(
      "cam", SyntheticSourceConfig{.width = 320, .height = 240, .fps = 15, .poolSize = 4, .manualClock = true});
  auto& enc = g.add<EncoderNode>(
      "h264",
      std::make_unique<mock::MockH264Encoder>(
          EncoderConfig{.width = 320, .height = 240, .fps = 15, .keyframeIntervalFrames = 1'000'000}),
      EncoderNode::Config{.minKeyframeInterval = std::chrono::milliseconds(0)});  // only frame 0 is a periodic keyframe
  auto& fastA = g.add<CountingSink>("p2p-a", CountingSink::Config{.queueCapacity = 512});
  auto& fastB = g.add<CountingSink>("sfu", CountingSink::Config{.queueCapacity = 512});
  auto& slow = g.add<CountingSink>("slow", CountingSink::Config{.queueCapacity = 2, .perFrameDelay = 5ms});
  ASSERT_TRUE(g.connect(src, enc));
  for (Node* sink : {static_cast<Node*>(&fastA), static_cast<Node*>(&fastB), static_cast<Node*>(&slow)}) {
    ASSERT_TRUE(g.connect(enc, *sink));
  }
  // A consumer that never reads at all: must not stall anything.
  auto stalled = enc.output().subscribe(3, OverflowPolicy::DropUntilKeyframe);
  ASSERT_TRUE(g.start());

  for (int i = 0; i < kFrames; ++i) {
    src.emitFrame();
    std::this_thread::sleep_for(500us);
  }
  auto& mock = enc.encoder();
  ASSERT_TRUE(test::waitFor([&] {
    const auto s = mock.stats().framesOut;
    return fastA.stats().received == s && fastB.stats().received == s && enc.inputStats().depth == 0;
  }));
  ASSERT_TRUE(test::waitFor([&] { return slow.stats().queue.depth == 0; }));
  g.stop();

  const auto es = mock.stats();
  const auto in = enc.inputStats();
  // Every captured frame was either encoded exactly once or dropped at the encoder input; never per consumer.
  EXPECT_EQ(src.framesProduced() + src.framesDropped(), static_cast<std::uint64_t>(kFrames));
  EXPECT_EQ(es.framesIn, in.delivered);
  EXPECT_EQ(in.offered, src.framesProduced());
  EXPECT_EQ(in.delivered + in.dropped, in.offered);
  EXPECT_GT(es.framesIn, kFrames / 2u);
  EXPECT_EQ(enc.output().subscriberCount(), 4u);

  // Fast consumers got the whole stream, sharing the very same payload objects.
  EXPECT_EQ(fastA.stats().received, es.framesOut);
  EXPECT_EQ(fastA.stats().lastPayload, fastB.stats().lastPayload);

  // Slow and stalled consumers dropped, stayed bounded, and never saw a broken dependency chain.
  const auto ss = slow.stats();
  EXPECT_GT(ss.queue.dropped, 0u);
  EXPECT_LE(ss.queue.highWatermark, 2u);
  EXPECT_LE(stalled->stats().highWatermark, 3u);
  for (const auto* s : {&fastA, &fastB, &slow}) EXPECT_EQ(s->stats().dependencyViolations, 0u) << s->name();
  // With no periodic GOP, the slow sink can only have resumed after drops via requested keyframes.
  // (With the limit at 0 an overloaded sink may see only keyframes -- why the default limit is 500 ms.)
  EXPECT_GE(ss.keyframes, 2u);
  EXPECT_GE(es.forcedKeyframes, 2u);

  // Memory stayed bounded by the pool.
  EXPECT_LE(src.pool().allocated(), src.pool().capacity());
}

TEST(FanOut, ThreadedSourceRunsAndStopsCleanly) {
  MediaGraph g;
  auto& src = g.add<SyntheticVideoSource>("cam", SyntheticSourceConfig{.width = 64, .height = 32, .fps = 200});
  auto& enc = g.add<EncoderNode>(
      "h264", std::make_unique<mock::MockH264Encoder>(EncoderConfig{.width = 64, .height = 32, .fps = 200}));
  auto& sink = g.add<CountingSink>("sink", CountingSink::Config{});
  ASSERT_TRUE(g.connect(src, enc));
  ASSERT_TRUE(g.connect(enc, sink));
  ASSERT_TRUE(g.start());
  EXPECT_TRUE(test::waitFor([&] { return sink.stats().received >= 20; }));
  g.stop();
  EXPECT_EQ(sink.stats().dependencyViolations, 0u);
  EXPECT_EQ(enc.encodeErrors(), 0u);
}

TEST(FanOut, KeyframeRequestsAreRateLimited) {
  MediaGraph g;
  auto& src = g.add<SyntheticVideoSource>("cam", SyntheticSourceConfig{.width = 64, .height = 32, .manualClock = true});
  auto& enc = g.add<EncoderNode>(
      "h264", std::make_unique<mock::MockH264Encoder>(EncoderConfig{.width = 64, .height = 32, .keyframeIntervalFrames = 1000}),
      EncoderNode::Config{.minKeyframeInterval = std::chrono::hours(1)});
  ASSERT_TRUE(g.connect(src, enc));
  ASSERT_TRUE(g.start());
  auto& mock = enc.encoder();
  auto emitAndWait = [&](std::uint64_t n) {
    src.emitFrame();
    ASSERT_TRUE(test::waitFor([&] { return mock.stats().framesOut == n; }));
  };
  emitAndWait(1);  // periodic keyframe
  auto late1 = enc.output().subscribe(4, OverflowPolicy::DropUntilKeyframe);
  emitAndWait(2);  // first request forwarded immediately
  auto late2 = enc.output().subscribe(4, OverflowPolicy::DropUntilKeyframe);
  auto late3 = enc.output().subscribe(4, OverflowPolicy::DropUntilKeyframe);
  emitAndWait(3);  // inside the interval: deferred
  g.stop();
  EXPECT_EQ(enc.keyframeRequestsReceived(), 3u);
  EXPECT_EQ(mock.stats().forcedKeyframes, 1u);
  EXPECT_EQ(late1->stats().depth, 2u);       // started at the forced keyframe
  EXPECT_TRUE(late2->stats().waitingForKeyframe);  // still waiting, but not dropping a valid chain
}
