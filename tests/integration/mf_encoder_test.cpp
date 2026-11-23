// Media Foundation hardware H.264 encoder (EME-006). Needs a hardware H.264 MFT: skipped on CI runners (no GPU),
// must PASS on the target device (docs/runbooks/eme-006-device.md).
#include <gtest/gtest.h>

#include <algorithm>
#include <thread>
#include <vector>

#include "eme/processing/h264_annexb.hpp"
#include "eme/processing/mf_h264_encoder.hpp"
#include "eme/sources/synthetic_video_source.hpp"
#include "eme/testing/barcode.hpp"

#include <wels/codec_api.h>
#include "test_util.hpp"

using namespace eme;

namespace {

#define CREATE_OR_SKIP(var, cfg)                                                       \
  auto var##_created = MfH264Encoder::create(cfg);                                     \
  if (!var##_created && var##_created.error().code == ErrorCode::Unsupported) {        \
    GTEST_SKIP() << var##_created.error().message;                                     \
  }                                                                                    \
  ASSERT_TRUE(var##_created) << var##_created.error().message;                         \
  auto& var = **var##_created

struct Run {
  std::vector<EncodedVideoFrame> out;
  std::vector<std::int64_t> inputCaptureNs;
};

// Encodes `frames` textured 720p frames stamped with their capture time; requests a keyframe before frame `forceAt`.
Run encodeFrames(MfH264Encoder& enc, PixelFormat format, int frames, int forceAt) {
  Run run;
  SyntheticVideoSource src("cam", SyntheticSourceConfig{.pixelFormat = format, .manualClock = true,
                                                        .pattern = SyntheticPattern::MovingTexture, .timestampBarcode = true});
  EXPECT_TRUE(src.start());
  auto sub = src.output().subscribe(4, OverflowPolicy::DropOldest);
  for (int i = 0; i < frames; ++i) {
    EXPECT_TRUE(src.emitFrame());
    const auto frame = sub->tryPop();
    EXPECT_TRUE(frame);
    if (!frame) break;
    if (i == forceAt) enc.requestKeyframe();
    run.inputCaptureNs.push_back(frame->captureTimestampNs);
    const auto r = enc.encode(*frame, [&](EncodedVideoFrame&& e) { run.out.push_back(std::move(e)); });
    EXPECT_TRUE(r) << (r ? "" : r.error().message);
  }
  return run;
}

}  // namespace

TEST(MfH264Encoder, RejectsInvalidConfigAndInput) {
  EXPECT_EQ(MfH264Encoder::create({.width = 321, .height = 240}).error().code, ErrorCode::InvalidArgument);
  EXPECT_EQ(MfH264Encoder::create({.bitrateBps = 0}).error().code, ErrorCode::InvalidArgument);
  EXPECT_EQ(MfH264Encoder::create({.keyframeIntervalFrames = 0}).error().code, ErrorCode::InvalidArgument);
  CREATE_OR_SKIP(enc, EncoderConfig{});
  EXPECT_TRUE(enc.supportsInput({.kind = MediaKind::RawVideo, .pixelFormat = PixelFormat::NV12, .width = 1280, .height = 720}));
  EXPECT_FALSE(enc.supportsInput({.kind = MediaKind::RawVideo, .pixelFormat = PixelFormat::YUY2, .width = 1280, .height = 720}));
  EXPECT_FALSE(enc.supportsInput({.kind = MediaKind::RawVideo, .pixelFormat = PixelFormat::I420, .width = 640, .height = 360}));
  EXPECT_FALSE(enc.encode(VideoFrame{.width = 1280, .height = 720, .pixelFormat = PixelFormat::I420}, [](EncodedVideoFrame&&) {}));
}

// Acceptance: decodable output, settings read back, SPS/PPS in CodecConfiguration, forced keyframe within <= 2 frames,
// output metadata belongs to the right input frame (the decoded barcode equals the frame's capture time).
TEST(MfH264Encoder, EncodesDecodableBaselineWithForcedKeyframe) {
  for (const auto format : {PixelFormat::I420, PixelFormat::NV12}) {
    SCOPED_TRACE(format == PixelFormat::I420 ? "I420 input" : "NV12 input");
    CREATE_OR_SKIP(enc, (EncoderConfig{.fps = 15, .bitrateBps = 1'200'000, .keyframeIntervalFrames = 30}));
    const auto& info = enc.info();
    EXPECT_EQ(info.profile, 66u);
    EXPECT_TRUE(info.cbr);
    EXPECT_EQ(info.bitrateBps, 1'200'000u);
    EXPECT_EQ(info.gop, 30u);
    EXPECT_EQ(info.bFrames, 0u);
    EXPECT_TRUE(info.forceKeyframeSupported);

    constexpr int kFrames = 60, kForceAt = 40;
    const auto run = encodeFrames(enc, format, kFrames, kForceAt);
    ASSERT_GE(run.out.size(), static_cast<std::size_t>(kFrames - 3)) << "output lags input by at most a few frames";
    EXPECT_EQ(enc.stats().framesIn, static_cast<std::uint64_t>(kFrames));
    EXPECT_EQ(enc.stats().forcedKeyframes, 1u);

    ASSERT_TRUE(run.out.front().isKeyframe);
    ASSERT_GE(run.out.front().codecConfig.version, 1u);
    ASSERT_TRUE(run.out.front().codecConfig.parameterSets);
    const auto ps = test::nalTypes(*run.out.front().codecConfig.parameterSets);
    EXPECT_TRUE(std::ranges::find(ps, 7) != ps.end() && std::ranges::find(ps, 8) != ps.end());

    // The first keyframe at or after the forced frame is at most 2 frames later (the regular GOP would be frame 60).
    const auto forcedNs = run.inputCaptureNs[kForceAt];
    const auto key = std::ranges::find_if(run.out, [&](const auto& e) { return e.isKeyframe && e.captureTimestampNs >= forcedNs; });
    ASSERT_NE(key, run.out.end()) << "no keyframe after the request";
    const auto keyIndex = std::ranges::find(run.inputCaptureNs, key->captureTimestampNs) - run.inputCaptureNs.begin();
    EXPECT_LE(keyIndex - kForceAt, 2);

    ISVCDecoder* dec = nullptr;
    ASSERT_EQ(WelsCreateDecoder(&dec), 0);
    SDecodingParam dp{};
    dp.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_AVC;
    ASSERT_EQ(dec->Initialize(&dp), 0);
    int decoded = 0;
    for (std::size_t i = 0; i < run.out.size(); ++i) {
      const auto& e = run.out[i];
      EXPECT_EQ(e.sequence, i);
      unsigned char* planes[3] = {};
      SBufferInfo bi{};
      std::vector<std::byte> au = e.payload->bytes;  // decoder API takes a mutable buffer
      ASSERT_EQ(dec->DecodeFrameNoDelay(reinterpret_cast<unsigned char*>(au.data()), static_cast<int>(au.size()), planes, &bi), 0)
          << "frame " << i;
      if (bi.iBufferStatus != 1) continue;
      EXPECT_EQ(bi.UsrData.sSystemBuffer.iWidth, 1280);
      EXPECT_EQ(bi.UsrData.sSystemBuffer.iHeight, 720);
      const auto stamp = mock::readBarcode(planes[0], 1280, bi.UsrData.sSystemBuffer.iStride[0]);
      ASSERT_TRUE(stamp) << "frame " << i;
      EXPECT_EQ(*stamp, static_cast<std::uint64_t>(e.captureTimestampNs / 1000)) << "frame " << i;
      ++decoded;
    }
    dec->Uninitialize();
    WelsDestroyDecoder(dec);
    EXPECT_EQ(decoded, static_cast<int>(run.out.size()));
  }
}

// Acceptance: two sessions at once (two cameras on the target).
TEST(MfH264Encoder, TwoSessionsConcurrently) {
  CREATE_OR_SKIP(a, EncoderConfig{});
  CREATE_OR_SKIP(b, EncoderConfig{});
  Run ra, rb;
  {
    std::jthread ta([&] { ra = encodeFrames(a, PixelFormat::I420, 45, -1); });
    std::jthread tb([&] { rb = encodeFrames(b, PixelFormat::I420, 45, -1); });
  }
  EXPECT_GE(ra.out.size(), 42u);
  EXPECT_GE(rb.out.size(), 42u);
  EXPECT_GE(a.stats().keyframes, 2u);  // GOP 30
  EXPECT_GE(b.stats().keyframes, 2u);
}

// Destroying an encoder must free its hardware session (the event callback must not keep the MFT alive): Quick Sync
// has a small session limit, so leaked sessions make later creates fail.
TEST(MfH264Encoder, ReleasesSessionsOnDestroy) {
  for (int i = 0; i < 20; ++i) {
    SCOPED_TRACE(i);
    CREATE_OR_SKIP(enc, EncoderConfig{});
    encodeFrames(enc, PixelFormat::I420, 3, -1);
    EXPECT_EQ(enc.stats().framesIn, 3u);
  }
}
