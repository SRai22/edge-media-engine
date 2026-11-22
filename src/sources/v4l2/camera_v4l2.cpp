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
#include <fstream>
#include <libyuv.h>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "eme/core/frame_pool.hpp"
#include "eme/sources/camera.hpp"
#include "../camera_common.hpp"

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

// Formats libyuv::ConvertToI420 handles that cameras commonly offer (V4L2 fourcc names). Order = preference when the
// size/rate fit (PLAN §5.2: raw before MJPEG; native H.264 passthrough is a later optimisation).
constexpr std::string_view kPreference[] = {"NV12", "YU12", "YUYV", "UYVY", "MJPG"};
using camera_detail::claims;

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

// Cheap rediscovery: sysfs names need no device open. Only nodes whose name equals `card` are candidates.
std::vector<std::string> nodesNamed(const std::string& card) {
  std::vector<std::string> out;
  std::error_code ec;
  for (const auto& e : std::filesystem::directory_iterator("/sys/class/video4linux", ec)) {
    const auto f = e.path().filename().string();
    if (!f.starts_with("video")) continue;
    std::ifstream in(e.path() / "name");
    std::string name;
    std::getline(in, name);
    if (name == card) out.push_back("/dev/" + f);
  }
  std::ranges::sort(out);
  return out;
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
  // Identity used to find the camera again after unplug/replug: the USB path is stable for the same port,
  // the card name is the fallback when it was moved to another port.
  std::string bus, card, path, lastPath;  // lastPath: node before the loss (tried first)
  int fd = -1;
  std::uint32_t pixfmt = 0;
  int width = 0, height = 0;
  double requestRate = 0, cameraFps = 0;
  struct Mapped {
    void* data = MAP_FAILED;
    std::size_t length = 0;
  };
  std::vector<Mapped> buffers;
  std::unique_ptr<FramePool> pool;
  bool streaming = false;
  std::atomic<std::uint64_t> captured{0}, published{0}, dropped{0}, reconnects{0};
  std::atomic<bool> lost{false}, injectLoss{false};
  std::uint64_t nextId = 0;
  std::int64_t firstNs = -1, nextEmitNs = 0;
  std::jthread worker;

  ~Impl() { closeDevice(); }

  // Opens `devicePath` with the format chosen at first open. initial=false requires the driver to give exactly that
  // format back, so downstream (encoder, viewers) never sees a change across a reconnect.
  Result openDevice(const std::string& devicePath, const std::string& name, bool initial) {
    if (!claims().claim(devicePath)) return fail(ErrorCode::ResourceExhausted, name + ": " + devicePath + " is used by another source");
    path = devicePath;  // claimed; released by closeDevice()
    fd = ::open(devicePath.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return closeWith(fail(ErrorCode::DeviceLost, name + ": cannot open " + devicePath + ": " + std::strerror(errno)));
    v4l2_capability cap{};
    if (!isCaptureDevice(fd, cap)) return closeWith(fail(ErrorCode::Unsupported, name + ": not a streaming capture device"));
    v4l2_format fmt{};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = static_cast<std::uint32_t>(width);
    fmt.fmt.pix.height = static_cast<std::uint32_t>(height);
    fmt.fmt.pix.pixelformat = pixfmt;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    if (xioctl(fd, VIDIOC_S_FMT, &fmt) != 0) {
      // EBUSY: another process is streaming from this camera.
      return closeWith(fail(errno == EBUSY ? ErrorCode::ResourceExhausted : ErrorCode::Unsupported,
                            name + ": S_FMT failed: " + std::strerror(errno)));
    }
    const int w = static_cast<int>(fmt.fmt.pix.width), h = static_cast<int>(fmt.fmt.pix.height);
    if (!initial && (fmt.fmt.pix.pixelformat != pixfmt || w != width || h != height)) {
      return closeWith(fail(ErrorCode::Unsupported, name + ": camera came back with a different format"));
    }
    if (!libyuvFourcc(fmt.fmt.pix.pixelformat) || w % 2 || h % 2) {
      return closeWith(fail(ErrorCode::Unsupported, name + ": driver chose unsupported format " + fourccString(fmt.fmt.pix.pixelformat)));
    }
    // ConvertToI420 assumes tightly packed rows. shortcut: padded raw layouts are rejected; pass strides to the
    // per-format libyuv calls if a camera needs them.
    const auto pf = fmt.fmt.pix.pixelformat;
    const std::uint32_t packed = pf == V4L2_PIX_FMT_YUYV || pf == V4L2_PIX_FMT_UYVY ? fmt.fmt.pix.width * 2
                                 : pf == V4L2_PIX_FMT_MJPEG                       ? 0
                                                                                  : fmt.fmt.pix.width;
    if (packed && fmt.fmt.pix.bytesperline && fmt.fmt.pix.bytesperline != packed) {
      return closeWith(fail(ErrorCode::Unsupported, name + ": padded rows (bytesperline " + std::to_string(fmt.fmt.pix.bytesperline) + ") not supported"));
    }
    if (initial) {  // fixed afterwards: other threads read these via outputFormat()/negotiated()
      pixfmt = pf;
      width = w;
      height = h;
    }

    v4l2_streamparm parm{};
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe = {1000, static_cast<std::uint32_t>(std::lround(requestRate * 1000))};
    if (requestRate > 0 && xioctl(fd, VIDIOC_S_PARM, &parm) == 0 && parm.parm.capture.timeperframe.numerator) {
      const double got = static_cast<double>(parm.parm.capture.timeperframe.denominator) / parm.parm.capture.timeperframe.numerator;
      if (initial) cameraFps = got;
    } else if (initial) {
      cameraFps = requestRate;
    }

    v4l2_requestbuffers req{};
    req.count = 4;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd, VIDIOC_REQBUFS, &req) != 0 || req.count < 2) {
      return closeWith(fail(ErrorCode::ResourceExhausted, name + ": REQBUFS failed (camera busy?): " + std::strerror(errno)));
    }
    for (std::uint32_t i = 0; i < req.count; ++i) {
      v4l2_buffer b{};
      b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      b.memory = V4L2_MEMORY_MMAP;
      b.index = i;
      if (xioctl(fd, VIDIOC_QUERYBUF, &b) != 0) return closeWith(fail(ErrorCode::Internal, name + ": QUERYBUF failed"));
      void* p = mmap(nullptr, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, b.m.offset);
      if (p == MAP_FAILED) return closeWith(fail(ErrorCode::ResourceExhausted, name + ": mmap failed"));
      buffers.push_back({p, b.length});
    }
    const std::string gotBus = reinterpret_cast<const char*>(cap.bus_info);
    const std::string gotCard = reinterpret_cast<const char*>(cap.card);
    if (!initial && gotCard != card) return closeWith(fail(ErrorCode::Unsupported, name + ": a different camera is on " + devicePath));
    bus = gotBus;
    card = gotCard;
    return {};
  }

  Result startStreaming(const std::string& name) {
    for (std::uint32_t i = 0; i < buffers.size(); ++i) {
      v4l2_buffer b{};
      b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      b.memory = V4L2_MEMORY_MMAP;
      b.index = i;
      if (xioctl(fd, VIDIOC_QBUF, &b) != 0) return fail(ErrorCode::DeviceLost, name + ": QBUF failed: " + std::strerror(errno));
    }
    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd, VIDIOC_STREAMON, &type) != 0) return fail(ErrorCode::DeviceLost, name + ": STREAMON failed: " + std::strerror(errno));
    streaming = true;
    return {};
  }

  void closeDevice() {
    if (fd >= 0 && streaming) {
      v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      xioctl(fd, VIDIOC_STREAMOFF, &type);
    }
    streaming = false;
    for (auto& b : buffers) if (b.data != MAP_FAILED) munmap(b.data, b.length);
    buffers.clear();
    if (fd >= 0) ::close(fd);
    fd = -1;
    if (!path.empty()) claims().release(path);
    path.clear();
  }

  Result closeWith(Result r) {
    closeDevice();
    return r;
  }
};

namespace {
int effectiveFps(const CameraSource::Impl& d) { return camera_detail::effectiveFps(d.config.fps, d.cameraFps); }
}  // namespace

std::expected<std::unique_ptr<CameraSource>, Error> CameraSource::open(std::string name, CameraSourceConfig config) {
  if (!camera_detail::validConfig(config)) return fail(ErrorCode::InvalidArgument, name + ": invalid camera config");
  std::vector<CameraMode> modes;
  {
    Fd probe(::open(config.id.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC));
    if (probe.fd < 0) return fail(ErrorCode::DeviceLost, name + ": cannot open " + config.id + ": " + std::strerror(errno));
    v4l2_capability cap{};
    if (!isCaptureDevice(probe.fd, cap)) return fail(ErrorCode::Unsupported, name + ": not a streaming capture device");
    modes = listModes(probe.fd);
  }
  if (modes.empty()) return fail(ErrorCode::Unsupported, name + ": no supported pixel format");

  const auto [chosen, rate] = camera_detail::chooseMode(modes, kPreference, config);
  auto impl = std::make_unique<Impl>();
  impl->config = config;
  impl->pixfmt = fourccFromString(chosen->fourcc);
  impl->width = chosen->width;
  impl->height = chosen->height;
  impl->requestRate = rate;
  if (auto r = impl->openDevice(config.id, name, true); !r) return std::unexpected(r.error());
  impl->pool = std::make_unique<FramePool>(static_cast<std::size_t>(impl->width) * static_cast<std::size_t>(impl->height) * 3 / 2,
                                           config.poolSize);
  return std::unique_ptr<CameraSource>(new CameraSource(std::move(name), std::move(impl)));
}

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
  return {impl_->captured.load(), impl_->published.load(), impl_->dropped.load(), impl_->lost.load(), impl_->reconnects.load()};
}

void CameraSource::simulateDeviceLossForTesting() { impl_->injectLoss = true; }

Result CameraSource::start() {
  auto& d = *impl_;
  if (d.fd >= 0 && !d.streaming) {
    if (auto r = d.startStreaming(name()); !r) return r;
  }
  d.worker = std::jthread([this](std::stop_token st) { run(st); });
  spdlog::info("{}: {} via {}", name(), negotiated(), impl_->path);
  return {};
}

// Finds the same camera again and resumes streaming with the original format. Candidates: nodes with the same
// name (from sysfs, no device opens), not owned by another source; the old node first. The USB path must match,
// except when the camera is the only one with that name (it was moved to another port).
bool CameraSource::reconnect() {
  auto& d = *impl_;
  auto candidates = nodesNamed(d.card);
  std::erase_if(candidates, [](const std::string& p) { return claims().claimed(p); });
  std::ranges::stable_partition(candidates, [&](const std::string& p) { return p == d.lastPath; });
  const bool allowMoved = std::ranges::count_if(nodesNamed(d.card), [](const std::string& p) {
                            Fd fd(::open(p.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC));
                            v4l2_capability cap{};
                            return fd.fd >= 0 && isCaptureDevice(fd.fd, cap);
                          }) == 1;
  const std::string wantBus = d.bus;
  for (const auto& p : candidates) {
    if (auto r = d.openDevice(p, name(), false); !r) {
      spdlog::debug("{}: reconnect via {} failed: {}", name(), p, r.error().message);
      continue;
    }
    if (d.bus != wantBus && !allowMoved) {  // same model on another port while ours is still missing: not ours
      d.bus = wantBus;
      d.closeDevice();
      continue;
    }
    if (auto r = d.startStreaming(name()); !r) {
      d.closeDevice();
      continue;
    }
    return true;
  }
  return false;
}

void CameraSource::run(std::stop_token st) {
  auto& d = *impl_;
  const std::int64_t periodNs = 1'000'000'000LL / effectiveFps(d);
  const auto w = static_cast<std::size_t>(d.width), h = static_cast<std::size_t>(d.height);
  std::mutex waitMutex;
  std::condition_variable_any waitCv;
  auto lose = [&](const std::string& why) {
    spdlog::warn("{}: camera lost ({}); will reconnect when it comes back", name(), why);
    d.lastPath = d.path;
    d.closeDevice();
    d.lost = true;
  };
  while (!st.stop_requested()) {
    if (d.fd < 0) {
      std::unique_lock lock(waitMutex);
      if (waitCv.wait_for(lock, st, d.config.reconnectInterval, [] { return false; }) || st.stop_requested()) break;
      lock.unlock();
      if (reconnect()) {
        d.lost = false;
        ++d.reconnects;
        spdlog::info("{}: camera back via {}, streaming resumed", name(), d.path);
      }
      continue;
    }
    if (d.injectLoss.exchange(false)) {
      lose("simulated");
      continue;
    }
    pollfd p{d.fd, POLLIN, 0};
    const int r = ::poll(&p, 1, 100);  // 100 ms: bounded stop latency without an extra wake fd
    if (r == 0 || (r < 0 && errno == EINTR)) continue;
    if (r < 0 || (p.revents & (POLLERR | POLLHUP | POLLNVAL))) {
      lose("poll error");
      continue;
    }
    v4l2_buffer b{};
    b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    b.memory = V4L2_MEMORY_MMAP;
    if (xioctl(d.fd, VIDIOC_DQBUF, &b) != 0) {
      if (errno == EAGAIN) continue;
      lose(std::strerror(errno));
      continue;
    }
    ++d.captured;
    const std::int64_t ts = (b.flags & V4L2_BUF_FLAG_TIMESTAMP_MASK) == V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC
                                ? static_cast<std::int64_t>(b.timestamp.tv_sec) * 1'000'000'000 + b.timestamp.tv_usec * 1000
                                : std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (d.firstNs < 0) d.firstNs = d.nextEmitNs = ts;
    // Evenly drop camera frames above the target rate; after a gap (e.g. reconnect) resume immediately.
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
    if (xioctl(d.fd, VIDIOC_QBUF, &b) != 0) lose(std::string("QBUF: ") + std::strerror(errno));
  }
}

void CameraSource::stop() noexcept {
  if (!impl_) return;
  impl_->worker = {};  // request stop + join
  if (impl_->fd >= 0 && impl_->streaming) {
    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    xioctl(impl_->fd, VIDIOC_STREAMOFF, &type);
    impl_->streaming = false;
  }
}

}  // namespace eme
