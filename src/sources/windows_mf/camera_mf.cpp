// Media Foundation backend for eme::CameraSource (Windows). Mirrors src/sources/v4l2/camera_v4l2.cpp.
//
// Threads: MF delivers samples on its own work-queue thread (OnReadSample). That callback only converts into a pool
// buffer and publishes (both non-blocking), then asks for the next sample. Device loss is flagged from the callback;
// the CameraSource worker thread tears the reader down and rediscovers the camera.
#include <windows.h>
#include <objbase.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <spdlog/spdlog.h>
#include <wil/com.h>
#include <wil/resource.h>
#include <wil/result_macros.h>
#include <wrl/client.h>
#include <wrl/implements.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <format>
#include <functional>
#include <libyuv.h>
#include <mutex>
#include <optional>
#include <thread>

#include "../camera_common.hpp"
#include "../../platform/windows_mf/mf_platform.hpp"
#include "eme/core/frame_pool.hpp"

namespace eme {
namespace {

using camera_detail::claims;
using mf::hr;
using mf::platform;
using mf::utf8;
using wil::com_ptr_nothrow;

// MF subtypes libyuv::ConvertToI420 handles; for these the GUID's Data1 is the FOURCC libyuv expects.
// Order = preference when the size/rate fit (PLAN §5.2: raw before MJPEG).
constexpr std::string_view kPreference[] = {"NV12", "I420", "YUY2", "UYVY", "MJPG"};

std::string fourccString(std::uint32_t f) {
  std::string s(4, ' ');
  for (int i = 0; i < 4; ++i) s[static_cast<std::size_t>(i)] = static_cast<char>((f >> (8 * i)) & 0xff);
  return s;
}

bool supported(const GUID& subtype) {
  const GUID base = MFVideoFormat_Base;  // FOURCC subtypes are {fourcc-0000-0010-8000-00AA00389B71}
  if (std::memcmp(&subtype.Data2, &base.Data2, sizeof(GUID) - sizeof(subtype.Data1)) != 0) return false;
  return std::ranges::find(kPreference, std::string_view{fourccString(subtype.Data1)}) != std::end(kPreference);
}

std::string lower(std::string s) {
  std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

struct Device {
  std::string id, name, bus;
  com_ptr_nothrow<IMFActivate> activate;
};

// Device instance path from the symbolic link (\\?\usb#vid_..&pid_..#<instance>#{interface} -> usb\vid_..&pid_..\<instance>).
// Like the Linux USB path it identifies the physical port (or the serial number, when the camera has one).
std::string instanceFromLink(std::string link) {
  if (link.starts_with("\\\\?\\")) link.erase(0, 4);
  if (const auto g = link.rfind("#{"); g != std::string::npos) link.erase(g);
  std::ranges::replace(link, '#', '\\');
  return lower(link);
}

// Cheap listing (no device is opened): what reconnect scans every interval.
std::vector<Device> listDevices() {
  std::vector<Device> out;
  if (FAILED(platform())) return out;
  com_ptr_nothrow<IMFAttributes> attrs;
  if (FAILED(MFCreateAttributes(attrs.put(), 1)) ||
      FAILED(attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID))) {
    return out;
  }
  IMFActivate** raw = nullptr;
  UINT32 count = 0;
  if (FAILED(MFEnumDeviceSources(attrs.get(), &raw, &count))) return out;
  wil::unique_cotaskmem_ptr<IMFActivate*> array(raw);
  std::vector<com_ptr_nothrow<IMFActivate>> activates(count);
  for (UINT32 i = 0; i < count; ++i) activates[i].attach(raw[i]);  // owned from here on
  for (auto& a : activates) {
    wil::unique_cotaskmem_string link, name;
    UINT32 len = 0;
    if (FAILED(a->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, link.put(), &len))) continue;
    if (FAILED(a->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, name.put(), &len))) continue;
    const std::string id = utf8(link.get());
    out.push_back({id, utf8(name.get()), instanceFromLink(id), std::move(a)});
  }
  return out;
}

struct NativeType {
  GUID subtype{};
  UINT32 width = 0, height = 0, rateNum = 0, rateDen = 0;
  double fps() const { return rateDen ? static_cast<double>(rateNum) / rateDen : 0; }
};

std::optional<NativeType> describe(IMFMediaType* t) {
  NativeType n;
  if (FAILED(t->GetGUID(MF_MT_SUBTYPE, &n.subtype)) || !supported(n.subtype)) return std::nullopt;
  if (FAILED(MFGetAttributeSize(t, MF_MT_FRAME_SIZE, &n.width, &n.height)) || !n.width || !n.height) return std::nullopt;
  // shortcut: only the nominal MF_MT_FRAME_RATE is listed, not MF_MT_FRAME_RATE_RANGE_*; webcams expose one type per rate.
  if (FAILED(MFGetAttributeRatio(t, MF_MT_FRAME_RATE, &n.rateNum, &n.rateDen))) n.rateNum = n.rateDen = 0;
  return n;
}

// Supported native types of the first video stream, grouped into modes like V4L2 reports them.
std::vector<CameraMode> listModes(IMFSourceReader* reader) {
  std::vector<CameraMode> modes;
  for (DWORD i = 0;; ++i) {
    com_ptr_nothrow<IMFMediaType> t;
    if (FAILED(reader->GetNativeMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), i, t.put()))) break;
    const auto n = describe(t.get());
    if (!n) continue;
    const std::string fourcc = fourccString(n->subtype.Data1);
    const int w = static_cast<int>(n->width), h = static_cast<int>(n->height);
    auto it = std::ranges::find_if(modes, [&](const CameraMode& m) { return m.fourcc == fourcc && m.width == w && m.height == h; });
    if (it == modes.end()) it = modes.insert(modes.end(), CameraMode{fourcc, w, h, {}});
    if (n->fps() > 0 && std::ranges::find(it->fps, n->fps()) == it->fps.end()) it->fps.push_back(n->fps());
  }
  for (auto& m : modes) std::ranges::sort(m.fps, std::greater<>());
  return modes;
}

// A source reader without a callback, only for reading the native types (enumeration and the initial probe).
com_ptr_nothrow<IMFSourceReader> probeReader(IMFMediaSource* source) {
  com_ptr_nothrow<IMFSourceReader> reader;
  if (FAILED(MFCreateSourceReaderFromMediaSource(source, nullptr, reader.put()))) reader.reset();
  return reader;
}

ErrorCode classify(HRESULT h) {
  switch (h) {
    case E_ACCESSDENIED:  // camera privacy setting, or another app holds it exclusively
    case MF_E_VIDEO_RECORDING_DEVICE_PREEMPTED:
    case MF_E_HW_MFT_FAILED_START_STREAMING:
      return ErrorCode::ResourceExhausted;
    default:
      return ErrorCode::DeviceLost;
  }
}

}  // namespace

std::vector<CameraInfo> enumerateCameras() {
  std::vector<CameraInfo> out;
  for (auto& d : listDevices()) {
    com_ptr_nothrow<IMFMediaSource> source;
    if (FAILED(d.activate->ActivateObject(IID_PPV_ARGS(source.put())))) continue;  // in use or no permission: skipped
    std::vector<CameraMode> modes;
    if (auto reader = probeReader(source.get())) modes = listModes(reader.get());
    d.activate->ShutdownObject();
    if (!modes.empty()) out.push_back({d.id, d.name, d.bus, std::move(modes)});
  }
  return out;
}

class ReaderCallback;

struct CameraSource::Impl {
  CameraSourceConfig config;
  std::string sourceName;
  // Identity used to find the camera again after unplug/replug: the device instance (port, or serial) is stable for
  // the same port, the friendly name is the fallback when it was moved to another port.
  std::string bus, card, id, lastId;  // id: symbolic link currently open; lastId: before the loss (tried first)
  NativeType type;                     // fixed at the first open
  std::size_t minBytes = 0;            // raw formats: a smaller buffer would make libyuv read past its end
  double cameraFps = 0;
  Distributor<VideoFrame>* output = nullptr;

  com_ptr_nothrow<IMFMediaSource> source;
  com_ptr_nothrow<IMFSourceReader> reader;
  Microsoft::WRL::ComPtr<ReaderCallback> callback;
  bool streaming = false;

  std::unique_ptr<FramePool> pool;
  std::atomic<std::uint64_t> captured{0}, published{0}, dropped{0}, reconnects{0};
  std::atomic<bool> lost{false}, injectLoss{false}, failed{false};  // failed: set by the callback, handled by run()
  std::mutex wakeMutex;
  std::condition_variable_any wake;

  // Touched only by the callback thread (MF serialises callbacks: one ReadSample is in flight at a time).
  std::uint64_t nextId = 0;
  std::int64_t firstNs = -1, nextEmitNs = 0, lastTs = -1, anchorNs = 0, lastMfNs = -1;
  bool anchored = false;
  std::int64_t periodNs = 0;

  std::jthread worker;

  ~Impl() { closeDevice(); }

  bool isOpen() const { return reader != nullptr; }
  Result openDevice(const Device& dev, bool initial);
  Result startStreaming();
  void closeDevice();
  Result closeWith(Result r) {
    closeDevice();
    return r;
  }
  void onSample(HRESULT status, DWORD flags, LONGLONG time, IMFSample* sample);
  void flagFailure(const std::string& why) {
    if (failed.exchange(true)) return;
    spdlog::warn("{}: camera error ({})", sourceName, why);
    wake.notify_all();  // no lock: run() also wakes every 100 ms, so a missed notify only delays recovery
  }
};

// Async source-reader callback. MF may still deliver a callback after the reader is released, so the callback keeps
// its own (ref-counted) state and only reaches the CameraSource while attached. Callbacks never wait: they announce
// themselves in busy_ and check owner_; detach() clears owner_ and waits for announced callbacks to leave (seq_cst
// makes the two orders exclusive). OnEvent and OnReadSample may run at the same time on different MF threads.
class ReaderCallback final
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IMFSourceReaderCallback> {
 public:
  explicit ReaderCallback(CameraSource::Impl* owner) : owner_(owner) {}

  // After this returns no callback touches the owner. Called by the teardown thread only.
  void detach() {
    owner_ = nullptr;
    for (int b = busy_.load(); b != 0; b = busy_.load()) busy_.wait(b);
  }

  STDMETHODIMP OnReadSample(HRESULT status, DWORD, DWORD flags, LONGLONG time, IMFSample* sample) override {
    withOwner([&](CameraSource::Impl& o) { o.onSample(status, flags, time, sample); });
    return S_OK;
  }
  STDMETHODIMP OnEvent(DWORD, IMFMediaEvent* event) override {
    MediaEventType type = MEUnknown;
    HRESULT status = S_OK;
    if (!event || FAILED(event->GetType(&type))) return S_OK;
    event->GetStatus(&status);
    if (type == MEVideoCaptureDeviceRemoved || type == MEVideoCaptureDevicePreempted || type == MEError) {
      withOwner([&](CameraSource::Impl& o) {
        o.flagFailure(type == MEVideoCaptureDeviceRemoved ? "device removed" : type == MEError ? "error " + hr(status) : "preempted");
      });
    }
    return S_OK;
  }
  STDMETHODIMP OnFlush(DWORD) override { return S_OK; }

 private:
  template <class F>
  void withOwner(F&& f) {
    ++busy_;
    if (auto* o = owner_.load()) f(*o);
    if (--busy_ == 0) busy_.notify_all();
  }
  std::atomic<CameraSource::Impl*> owner_;
  std::atomic<int> busy_{0};
};

// Opens `dev` with the format chosen at first open (`type`). initial=false requires exactly that native type, so
// downstream (encoder, viewers) never sees a change across a reconnect.
Result CameraSource::Impl::openDevice(const Device& dev, bool initial) {
  const std::string key = lower(dev.id);
  if (!claims().claim(key)) return fail(ErrorCode::ResourceExhausted, sourceName + ": " + dev.name + " is used by another source");
  id = key;  // claimed; released by closeDevice()
  if (const HRESULT h = dev.activate->ActivateObject(IID_PPV_ARGS(source.put())); FAILED(h)) {
    return closeWith(fail(classify(h), sourceName + ": cannot open " + dev.name + ": " + hr(h)));
  }
  callback = Microsoft::WRL::Make<ReaderCallback>(this);
  com_ptr_nothrow<IMFAttributes> attrs;
  HRESULT h = MFCreateAttributes(attrs.put(), 2);
  if (SUCCEEDED(h)) h = attrs->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, callback.Get());
  if (SUCCEEDED(h)) h = attrs->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, TRUE);  // native frames: libyuv converts
  if (SUCCEEDED(h)) h = MFCreateSourceReaderFromMediaSource(source.get(), attrs.get(), reader.put());
  if (FAILED(h)) return closeWith(fail(classify(h), sourceName + ": cannot create source reader: " + hr(h)));

  constexpr auto stream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
  com_ptr_nothrow<IMFMediaType> match;
  for (DWORD i = 0; !match; ++i) {
    com_ptr_nothrow<IMFMediaType> t;
    if (FAILED(reader->GetNativeMediaType(stream, i, t.put()))) break;
    const auto n = describe(t.get());
    if (n && n->subtype == type.subtype && n->width == type.width && n->height == type.height &&
        (initial ? std::abs(n->fps() - cameraFps) < 0.01 : n->rateNum == type.rateNum && n->rateDen == type.rateDen)) {
      match = t;
      if (initial) {  // fixed afterwards: other threads read these via outputFormat()/negotiated()
        type = *n;
        const std::size_t px = std::size_t{n->width} * n->height;
        const auto f = fourccString(n->subtype.Data1);
        minBytes = f == "MJPG" ? 1 : f == "YUY2" || f == "UYVY" ? px * 2 : px * 3 / 2;
      }
    }
  }
  if (!match) {
    return closeWith(fail(ErrorCode::Unsupported, sourceName + (initial ? ": chosen mode not offered" : ": camera came back with a different format")));
  }
  if (h = reader->SetCurrentMediaType(stream, nullptr, match.get()); FAILED(h)) {
    return closeWith(fail(classify(h), sourceName + ": SetCurrentMediaType failed: " + hr(h)));
  }
  if (!initial && dev.name != card) return closeWith(fail(ErrorCode::Unsupported, sourceName + ": a different camera"));
  bus = dev.bus;
  card = dev.name;
  return {};
}

Result CameraSource::Impl::startStreaming() {
  anchored = false;  // MF sample times restart from zero on every new reader
  failed = false;
  streaming = true;
  if (const HRESULT h = reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, nullptr, nullptr, nullptr, nullptr);
      FAILED(h)) {
    streaming = false;
    return fail(classify(h), sourceName + ": ReadSample failed: " + hr(h));
  }
  return {};
}

void CameraSource::Impl::closeDevice() {
  if (callback) callback->detach();  // waits for a callback in progress; later ones are ignored
  callback.Reset();  // MF keeps its own reference while a late callback is pending
  reader.reset();
  if (source) source->Shutdown();  // releases the camera; required for device sources
  source.reset();
  streaming = false;
  if (!id.empty()) claims().release(id);
  id.clear();
}

void CameraSource::Impl::onSample(HRESULT status, DWORD flags, LONGLONG time, IMFSample* sample) {
  // A type change mid-stream would feed libyuv the wrong layout: reopen, which insists on the original type.
  constexpr DWORD kFatal = MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED |
                           MF_SOURCE_READERF_NATIVEMEDIATYPECHANGED;
  if (FAILED(status) || (flags & kFatal)) {
    flagFailure(FAILED(status) ? "read " + hr(status) : (flags & MF_SOURCE_READERF_ENDOFSTREAM) ? "stream ended" : "format changed");
    return;  // no further reads: run() reopens
  }
  if (sample) {
    ++captured;
    // MF sample time is in 100 ns units relative to the start of this reader: anchor it on steady_clock once.
    const std::int64_t mfNs = time * 100;
    const std::int64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (!anchored) {
      anchorNs = nowNs - mfNs;
      anchored = true;
      lastMfNs = -1;
    }
    // Some drivers report zero or frozen sample times: then use arrival time, or decimation would drop everything.
    const std::int64_t raw = mfNs > lastMfNs ? anchorNs + mfNs : nowNs;
    lastMfNs = std::max(lastMfNs, mfNs);
    const std::int64_t ts = std::max(raw, lastTs + 1);  // strictly increasing across reconnects
    lastTs = ts;
    if (firstNs < 0) firstNs = nextEmitNs = ts;
    // Evenly drop camera frames above the target rate; after a gap (e.g. reconnect) resume immediately.
    if (ts >= nextEmitNs - periodNs / 4) {
      nextEmitNs = std::max(nextEmitNs + periodNs, ts);
      auto buf = pool->acquire();
      com_ptr_nothrow<IMFMediaBuffer> mb;
      BYTE* data = nullptr;
      DWORD len = 0;
      int rc = -1;
      // Lock() gives the contiguous layout (rows packed, as ConvertToI420 assumes) even for 2-D buffers.
      if (buf && SUCCEEDED(sample->ConvertToContiguousBuffer(mb.put())) && SUCCEEDED(mb->Lock(&data, nullptr, &len))) {
        if (len >= minBytes) {
        const auto w = static_cast<std::size_t>(type.width), h = static_cast<std::size_t>(type.height);
        const int iw = static_cast<int>(type.width), ih = static_cast<int>(type.height);
        auto* y = reinterpret_cast<std::uint8_t*>(buf->data().data());
        rc = libyuv::ConvertToI420(data, len, y, iw, y + w * h, iw / 2, y + w * h + w * h / 4, iw / 2, 0, 0, iw, ih, iw, ih,
                                   libyuv::kRotate0, type.subtype.Data1);
        }
        mb->Unlock();
      }
      if (rc == 0) {
        const VideoFrame frame{.frameId = nextId++, .sourceId = config.sourceId, .width = static_cast<int>(type.width),
                               .height = static_cast<int>(type.height), .pixelFormat = PixelFormat::I420,
                               .captureTimestampNs = ts, .presentationTimestampNs = ts - firstNs, .buffer = std::move(buf)};
        ++published;
        output->publish(frame);
      } else {
        ++dropped;  // pool exhausted (consumers slow), short buffer or corrupt MJPEG frame
      }
    }
  }
  if (const HRESULT h = reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, nullptr, nullptr, nullptr, nullptr);
      FAILED(h)) {
    flagFailure("ReadSample " + hr(h));
  }
}

namespace {
int effectiveFps(const CameraSource::Impl& d) { return camera_detail::effectiveFps(d.config.fps, d.cameraFps); }

std::optional<Device> findDevice(const std::string& id) {
  for (auto& d : listDevices()) if (lower(d.id) == lower(id)) return std::move(d);
  return std::nullopt;
}
}  // namespace

std::expected<std::unique_ptr<CameraSource>, Error> CameraSource::open(std::string name, CameraSourceConfig config) {
  if (!camera_detail::validConfig(config)) return fail(ErrorCode::InvalidArgument, name + ": invalid camera config");
  if (const HRESULT h = platform(); FAILED(h)) return fail(ErrorCode::DeviceLost, name + ": no cameras: Media Foundation unavailable (" + hr(h) + ")");
  if (claims().claimed(lower(config.id))) return fail(ErrorCode::ResourceExhausted, name + ": " + config.id + " is used by another source");
  auto dev = findDevice(config.id);
  if (!dev) return fail(ErrorCode::DeviceLost, name + ": no camera " + config.id);
  std::vector<CameraMode> modes;
  {
    com_ptr_nothrow<IMFMediaSource> probe;
    if (const HRESULT h = dev->activate->ActivateObject(IID_PPV_ARGS(probe.put())); FAILED(h)) {
      return fail(classify(h), name + ": cannot open " + dev->name + ": " + hr(h));
    }
    if (auto reader = probeReader(probe.get())) modes = listModes(reader.get());
    dev->activate->ShutdownObject();
  }
  if (modes.empty()) return fail(ErrorCode::Unsupported, name + ": no supported pixel format");
  const auto [chosen, rate] = camera_detail::chooseMode(modes, kPreference, config);

  auto impl = std::make_unique<Impl>();
  impl->config = config;
  impl->sourceName = name;
  const std::string& f = chosen->fourcc;
  impl->type.subtype = MFVideoFormat_Base;
  impl->type.subtype.Data1 = static_cast<DWORD>(f[0]) | static_cast<DWORD>(f[1]) << 8 | static_cast<DWORD>(f[2]) << 16 |
                             static_cast<DWORD>(f[3]) << 24;
  impl->type.width = static_cast<UINT32>(chosen->width);
  impl->type.height = static_cast<UINT32>(chosen->height);
  impl->cameraFps = rate;
  if (auto r = impl->openDevice(*dev, true); !r) return std::unexpected(r.error());
  impl->pool = std::make_unique<FramePool>(static_cast<std::size_t>(impl->type.width) * impl->type.height * 3 / 2, config.poolSize);
  impl->periodNs = 1'000'000'000LL / effectiveFps(*impl);
  auto src = std::unique_ptr<CameraSource>(new CameraSource(std::move(name), std::move(impl)));
  src->impl_->output = &src->output_;
  return src;
}

CameraSource::CameraSource(std::string name, std::unique_ptr<Impl> impl) : Node(std::move(name)), impl_(std::move(impl)) {}

CameraSource::~CameraSource() { stop(); }

std::optional<VideoFormat> CameraSource::outputFormat() const {
  return VideoFormat{.kind = MediaKind::RawVideo, .pixelFormat = PixelFormat::I420, .width = static_cast<int>(impl_->type.width),
                     .height = static_cast<int>(impl_->type.height), .fps = effectiveFps(*impl_)};
}

std::string CameraSource::negotiated() const {
  return fourccString(impl_->type.subtype.Data1) + " " + std::to_string(impl_->type.width) + "x" + std::to_string(impl_->type.height) +
         " @" + std::to_string(static_cast<int>(std::lround(impl_->cameraFps))) + " fps (emitting " + std::to_string(effectiveFps(*impl_)) + ")";
}

CameraStats CameraSource::stats() const {
  return {impl_->captured.load(), impl_->published.load(), impl_->dropped.load(), impl_->lost.load(), impl_->reconnects.load()};
}

void CameraSource::simulateDeviceLossForTesting() {
  impl_->injectLoss = true;
  impl_->wake.notify_all();
}

Result CameraSource::start() {
  auto& d = *impl_;
  if (!d.isOpen() && !reconnect()) return fail(ErrorCode::DeviceLost, name() + ": camera not available");  // restart after stop()
  if (!d.streaming) {
    if (auto r = d.startStreaming(); !r) return r;
  }
  spdlog::info("{}: {} ({})", name(), negotiated(), d.card);  // before the worker can rewrite card
  d.worker = std::jthread([this](std::stop_token st) { run(st); });
  return {};
}

// Finds the same camera again and resumes streaming with the original format. Candidates: devices with the same
// friendly name not owned by another source; the old symbolic link first. The device instance must match, except
// when the camera is the only one with that name (it was moved to another port).
bool CameraSource::reconnect() {
  auto& d = *impl_;
  auto devices = listDevices();
  std::erase_if(devices, [&](const Device& c) { return c.name != d.card; });
  const bool allowMoved = devices.size() == 1;
  std::erase_if(devices, [](const Device& c) { return claims().claimed(lower(c.id)); });
  std::ranges::stable_partition(devices, [&](const Device& c) { return lower(c.id) == d.lastId; });
  for (const auto& c : devices) {
    if (c.bus != d.bus && !allowMoved) continue;  // same model on another port while ours is still missing: not ours
    if (auto r = d.openDevice(c, false); !r) {
      spdlog::debug("{}: reconnect via {} failed: {}", name(), c.id, r.error().message);
      continue;
    }
    if (auto r = d.startStreaming(); !r) {
      d.closeDevice();
      continue;
    }
    return true;
  }
  return false;
}

void CameraSource::run(std::stop_token st) {
  auto& d = *impl_;
  [[maybe_unused]] auto com = wil::CoInitializeEx_failfast(COINIT_MULTITHREADED);  // this thread creates/releases MF objects
  auto lose = [&](const std::string& why) {
    spdlog::warn("{}: camera lost ({}); will reconnect when it comes back", name(), why);
    d.lastId = d.id;
    d.closeDevice();
    d.lost = true;
  };
  while (!st.stop_requested()) {
    std::unique_lock lock(d.wakeMutex);
    if (!d.isOpen()) {
      if (d.wake.wait_for(lock, st, d.config.reconnectInterval, [] { return false; }) || st.stop_requested()) break;
      lock.unlock();
      if (reconnect()) {
        d.lost = false;
        ++d.reconnects;
        spdlog::info("{}: camera back, streaming resumed", name());
      }
      continue;
    }
    d.wake.wait_for(lock, st, std::chrono::milliseconds(100), [&] { return d.failed.load() || d.injectLoss.load(); });
    lock.unlock();
    if (d.injectLoss.exchange(false)) lose("simulated");
    else if (d.failed) lose("capture error");
  }
}

void CameraSource::stop() noexcept {
  if (!impl_) return;
  impl_->worker = {};  // request stop + join
  if (impl_->isOpen() && impl_->streaming) {
    // A detached callback cannot be re-armed, so stop releases the camera; start() reopens it like a reconnect.
    impl_->lastId = impl_->id;
    impl_->closeDevice();
  }
}

}  // namespace eme
