#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

#include "eme/core/frame_pool.hpp"
#include "eme/processing/openh264_encoder.hpp"
#include "test_util.hpp"

using namespace eme;

namespace {

bool has(const std::vector<int>& v, int t) { return std::ranges::find(v, t) != v.end(); }

}  // namespace

TEST(OpenH264Encoder, ProducesBrowserCompatibleAnnexB) {
  auto created = OpenH264Encoder::create({.width = 320, .height = 240, .fps = 15, .bitrateBps = 300'000, .keyframeIntervalFrames = 10});
  ASSERT_TRUE(created) << created.error().message;
  auto& enc = **created;
  FramePool pool(320 * 240 * 3 / 2, 2);
  std::vector<EncodedVideoFrame> out;
  for (std::uint64_t i = 0; i < 25; ++i) {
    if (i == 13) enc.requestKeyframe();
    auto buf = pool.acquire();
    std::ranges::fill(buf->data(), std::byte{static_cast<unsigned char>(i * 7)});
    VideoFrame f{.frameId = i, .width = 320, .height = 240, .pixelFormat = PixelFormat::I420,
                 .presentationTimestampNs = static_cast<std::int64_t>(i) * 66'666'667, .buffer = buf};
    ASSERT_TRUE(enc.encode(f, [&](EncodedVideoFrame&& e) { out.push_back(std::move(e)); }));
  }
  ASSERT_EQ(out.size(), 25u);  // frame skipping disabled
  const auto first = test::nalTypes(out[0].payload->bytes);
  EXPECT_TRUE(out[0].isKeyframe);
  EXPECT_TRUE(has(first, 7) && has(first, 8) && has(first, 5)) << "IDR must carry SPS+PPS in-band";
  // SPS: start code, NAL header 0x67, then profile_idc. 66 = Baseline; constraint_set1 (0x40) = Constrained Baseline.
  const auto& b = out[0].payload->bytes;
  const auto sps = std::ranges::search(b, std::vector<std::byte>{std::byte{0}, std::byte{0}, std::byte{1}, std::byte{0x67}});
  ASSERT_FALSE(sps.empty());
  const auto profile = std::to_integer<int>(*(sps.end()));
  const auto constraints = std::to_integer<int>(*(sps.end() + 1));
  EXPECT_EQ(profile, 66);
  EXPECT_TRUE(constraints & 0x40) << "constrained baseline expected";
  EXPECT_FALSE(out[1].isKeyframe);
  EXPECT_TRUE(has(test::nalTypes(out[1].payload->bytes), 1));
  EXPECT_TRUE(out[13].isKeyframe);  // forced
  EXPECT_EQ(enc.stats().forcedKeyframes, 1u);
  for (std::size_t i = 0; i < out.size(); ++i) EXPECT_EQ(out[i].sequence, i);
}

TEST(OpenH264Encoder, RejectsInvalidConfigAndInput) {
  EXPECT_FALSE(OpenH264Encoder::create({.width = 321, .height = 240}));
  EXPECT_FALSE(OpenH264Encoder::create({.bitrateBps = 0}));
  auto enc = OpenH264Encoder::create({.width = 64, .height = 32});
  ASSERT_TRUE(enc);
  EXPECT_FALSE((*enc)->supportsInput({.kind = MediaKind::RawVideo, .pixelFormat = PixelFormat::NV12, .width = 64, .height = 32}));
  EXPECT_FALSE((*enc)->encode(VideoFrame{.width = 64, .height = 32, .pixelFormat = PixelFormat::I420}, [](EncodedVideoFrame&&) {}));
}
