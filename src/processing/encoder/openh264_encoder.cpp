#include "eme/processing/openh264_encoder.hpp"

#include <wels/codec_api.h>

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstring>
#include <string>

namespace eme {

std::expected<std::unique_ptr<OpenH264Encoder>, Error> OpenH264Encoder::create(EncoderConfig c) {
  if (c.width <= 0 || c.height <= 0 || c.width % 2 || c.height % 2 || c.fps <= 0 || c.bitrateBps == 0 ||
      c.bitrateBps > INT32_MAX / 2 ||
      c.keyframeIntervalFrames <= 0) {
    return fail(ErrorCode::InvalidArgument, "openh264: invalid encoder config");
  }
  ISVCEncoder* enc = nullptr;
  if (WelsCreateSVCEncoder(&enc) != 0 || !enc) return fail(ErrorCode::Unsupported, "openh264: create failed");

  SEncParamExt p;
  enc->GetDefaultParams(&p);
  p.iUsageType = CAMERA_VIDEO_REAL_TIME;
  p.iPicWidth = c.width;
  p.iPicHeight = c.height;
  p.fMaxFrameRate = static_cast<float>(c.fps);
  p.iTargetBitrate = static_cast<int>(c.bitrateBps);
  p.iMaxBitrate = static_cast<int>(std::min<std::uint64_t>(std::uint64_t{c.bitrateBps} * 3 / 2, INT32_MAX));
  p.iRCMode = RC_BITRATE_MODE;
  p.bEnableFrameSkip = false;  // never silently drop frames; drops are the queues' job
  p.uiIntraPeriod = static_cast<unsigned>(c.keyframeIntervalFrames);
  p.eSpsPpsIdStrategy = CONSTANT_ID;
  p.iEntropyCodingModeFlag = 0;  // CAVLC -> Constrained Baseline, decodable by every browser
  p.iMultipleThreadIdc = 1;
  p.iSpatialLayerNum = 1;
  p.iTemporalLayerNum = 1;
  auto& layer = p.sSpatialLayers[0];
  layer.iVideoWidth = c.width;
  layer.iVideoHeight = c.height;
  layer.fFrameRate = static_cast<float>(c.fps);
  layer.iSpatialBitrate = p.iTargetBitrate;
  layer.iMaxSpatialBitrate = p.iMaxBitrate;
  layer.sSliceArgument.uiSliceMode = SM_SINGLE_SLICE;
  if (const int rc = enc->InitializeExt(&p); rc != cmResultSuccess) {
    WelsDestroySVCEncoder(enc);
    return fail(ErrorCode::Unsupported, "openh264: initialize failed (" + std::to_string(rc) + ")");
  }
  int format = videoFormatI420;
  enc->SetOption(ENCODER_OPTION_DATAFORMAT, &format);
  return std::unique_ptr<OpenH264Encoder>(new OpenH264Encoder(c, enc));
}

OpenH264Encoder::OpenH264Encoder(EncoderConfig config, ISVCEncoder* encoder)
    : config_(config), encoder_(encoder), bitrate_(config.bitrateBps) {}

OpenH264Encoder::~OpenH264Encoder() {
  encoder_->Uninitialize();
  WelsDestroySVCEncoder(encoder_);
}

EncoderConfig OpenH264Encoder::config() const {
  auto c = config_;
  c.bitrateBps = bitrate_.load();
  return c;
}

bool OpenH264Encoder::supportsInput(const VideoFormat& f) const {
  return f.kind == MediaKind::RawVideo && f.pixelFormat == PixelFormat::I420 && f.width == config_.width &&
         f.height == config_.height;
}

Result OpenH264Encoder::encode(const VideoFrame& frame, const EmitFn& emit) {
  if (!frame.buffer || frame.pixelFormat != PixelFormat::I420 || frame.width != config_.width ||
      frame.height != config_.height) {
    return fail(ErrorCode::InvalidArgument, "openh264: frame does not match encoder config");
  }
  const auto data = frame.buffer->cpuData();
  const auto w = static_cast<std::size_t>(frame.width), h = static_cast<std::size_t>(frame.height);
  if (data.size() < w * h * 3 / 2) return fail(ErrorCode::InvalidArgument, "openh264: frame needs a CPU I420 buffer");
  ++framesIn_;

  if (bitrateChanged_.exchange(false)) {
    const int target = static_cast<int>(bitrate_.load());
    SBitrateInfo maxInfo{SPATIAL_LAYER_ALL, target / 2 * 3};  // raise the ceiling first, or a higher target is clamped
    SBitrateInfo info{SPATIAL_LAYER_ALL, target};
    if (encoder_->SetOption(ENCODER_OPTION_MAX_BITRATE, &maxInfo) != cmResultSuccess ||
        encoder_->SetOption(ENCODER_OPTION_BITRATE, &info) != cmResultSuccess) {
      return fail(ErrorCode::Internal, "openh264: bitrate change rejected");
    }
  }
  const bool forced = keyframePending_.exchange(false);
  if (forced) {
    encoder_->ForceIntraFrame(true);
    ++forced_;
  }

  SSourcePicture pic{};
  pic.iColorFormat = videoFormatI420;
  pic.iPicWidth = frame.width;
  pic.iPicHeight = frame.height;
  pic.iStride[0] = frame.width;
  pic.iStride[1] = pic.iStride[2] = frame.width / 2;
  // OpenH264 takes non-const planes but does not write them.
  auto* y = const_cast<unsigned char*>(reinterpret_cast<const unsigned char*>(data.data()));
  pic.pData[0] = y;
  pic.pData[1] = y + w * h;
  pic.pData[2] = y + w * h + w * h / 4;
  pic.uiTimeStamp = frame.presentationTimestampNs / 1'000'000;

  SFrameBSInfo out{};
  if (const int rc = encoder_->EncodeFrame(&pic, &out); rc != cmResultSuccess) {
    if (forced) keyframePending_.store(true);  // retry on the next frame instead of losing the request
    return fail(ErrorCode::Internal, "openh264: encode failed (" + std::to_string(rc) + ")");
  }
  if (out.eFrameType == videoFrameTypeSkip || out.eFrameType == videoFrameTypeInvalid) return {};

  auto payload = std::make_shared<EncodedBuffer>();
  for (int l = 0; l < out.iLayerNum; ++l) {
    const auto& layer = out.sLayerInfo[l];
    std::size_t size = 0;
    for (int n = 0; n < layer.iNalCount; ++n) size += static_cast<std::size_t>(layer.pNalLengthInByte[n]);
    const auto* bytes = reinterpret_cast<const std::byte*>(layer.pBsBuf);
    payload->bytes.insert(payload->bytes.end(), bytes, bytes + size);
  }
  const bool key = out.eFrameType == videoFrameTypeIDR;
  if (key) ++keyframes_;
  emit(EncodedVideoFrame{
      .sequence = sequence_++,
      .sourceId = frame.sourceId,
      .codec = VideoCodec::H264,
      .captureTimestampNs = frame.captureTimestampNs,
      .presentationTimestampNs = frame.presentationTimestampNs,
      .isKeyframe = key,
      .payload = std::move(payload),
      .codecConfig = {},  // SPS/PPS are in-band before every IDR
  });
  ++framesOut_;
  return {};
}

void OpenH264Encoder::requestKeyframe() noexcept {
  ++requests_;
  keyframePending_.store(true);
}

Result OpenH264Encoder::setBitrate(std::uint32_t bitrateBps) {
  if (bitrateBps == 0 || bitrateBps > INT32_MAX / 2) return fail(ErrorCode::InvalidArgument, "bitrate out of range");
  bitrate_.store(bitrateBps);
  bitrateChanged_.store(true);  // applied on the encode thread
  return {};
}

EncoderStats OpenH264Encoder::stats() const {
  return {framesIn_.load(), framesOut_.load(), keyframes_.load(), requests_.load(), forced_.load()};
}

}  // namespace eme
