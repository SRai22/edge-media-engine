#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "eme/core/graph.hpp"
#include "eme/processing/encoder_node.hpp"
#include "eme/sources/synthetic_video_source.hpp"
#include "eme/testing/counting_sink.hpp"
#include "eme/testing/mock_h264_encoder.hpp"

using namespace eme;

namespace {

// Encoded in, encoded out; records lifecycle order. Lets tests build cycles and chains.
class Relay final : public Node, public Consumer<EncodedVideoFrame>, public Producer<EncodedVideoFrame> {
 public:
  Relay(std::string name, std::vector<std::string>& log, bool failStart = false)
      : Node(std::move(name)), log_(log), failStart_(failStart) {}
  std::optional<VideoFormat> outputFormat() const override { return VideoFormat{.kind = MediaKind::EncodedVideo}; }
  bool accepts(const VideoFormat& f) const override { return f.kind == MediaKind::EncodedVideo; }
  Result bindInput(Distributor<EncodedVideoFrame>& up) override {
    input_ = up.subscribe(1, OverflowPolicy::DropUntilKeyframe);
    return {};
  }
  Distributor<EncodedVideoFrame>& output() override { return out_; }
  Result start() override {
    log_.push_back("start " + name());
    if (failStart_) return fail(ErrorCode::DeviceLost, name() + " failed");
    return {};
  }
  void stop() noexcept override { log_.push_back("stop " + name()); }

 private:
  std::vector<std::string>& log_;
  bool failStart_;
  std::shared_ptr<Subscription<EncodedVideoFrame>> input_;
  Distributor<EncodedVideoFrame> out_;
};

SyntheticSourceConfig smallSource() { return {.width = 64, .height = 32, .fps = 15, .manualClock = true}; }
EncoderConfig smallEncoder() { return {.width = 64, .height = 32, .fps = 15}; }

}  // namespace

TEST(MediaGraph, RejectsIncompatibleEdges) {
  MediaGraph g;
  auto& src = g.add<SyntheticVideoSource>("src", smallSource());
  auto& enc = g.add<EncoderNode>("enc", std::make_unique<mock::MockH264Encoder>(smallEncoder()));
  auto& sink = g.add<mock::CountingSink>("sink", mock::CountingSink::Config{});
  EXPECT_EQ(g.connect(src, sink).error().code, ErrorCode::Incompatible);  // raw into encoded sink
  EXPECT_EQ(g.connect(sink, enc).error().code, ErrorCode::Incompatible);  // sink has no output

  auto& bigEnc = g.add<EncoderNode>("enc720", std::make_unique<mock::MockH264Encoder>(EncoderConfig{}));
  EXPECT_EQ(g.connect(src, bigEnc).error().code, ErrorCode::Incompatible);  // resolution mismatch
}

TEST(MediaGraph, RejectsCyclesForeignNodesAndDoubleInput) {
  std::vector<std::string> log;
  MediaGraph g, other;
  auto& a = g.add<Relay>("a", log);
  auto& b = g.add<Relay>("b", log);
  auto& foreign = other.add<Relay>("x", log);
  EXPECT_EQ(g.connect(a, a).error().code, ErrorCode::Cycle);
  ASSERT_TRUE(g.connect(a, b));
  EXPECT_EQ(g.connect(b, a).error().code, ErrorCode::Cycle);
  EXPECT_EQ(g.connect(a, foreign).error().code, ErrorCode::InvalidArgument);

  auto& src = g.add<SyntheticVideoSource>("src", smallSource());
  auto& enc = g.add<EncoderNode>("enc", std::make_unique<mock::MockH264Encoder>(smallEncoder()));
  ASSERT_TRUE(g.connect(src, enc));
  EXPECT_EQ(g.connect(src, enc).error().code, ErrorCode::InvalidState);
}

TEST(MediaGraph, StartsConsumersFirstStopsProducersFirst) {
  std::vector<std::string> log;
  {
    MediaGraph g;
    auto& a = g.add<Relay>("a", log);
    auto& b = g.add<Relay>("b", log);
    auto& c = g.add<Relay>("c", log);
    ASSERT_TRUE(g.connect(a, b));
    ASSERT_TRUE(g.connect(b, c));
    ASSERT_TRUE(g.start());
    EXPECT_EQ(g.connect(a, c).error().code, ErrorCode::InvalidState);  // static once running
  }  // destructor stops
  EXPECT_EQ(log, (std::vector<std::string>{"start c", "start b", "start a", "stop a", "stop b", "stop c"}));
}

TEST(MediaGraph, FailedStartRollsBack) {
  std::vector<std::string> log;
  {
    MediaGraph g;
    auto& up = g.add<Relay>("up", log, true);
    auto& down = g.add<Relay>("down", log);
    ASSERT_TRUE(g.connect(up, down));
    EXPECT_EQ(g.start().error().code, ErrorCode::DeviceLost);
    EXPECT_FALSE(g.running());
  }
  // down started first, up failed, down was rolled back exactly once (destructor did not stop it again).
  EXPECT_EQ(log, (std::vector<std::string>{"start down", "start up", "stop down"}));
}

TEST(MediaGraph, StartFailsForUnconnectedOrInvalidNodes) {
  MediaGraph g;
  g.add<EncoderNode>("unconnected", std::make_unique<mock::MockH264Encoder>(smallEncoder()));
  auto r = g.start();
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().code, ErrorCode::InvalidState);
  EXPECT_FALSE(g.running());

  MediaGraph bad;
  bad.add<SyntheticVideoSource>("zero-fps", SyntheticSourceConfig{.fps = 0});
  EXPECT_EQ(bad.start().error().code, ErrorCode::InvalidArgument);
}

TEST(SyntheticVideoSource, EmitWithInvalidConfigIsRejected) {
  SyntheticVideoSource src("bad", SyntheticSourceConfig{.width = 0, .fps = 0, .manualClock = true});
  EXPECT_FALSE(src.emitFrame());
  EXPECT_EQ(src.framesProduced(), 0u);
}
