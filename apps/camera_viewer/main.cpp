// Example: every camera on this machine -> one H.264 encode each -> any number of browser tabs.
//
//   camera_viewer [--size 1280x720] [--fps 15] [--bitrate 1200000] [--port 8640] [--listen 127.0.0.1]
//                 [--max-viewers 4] [--test-pattern] [--seconds N] [--encoder auto|mf|openh264]
//
// --encoder auto (default): the Media Foundation hardware encoder where available (Windows), else OpenH264.
//
// Open the printed URL. Each extra tab is another WebRTC viewer of the same encoded streams; /api/stats shows
// that the encoded frame count does not grow with viewers.
#include <httplib.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <csignal>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "eme/core/graph.hpp"
#include "eme/processing/encoder_node.hpp"
#include "eme/processing/openh264_encoder.hpp"
#ifdef EME_HAVE_MF_ENCODER
#include "eme/processing/mf_h264_encoder.hpp"
#endif
#include "eme/sources/camera.hpp"
#include "eme/sources/synthetic_video_source.hpp"
#include "eme/transport/webrtc_peer_server.hpp"
#include "viewer_html.hpp"  // generated from viewer.html

using namespace eme;

namespace {

std::atomic<bool> gStop{false};

struct Options {
  int width = 1280, height = 720, fps = 15, port = 8640, maxViewers = 4, seconds = 0;
  std::uint32_t bitrate = 1'200'000;
  std::string listen = "127.0.0.1";
  std::string encoder = "auto";
  bool testPattern = false;
};

std::optional<Options> parse(int argc, char** argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> std::optional<std::string> { return i + 1 < argc ? std::optional(std::string(argv[++i])) : std::nullopt; };
    auto num = [](const std::string& v) {  // whole string must be a non-negative integer
      if (v.empty() || v.size() > 10 || !std::ranges::all_of(v, [](unsigned char c) { return std::isdigit(c); })) throw std::invalid_argument(v);
      return std::stoll(v);
    };
    try {
      if (a == "--size") {
        const auto v = next().value();
        const auto x = v.find('x');
        if (x == std::string::npos || num(v.substr(0, x)) > 7680 || num(v.substr(x + 1)) > 4320) return std::nullopt;
        o.width = static_cast<int>(num(v.substr(0, x)));
        o.height = static_cast<int>(num(v.substr(x + 1)));
      } else if (a == "--fps") o.fps = static_cast<int>(num(next().value()));
      else if (a == "--bitrate") {
        const auto b = num(next().value());
        if (b > 50'000'000) return std::nullopt;
        o.bitrate = static_cast<std::uint32_t>(b);
      } else if (a == "--port") o.port = static_cast<int>(std::min<long long>(num(next().value()), 70000));
      else if (a == "--listen") o.listen = next().value();
      else if (a == "--max-viewers") o.maxViewers = static_cast<int>(std::min<long long>(num(next().value()), 64));
      else if (a == "--seconds") o.seconds = static_cast<int>(std::min<long long>(num(next().value()), 86400 * 365));
      else if (a == "--test-pattern") o.testPattern = true;
      else if (a == "--encoder") {
        o.encoder = next().value();
        if (o.encoder != "auto" && o.encoder != "mf" && o.encoder != "openh264") return std::nullopt;
      }
      else return std::nullopt;
    } catch (const std::exception&) {
      return std::nullopt;
    }
  }
  const bool ok = o.width > 0 && o.height > 0 && o.width % 2 == 0 && o.height % 2 == 0 && o.fps > 0 && o.fps <= 60 &&
                  o.port > 0 && o.port < 65535 && o.maxViewers > 0 && o.bitrate > 0 && o.seconds >= 0;
  return ok ? std::optional(o) : std::nullopt;
}

std::string randomToken() {
  static constexpr char kAlphabet[] = "abcdefghijkmnopqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789";
  std::random_device rd;
  std::string t;
  for (int i = 0; i < 24; ++i) t += kAlphabet[rd() % (sizeof(kAlphabet) - 1)];
  return t;
}

struct Stream {
  std::string label;
  std::string mode;
  CameraSource* camera = nullptr;          // or
  SyntheticVideoSource* pattern = nullptr;
  EncoderNode* encoder = nullptr;
};

}  // namespace

int main(int argc, char** argv) {
  const auto opt = parse(argc, argv);
  if (!opt) {
    spdlog::error("usage: {} [--size WxH] [--fps N] [--bitrate bps] [--port N] [--listen ADDR] [--max-viewers N] [--test-pattern] [--seconds N] [--encoder auto|mf|openh264]", argv[0]);
    return 2;
  }
  const std::string token = randomToken();
  MediaGraph graph;
  auto& server = graph.add<WebRtcPeerServer>(
      "webrtc", WebRtcPeerServerConfig{.signalingPort = static_cast<std::uint16_t>(opt->port + 1),
                                       .bindAddress = opt->listen, .accessToken = token,
                                       .maxPeers = static_cast<std::size_t>(opt->maxViewers)});
  std::vector<Stream> streams;
  auto makeEncoder = [&](const VideoFormat& f) -> std::expected<std::unique_ptr<IVideoEncoder>, Error> {
    const EncoderConfig c{.width = f.width, .height = f.height, .fps = f.fps, .bitrateBps = opt->bitrate,
                          .keyframeIntervalFrames = f.fps * 2};
#ifdef EME_HAVE_MF_ENCODER
    if (opt->encoder != "openh264") {
      auto hw = MfH264Encoder::create(c);
      if (hw) return std::unique_ptr<IVideoEncoder>(std::move(*hw));
      if (opt->encoder == "mf") return std::unexpected(hw.error());
      spdlog::warn("hardware encoder unavailable ({}); using OpenH264", hw.error().message);
    }
#else
    if (opt->encoder == "mf") return fail(ErrorCode::Unsupported, "built without the Media Foundation encoder");
#endif
    auto sw = OpenH264Encoder::create(c);
    if (!sw) return std::unexpected(sw.error());
    spdlog::info("encoder: OpenH264 {}x{} @{} fps, {} bps", c.width, c.height, c.fps, c.bitrateBps);
    return std::unique_ptr<IVideoEncoder>(std::move(*sw));
  };
  auto addEncoder = [&](Node& source, std::unique_ptr<IVideoEncoder> enc, Stream s) -> bool {
    s.encoder = &graph.add<EncoderNode>(s.label + "/h264", std::move(enc));
    for (auto r : {graph.connect(source, *s.encoder), graph.connect(*s.encoder, server)}) {
      if (!r) return spdlog::error("{}: {}", s.label, r.error().message), false;
    }
    streams.push_back(std::move(s));
    return true;
  };

  std::uint32_t id = 0;
  for (const auto& cam : enumerateCameras()) {
    auto src = CameraSource::open(cam.name, {.id = cam.id, .sourceId = id, .width = opt->width, .height = opt->height, .fps = opt->fps});
    if (!src) {
      spdlog::warn("skipping {} ({}): {}", cam.name, cam.id, src.error().message);
      continue;
    }
    auto enc = makeEncoder(*(*src)->outputFormat());
    if (!enc) {
      spdlog::warn("skipping {}: {}", cam.name, enc.error().message);
      continue;
    }
    auto& node = graph.adopt(std::move(*src));
    if (!addEncoder(node, std::move(*enc), {.label = cam.name, .mode = node.negotiated(), .camera = &node})) return 1;
    ++id;
  }
  if (opt->testPattern) {
    auto& p = graph.add<SyntheticVideoSource>("test-pattern", SyntheticSourceConfig{
        .sourceId = id, .width = opt->width, .height = opt->height, .fps = opt->fps, .pixelFormat = PixelFormat::I420,
        .pattern = SyntheticPattern::MovingTexture});
    auto enc = makeEncoder(*p.outputFormat());
    if (!enc || !addEncoder(p, std::move(*enc), {.label = "Test pattern", .mode = "synthetic", .pattern = &p})) return 1;
  }
  if (streams.empty()) {
    spdlog::error("no usable camera found (try --test-pattern; check permissions on /dev/video*)");
    return 1;
  }
  // Page + small token-protected JSON API. Signalling (WebSocket) is on port+1.
  httplib::Server http;
  auto checkToken = [&](const httplib::Request& req, httplib::Response& res) {  // constant-time compare
    const auto t = req.get_param_value("token");
    unsigned char diff = t.size() == token.size() ? 0 : 1;
    for (std::size_t i = 0; i < std::min(t.size(), token.size()); ++i) diff |= static_cast<unsigned char>(t[i] ^ token[i]);
    if (diff) res.status = 403;
    return diff == 0;
  };
  http.Get("/", [](const httplib::Request&, httplib::Response& res) { res.set_content(kViewerHtml, "text/html; charset=utf-8"); });
  http.Get("/api/cameras", [&](const httplib::Request& req, httplib::Response& res) {
    if (!checkToken(req, res)) return;
    nlohmann::json j = nlohmann::json::array();
    for (std::size_t i = 0; i < streams.size(); ++i) j.push_back({{"mid", "video" + std::to_string(i)}, {"label", streams[i].label}, {"mode", streams[i].mode}});
    res.set_content(j.dump(), "application/json");
  });
  http.Get("/api/stats", [&](const httplib::Request& req, httplib::Response& res) {
    if (!checkToken(req, res)) return;
    const auto s = server.stats();
    nlohmann::json cams = nlohmann::json::array();
    for (const auto& st : streams) {
      const auto es = st.encoder->encoder().stats();
      nlohmann::json c{{"label", st.label}, {"encoded", es.framesOut}, {"keyframes", es.keyframes},
                       {"encoderInputDropped", st.encoder->inputStats().dropped}};  // frames the encoder fell behind on
      if (st.camera) {
        const auto cs = st.camera->stats();
        c["captured"] = cs.framesCaptured;
        c["dropped"] = cs.framesDropped;
        c["deviceLost"] = cs.deviceLost;
        c["reconnects"] = cs.reconnects;
      }
      cams.push_back(c);
    }
    std::uint64_t sent = 0;
    for (const auto& p : s.peers) sent += p.framesSent;
    res.set_content(nlohmann::json{{"viewers", s.peers.size()}, {"framesSentToViewers", sent}, {"cameras", cams}}.dump(),
                    "application/json");
  });
  if (!http.bind_to_port(opt->listen, opt->port)) {
    spdlog::error("cannot listen on {}:{} (in use?) - pick another with --port", opt->listen, opt->port);
    return 1;
  }
  if (auto r = graph.start(); !r) return spdlog::error("start: {} (signalling uses --port + 1)", r.error().message), 1;
  std::thread httpThread([&] { http.listen_after_bind(); });

  std::signal(SIGINT, [](int) { gStop = true; });
  std::signal(SIGTERM, [](int) { gStop = true; });
  const std::string host = opt->listen == "0.0.0.0" ? "<this-host>" : opt->listen;
  spdlog::info("{} stream(s) ready. Open: http://{}:{}/?token={}", streams.size(), host, opt->port, token);
  if (opt->listen != "127.0.0.1") spdlog::warn("listening beyond localhost without TLS: use only on a trusted network");
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(opt->seconds);
  while (!gStop && (opt->seconds == 0 || std::chrono::steady_clock::now() < deadline)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  http.stop();
  httpThread.join();
  graph.stop();
  return 0;
}
