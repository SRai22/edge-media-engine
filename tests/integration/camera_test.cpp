// Real-camera tests: run when a V4L2 camera is present, skip cleanly otherwise (CI).
#include <gtest/gtest.h>

#include <algorithm>

#include "eme/core/graph.hpp"
#include "eme/sources/camera.hpp"
#include "test_util.hpp"

using namespace eme;
using namespace std::chrono_literals;

TEST(Camera, OpenRejectsBadConfigAndMissingDevice) {
  EXPECT_EQ(CameraSource::open("cam", {.id = "/dev/video0", .width = 641}).error().code, ErrorCode::InvalidArgument);
  EXPECT_EQ(CameraSource::open("cam", {.id = "/dev/video0", .fps = 0}).error().code, ErrorCode::InvalidArgument);
  EXPECT_EQ(CameraSource::open("cam", {.id = "/dev/does-not-exist"}).error().code, ErrorCode::DeviceLost);
}

TEST(Camera, EnumeratedCamerasHaveUsableModes) {
  const auto cams = enumerateCameras();
  if (cams.empty()) GTEST_SKIP() << "no camera";
  for (const auto& c : cams) {
    EXPECT_FALSE(c.name.empty());
    ASSERT_FALSE(c.modes.empty()) << c.id;
    for (const auto& m : c.modes) EXPECT_TRUE(m.width > 0 && m.height > 0 && m.fourcc.size() == 4) << c.id;
  }
}

TEST(Camera, CapturesI420FramesWithMonotonicTimestamps) {
  const auto cams = enumerateCameras();
  if (cams.empty()) GTEST_SKIP() << "no camera";
  auto src = CameraSource::open("cam", {.id = cams.front().id, .width = 640, .height = 480, .fps = 15});
  if (!src && (src.error().code == ErrorCode::DeviceLost || src.error().code == ErrorCode::ResourceExhausted)) {
    GTEST_SKIP() << "camera unavailable or busy: " << src.error().message;
  }
  ASSERT_TRUE(src) << src.error().message;
  MediaGraph g;
  auto& cam = g.adopt(std::move(*src));
  auto sub = cam.output().subscribe(2, OverflowPolicy::DropOldest);  // queue + held frame < pool (4)
  ASSERT_TRUE(g.start());
  const auto fmt = *cam.outputFormat();
  // Consume like a real consumer: inspect and release each frame (the pool has only poolSize buffers).
  std::int64_t lastTs = -1, firstTs = -1;
  int n = 0;
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (n < 10 && std::chrono::steady_clock::now() < deadline) {
    auto f = sub->tryPop();
    if (!f) {
      std::this_thread::sleep_for(5ms);
      continue;
    }
    EXPECT_EQ(f->pixelFormat, PixelFormat::I420);
    EXPECT_EQ(f->width, fmt.width);
    EXPECT_EQ(f->height, fmt.height);
    ASSERT_GE(f->buffer->cpuData().size(), static_cast<std::size_t>(f->width * f->height * 3 / 2));
    // shortcut: no pixel-content check (a covered camera is legitimately black); content was verified against ffmpeg (EME-017).
    EXPECT_GT(f->captureTimestampNs, lastTs);
    EXPECT_GE(f->presentationTimestampNs, 0);
    if (firstTs < 0) firstTs = f->captureTimestampNs;
    lastTs = f->captureTimestampNs;
    ++n;
  }
  g.stop();
  // Decimation: never faster than the requested rate (camera may be slower, e.g. auto-exposure in the dark).
  if (n >= 2) {
    const double seconds = static_cast<double>(lastTs - firstTs) / 1e9;
    EXPECT_LE((n - 1) / seconds, fmt.fps * 1.25) << cam.negotiated();
  }
  EXPECT_GE(n, 10) << cam.negotiated() << " captured=" << cam.stats().framesCaptured << " dropped=" << cam.stats().framesDropped;
  EXPECT_EQ(cam.stats().framesDropped, 0u);
  EXPECT_FALSE(cam.stats().deviceLost);
}

TEST(Camera, SecondSourceCannotClaimTheSameCamera) {
  const auto cams = enumerateCameras();
  if (cams.empty()) GTEST_SKIP() << "no camera";
  auto a = CameraSource::open("a", {.id = cams.front().id, .width = 640, .height = 480});
  if (!a) GTEST_SKIP() << "camera unavailable: " << a.error().message;
  auto b = CameraSource::open("b", {.id = cams.front().id, .width = 640, .height = 480});
  ASSERT_FALSE(b);
  EXPECT_EQ(b.error().code, ErrorCode::ResourceExhausted);
  a->reset();  // released on close
  EXPECT_TRUE(CameraSource::open("c", {.id = cams.front().id, .width = 640, .height = 480}));
}

// Simulated loss exercises release -> rediscover -> reopen on the same node. Real ENODEV detection and a changed
// /dev node were verified with a physical unplug/replug (checks.md, EME-017).
TEST(Camera, ResumesAfterDeviceLoss) {
  const auto cams = enumerateCameras();
  if (cams.empty()) GTEST_SKIP() << "no camera";
  auto src = CameraSource::open("cam", {.id = cams.front().id, .width = 640, .height = 480, .fps = 15,
                                        .reconnectInterval = std::chrono::milliseconds(1000)});
  if (!src && (src.error().code == ErrorCode::DeviceLost || src.error().code == ErrorCode::ResourceExhausted)) {
    GTEST_SKIP() << "camera unavailable or busy: " << src.error().message;
  }
  ASSERT_TRUE(src) << src.error().message;
  MediaGraph g;
  auto& cam = g.adopt(std::move(*src));
  auto sub = cam.output().subscribe(2, OverflowPolicy::DropOldest);
  ASSERT_TRUE(g.start());
  std::uint64_t lastId = 0;
  std::int64_t lastTs = -1;
  int bad = 0;
  auto drainUntil = [&](auto done, std::chrono::seconds timeout) {  // consume frames like a real consumer
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!done() && std::chrono::steady_clock::now() < deadline) {
      if (auto f = sub->tryPop()) {
        if (lastTs >= 0 && (f->frameId <= lastId || f->captureTimestampNs <= lastTs)) ++bad;
        lastId = f->frameId;
        lastTs = f->captureTimestampNs;
      } else {
        std::this_thread::sleep_for(5ms);
      }
    }
    return done();
  };
  ASSERT_TRUE(drainUntil([&] { return cam.stats().framesPublished >= 5; }, 10s));
  cam.simulateDeviceLossForTesting();
  ASSERT_TRUE(drainUntil([&] { return cam.stats().deviceLost; }, 5s)) << "loss not reported";
  const auto atLoss = cam.stats().framesPublished;
  std::this_thread::sleep_for(500ms);  // inside the 1 s reconnect wait
  EXPECT_TRUE(cam.stats().deviceLost);
  EXPECT_EQ(cam.stats().framesPublished, atLoss) << "must not publish while the device is gone";
  ASSERT_TRUE(drainUntil([&] { return cam.stats().reconnects == 1; }, 10s)) << "never reconnected";
  const auto resumedAt = cam.stats().framesPublished;
  ASSERT_TRUE(drainUntil([&] { return cam.stats().framesPublished >= resumedAt + 5; }, 10s)) << "no frames after reconnect";
  g.stop();
  EXPECT_FALSE(cam.stats().deviceLost);
  EXPECT_EQ(bad, 0) << "frame ids and timestamps must keep increasing across the reconnect";
}
