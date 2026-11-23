// Media Foundation hardware H.264 encoder (EME-006). Grew out of the EME-001 probe's MftEncoder, which ran on the
// target (Intel Quick Sync, two sessions at 720p30): docs/reports/eme-001-feasibility.md.
//
// Hardware MFTs are async: they ask for input (METransformNeedInput) and announce output (METransformHaveOutput) as
// events, and allow exactly one ProcessInput/ProcessOutput per event (a second call fails with E_UNEXPECTED; the probe's
// first device run hit that). Events arrive on an MF work-queue thread via BeginGetEvent and are queued; encode() runs on
// the EncoderNode worker and handles them there, so all MFT calls stay on one thread.
#include "eme/processing/mf_h264_encoder.hpp"

#include <windows.h>
#include <strmif.h>  // ICodecAPI
#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <oleauto.h>
#include <libyuv.h>
#include <spdlog/spdlog.h>
#include <wil/com.h>
#include <wil/resource.h>
#include <wrl/client.h>
#include <wrl/implements.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <vector>

#include "../../platform/windows_mf/mf_platform.hpp"
#include "eme/processing/h264_annexb.hpp"

namespace eme {
namespace {

using Clock = std::chrono::steady_clock;
using mf::hr;
using wil::com_ptr_nothrow;
using namespace std::chrono_literals;

constexpr auto kInputStall = 2s;     // no input request for this long: the session is wedged
constexpr auto kOutputWait = 10ms;   // after a frame goes in, wait this long for its output (encode takes a few ms)
constexpr std::size_t kMaxPending = 64;

struct Event {
  MediaEventType type = MEUnknown;
  HRESULT status = S_OK;
};

// Receives the MFT's events on an MF work-queue thread and queues them for the encode thread. Ref-counted on its own:
// a callback still in flight after the encoder is gone only touches this object. The MFT completes the outstanding
// BeginGetEvent with MF_E_SHUTDOWN when it is shut down, which drops the last reference.
class EventQueue final
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IMFAsyncCallback> {
 public:
  explicit EventQueue(com_ptr_nothrow<IMFMediaEventGenerator> generator) : generator_(std::move(generator)) {}

  HRESULT start() { return generator_->BeginGetEvent(this, nullptr); }
  void stop() noexcept { stopped_ = true; }  // no re-arm: the pending request completes on MFT shutdown and frees us

  // The next event, waiting up to `timeout`; nullopt when none arrived.
  std::optional<Event> next(Clock::duration timeout) {
    std::unique_lock lock(mutex_);
    if (!cv_.wait_for(lock, std::max(timeout, Clock::duration::zero()), [&] { return !events_.empty(); })) return std::nullopt;
    const Event e = events_.front();
    events_.pop_front();
    return e;
  }

  STDMETHODIMP GetParameters(DWORD*, DWORD*) override { return E_NOTIMPL; }

  STDMETHODIMP Invoke(IMFAsyncResult* result) override {
    com_ptr_nothrow<IMFMediaEvent> ev;
    Event e{.type = MEError};
    e.status = generator_->EndGetEvent(result, ev.put());
    if (SUCCEEDED(e.status)) {
      ev->GetType(&e.type);
      ev->GetStatus(&e.status);
    }
    {
      std::lock_guard lock(mutex_);
      events_.push_back(e);
    }
    cv_.notify_one();
    if (e.type != MEError && !stopped_) {
      if (const HRESULT h = generator_->BeginGetEvent(this, nullptr); FAILED(h) && h != MF_E_SHUTDOWN) {
        std::lock_guard lock(mutex_);
        events_.push_back({MEError, h});
        cv_.notify_one();
      }
    }
    return S_OK;
  }

 private:
  com_ptr_nothrow<IMFMediaEventGenerator> generator_;
  std::atomic<bool> stopped_{false};
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<Event> events_;
};

VARIANT u4(ULONG x) {
  VARIANT v;
  VariantInit(&v);
  v.vt = VT_UI4;
  v.ulVal = x;
  return v;
}

std::string attrString(IMFAttributes* a, const GUID& key) {
  wil::unique_cotaskmem_string s;
  UINT32 len = 0;
  return SUCCEEDED(a->GetAllocatedString(key, s.put(), &len)) ? mf::utf8(s.get()) : std::string{};
}

std::vector<com_ptr_nothrow<IMFActivate>> hardwareEncoders() {
  MFT_REGISTER_TYPE_INFO in{MFMediaType_Video, MFVideoFormat_NV12}, out{MFMediaType_Video, MFVideoFormat_H264};
  IMFActivate** list = nullptr;
  UINT32 count = 0;
  std::vector<com_ptr_nothrow<IMFActivate>> result;
  if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, &in, &out, &list,
                       &count))) {
    return result;
  }
  for (UINT32 i = 0; i < count; ++i) result.emplace_back().attach(list[i]);  // takes the enumeration's reference
  CoTaskMemFree(list);
  return result;
}

}  // namespace

struct MfH264Encoder::Impl {
  struct Pending {
    LONGLONG time = 0;  // 100 ns, as given to the MFT
    std::uint32_t sourceId = 0;
    std::int64_t captureNs = 0, presentationNs = 0;
  };

  EncoderConfig config;
  MfEncoderInfo info;
  com_ptr_nothrow<IMFActivate> activate;
  com_ptr_nothrow<IMFTransform> mft;
  com_ptr_nothrow<ICodecAPI> codecApi;
  Microsoft::WRL::ComPtr<EventQueue> events;
  DWORD in = 0, out = 0;
  MFT_OUTPUT_STREAM_INFO outInfo{};

  // encode() thread only
  int inputRequests = 0;
  std::deque<Pending> pending;
  CodecConfiguration codecConfig;
  std::uint64_t sequence = 0;
  std::optional<Error> dead;  // the session failed: every later encode() returns this at once
  com_ptr_nothrow<IMFMediaType> outType;  // as requested, reapplied after a stream change

  std::atomic<std::uint32_t> bitrate{0};
  std::atomic<bool> bitrateChanged{false}, keyframePending{false};
  std::atomic<std::uint64_t> framesIn{0}, framesOut{0}, keyframes{0}, requests{0}, forced{0};

  ~Impl() {
    if (events) events->stop();
    if (mft) mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    codecApi.reset();
    mft.reset();
    if (activate) activate->ShutdownObject();  // shuts the MFT down; completes the pending BeginGetEvent
  }

  Error failure(const std::string& step, HRESULT h) const {
    return Error{ErrorCode::Unsupported, info.name + ": " + step + " failed (" + hr(h) + ")"};
  }

  // Sets up one hardware MFT; on failure the caller tries the next one.
  std::expected<void, Error> configure() {
    info.name = attrString(activate.get(), MFT_FRIENDLY_NAME_Attribute);
    HRESULT h = activate->ActivateObject(IID_PPV_ARGS(mft.put()));
    if (FAILED(h)) return std::unexpected(failure("ActivateObject", h));
    com_ptr_nothrow<IMFAttributes> attrs;
    if (FAILED(mft->GetAttributes(attrs.put())) || !MFGetAttributeUINT32(attrs.get(), MF_TRANSFORM_ASYNC, FALSE)) {
      return std::unexpected(failure("async MFT check", E_NOTIMPL));  // hardware MFTs are async; no sync path needed
    }
    if (FAILED(h = attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE))) return std::unexpected(failure("MF_TRANSFORM_ASYNC_UNLOCK", h));
    auto generator = mft.try_query<IMFMediaEventGenerator>();
    codecApi = mft.try_query<ICodecAPI>();
    if (!generator) return std::unexpected(failure("IMFMediaEventGenerator", E_NOINTERFACE));
    if (!codecApi) return std::unexpected(failure("ICodecAPI", E_NOINTERFACE));  // needed for CBR and forced keyframes
    if (mft->GetStreamIDs(1, &in, 1, &out) == E_NOTIMPL) in = out = 0;

    // Rate control before the media types: some encoders only take it then. Results are read back below.
    VARIANT on;
    VariantInit(&on);
    on.vt = VT_BOOL;
    on.boolVal = VARIANT_TRUE;
    const auto set = [&](const GUID& key, VARIANT v) { codecApi->SetValue(&key, &v); };
    set(CODECAPI_AVLowLatencyMode, on);
    set(CODECAPI_AVEncCommonRateControlMode, u4(eAVEncCommonRateControlMode_CBR));
    set(CODECAPI_AVEncCommonMeanBitRate, u4(config.bitrateBps));
    set(CODECAPI_AVEncMPVGOPSize, u4(static_cast<ULONG>(config.keyframeIntervalFrames)));
    set(CODECAPI_AVEncMPVDefaultBPictureCount, u4(0));

    const auto w = static_cast<UINT32>(config.width), hgt = static_cast<UINT32>(config.height);
    const auto fps = static_cast<UINT32>(config.fps);
    if (FAILED(h = MFCreateMediaType(outType.put()))) return std::unexpected(failure("MFCreateMediaType", h));
    outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    outType->SetUINT32(MF_MT_AVG_BITRATE, config.bitrateBps);
    MFSetAttributeSize(outType.get(), MF_MT_FRAME_SIZE, w, hgt);
    MFSetAttributeRatio(outType.get(), MF_MT_FRAME_RATE, fps, 1);
    MFSetAttributeRatio(outType.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    outType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Base);  // every browser decodes it; the device gave Constrained Baseline
    if (FAILED(h = mft->SetOutputType(out, outType.get(), 0))) return std::unexpected(failure("SetOutputType(H264 Baseline)", h));

    bool inputSet = false;
    for (DWORD i = 0; !inputSet; ++i) {
      com_ptr_nothrow<IMFMediaType> t;
      if (FAILED(mft->GetInputAvailableType(in, i, t.put()))) break;
      GUID sub{};
      if (FAILED(t->GetGUID(MF_MT_SUBTYPE, &sub)) || sub != MFVideoFormat_NV12) continue;
      MFSetAttributeSize(t.get(), MF_MT_FRAME_SIZE, w, hgt);
      MFSetAttributeRatio(t.get(), MF_MT_FRAME_RATE, fps, 1);
      MFSetAttributeRatio(t.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
      t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
      if (FAILED(h = mft->SetInputType(in, t.get(), 0))) return std::unexpected(failure("SetInputType(NV12)", h));
      inputSet = true;
    }
    if (!inputSet) return std::unexpected(failure("find NV12 input type", MF_E_INVALIDMEDIATYPE));
    if (FAILED(h = mft->GetOutputStreamInfo(out, &outInfo))) return std::unexpected(failure("GetOutputStreamInfo", h));
    readBack();

    events = Microsoft::WRL::Make<EventQueue>(std::move(generator));
    if (!events) return std::unexpected(failure("event queue", E_OUTOFMEMORY));
    if (FAILED(h = events->start())) return std::unexpected(failure("BeginGetEvent", h));
    mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    if (FAILED(h = mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0))) return std::unexpected(failure("BEGIN_STREAMING", h));
    if (FAILED(h = mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0))) return std::unexpected(failure("START_OF_STREAM", h));
    return {};
  }

  void readBack() {
    const auto get = [&](const GUID& key) -> std::optional<VARIANT> {
      VARIANT v;
      VariantInit(&v);
      if (FAILED(codecApi->GetValue(&key, &v))) return std::nullopt;
      return v;  // VT_UI4/VT_BOOL/VT_UI8: nothing to VariantClear
    };
    const auto ul = [&](const GUID& key) -> std::uint32_t {
      const auto v = get(key);
      return !v ? 0 : v->vt == VT_UI4 ? v->ulVal : v->vt == VT_UI8 ? static_cast<std::uint32_t>(v->ullVal) : 0;
    };
    const auto lowLatency = get(CODECAPI_AVLowLatencyMode);
    info.lowLatency = lowLatency && lowLatency->vt == VT_BOOL && lowLatency->boolVal != VARIANT_FALSE;
    info.cbr = ul(CODECAPI_AVEncCommonRateControlMode) == static_cast<std::uint32_t>(eAVEncCommonRateControlMode_CBR);
    info.bitrateBps = ul(CODECAPI_AVEncCommonMeanBitRate);
    info.gop = ul(CODECAPI_AVEncMPVGOPSize);
    info.bFrames = ul(CODECAPI_AVEncMPVDefaultBPictureCount);
    info.forceKeyframeSupported = codecApi->IsSupported(&CODECAPI_AVEncVideoForceKeyFrame) == S_OK;
    com_ptr_nothrow<IMFMediaType> current;
    if (SUCCEEDED(mft->GetOutputCurrentType(out, current.put()))) info.profile = MFGetAttributeUINT32(current.get(), MF_MT_MPEG2_PROFILE, 0);

    spdlog::info("{}: profile {}, {}, {} bps, GOP {}, B-frames {}, low latency {}, forced keyframes {}", info.name, info.profile,
                 info.cbr ? "CBR" : "not CBR", info.bitrateBps, info.gop, info.bFrames, info.lowLatency,
                 info.forceKeyframeSupported ? "supported" : "UNSUPPORTED");
    if (!info.cbr || info.bitrateBps != config.bitrateBps || info.gop != static_cast<std::uint32_t>(config.keyframeIntervalFrames) ||
        info.bFrames != 0 || !info.lowLatency || info.profile != static_cast<std::uint32_t>(eAVEncH264VProfile_Base)) {
      spdlog::warn("{}: encoder did not accept every requested setting (asked: CBR, {} bps, GOP {}, 0 B-frames, low latency, "
                   "Baseline)", info.name, config.bitrateBps, config.keyframeIntervalFrames);
    }
  }

  // Handles queued events, waiting up to `timeout` while done() is false. Errors end the session.
  template <class Done>
  Result pump(Clock::duration timeout, Done done, const EmitFn& emit) {
    const auto deadline = Clock::now() + timeout;
    for (;;) {
      const auto ev = events->next(done() ? Clock::duration::zero() : deadline - Clock::now());
      if (!ev) return {};
      if (ev->type == MEError || FAILED(ev->status)) {
        return fail(ErrorCode::Internal, info.name + ": MFT error event (" + hr(ev->status) + ")");
      }
      if (ev->type == METransformNeedInput) {
        ++inputRequests;
      } else if (ev->type == METransformHaveOutput) {
        if (auto r = pullOne(emit); !r) return r;
      }
    }
  }

  Result pullOne(const EmitFn& emit) {
    MFT_OUTPUT_DATA_BUFFER ob{};
    ob.dwStreamID = out;
    com_ptr_nothrow<IMFSample> own;
    if (!(outInfo.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES))) {
      com_ptr_nothrow<IMFMediaBuffer> b;
      const DWORD size = std::max<DWORD>(outInfo.cbSize, static_cast<DWORD>(config.width * config.height * 3 / 2));
      HRESULT h = MFCreateMemoryBuffer(size, b.put());
      if (SUCCEEDED(h)) h = MFCreateSample(own.put());
      if (SUCCEEDED(h)) h = own->AddBuffer(b.get());
      if (FAILED(h)) return fail(ErrorCode::Internal, info.name + ": output buffer (" + hr(h) + ")");
      ob.pSample = own.get();
    }
    DWORD status = 0;
    const HRESULT h = mft->ProcessOutput(0, 1, &ob, &status);
    if (ob.pEvents) ob.pEvents->Release();
    com_ptr_nothrow<IMFSample> got = own;
    if (ob.pSample && ob.pSample != own.get()) got.attach(ob.pSample);  // MFT-allocated: we own its reference
    if (h == MF_E_TRANSFORM_STREAM_CHANGE) {
      com_ptr_nothrow<IMFMediaType> t;
      HRESULT r = mft->GetOutputAvailableType(out, 0, t.put());
      for (const GUID& key : {MF_MT_MPEG2_PROFILE, MF_MT_AVG_BITRATE, MF_MT_FRAME_RATE, MF_MT_FRAME_SIZE}) {  // keep what we asked for
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(r) && SUCCEEDED(outType->GetItem(key, &v))) t->SetItem(key, v);
        PropVariantClear(&v);
      }
      if (SUCCEEDED(r)) r = mft->SetOutputType(out, t.get(), 0);
      if (SUCCEEDED(r)) r = mft->GetOutputStreamInfo(out, &outInfo);
      if (FAILED(r)) return fail(ErrorCode::Internal, info.name + ": output renegotiation (" + hr(r) + ")");
      readBack();
      return {};  // one ProcessOutput per METransformHaveOutput: the MFT signals the pending output again
    }
    if (h == MF_E_TRANSFORM_NEED_MORE_INPUT) return {};
    if (FAILED(h)) return fail(ErrorCode::Internal, info.name + ": ProcessOutput (" + hr(h) + ")");
    com_ptr_nothrow<IMFMediaBuffer> data;
    if (!got || FAILED(got->ConvertToContiguousBuffer(data.put()))) return {};
    BYTE* p = nullptr;
    DWORD len = 0;
    if (FAILED(data->Lock(&p, nullptr, &len))) return {};
    auto payload = std::make_shared<EncodedBuffer>();
    const auto* bytes = reinterpret_cast<const std::byte*>(p);
    payload->bytes.assign(bytes, bytes + len);
    data->Unlock();
    if (payload->bytes.empty()) return {};

    LONGLONG time = 0;
    got->GetSampleTime(&time);
    const Pending meta = takePending(time);
    const bool key = MFGetAttributeUINT32(got.get(), MFSampleExtension_CleanPoint, FALSE) || h264::containsIdr(payload->bytes);
    if (key) {
      auto ps = h264::parameterSets(payload->bytes);
      if (ps.empty() && !codecConfig.parameterSets) ps = sequenceHeader();  // some MFTs keep SPS/PPS out of band
      if (!ps.empty() && (!codecConfig.parameterSets || *codecConfig.parameterSets != ps)) {
        codecConfig = {.version = codecConfig.version + 1,
                       .parameterSets = std::make_shared<const std::vector<std::byte>>(std::move(ps))};
      }
      ++keyframes;
    }
    emit(EncodedVideoFrame{
        .sequence = sequence++,
        .sourceId = meta.sourceId,
        .codec = VideoCodec::H264,
        .captureTimestampNs = meta.captureNs,
        .presentationTimestampNs = meta.presentationNs,
        .isKeyframe = key,
        .payload = std::move(payload),
        .codecConfig = codecConfig,
    });
    ++framesOut;
    return {};
  }

  std::vector<std::byte> sequenceHeader() const {
    com_ptr_nothrow<IMFMediaType> t;
    UINT32 size = 0;
    if (FAILED(mft->GetOutputCurrentType(out, t.put())) || FAILED(t->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &size)) || !size) return {};
    std::vector<std::byte> blob(size);
    if (FAILED(t->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, reinterpret_cast<UINT8*>(blob.data()), size, nullptr))) return {};
    return h264::parameterSets(blob);  // Annex-B for H.264
  }

  // The input frame an output belongs to. No B-frames, so outputs come in input order; entries the MFT skipped are dropped.
  Pending takePending(LONGLONG time) {
    const auto match = std::ranges::find(pending, time, &Pending::time);
    if (match == pending.end()) {  // MFT rewrote the timestamp: take the oldest
      if (pending.empty()) return {.time = time, .captureNs = time * 100, .presentationNs = time * 100};
      const Pending p = pending.front();
      pending.pop_front();
      return p;
    }
    const Pending p = *match;
    pending.erase(pending.begin(), match + 1);
    return p;
  }
};

std::expected<std::unique_ptr<MfH264Encoder>, Error> MfH264Encoder::create(EncoderConfig c) {
  if (c.width <= 0 || c.height <= 0 || c.width % 2 || c.height % 2 || c.fps <= 0 || c.bitrateBps == 0 ||
      c.bitrateBps > INT32_MAX / 2 || c.keyframeIntervalFrames <= 0) {
    return fail(ErrorCode::InvalidArgument, "mf-h264: invalid encoder config");
  }
  if (const HRESULT h = mf::platform(); FAILED(h)) {
    return fail(ErrorCode::Unsupported, "mf-h264: Media Foundation unavailable (" + hr(h) + ")");
  }
  auto encoders = hardwareEncoders();
  if (encoders.empty()) return fail(ErrorCode::Unsupported, "mf-h264: no hardware H.264 encoder MFT");
  std::string errors;
  for (auto& activate : encoders) {
    auto impl = std::make_unique<Impl>();
    impl->config = c;
    impl->bitrate = c.bitrateBps;
    impl->activate = std::move(activate);
    if (auto r = impl->configure()) return std::unique_ptr<MfH264Encoder>(new MfH264Encoder(std::move(impl)));
    else errors += (errors.empty() ? "" : "; ") + r.error().message;
  }
  return fail(ErrorCode::Unsupported, "mf-h264: no hardware encoder could be configured: " + errors);
}

MfH264Encoder::MfH264Encoder(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
MfH264Encoder::~MfH264Encoder() = default;

EncoderConfig MfH264Encoder::config() const {
  auto c = impl_->config;
  c.bitrateBps = impl_->bitrate.load();
  return c;
}

const MfEncoderInfo& MfH264Encoder::info() const noexcept { return impl_->info; }

bool MfH264Encoder::supportsInput(const VideoFormat& f) const {
  return f.kind == MediaKind::RawVideo && (f.pixelFormat == PixelFormat::I420 || f.pixelFormat == PixelFormat::NV12) &&
         f.width == impl_->config.width && f.height == impl_->config.height;
}

Result MfH264Encoder::encode(const VideoFrame& frame, const EmitFn& emit) {
  auto& s = *impl_;
  if (s.dead) return std::unexpected(*s.dead);
  const auto w = static_cast<std::size_t>(s.config.width), h = static_cast<std::size_t>(s.config.height);
  const std::size_t size = w * h * 3 / 2;
  if (!frame.buffer || (frame.pixelFormat != PixelFormat::I420 && frame.pixelFormat != PixelFormat::NV12) ||
      frame.width != s.config.width || frame.height != s.config.height || frame.buffer->cpuData().size() < size) {
    return fail(ErrorCode::InvalidArgument, "mf-h264: frame does not match encoder config");
  }
  if (s.bitrateChanged.exchange(false)) {
    VARIANT v = u4(s.bitrate.load());
    if (const HRESULT r = s.codecApi->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &v); FAILED(r)) {
      return fail(ErrorCode::Internal, s.info.name + ": bitrate change rejected (" + hr(r) + ")");
    }
  }
  const auto die = [&](Error e) -> Result {
    s.dead = e;
    return std::unexpected(std::move(e));
  };
  if (auto r = s.pump(kInputStall, [&] { return s.inputRequests > 0; }, emit); !r) return die(r.error());
  if (s.inputRequests == 0) return die({ErrorCode::Internal, s.info.name + ": no input request for 2 s; session stopped"});

  // shortcut: one system-memory buffer per frame plus an I420->NV12 copy; EME-005 feeds D3D11 surfaces instead.
  com_ptr_nothrow<IMFMediaBuffer> buf;
  com_ptr_nothrow<IMFSample> sample;
  HRESULT r = MFCreateMemoryBuffer(static_cast<DWORD>(size), buf.put());
  BYTE* p = nullptr;
  if (SUCCEEDED(r)) r = buf->Lock(&p, nullptr, nullptr);
  if (FAILED(r)) return fail(ErrorCode::Internal, s.info.name + ": input buffer (" + hr(r) + ")");
  const auto* src = reinterpret_cast<const std::uint8_t*>(frame.buffer->cpuData().data());
  if (frame.pixelFormat == PixelFormat::NV12) {
    std::memcpy(p, src, size);
  } else {
    const int iw = s.config.width, ih = s.config.height;
    libyuv::I420ToNV12(src, iw, src + w * h, iw / 2, src + w * h * 5 / 4, iw / 2, p, iw, p + w * h, iw, iw, ih);
  }
  buf->Unlock();
  buf->SetCurrentLength(static_cast<DWORD>(size));
  const LONGLONG time = frame.presentationTimestampNs / 100;
  if (SUCCEEDED(r = MFCreateSample(sample.put()))) r = sample->AddBuffer(buf.get());
  if (SUCCEEDED(r)) r = sample->SetSampleTime(time);
  if (SUCCEEDED(r)) r = sample->SetSampleDuration(10'000'000LL / s.config.fps);
  if (FAILED(r)) return fail(ErrorCode::Internal, s.info.name + ": input sample (" + hr(r) + ")");

  bool forced = false;
  if (s.keyframePending.exchange(false)) {
    VARIANT one = u4(1);
    forced = SUCCEEDED(s.codecApi->SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &one));
    if (!forced) s.keyframePending.store(true);  // try again on the next frame
  }
  if (r = s.mft->ProcessInput(s.in, sample.get(), 0); FAILED(r)) {
    if (forced) s.keyframePending.store(true);  // retry on the next frame instead of losing the request
    return fail(ErrorCode::Internal, s.info.name + ": ProcessInput (" + hr(r) + ")");
  }
  if (forced) ++s.forced;
  --s.inputRequests;
  ++s.framesIn;
  s.pending.push_back({.time = time, .sourceId = frame.sourceId, .captureNs = frame.captureTimestampNs,
                       .presentationNs = frame.presentationTimestampNs});
  if (s.pending.size() > kMaxPending) s.pending.pop_front();
  // Collect output now rather than on the next call (a frame interval of latency otherwise). Async MFTs usually ask for
  // the next input before the output is ready, so wait for the output itself, bounded by kOutputWait.
  const auto outBefore = s.framesOut.load();
  if (auto got = s.pump(kOutputWait, [&] { return s.framesOut.load() > outBefore; }, emit); !got) return die(got.error());
  return {};
}

void MfH264Encoder::requestKeyframe() noexcept {
  ++impl_->requests;
  impl_->keyframePending.store(true);
}

Result MfH264Encoder::setBitrate(std::uint32_t bitrateBps) {
  if (bitrateBps == 0 || bitrateBps > INT32_MAX / 2) return fail(ErrorCode::InvalidArgument, "bitrate out of range");
  impl_->bitrate.store(bitrateBps);
  impl_->bitrateChanged.store(true);  // applied on the encode thread
  return {};
}

EncoderStats MfH264Encoder::stats() const {
  const auto& s = *impl_;
  return {s.framesIn.load(), s.framesOut.load(), s.keyframes.load(), s.requests.load(), s.forced.load()};
}

}  // namespace eme
