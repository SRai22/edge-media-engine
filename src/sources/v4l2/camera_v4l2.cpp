// V4L2 backend for eme::CameraSource (Linux).
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <spdlog/spdlog.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <libyuv.h>
#include <mutex>
#include <thread>

#include "eme/core/frame_pool.hpp"
#include "eme/sources/camera.hpp"

namespace eme {
namespace {

std::string fourccString(std::uint32_t f) {
  std::string s(4, ' ');
  for (int i = 0; i < 4; ++i) s[static_cast<std::size_t>(i)] = static_cast<char>((f >> (8 * i)) & 0xff);
  return s;
}

int xioctl(int fd, unsigned long req, void* arg) {
  int r;
  do r = ioctl(fd, req, arg);
  while (r == -1 && errno == EINTR);
  return r;
}

// Formats libyuv::ConvertToI420 handles that cameras commonly offer. Order = preference when the size/rate fit
// (PLAN §5.2: raw before MJPEG; native H.264 passthrough is a later optimisation).
constexpr std::uint32_t kSupported[] = {V4L2_PIX_FMT_NV12, V4L2_PIX_FMT_YUV420, V4L2_PIX_FMT_YUYV, V4L2_PIX_FMT_UYVY,
                                        V4L2_PIX_FMT_MJPEG};

std::uint32_t libyuvFourcc(std::uint32_t v4l2) {
  switch (v4l2) {
    case V4L2_PIX_FMT_NV12: return libyuv::FOURCC_NV12;
    case V4L2_PIX_FMT_YUV420: return libyuv::FOURCC_I420;
    case V4L2_PIX_FMT_YUYV: return libyuv::FOURCC_YUY2;
    case V4L2_PIX_FMT_UYVY: return libyuv::FOURCC_UYVY;
    case V4L2_PIX_FMT_MJPEG: return libyuv::FOURCC_MJPG;
    default: return 0;
  }
}

struct Fd {
  int fd = -1;
  explicit Fd(int f) : fd(f) {}
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
  ~Fd() {
    if (fd >= 0) ::close(fd);
  }
};

bool isCaptureDevice(int fd, v4l2_capability& cap) {
  if (xioctl(fd, VIDIOC_QUERYCAP, &cap) != 0) return false;
  const auto caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
  return (caps & V4L2_CAP_VIDEO_CAPTURE) && (caps & V4L2_CAP_STREAMING);
}

std::vector<CameraMode> listModes(int fd) {
  std::vector<CameraMode> modes;
  v4l2_fmtdesc fmt{};
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  for (fmt.index = 0; xioctl(fd, VIDIOC_ENUM_FMT, &fmt) == 0; ++fmt.index) {
    if (!libyuvFourcc(fmt.pixelformat)) continue;
    v4l2_frmsizeenum size{};
    size.pixel_format = fmt.pixelformat;
    for (size.index = 0; xioctl(fd, VIDIOC_ENUM_FRAMESIZES, &size) == 0; ++size.index) {
      if (size.type != V4L2_FRMSIZE_TYPE_DISCRETE) break;  // shortcut: stepwise/continuous sizes not listed (rare on webcams)
      CameraMode m{fourccString(fmt.pixelformat), static_cast<int>(size.discrete.width), static_cast<int>(size.discrete.height), {}};
      v4l2_frmivalenum ival{};
      ival.pixel_format = fmt.pixelformat;
      ival.width = size.discrete.width;
      ival.height = size.discrete.height;
      for (ival.index = 0; xioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &ival) == 0; ++ival.index) {
        if (ival.type != V4L2_FRMIVAL_TYPE_DISCRETE) break;
        if (ival.discrete.numerator) m.fps.push_back(static_cast<double>(ival.discrete.denominator) / ival.discrete.numerator);
      }
      modes.push_back(std::move(m));
    }
  }
  return modes;
}

std::uint32_t fourccFromString(const std::string& s) {
  return v4l2_fourcc(s[0], s[1], s[2], s[3]);
}

}  // namespace

std::vector<CameraInfo> enumerateCameras() {
  std::vector<CameraInfo> out;
  std::error_code ec;
  std::vector<std::filesystem::path> nodes;
  for (const auto& e : std::filesystem::directory_iterator("/dev", ec)) {
    const auto f = e.path().filename().string();
    if (f.size() > 5 && f.starts_with("video") && std::ranges::all_of(f.substr(5), [](unsigned char c) { return std::isdigit(c); })) {
      nodes.push_back(e.path());
    }
  }
  std::ranges::sort(nodes, [](const auto& a, const auto& b) {
    return std::stoi(a.filename().string().substr(5)) < std::stoi(b.filename().string().substr(5));
  });
  for (const auto& path : nodes) {
    Fd fd(::open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC));
    if (fd.fd < 0) continue;  // busy or no permission: skipped (probe reports what it can open)
    v4l2_capability cap{};
    if (!isCaptureDevice(fd.fd, cap)) continue;
    auto modes = listModes(fd.fd);
    if (modes.empty()) continue;  // metadata nodes and unsupported formats
    out.push_back({path.string(), reinterpret_cast<const char*>(cap.card), reinterpret_cast<const char*>(cap.bus_info), std::move(modes)});
  }
  return out;
}

struct CameraSource::Impl {
  CameraSourceConfig config;
  int fd = -1;
  std::uint32_t pixfmt = 0;
  int width = 0, height = 0;
  double cameraFps = 0;
  struct Mapped {
    void* data = MAP_FAILED;
    std::size_t length = 0;
  };
  std::vector<Mapped> buffers;
  std::unique_ptr<FramePool> pool;
  bool streaming = false;
  std::atomic<std::uint64_t> captured{0}, published{0}, dropped{0};
  std::atomic<bool> lost{false};
  std::uint64_t nextId = 0;
  std::int64_t firstNs = -1, nextEmitNs = 0;
  std::jthread worker;

  ~Impl() {
    unmapAll();
    if (fd >= 0) ::close(fd);
  }
  void unmapAll() {
    for (auto& b : buffers) if (b.data != MAP_FAILED) munmap(b.data, b.length);
    buffers.clear();
  }
};

std::expected<std::unique_ptr<CameraSource>, Error> CameraSource::open(std::string name, CameraSourceConfig config) {
  if (config.width <= 0 || config.height <= 0 || config.width % 2 || config.height % 2 || config.fps <= 0 || config.poolSize == 0) {
    return fail(ErrorCode::InvalidArgument, name + ": invalid camera config");
  }
  auto impl = std::make_unique<Impl>();
  impl->config = config;
  impl->fd = ::open(config.id.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
  if (impl->fd < 0) return fail(ErrorCode::DeviceLost, name + ": cannot open " + config.id + ": " + std::strerror(errno));
  v4l2_capability cap{};
  if (!isCaptureDevice(impl->fd, cap)) return fail(ErrorCode::Unsupported, name + ": not a streaming capture device");

  // Pick a mode: the size closest to the request (area difference), then a format that reaches the requested rate,
  // preferring raw over MJPEG (kSupported order).
  const auto modes = listModes(impl->fd);
  if (modes.empty()) return fail(ErrorCode::Unsupported, name + ": no supported pixel format");
  const auto area = [&](const CameraMode& m) { return std::abs(m.width * m.height - config.width * config.height); };
  const double wantAspect = static_cast<double>(config.width) / config.height;
  const auto aspectOff = [&](const CameraMode& m) { return std::abs(static_cast<double>(m.width) / m.height - wantAspect); };
  const auto bestArea = area(*std::ranges::min_element(modes, {}, area));
  const CameraMode* chosen = nullptr;
  double chosenFps = 0;
  for (auto f : kSupported) {
    for (const auto& m : modes) {
      if (area(m) != bestArea || fourccFromString(m.fourcc) != f) continue;
      const double maxFps = m.fps.empty() ? 0 : *std::ranges::max_element(m.fps);
      const bool betterShape = chosen && aspectOff(m) < aspectOff(*chosen) && maxFps >= std::min<double>(chosenFps, config.fps);
      if (!chosen || betterShape || (chosenFps < config.fps && maxFps > chosenFps)) {
        chosen = &m;
        chosenFps = maxFps;
      }
    }
    if (chosen && chosenFps >= config.fps) break;  // first format (in preference order) that is fast enough wins
  }

  v4l2_format fmt{};
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  fmt.fmt.pix.width = static_cast<std::uint32_t>(chosen->width);
  fmt.fmt.pix.height = static_cast<std::uint32_t>(chosen->height);
  fmt.fmt.pix.pixelformat = fourccFromString(chosen->fourcc);
  fmt.fmt.pix.field = V4L2_FIELD_NONE;
  if (xioctl(impl->fd, VIDIOC_S_FMT, &fmt) != 0) {
    // EBUSY: another process is streaming from this camera.
    return fail(errno == EBUSY ? ErrorCode::ResourceExhausted : ErrorCode::Unsupported, name + ": S_FMT failed: " + std::strerror(errno));
  }
  impl->pixfmt = fmt.fmt.pix.pixelformat;
  impl->width = static_cast<int>(fmt.fmt.pix.width);
  impl->height = static_cast<int>(fmt.fmt.pix.height);
  if (!libyuvFourcc(impl->pixfmt) || impl->width % 2 || impl->height % 2) {
    return fail(ErrorCode::Unsupported, name + ": driver chose unsupported format " + fourccString(impl->pixfmt));
  }
  // ConvertToI420 assumes tightly packed rows. shortcut: padded raw layouts are rejected; pass strides to the
  // per-format libyuv calls if a camera needs them.
  const std::uint32_t packed = impl->pixfmt == V4L2_PIX_FMT_YUYV || impl->pixfmt == V4L2_PIX_FMT_UYVY ? fmt.fmt.pix.width * 2
                               : impl->pixfmt == V4L2_PIX_FMT_MJPEG                                  ? 0
                                                                                                    : fmt.fmt.pix.width;
  if (packed && fmt.fmt.pix.bytesperline && fmt.fmt.pix.bytesperline != packed) {
    return fail(ErrorCode::Unsupported, name + ": padded rows (bytesperline " + std::to_string(fmt.fmt.pix.bytesperline) + ") not supported");
  }

  // Lowest offered rate that still reaches the request: less USB bandwidth and work than running at max.
  double rate = chosenFps;
  for (double f : chosen->fps) if (f >= config.fps && f < rate) rate = f;
  v4l2_streamparm parm{};
  parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  parm.parm.capture.timeperframe = {1000, static_cast<std::uint32_t>(std::lround(rate * 1000))};
  if (rate > 0 && xioctl(impl->fd, VIDIOC_S_PARM, &parm) == 0 && parm.parm.capture.timeperframe.numerator) {
    impl->cameraFps = static_cast<double>(parm.parm.capture.timeperframe.denominator) / parm.parm.capture.timeperframe.numerator;
  } else {
    impl->cameraFps = rate;
  }

  v4l2_requestbuffers req{};
  req.count = 4;
  req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  req.memory = V4L2_MEMORY_MMAP;
  if (xioctl(impl->fd, VIDIOC_REQBUFS, &req) != 0 || req.count < 2) {
    return fail(ErrorCode::ResourceExhausted, name + ": REQBUFS failed (camera busy?): " + std::strerror(errno));
  }
  for (std::uint32_t i = 0; i < req.count; ++i) {
    v4l2_buffer b{};
    b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    b.memory = V4L2_MEMORY_MMAP;
    b.index = i;
    if (xioctl(impl->fd, VIDIOC_QUERYBUF, &b) != 0) return fail(ErrorCode::Internal, name + ": QUERYBUF failed");
    void* p = mmap(nullptr, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, impl->fd, b.m.offset);
    if (p == MAP_FAILED) return fail(ErrorCode::ResourceExhausted, name + ": mmap failed");
    impl->buffers.push_back({p, b.length});
  }
  impl->pool = std::make_unique<FramePool>(static_cast<std::size_t>(impl->width) * static_cast<std::size_t>(impl->height) * 3 / 2,
                                           config.poolSize);
  return std::unique_ptr<CameraSource>(new CameraSource(std::move(name), std::move(impl)));
}

namespace {
// What downstream actually gets: the request, or less if the camera cannot go that fast.
int effectiveFps(const CameraSource::Impl& d) {
  return d.cameraFps > 0 ? std::min(d.config.fps, std::max(1, static_cast<int>(std::floor(d.cameraFps + 0.01)))) : d.config.fps;
}
}  // namespace

CameraSource::CameraSource(std::string name, std::unique_ptr<Impl> impl) : Node(std::move(name)), impl_(std::move(impl)) {}

CameraSource::~CameraSource() { stop(); }

std::optional<VideoFormat> CameraSource::outputFormat() const {
  return VideoFormat{.kind = MediaKind::RawVideo, .pixelFormat = PixelFormat::I420, .width = impl_->width,
                     .height = impl_->height, .fps = effectiveFps(*impl_)};
}

std::string CameraSource::negotiated() const {
  return fourccString(impl_->pixfmt) + " " + std::to_string(impl_->width) + "x" + std::to_string(impl_->height) + " @" +
         std::to_string(static_cast<int>(std::lround(impl_->cameraFps))) + " fps (emitting " + std::to_string(effectiveFps(*impl_)) + ")";
}

CameraStats CameraSource::stats() const {
  return {impl_->captured.load(), impl_->published.load(), impl_->dropped.load(), impl_->lost.load()};
}

Result CameraSource::start() {
  auto& d = *impl_;
  for (std::uint32_t i = 0; i < d.buffers.size(); ++i) {
    v4l2_buffer b{};
    b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    b.memory = V4L2_MEMORY_MMAP;
    b.index = i;
    if (xioctl(d.fd, VIDIOC_QBUF, &b) != 0) return fail(ErrorCode::Internal, name() + ": QBUF failed");
  }
  v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (xioctl(d.fd, VIDIOC_STREAMON, &type) != 0) return fail(ErrorCode::DeviceLost, name() + ": STREAMON failed: " + std::strerror(errno));
  d.streaming = true;
  d.worker = std::jthread([this](std::stop_token st) {
    auto& d = *impl_;
    const std::int64_t periodNs = 1'000'000'000LL / effectiveFps(d);
    const auto w = static_cast<std::size_t>(d.width), h = static_cast<std::size_t>(d.height);
    while (!st.stop_requested()) {
      pollfd p{d.fd, POLLIN, 0};
      const int r = ::poll(&p, 1, 100);  // 100 ms: bounded stop latency without an extra wake fd
      if (r == 0 || (r < 0 && errno == EINTR)) continue;
      if (r < 0 || (p.revents & (POLLERR | POLLHUP | POLLNVAL))) {
        d.lost = true;
        spdlog::error("{}: camera lost (poll)", name());
        return;
      }
      v4l2_buffer b{};
      b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      b.memory = V4L2_MEMORY_MMAP;
      if (xioctl(d.fd, VIDIOC_DQBUF, &b) != 0) {
        if (errno == EAGAIN) continue;
        d.lost = true;
        spdlog::error("{}: camera lost: {}", name(), std::strerror(errno));
        return;
      }
      ++d.captured;
      const std::int64_t ts = (b.flags & V4L2_BUF_FLAG_TIMESTAMP_MASK) == V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC
                                  ? static_cast<std::int64_t>(b.timestamp.tv_sec) * 1'000'000'000 + b.timestamp.tv_usec * 1000
                                  : std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
      if (d.firstNs < 0) d.firstNs = d.nextEmitNs = ts;
      // Evenly drop camera frames above the target rate; never fall behind by more than one period.
      const bool emit = ts >= d.nextEmitNs - periodNs / 4 && !(b.flags & V4L2_BUF_FLAG_ERROR);
      if (emit) {
        d.nextEmitNs = std::max(d.nextEmitNs + periodNs, ts);
        auto buf = d.pool->acquire();
        int rc = -1;
        if (buf) {
          auto* y = reinterpret_cast<std::uint8_t*>(buf->data().data());
          rc = libyuv::ConvertToI420(static_cast<const std::uint8_t*>(d.buffers[b.index].data), b.bytesused, y, d.width,
                                     y + w * h, d.width / 2, y + w * h + w * h / 4, d.width / 2, 0, 0, d.width, d.height,
                                     d.width, d.height, libyuv::kRotate0, libyuvFourcc(d.pixfmt));
        }
        if (rc == 0) {
          const VideoFrame frame{.frameId = d.nextId++, .sourceId = d.config.sourceId, .width = d.width, .height = d.height,
                                 .pixelFormat = PixelFormat::I420, .captureTimestampNs = ts,
                                 .presentationTimestampNs = ts - d.firstNs, .buffer = std::move(buf)};
          ++d.published;
          output_.publish(frame);
        } else {
          ++d.dropped;  // pool exhausted (consumers slow) or corrupt MJPEG frame
        }
      }
      if (xioctl(d.fd, VIDIOC_QBUF, &b) != 0) {
        d.lost = true;
        spdlog::error("{}: camera lost (QBUF): {}", name(), std::strerror(errno));
        return;
      }
    }
  });
  spdlog::info("{}: {} via {}", name(), negotiated(), impl_->config.id);
  return {};
}

void CameraSource::stop() noexcept {
  if (!impl_) return;
  impl_->worker = {};  // request stop + join
  if (impl_->streaming) {
    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    xioctl(impl_->fd, VIDIOC_STREAMOFF, &type);
    impl_->streaming = false;
  }
}

}  // namespace eme
