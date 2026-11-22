#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

#include "eme/core/frame_pool.hpp"
#include "eme/processing/openh264_encoder.hpp"
#include "eme/sources/synthetic_video_source.hpp"
#include "eme/testing/barcode.hpp"

#include <wels/codec_api.h>
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

TEST(Barcode, ReaderRejectsAmbiguousCells) {
  std::vector<std::uint8_t> luma(1280 * 720, 77);
  mock::writeBarcode(luma.data(), 1280, 1280, 0x0123456789abcdefULL);
  EXPECT_EQ(mock::readBarcode(luma.data(), 1280, 1280), 0x0123456789abcdefULL);
  std::fill(luma.begin(), luma.begin() + 64 * 1280, 128);  // erased band -> rejected, never a wrong value
  EXPECT_FALSE(mock::readBarcode(luma.data(), 1280, 1280));
}

TEST(Barcode, SurvivesOpenH264At720p) {
  // Encode textured 720p frames carrying a stamp at 1.2 Mbps, decode with OpenH264's decoder, read it back.
  SyntheticVideoSource src("cam", SyntheticSourceConfig{.pixelFormat = PixelFormat::I420, .manualClock = true,
                                                        .pattern = SyntheticPattern::MovingTexture, .timestampBarcode = true});
  ASSERT_TRUE(src.start());
  auto sub = src.output().subscribe(64, OverflowPolicy::DropOldest);
  auto enc = OpenH264Encoder::create(EncoderConfig{});
  ASSERT_TRUE(enc);
  ISVCDecoder* dec = nullptr;
  ASSERT_EQ(WelsCreateDecoder(&dec), 0);
  SDecodingParam dp{};
  dp.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_AVC;
  ASSERT_EQ(dec->Initialize(&dp), 0);
  int read = 0;
  for (int i = 0; i < 30; ++i) {
    ASSERT_TRUE(src.emitFrame());
    const auto frame = sub->tryPop();
    ASSERT_TRUE(frame);
    const auto stamp = static_cast<std::uint64_t>(frame->captureTimestampNs / 1000);
    ASSERT_TRUE((*enc)->encode(*frame, [&](EncodedVideoFrame&& e) {
      unsigned char* planes[3] = {};
      SBufferInfo info{};
      std::vector<std::byte> au = e.payload->bytes;  // decoder API takes a mutable buffer
      ASSERT_EQ(dec->DecodeFrameNoDelay(reinterpret_cast<unsigned char*>(au.data()), static_cast<int>(au.size()), planes, &info), 0);
      if (info.iBufferStatus != 1) return;
      const auto got = mock::readBarcode(planes[0], 1280, info.UsrData.sSystemBuffer.iStride[0]);
      ASSERT_TRUE(got) << "frame " << i;
      EXPECT_EQ(*got, stamp);
      ++read;
    }));
  }
  dec->Uninitialize();
  WelsDestroyDecoder(dec);
  EXPECT_EQ(read, 30);
  EXPECT_GT((*enc)->stats().framesOut, 0u);
}
