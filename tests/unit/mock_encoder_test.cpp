#include <gtest/gtest.h>

#include <vector>

#include "eme/core/frame_pool.hpp"
#include "eme/testing/mock_h264_encoder.hpp"

using namespace eme;

namespace {

struct Harness {
  mock::MockH264Encoder encoder{{.width = 64, .height = 32, .fps = 15, .keyframeIntervalFrames = 4}};
  FramePool pool{64 * 32 * 3 / 2, 2};
  std::vector<EncodedVideoFrame> out;

  void encodeOne(std::uint64_t id) {
    VideoFrame f{.frameId = id, .width = 64, .height = 32, .pixelFormat = PixelFormat::NV12, .buffer = pool.acquire()};
    ASSERT_TRUE(encoder.encode(f, [&](EncodedVideoFrame&& e) { out.push_back(std::move(e)); }));
  }
};

}  // namespace

TEST(MockH264Encoder, PeriodicKeyframesAndAnnexB) {
  Harness h;
  for (std::uint64_t i = 0; i < 9; ++i) h.encodeOne(i);
  ASSERT_EQ(h.out.size(), 9u);
  for (std::size_t i = 0; i < h.out.size(); ++i) {
    EXPECT_EQ(h.out[i].isKeyframe, i % 4 == 0) << i;
    EXPECT_EQ(h.out[i].sequence, i);
    const auto& b = h.out[i].payload->bytes;
    EXPECT_EQ(b[3], std::byte{1});
    EXPECT_EQ(b[4], h.out[i].isKeyframe ? std::byte{0x65} : std::byte{0x41});
  }
  EXPECT_EQ(h.out[0].payload->bytes.size(), 1'200'000u / 8 / 15);
  EXPECT_EQ(h.out[0].codecConfig.parameterSets, h.out[5].codecConfig.parameterSets);  // shared
}

TEST(MockH264Encoder, CoalescesKeyframeRequests) {
  Harness h;
  h.encodeOne(0);
  for (int i = 0; i < 10; ++i) h.encoder.requestKeyframe();
  h.encodeOne(1);
  h.encodeOne(2);
  EXPECT_TRUE(h.out[1].isKeyframe);
  EXPECT_FALSE(h.out[2].isKeyframe);
  const auto s = h.encoder.stats();
  EXPECT_EQ(s.keyframeRequests, 10u);
  EXPECT_EQ(s.forcedKeyframes, 1u);
}

TEST(MockH264Encoder, RejectsBadInput) {
  Harness h;
  EXPECT_FALSE(h.encoder.encode(VideoFrame{}, [](EncodedVideoFrame&&) {}));
  EXPECT_FALSE(h.encoder.setBitrate(0));
  EXPECT_FALSE(h.encoder.supportsInput({.kind = MediaKind::RawVideo, .pixelFormat = PixelFormat::NV12, .width = 1280, .height = 720}));
  EXPECT_FALSE(h.encoder.supportsInput({.kind = MediaKind::RawVideo, .pixelFormat = PixelFormat::MJPEG, .width = 64, .height = 32}));
}
