// EME-001 / PLAN Milestone 0: camera + hardware-encoder feasibility probe for the Windows target.
//
//   feasibility_probe [--seconds 60] [--size 1280x720] [--fps 15] [--bitrate 1200000] [--out eme001-out] [--no-sw]
//
// Writes <out>/report.json and the H.264 it produced (<out>/*.h264, Annex B) for ffprobe. Phases, each recorded even
// when it fails: system, cameras (every native type, incl. camera-native H.264), H.264 encoder MFTs, two concurrent
// hardware sessions on synthetic frames, then N cameras -> N hardware encoders for --seconds with CPU/GPU/memory
// measured, then the same with OpenH264 as the software baseline.
//
// The MFT session below is a prototype for EME-006, not production code: system-memory NV12 input, polled events.
#include <windows.h>
#include <objbase.h>
#include <codecapi.h>
#include <dxgi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>
#include <strmif.h>
#include <wil/com.h>
#include <wil/resource.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <libyuv.h>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "eme/core/graph.hpp"
#include "eme/processing/openh264_encoder.hpp"
#include "eme/sources/camera.hpp"

using nlohmann::json;
using wil::com_ptr_nothrow;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

namespace {

struct Options {
  int seconds = 60, width = 1280, height = 720, fps = 15;
  std::uint32_t bitrate = 1'200'000;
  std::filesystem::path out = "eme001-out";
  bool sw = true;
};

std::string hr(HRESULT h) { return std::format("0x{:08X}", static_cast<std::uint32_t>(h)); }

std::string utf8(const wchar_t* w) {
  if (!w || !*w) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  std::string s(static_cast<std::size_t>(n > 0 ? n - 1 : 0), '\0');
  if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
  return s;
}

// FOURCC-based subtypes print as their four characters (NV12, MJPG, H264); others (RGB) as the GUID's Data1.
std::string subtypeName(const GUID& g) {
  std::string s;
  for (int i = 0; i < 4; ++i) s += static_cast<char>((g.Data1 >> (8 * i)) & 0xff);
  const bool printable = std::ranges::all_of(s, [](char c) { return c >= 0x20 && c < 0x7f; });
  return printable ? s : std::format("0x{:08X}", static_cast<std::uint32_t>(g.Data1));
}

std::string regString(HKEY root, const wchar_t* key, const wchar_t* value) {
  wchar_t buf[256]{};
  DWORD size = sizeof(buf);
  if (RegGetValueW(root, key, value, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return {};
  return utf8(buf);
}

// ---------------------------------------------------------------- system

json systemInfo() {
  json j;
  const wchar_t* nt = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
  j["os"] = {{"productName", regString(HKEY_LOCAL_MACHINE, nt, L"ProductName")},
             {"editionId", regString(HKEY_LOCAL_MACHINE, nt, L"EditionID")},
             {"displayVersion", regString(HKEY_LOCAL_MACHINE, nt, L"DisplayVersion")},
             {"build", regString(HKEY_LOCAL_MACHINE, nt, L"CurrentBuild")}};
  j["cpu"] = {{"name", regString(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString")},
              {"logicalProcessors", std::thread::hardware_concurrency()}};
  json gpus = json::array();
  com_ptr_nothrow<IDXGIFactory1> factory;
  if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(factory.put())))) {
    com_ptr_nothrow<IDXGIAdapter1> a;
    for (UINT i = 0; factory->EnumAdapters1(i, a.put()) != DXGI_ERROR_NOT_FOUND; ++i) {
      DXGI_ADAPTER_DESC1 d{};
      if (SUCCEEDED(a->GetDesc1(&d))) {
        gpus.push_back({{"description", utf8(d.Description)}, {"vendorId", std::format("0x{:04X}", d.VendorId)},
                        {"deviceId", std::format("0x{:04X}", d.DeviceId)},
                        {"dedicatedVideoMemoryMB", d.DedicatedVideoMemory >> 20},
                        {"software", (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0}});
      }
      a.reset();
    }
  }
  j["gpus"] = gpus;
  return j;
}

// ---------------------------------------------------------------- cameras

// Every native type of every capture device (eme's CameraInfo only lists formats it can convert; this also shows
// camera-native H.264, which decides PLAN §7's "camera H.264 vs NV12 + hardware encode").
json cameraInfo() {
  json out = json::array();
  com_ptr_nothrow<IMFAttributes> attrs;
  if (FAILED(MFCreateAttributes(attrs.put(), 1)) ||
      FAILED(attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID))) {
    return out;
  }
  IMFActivate** raw = nullptr;
  UINT32 count = 0;
  if (FAILED(MFEnumDeviceSources(attrs.get(), &raw, &count))) return out;
  wil::unique_cotaskmem_ptr<IMFActivate*> array(raw);
  std::vector<com_ptr_nothrow<IMFActivate>> devices(count);
  for (UINT32 i = 0; i < count; ++i) devices[i].attach(raw[i]);
  const auto emeCams = eme::enumerateCameras();
  for (auto& dev : devices) {
    wil::unique_cotaskmem_string link, name;
    UINT32 len = 0;
    dev->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, link.put(), &len);
    dev->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, name.put(), &len);
    json cam = {{"name", utf8(name.get())}, {"id", utf8(link.get())}};
    if (auto it = std::ranges::find_if(emeCams, [&](const eme::CameraInfo& c) { return c.id == cam["id"].get<std::string>(); });
        it != emeCams.end()) {
      cam["bus"] = it->bus;
      cam["emeConvertibleModes"] = it->modes.size();
    }
    com_ptr_nothrow<IMFMediaSource> source;
    com_ptr_nothrow<IMFPresentationDescriptor> pd;
    com_ptr_nothrow<IMFStreamDescriptor> sd;
    com_ptr_nothrow<IMFMediaTypeHandler> handler;
    BOOL selected = FALSE;
    DWORD types = 0;
    HRESULT h = dev->ActivateObject(IID_PPV_ARGS(source.put()));
    if (SUCCEEDED(h)) h = source->CreatePresentationDescriptor(pd.put());
    if (SUCCEEDED(h)) h = pd->GetStreamDescriptorByIndex(0, &selected, sd.put());
    if (SUCCEEDED(h)) h = sd->GetMediaTypeHandler(handler.put());
    if (SUCCEEDED(h)) h = handler->GetMediaTypeCount(&types);
    if (FAILED(h)) cam["error"] = "cannot read native types: " + hr(h);
    std::map<std::tuple<std::string, UINT32, UINT32>, std::vector<double>> modes;
    for (DWORD i = 0; SUCCEEDED(h) && i < types; ++i) {
      com_ptr_nothrow<IMFMediaType> t;
      if (FAILED(handler->GetMediaTypeByIndex(i, t.put()))) continue;
      GUID sub{};
      UINT32 w = 0, hgt = 0, num = 0, den = 0;
      t->GetGUID(MF_MT_SUBTYPE, &sub);
      MFGetAttributeSize(t.get(), MF_MT_FRAME_SIZE, &w, &hgt);
      MFGetAttributeRatio(t.get(), MF_MT_FRAME_RATE, &num, &den);
      auto& fps = modes[{subtypeName(sub), w, hgt}];
      const double f = den ? static_cast<double>(num) / den : 0;
      if (std::ranges::find(fps, f) == fps.end()) fps.push_back(f);
    }
    json list = json::array();
    bool nativeH264 = false;
    for (const auto& [k, fps] : modes) {
      const auto& [format, w, hgt] = k;
      nativeH264 |= format == "H264";
      list.push_back({{"format", format}, {"width", w}, {"height", hgt}, {"fps", fps}});
    }
    cam["nativeTypes"] = list;
    cam["nativeH264"] = nativeH264;
    if (source) source->Shutdown();
    dev->ShutdownObject();
    out.push_back(cam);
  }
  return out;
}

// ---------------------------------------------------------------- H.264 encoder MFTs

std::vector<com_ptr_nothrow<IMFActivate>> enumEncoders(bool hardware) {
  MFT_REGISTER_TYPE_INFO in{MFMediaType_Video, MFVideoFormat_NV12}, out{MFMediaType_Video, MFVideoFormat_H264};
  const UINT32 flags = (hardware ? MFT_ENUM_FLAG_HARDWARE : MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT) | MFT_ENUM_FLAG_SORTANDFILTER;
  IMFActivate** raw = nullptr;
  UINT32 count = 0;
  std::vector<com_ptr_nothrow<IMFActivate>> list;
  if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, &in, &out, &raw, &count))) return list;
  wil::unique_cotaskmem_ptr<IMFActivate*> array(raw);
  list.resize(count);
  for (UINT32 i = 0; i < count; ++i) list[i].attach(raw[i]);
  return list;
}

std::string attrString(IMFAttributes* a, const GUID& key) {
  wil::unique_cotaskmem_string s;
  UINT32 len = 0;
  return SUCCEEDED(a->GetAllocatedString(key, s.put(), &len)) ? utf8(s.get()) : std::string();
}

json encoderList(bool hardware) {
  json list = json::array();
  for (auto& a : enumEncoders(hardware)) {
    list.push_back({{"name", attrString(a.get(), MFT_FRIENDLY_NAME_Attribute)},
                    {"hardwareVendorId", attrString(a.get(), MFT_ENUM_HARDWARE_VENDOR_ID_Attribute)},
                    {"hardwareUrl", attrString(a.get(), MFT_ENUM_HARDWARE_URL_Attribute)}});
  }
  return list;
}

struct EncodeStats {
  std::uint64_t framesIn = 0, framesOut = 0, keyframes = 0, bytes = 0;
  double firstOutputMs = -1;
  std::string error;
  json toJson(double seconds) const {
    json j = {{"framesIn", framesIn}, {"framesOut", framesOut}, {"keyframes", keyframes}, {"bytes", bytes},
              {"firstOutputLatencyMs", firstOutputMs}};
    if (seconds > 0) j["kbps"] = static_cast<double>(bytes) * 8 / seconds / 1000;
    if (!error.empty()) j["error"] = error;
    return j;
  }
};

bool hasIdr(const BYTE* p, DWORD n) {
  for (DWORD i = 0; i + 3 < n; ++i) {
    if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1 && (p[i + 3] & 0x1f) == 5) return true;
  }
  return false;
}

// Fills one NV12 frame; returns false at end of input. `time100ns` is the sample time.
using Fill = std::function<bool(BYTE* nv12, LONGLONG& time100ns)>;
using Sink = std::function<void(const BYTE* data, DWORD size, bool keyframe)>;

// Prototype H.264 MFT session (hardware MFTs are async: driven by METransformNeedInput/HaveOutput events).
class MftEncoder {
 public:
  ~MftEncoder() {
    if (mft_) mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    mft_.reset();
    if (activate_) activate_->ShutdownObject();
  }

  // Configures the session and records which settings the MFT accepted (PLAN §9.1: do not assume they are honoured).
  static std::unique_ptr<MftEncoder> open(com_ptr_nothrow<IMFActivate> activate, const Options& o, json& report) {
    auto e = std::unique_ptr<MftEncoder>(new MftEncoder);
    e->activate_ = std::move(activate);
    e->w_ = static_cast<UINT32>(o.width);
    e->h_ = static_cast<UINT32>(o.height);
    e->fps_ = static_cast<UINT32>(o.fps);
    report["name"] = attrString(e->activate_.get(), MFT_FRIENDLY_NAME_Attribute);
    auto failed = [&](const char* step, HRESULT h) {
      report["error"] = std::string(step) + " failed: " + hr(h);
      return nullptr;
    };
    HRESULT h = e->activate_->ActivateObject(IID_PPV_ARGS(e->mft_.put()));
    if (FAILED(h)) return failed("ActivateObject", h);
    com_ptr_nothrow<IMFAttributes> attrs;
    if (SUCCEEDED(e->mft_->GetAttributes(attrs.put())) && MFGetAttributeUINT32(attrs.get(), MF_TRANSFORM_ASYNC, FALSE)) {
      e->async_ = true;
      if (FAILED(h = attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE))) return failed("MF_TRANSFORM_ASYNC_UNLOCK", h);
      e->events_ = e->mft_.try_query<IMFMediaEventGenerator>();
      if (!e->events_) return failed("IMFMediaEventGenerator", E_NOINTERFACE);
    }
    report["async"] = e->async_;
    if (e->mft_->GetStreamIDs(1, &e->in_, 1, &e->out_) == E_NOTIMPL) e->in_ = e->out_ = 0;

    // Codec API before the media types: some encoders only take rate control then.
    json codec = json::object();
    if (auto api = e->mft_.try_query<ICodecAPI>()) {
      auto set = [&](const char* name, const GUID& key, VARIANT v) {
        json r = {{"supported", api->IsSupported(&key) == S_OK}};
        r["setResult"] = hr(api->SetValue(&key, &v));
        VARIANT got;
        VariantInit(&got);
        if (SUCCEEDED(api->GetValue(&key, &got))) {
          r["readBack"] = got.vt == VT_BOOL ? json(got.boolVal != VARIANT_FALSE) : got.vt == VT_UI4 ? json(got.ulVal)
                          : got.vt == VT_UI8 ? json(got.ullVal) : json(std::format("vt={}", static_cast<int>(got.vt)));
        }
        VariantClear(&got);
        codec[name] = r;
      };
      auto u4 = [](ULONG x) { VARIANT v; VariantInit(&v); v.vt = VT_UI4; v.ulVal = x; return v; };
      VARIANT on;
      VariantInit(&on);
      on.vt = VT_BOOL;
      on.boolVal = VARIANT_TRUE;
      set("lowLatency", CODECAPI_AVLowLatencyMode, on);
      set("rateControlCBR", CODECAPI_AVEncCommonRateControlMode, u4(eAVEncCommonRateControlMode_CBR));
      set("meanBitrate", CODECAPI_AVEncCommonMeanBitRate, u4(o.bitrate));
      set("gopSize", CODECAPI_AVEncMPVGOPSize, u4(static_cast<ULONG>(o.fps * 2)));
      set("bFrames", CODECAPI_AVEncMPVDefaultBPictureCount, u4(0));
      codec["forceKeyFrameSupported"] = api->IsSupported(&CODECAPI_AVEncVideoForceKeyFrame) == S_OK;
    } else {
      codec["error"] = "no ICodecAPI";
    }
    report["codecApi"] = codec;

    com_ptr_nothrow<IMFMediaType> outType;
    MFCreateMediaType(outType.put());
    outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    outType->SetUINT32(MF_MT_AVG_BITRATE, o.bitrate);
    MFSetAttributeSize(outType.get(), MF_MT_FRAME_SIZE, e->w_, e->h_);
    MFSetAttributeRatio(outType.get(), MF_MT_FRAME_RATE, e->fps_, 1);
    MFSetAttributeRatio(outType.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    outType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Base);  // most browser-compatible; read back below
    if (FAILED(h = e->mft_->SetOutputType(e->out_, outType.get(), 0))) return failed("SetOutputType(H264 Base)", h);

    bool inputSet = false;
    for (DWORD i = 0; !inputSet; ++i) {
      com_ptr_nothrow<IMFMediaType> t;
      if (FAILED(e->mft_->GetInputAvailableType(e->in_, i, t.put()))) break;
      GUID sub{};
      if (FAILED(t->GetGUID(MF_MT_SUBTYPE, &sub)) || sub != MFVideoFormat_NV12) continue;
      MFSetAttributeSize(t.get(), MF_MT_FRAME_SIZE, e->w_, e->h_);
      MFSetAttributeRatio(t.get(), MF_MT_FRAME_RATE, e->fps_, 1);
      MFSetAttributeRatio(t.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
      t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
      if (FAILED(h = e->mft_->SetInputType(e->in_, t.get(), 0))) return failed("SetInputType(NV12)", h);
      inputSet = true;
    }
    if (!inputSet) return failed("find NV12 input type", MF_E_INVALIDMEDIATYPE);
    com_ptr_nothrow<IMFMediaType> current;
    if (SUCCEEDED(e->mft_->GetOutputCurrentType(e->out_, current.put()))) {
      report["profile"] = MFGetAttributeUINT32(current.get(), MF_MT_MPEG2_PROFILE, 0);  // 66 baseline, 77 main, 100 high
    }
    if (FAILED(h = e->mft_->GetOutputStreamInfo(e->out_, &e->outInfo_))) return failed("GetOutputStreamInfo", h);
    e->mft_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    if (FAILED(h = e->mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0))) return failed("BEGIN_STREAMING", h);
    if (FAILED(h = e->mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0))) return failed("START_OF_STREAM", h);
    return e;
  }

  // Encodes until `fill` returns false, then drains. Runs on the calling thread.
  EncodeStats run(const Fill& fill, const Sink& sink) {
    EncodeStats s;
    start_ = Clock::now();
    bool ended = false;
    auto endInput = [&] {
      ended = true;
      mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, in_);
      mft_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    };
    if (async_) {
      auto lastEvent = Clock::now();
      for (;;) {
        com_ptr_nothrow<IMFMediaEvent> ev;
        const HRESULT h = events_->GetEvent(MF_EVENT_FLAG_NO_WAIT, ev.put());
        if (h == MF_E_NO_EVENTS_AVAILABLE) {
          if (Clock::now() - lastEvent > 5s) return s.error = "MFT stalled: no event for 5 s", s;
          std::this_thread::sleep_for(1ms);  // shortcut: polled; EME-006 uses BeginGetEvent callbacks
          continue;
        }
        if (FAILED(h)) return s.error = "GetEvent " + hr(h), s;
        lastEvent = Clock::now();
        MediaEventType type = MEUnknown;
        ev->GetType(&type);
        if (type == METransformNeedInput && !ended) {
          if (auto r = push(fill, s); r == S_FALSE) endInput();
          else if (FAILED(r)) return s.error = "ProcessInput " + hr(r), s;
        } else if (type == METransformHaveOutput) {
          if (auto r = pull(sink, s); FAILED(r) && r != MF_E_TRANSFORM_NEED_MORE_INPUT) return s.error = "ProcessOutput " + hr(r), s;
        } else if (type == METransformDrainComplete) {
          return s;
        } else if (type == MEError) {
          HRESULT st = S_OK;
          ev->GetStatus(&st);
          return s.error = "MEError " + hr(st), s;
        }
      }
    }
    for (;;) {  // synchronous MFT
      if (!ended) {
        if (auto r = push(fill, s); r == S_FALSE) endInput();
        else if (FAILED(r) && r != MF_E_NOTACCEPTING) return s.error = "ProcessInput " + hr(r), s;
      }
      HRESULT r;
      while (SUCCEEDED(r = pull(sink, s))) {}
      if (r != MF_E_TRANSFORM_NEED_MORE_INPUT) return s.error = "ProcessOutput " + hr(r), s;
      if (ended) return s;
    }
  }

 private:
  MftEncoder() = default;

  // S_FALSE: no more input.
  HRESULT push(const Fill& fill, EncodeStats& s) {
    const DWORD size = w_ * h_ * 3 / 2;
    com_ptr_nothrow<IMFMediaBuffer> buf;
    com_ptr_nothrow<IMFSample> sample;
    HRESULT h = MFCreateMemoryBuffer(size, buf.put());
    if (FAILED(h)) return h;
    BYTE* p = nullptr;
    if (FAILED(h = buf->Lock(&p, nullptr, nullptr))) return h;
    LONGLONG time = 0;
    const bool more = fill(p, time);
    buf->Unlock();
    if (!more) return S_FALSE;
    buf->SetCurrentLength(size);
    if (FAILED(h = MFCreateSample(sample.put()))) return h;
    sample->AddBuffer(buf.get());
    sample->SetSampleTime(time);
    sample->SetSampleDuration(10'000'000LL / fps_);
    h = mft_->ProcessInput(in_, sample.get(), 0);
    if (SUCCEEDED(h)) ++s.framesIn;
    return h;
  }

  HRESULT pull(const Sink& sink, EncodeStats& s) {
    MFT_OUTPUT_DATA_BUFFER ob{};
    ob.dwStreamID = out_;
    com_ptr_nothrow<IMFSample> own;
    const bool mftAllocates = outInfo_.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES);
    if (!mftAllocates) {
      com_ptr_nothrow<IMFMediaBuffer> b;
      HRESULT h = MFCreateMemoryBuffer(std::max<DWORD>(outInfo_.cbSize, w_ * h_ * 3 / 2), b.put());
      if (SUCCEEDED(h)) h = MFCreateSample(own.put());
      if (SUCCEEDED(h)) h = own->AddBuffer(b.get());
      if (FAILED(h)) return h;
      ob.pSample = own.get();
    }
    DWORD status = 0;
    const HRESULT h = mft_->ProcessOutput(0, 1, &ob, &status);
    if (ob.pEvents) ob.pEvents->Release();
    com_ptr_nothrow<IMFSample> got = own;
    if (ob.pSample && ob.pSample != own.get()) got.attach(ob.pSample);  // MFT-allocated: we own its reference
    if (h == MF_E_TRANSFORM_STREAM_CHANGE) {  // renegotiate the output type, then go on
      com_ptr_nothrow<IMFMediaType> t;
      HRESULT r = mft_->GetOutputAvailableType(out_, 0, t.put());
      if (SUCCEEDED(r)) r = mft_->SetOutputType(out_, t.get(), 0);
      if (SUCCEEDED(r)) r = mft_->GetOutputStreamInfo(out_, &outInfo_);
      if (FAILED(r)) return r;
      // Async MFTs allow one ProcessOutput per METransformHaveOutput (else E_UNEXPECTED); they signal the output again.
      return async_ ? S_OK : pull(sink, s);
    }
    if (FAILED(h)) return h;
    com_ptr_nothrow<IMFMediaBuffer> data;
    if (!got || FAILED(got->ConvertToContiguousBuffer(data.put()))) return S_OK;
    BYTE* p = nullptr;
    DWORD len = 0;
    if (SUCCEEDED(data->Lock(&p, nullptr, &len))) {
      const bool key = MFGetAttributeUINT32(got.get(), MFSampleExtension_CleanPoint, FALSE) || hasIdr(p, len);
      if (s.firstOutputMs < 0) s.firstOutputMs = std::chrono::duration<double, std::milli>(Clock::now() - start_).count();
      ++s.framesOut;
      s.keyframes += key;
      s.bytes += len;
      sink(p, len, key);
      data->Unlock();
    }
    return S_OK;
  }

  com_ptr_nothrow<IMFActivate> activate_;
  com_ptr_nothrow<IMFTransform> mft_;
  com_ptr_nothrow<IMFMediaEventGenerator> events_;
  bool async_ = false;
  DWORD in_ = 0, out_ = 0;
  UINT32 w_ = 0, h_ = 0, fps_ = 0;
  MFT_OUTPUT_STREAM_INFO outInfo_{};
  Clock::time_point start_;
};

// Moving gradient: cheap, but not trivially compressible.
void syntheticNv12(BYTE* p, UINT32 w, UINT32 h, std::uint64_t n) {
  for (UINT32 y = 0; y < h; ++y) {
    for (UINT32 x = 0; x < w; ++x) p[y * w + x] = static_cast<BYTE>(x + y + n * 4);
  }
  std::fill(p + w * h, p + w * h * 3 / 2, BYTE{128});
}

// ---------------------------------------------------------------- resource measurement

// Samples process/system CPU, process memory and GPU engine utilisation (PDH "GPU Engine" counters, Win10 1709+).
class Meter {
 public:
  Meter() {
    GetProcessTimes(GetCurrentProcess(), &unused_, &unused_, &k0_, &u0_);
    GetSystemTimes(&si0_, &sk0_, &su0_);
    t0_ = Clock::now();
    if (PdhOpenQueryW(nullptr, 0, &query_) == ERROR_SUCCESS &&
        PdhAddEnglishCounterW(query_, L"\\GPU Engine(*)\\Utilization Percentage", 0, &gpu_) == ERROR_SUCCESS) {
      PdhCollectQueryData(query_);
    } else {
      gpuError_ = "PDH GPU Engine counters unavailable";
    }
  }
  ~Meter() {
    if (query_) PdhCloseQuery(query_);
  }

  void sample() {  // call about once per second
    if (!gpu_ || PdhCollectQueryData(query_) != ERROR_SUCCESS) return;
    DWORD size = 0, count = 0;
    if (PdhGetFormattedCounterArrayW(gpu_, PDH_FMT_DOUBLE, &size, &count, nullptr) != static_cast<PDH_STATUS>(PDH_MORE_DATA)) return;
    std::vector<BYTE> buf(size);
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf.data());
    if (PdhGetFormattedCounterArrayW(gpu_, PDH_FMT_DOUBLE, &size, &count, items) != ERROR_SUCCESS) return;
    const std::string pid = std::format("pid_{}_", GetCurrentProcessId());
    std::map<std::string, double> all, mine;
    for (DWORD i = 0; i < count; ++i) {
      const std::string n = utf8(items[i].szName);
      const auto at = n.find("engtype_");
      if (at == std::string::npos || items[i].FmtValue.CStatus != ERROR_SUCCESS) continue;
      const std::string engine = n.substr(at + 8);
      all[engine] += items[i].FmtValue.doubleValue;
      if (n.starts_with(pid)) mine[engine] += items[i].FmtValue.doubleValue;
    }
    for (const auto& [k, v] : all) sumAll_[k] += v;
    for (const auto& [k, v] : mine) sumMine_[k] += v;
    ++samples_;
  }

  json result() const {
    FILETIME unused, k1, u1, si1, sk1, su1;
    GetProcessTimes(GetCurrentProcess(), &unused, &unused, &k1, &u1);
    GetSystemTimes(&si1, &sk1, &su1);
    auto ticks = [](FILETIME f) { return (static_cast<std::uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    const double wall = std::chrono::duration<double>(Clock::now() - t0_).count();
    const double cores = std::max(1u, std::thread::hardware_concurrency());
    const double proc = static_cast<double>(ticks(k1) - ticks(k0_) + ticks(u1) - ticks(u0_)) / 1e7;
    const double sysTotal = static_cast<double>(ticks(sk1) - ticks(sk0_) + ticks(su1) - ticks(su0_));  // kernel includes idle
    const double sysIdle = static_cast<double>(ticks(si1) - ticks(si0_));
    PROCESS_MEMORY_COUNTERS_EX mem{};
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&mem), sizeof(mem));
    json j = {{"seconds", wall},
              {"processCpuPercentOfMachine", 100.0 * proc / wall / cores},
              {"processCpuCores", proc / wall},
              {"systemCpuPercent", sysTotal > 0 ? 100.0 * (sysTotal - sysIdle) / sysTotal : 0.0},
              {"workingSetMB", static_cast<double>(mem.WorkingSetSize) / (1 << 20)},
              {"peakWorkingSetMB", static_cast<double>(mem.PeakWorkingSetSize) / (1 << 20)},
              {"privateMB", static_cast<double>(mem.PrivateUsage) / (1 << 20)}};
    json gpu = {{"samples", samples_}, {"note", "average over samples of the summed Utilization Percentage per engine type"}};
    if (!gpuError_.empty()) gpu["error"] = gpuError_;
    for (const auto& [k, v] : sumAll_) gpu["allProcesses"][k] = samples_ ? v / samples_ : 0;
    for (const auto& [k, v] : sumMine_) gpu["thisProcess"][k] = samples_ ? v / samples_ : 0;
    j["gpu"] = gpu;
    return j;
  }

 private:
  FILETIME unused_{}, k0_{}, u0_{}, si0_{}, sk0_{}, su0_{};
  Clock::time_point t0_;
  PDH_HQUERY query_ = nullptr;
  PDH_HCOUNTER gpu_ = nullptr;
  std::string gpuError_;
  std::map<std::string, double> sumAll_, sumMine_;
  std::uint64_t samples_ = 0;
};

// ---------------------------------------------------------------- phases

std::ofstream openOut(const Options& o, const std::string& file) { return std::ofstream(o.out / file, std::ios::binary); }

// Two (or more) hardware sessions at once on synthetic frames: does the GPU give us one session per camera?
json concurrentSessions(const Options& o, int sessions) {
  json j = {{"sessionsRequested", sessions}, {"framesPerSession", o.fps * 5}};
  if (enumEncoders(true).empty()) return j["error"] = "no hardware H.264 encoder MFT", j;
  std::vector<std::unique_ptr<MftEncoder>> enc;
  json reports = json::array();
  for (int i = 0; i < sessions; ++i) {
    json r;
    auto list = enumEncoders(true);  // fresh activation object per session
    enc.push_back(MftEncoder::open(std::move(list.front()), o, r));
    reports.push_back(r);
  }
  std::vector<EncodeStats> stats(enc.size());
  std::vector<std::thread> threads;
  const auto t0 = Clock::now();
  for (std::size_t i = 0; i < enc.size(); ++i) {
    if (!enc[i]) continue;
    threads.emplace_back([&, i] {
      auto file = openOut(o, std::format("session_{}.h264", i));
      std::uint64_t n = 0;
      const auto total = static_cast<std::uint64_t>(o.fps) * 5;
      stats[i] = enc[i]->run(
          [&](BYTE* p, LONGLONG& t) {
            if (n == total) return false;
            syntheticNv12(p, static_cast<UINT32>(o.width), static_cast<UINT32>(o.height), n);
            t = static_cast<LONGLONG>(n++ * 10'000'000ULL / static_cast<std::uint64_t>(o.fps));
            return true;
          },
          [&](const BYTE* d, DWORD len, bool) { file.write(reinterpret_cast<const char*>(d), len); });
    });
  }
  for (auto& t : threads) t.join();
  const double secs = std::chrono::duration<double>(Clock::now() - t0).count();
  int ok = 0;
  for (std::size_t i = 0; i < enc.size(); ++i) {
    if (enc[i]) {
      reports[i]["result"] = stats[i].toJson(0);
      reports[i]["file"] = std::format("session_{}.h264", i);
      ok += stats[i].error.empty() && stats[i].framesOut > 0;
    }
  }
  j["sessions"] = reports;
  j["wallSeconds"] = secs;
  j["concurrentSessionsWorking"] = ok;
  return j;
}

// N cameras -> N encoders for o.seconds, measured. hardware=false: OpenH264 (the software baseline).
json captureEncode(const Options& o, bool hardware, eme::MediaGraph&, std::vector<eme::CameraSource*>& cams,
                   std::vector<std::shared_ptr<eme::Subscription<eme::VideoFrame>>>& subs) {
  const std::string tag = hardware ? "hw" : "openh264";
  json j = {{"encoder", tag}, {"cameras", cams.size()}};
  std::vector<json> perCam(cams.size());
  std::vector<EncodeStats> stats(cams.size());
  std::vector<eme::CameraStats> before(cams.size());
  for (std::size_t i = 0; i < cams.size(); ++i) before[i] = cams[i]->stats();
  for (auto& s : subs) while (s->tryPop()) {}  // start from fresh frames
  Meter meter;
  std::vector<std::jthread> threads;
  const auto deadline = Clock::now() + std::chrono::seconds(o.seconds);
  for (std::size_t i = 0; i < cams.size(); ++i) {
    threads.emplace_back([&, i](std::stop_token st) {
      auto file = openOut(o, std::format("capture_{}_{}.h264", tag, i));
      const auto w = static_cast<std::size_t>(o.width), h = static_cast<std::size_t>(o.height);
      std::uint64_t badFrames = 0;
      auto nextFrame = [&]() -> std::optional<eme::VideoFrame> {
        while (Clock::now() < deadline && !st.stop_requested()) {
          if (auto f = subs[i]->tryPop()) return f;
          std::this_thread::sleep_for(2ms);
        }
        return std::nullopt;
      };
      if (hardware) {
        json r;
        auto list = enumEncoders(true);
        auto enc = list.empty() ? nullptr : MftEncoder::open(std::move(list.front()), o, r);
        if (!enc) {
          perCam[i] = r.empty() ? json{{"error", "no hardware H.264 encoder MFT"}} : r;
          return;
        }
        stats[i] = enc->run(
            [&](BYTE* p, LONGLONG& t) {
              auto f = nextFrame();
              while (f && (f->width != o.width || f->height != o.height || f->buffer->cpuData().size() < w * h * 3 / 2)) {
                ++badFrames;  // never feed libyuv a frame smaller than the encoder's size
                f = nextFrame();
              }
              if (!f) return false;
              const auto* src = reinterpret_cast<const std::uint8_t*>(f->buffer->cpuData().data());
              libyuv::I420ToNV12(src, f->width, src + w * h, f->width / 2, src + w * h * 5 / 4, f->width / 2, p, f->width,
                                 p + w * h, f->width, f->width, f->height);
              t = f->presentationTimestampNs / 100;
              return true;
            },
            [&](const BYTE* d, DWORD len, bool) { file.write(reinterpret_cast<const char*>(d), len); });
        r["skippedFramesWrongSize"] = badFrames;
        perCam[i] = r;
      } else {
        auto enc = eme::OpenH264Encoder::create({.width = o.width, .height = o.height, .fps = o.fps, .bitrateBps = o.bitrate,
                                                 .keyframeIntervalFrames = o.fps * 2});
        if (!enc) {
          perCam[i] = {{"error", enc.error().message}};
          return;
        }
        const auto t0 = Clock::now();
        while (auto f = nextFrame()) {
          ++stats[i].framesIn;
          auto r = (*enc)->encode(*f, [&](eme::EncodedVideoFrame&& e) {
            const auto& bytes = e.payload->bytes;
            if (stats[i].firstOutputMs < 0) stats[i].firstOutputMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
            ++stats[i].framesOut;
            stats[i].keyframes += e.isKeyframe;
            stats[i].bytes += bytes.size();
            file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
          });
          if (!r) {
            stats[i].error = r.error().message;
            break;
          }
        }
      }
    });
  }
  while (Clock::now() < deadline) {
    std::this_thread::sleep_for(std::min<Clock::duration>(1s, deadline - Clock::now()));
    meter.sample();
  }
  j["resources"] = meter.result();  // at the deadline, before the drain tail
  threads.clear();  // join (encoders drain)
  const double measured = j["resources"]["seconds"].get<double>();
  json list = json::array();
  for (std::size_t i = 0; i < cams.size(); ++i) {
    const auto after = cams[i]->stats();
    json c = perCam[i];
    c["camera"] = cams[i]->name();
    c["negotiated"] = cams[i]->negotiated();
    c["file"] = std::format("capture_{}_{}.h264", tag, i);
    c["capture"] = {{"framesCaptured", after.framesCaptured - before[i].framesCaptured},
                    {"framesPublished", after.framesPublished - before[i].framesPublished},
                    {"framesDropped", after.framesDropped - before[i].framesDropped},
                    {"deviceLost", after.deviceLost}};
    c["encode"] = stats[i].toJson(measured);
    list.push_back(c);
  }
  j["perCamera"] = list;
  return j;
}

std::optional<Options> parse(int argc, char** argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    const bool hasValue = i + 1 < argc;
    try {
      if (a == "--seconds" && hasValue) o.seconds = std::stoi(argv[++i]);
      else if (a == "--fps" && hasValue) o.fps = std::stoi(argv[++i]);
      else if (a == "--bitrate" && hasValue) o.bitrate = static_cast<std::uint32_t>(std::stoul(argv[++i]));
      else if (a == "--out" && hasValue) o.out = argv[++i];
      else if (a == "--no-sw") o.sw = false;
      else if (a == "--size" && hasValue) {
        const std::string v = argv[++i];
        const auto x = v.find('x');
        if (x == std::string::npos) return std::nullopt;
        o.width = std::stoi(v.substr(0, x));
        o.height = std::stoi(v.substr(x + 1));
      } else {
        return std::nullopt;
      }
    } catch (const std::exception&) {
      return std::nullopt;
    }
  }
  const bool ok = o.seconds > 0 && o.seconds <= 3600 && o.fps > 0 && o.fps <= 60 && o.width > 0 && o.height > 0 &&
                  o.width % 2 == 0 && o.height % 2 == 0 && o.width <= 4096 && o.height <= 2160 && o.bitrate >= 10'000;
  return ok ? std::optional(o) : std::nullopt;
}

}  // namespace

int main(int argc, char** argv) {
  const auto opt = parse(argc, argv);
  if (!opt) {
    std::cerr << "usage: feasibility_probe [--seconds 60] [--size 1280x720] [--fps 15] [--bitrate 1200000] [--out DIR] [--no-sw]\n";
    return 2;
  }
  const Options& o = *opt;
  auto com = wil::CoInitializeEx_failfast(COINIT_MULTITHREADED);
  std::error_code ec;
  std::filesystem::create_directories(o.out, ec);
  json report = {{"probe", "EME-001 feasibility_probe"},
                 {"built", __DATE__},
                 {"options", {{"seconds", o.seconds}, {"width", o.width}, {"height", o.height}, {"fps", o.fps}, {"bitrate", o.bitrate}}}};
  auto save = [&] {
    std::ofstream(o.out / "report.json") << report.dump(2) << '\n';
  };
  auto phase = [&](const char* name) {
    std::cerr << "[eme001] " << name << "...\n";
    save();  // a crash in a later phase still leaves the earlier results
  };

  phase("system");
  report["system"] = systemInfo();
  // shortcut: MF is linked directly (not delay-loaded like eme_camera), so this exe needs Media Foundation installed.
  if (const HRESULT h = MFStartup(MF_VERSION); FAILED(h)) {
    report["error"] = "MFStartup failed: " + hr(h) + " (Media Foundation not installed?)";
    save();
    std::cout << report.dump(2) << std::endl;
    return 0;
  }
  phase("cameras");
  report["cameras"] = cameraInfo();
  phase("encoders");
  report["encoders"] = {{"hardware", encoderList(true)}, {"software", encoderList(false)}};
  phase("concurrent hardware sessions");
  report["concurrentSessions"] = concurrentSessions(o, 2);

  phase("capture");
  {  // the graph (and its MF camera readers) must be gone before MFShutdown
  eme::MediaGraph graph;
  std::vector<eme::CameraSource*> cams;
  std::vector<std::shared_ptr<eme::Subscription<eme::VideoFrame>>> subs;
  json opened = json::array();
  const auto infos = eme::enumerateCameras();
  for (std::size_t i = 0; i < infos.size() && cams.size() < 2; ++i) {
    auto src = eme::CameraSource::open(infos[i].name, {.id = infos[i].id, .sourceId = static_cast<std::uint32_t>(i),
                                                       .width = o.width, .height = o.height, .fps = o.fps, .poolSize = 6});
    if (!src) {
      opened.push_back({{"camera", infos[i].name}, {"error", src.error().message}});
      continue;
    }
    auto& cam = graph.adopt(std::move(*src));
    if (const auto f = cam.outputFormat(); f->width != o.width || f->height != o.height) {  // encoders are sized from --size
      opened.push_back({{"camera", infos[i].name}, {"negotiated", cam.negotiated()},
                        {"error", "camera has no " + std::to_string(o.width) + "x" + std::to_string(o.height) + " mode; rerun with --size"}});
      continue;  // stays in the graph unused
    }
    subs.push_back(cam.output().subscribe(2, eme::OverflowPolicy::DropOldest));
    opened.push_back({{"camera", infos[i].name}, {"negotiated", cam.negotiated()}});
    cams.push_back(&cam);
  }
  report["captureOpen"] = opened;
  if (cams.empty()) {
    report["captureEncode"] = {{"error", "no camera could be opened"}};
  } else if (auto r = graph.start(); !r) {
    report["captureEncode"] = {{"error", "start: " + r.error().message}};
  } else {
    json runs = json::array();
    phase("capture + hardware encode");
    runs.push_back(captureEncode(o, true, graph, cams, subs));
    report["captureEncode"] = runs;
    if (o.sw) {
      phase("capture + OpenH264 encode");
      runs.push_back(captureEncode(o, false, graph, cams, subs));
      report["captureEncode"] = runs;
    }
    graph.stop();
  }
  }
  save();
  std::cerr << "[eme001] done: " << (o.out / "report.json").string() << '\n';
  std::cout << report.dump(2) << std::endl;
  MFShutdown();
  return 0;
}
