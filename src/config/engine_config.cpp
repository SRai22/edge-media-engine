#include "eme/config/engine_config.hpp"

#include "eme/testing/barcode.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>

namespace eme {
namespace {

using nlohmann::json;

// One JSON object plus its path, for error messages.
class Obj {
 public:
  Obj(const json& j, std::string path) : j_(j), path_(std::move(path)) {}

  const std::string& path() const { return path_; }
  std::string at(std::string_view key) const { return path_.empty() ? std::string(key) : path_ + "." + std::string(key); }

  Result onlyKeys(std::initializer_list<std::string_view> allowed) const {
    if (!j_.is_object()) return bad(path_.empty() ? "<root>" : path_, "must be an object");
    for (const auto& [k, v] : j_.items()) {
      if (std::ranges::find(allowed, std::string_view(k)) == allowed.end()) return bad(at(k), "unknown field");
    }
    return {};
  }

  // Integer field within [lo, hi]; absent keeps `out`.
  template <class T>
  Result integer(std::string_view key, T& out, std::int64_t lo, std::int64_t hi) const {
    const auto* v = find(key);
    if (!v) return {};
    if (!v->is_number_integer()) return bad(at(key), "must be an integer");
    const bool tooBig = v->is_number_unsigned() && v->get<std::uint64_t>() > static_cast<std::uint64_t>(hi);
    const auto n = tooBig ? hi : v->get<std::int64_t>();
    if (tooBig || n < lo || n > hi) return bad(at(key), "must be in [" + std::to_string(lo) + ", " + std::to_string(hi) + "]");
    out = static_cast<T>(n);
    return {};
  }

  Result boolean(std::string_view key, bool& out) const {
    const auto* v = find(key);
    if (!v) return {};
    if (!v->is_boolean()) return bad(at(key), "must be true or false");
    out = v->get<bool>();
    return {};
  }

  // String field; `choices` empty = any non-empty string.
  Result string(std::string_view key, std::string& out, std::initializer_list<std::string_view> choices = {}) const {
    const auto* v = find(key);
    if (!v) return {};
    if (!v->is_string() || v->get_ref<const std::string&>().empty()) return bad(at(key), "must be a non-empty string");
    const auto& s = v->get_ref<const std::string&>();
    if (choices.size() && std::ranges::find(choices, std::string_view(s)) == choices.end()) {
      std::string list;
      for (auto c : choices) list += (list.empty() ? "" : ", ") + std::string(c);
      return bad(at(key), "must be one of: " + list);
    }
    out = s;
    return {};
  }

  const json* find(std::string_view key) const {
    const auto it = j_.find(key);
    return it == j_.end() ? nullptr : &*it;
  }

  static Result bad(const std::string& path, const std::string& why) { return fail(ErrorCode::InvalidArgument, path + ": " + why); }

 private:
  const json& j_;
  std::string path_;
};

// Returns the failure of a Result-producing check.
#define EME_TRY(expr)        \
  do {                       \
    if (auto r_ = (expr); !r_) return r_; \
  } while (0)

Result parseSource(const Obj& o, SyntheticSourceConfig& s) {
  EME_TRY(o.onlyKeys({"sourceId", "width", "height", "fps", "pixelFormat", "poolSize", "pattern", "timestampBarcode"}));
  EME_TRY(o.integer("sourceId", s.sourceId, 0, std::numeric_limits<std::uint32_t>::max()));
  EME_TRY(o.integer("width", s.width, 4, 7680));  // >= 4x4: the source stamps an 8-byte frame counter
  EME_TRY(o.integer("height", s.height, 4, 4320));
  if (s.width % 2) return Obj::bad(o.at("width"), "must be even");
  if (s.height % 2) return Obj::bad(o.at("height"), "must be even");
  EME_TRY(o.integer("fps", s.fps, 1, 240));
  EME_TRY(o.integer("poolSize", s.poolSize, 1, 64));
  // The pool is allocated up front: a typo like 7680x4320 x 64 must fail here, not as bad_alloc in the source.
  constexpr std::size_t kMaxPoolBytes = 512u << 20;
  if (static_cast<std::size_t>(s.width) * static_cast<std::size_t>(s.height) * 3 / 2 * s.poolSize > kMaxPoolBytes) {
    return Obj::bad(o.at("poolSize"), "frame pool would exceed 512 MiB (width x height x 1.5 x poolSize)");
  }
  std::string format = s.pixelFormat == PixelFormat::I420 ? "I420" : "NV12";
  EME_TRY(o.string("pixelFormat", format, {"NV12", "I420"}));
  s.pixelFormat = format == "I420" ? PixelFormat::I420 : PixelFormat::NV12;
  std::string pattern = s.pattern == SyntheticPattern::MovingTexture ? "moving-texture" : "moving-bar";
  EME_TRY(o.string("pattern", pattern, {"moving-bar", "moving-texture"}));
  s.pattern = pattern == "moving-texture" ? SyntheticPattern::MovingTexture : SyntheticPattern::MovingBar;
  EME_TRY(o.boolean("timestampBarcode", s.timestampBarcode));
  if (s.timestampBarcode && !mock::barcodeFits(s.width, s.height)) return Obj::bad(o.at("timestampBarcode"), "needs width >= 512 and height >= 128");
  return {};
}

Result parseEncoder(const Obj& o, EncoderConfig& e, EncoderNodeConfig& n) {
  EME_TRY(o.onlyKeys({"bitrateBps", "keyframeIntervalFrames", "inputQueueCapacity", "minKeyframeIntervalMs"}));
  EME_TRY(o.integer("bitrateBps", e.bitrateBps, 10'000, 100'000'000));  // 10 kbps .. 100 Mbps
  EME_TRY(o.integer("keyframeIntervalFrames", e.keyframeIntervalFrames, 1, 3600));
  EME_TRY(o.integer("inputQueueCapacity", n.inputQueueCapacity, 1, 64));
  auto ms = n.minKeyframeInterval.count();
  EME_TRY(o.integer("minKeyframeIntervalMs", ms, 0, 60'000));
  n.minKeyframeInterval = std::chrono::milliseconds(ms);
  return {};
}

Result parseSink(const Obj& o, SinkEntry& s) {
  EME_TRY(o.onlyKeys({"name", "queueCapacity", "perFrameDelayMs"}));
  if (!o.find("name")) return Obj::bad(o.at("name"), "is required");
  EME_TRY(o.string("name", s.name));
  if (s.name.contains('/')) return Obj::bad(o.at("name"), "must not contain '/'");
  if (s.name == "h264") return Obj::bad(o.at("name"), "'h264' is the encoder's node name");
  EME_TRY(o.integer("queueCapacity", s.config.queueCapacity, 1, 1024));
  std::int64_t ms = 0;
  EME_TRY(o.integer("perFrameDelayMs", ms, 0, 10'000));
  s.config.perFrameDelay = std::chrono::milliseconds(ms);
  return {};
}

std::expected<EngineConfig, Error> parse(const json& root) {
  const Obj top(root, "");
  if (auto r = top.onlyKeys({"pipelines"}); !r) return std::unexpected(r.error());
  const auto* list = top.find("pipelines");
  if (!list || !list->is_array() || list->empty()) return fail(ErrorCode::InvalidArgument, "pipelines: must be a non-empty array");
  EngineConfig cfg;
  std::set<std::string> names;
  std::set<std::uint32_t> ids;
  for (std::size_t i = 0; i < list->size(); ++i) {
    const std::string path = "pipelines[" + std::to_string(i) + "]";
    const Obj o((*list)[i], path);
    PipelineConfig p;
    p.source.sourceId = static_cast<std::uint32_t>(i);
    auto check = [&]() -> Result {
      EME_TRY(o.onlyKeys({"name", "source", "encoder", "sinks"}));
      if (!o.find("name")) return Obj::bad(o.at("name"), "is required");
      EME_TRY(o.string("name", p.name));
      if (p.name.contains('/')) return Obj::bad(o.at("name"), "must not contain '/' (node names are <pipeline>/<node>)");
      if (!names.insert(p.name).second) return Obj::bad(o.at("name"), "duplicate pipeline name '" + p.name + "'");
      if (const auto* s = o.find("source")) EME_TRY(parseSource(Obj(*s, o.at("source")), p.source));
      if (!ids.insert(p.source.sourceId).second) return Obj::bad(o.at("source.sourceId"), "duplicate sourceId " + std::to_string(p.source.sourceId));
      if (const auto* e = o.find("encoder")) EME_TRY(parseEncoder(Obj(*e, o.at("encoder")), p.encoder, p.encoderNode));
      p.encoder.width = p.source.width;
      p.encoder.height = p.source.height;
      p.encoder.fps = p.source.fps;
      const auto* sinks = o.find("sinks");
      if (!sinks || !sinks->is_array() || sinks->empty()) return Obj::bad(o.at("sinks"), "must be a non-empty array");
      std::set<std::string> sinkNames;
      for (std::size_t k = 0; k < sinks->size(); ++k) {
        SinkEntry s;
        const Obj so((*sinks)[k], o.at("sinks") + "[" + std::to_string(k) + "]");
        EME_TRY(parseSink(so, s));
        if (!sinkNames.insert(s.name).second) return Obj::bad(so.at("name"), "duplicate sink name '" + s.name + "'");
        p.sinks.push_back(std::move(s));
      }
      return {};
    };
    if (auto r = check(); !r) return std::unexpected(r.error());
    cfg.pipelines.push_back(std::move(p));
  }
  return cfg;
}

}  // namespace

std::expected<EngineConfig, Error> parseConfig(std::string_view text) {
  const json root = json::parse(text, nullptr, /*allow_exceptions=*/false, /*ignore_comments=*/true);
  if (root.is_discarded()) return fail(ErrorCode::InvalidArgument, "config is not valid JSON");
  return parse(root);
}

std::expected<EngineConfig, Error> loadConfig(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return fail(ErrorCode::InvalidArgument, path.string() + ": cannot read");
  std::ostringstream text;
  text << in.rdbuf();
  auto cfg = parseConfig(text.str());
  if (!cfg) return fail(cfg.error().code, path.string() + ": " + cfg.error().message);
  return cfg;
}

}  // namespace eme
