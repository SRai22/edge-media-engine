#include <gtest/gtest.h>

#include "eme/config/engine_config.hpp"
#include "eme/core/graph.hpp"

using namespace eme;
using namespace std::chrono_literals;

TEST(Config, ShippedExampleLoads) {
  const auto cfg = loadConfig(EME_SOURCE_DIR "/configs/synthetic_streamer.json");
  ASSERT_TRUE(cfg) << cfg.error().message;
  ASSERT_EQ(cfg->pipelines.size(), 2u);
  const auto& p = cfg->pipelines[1];
  EXPECT_EQ(p.name, "cam1");
  EXPECT_EQ(p.source.sourceId, 1u);  // defaults to the pipeline index
  EXPECT_EQ(p.source.width, 1280);
  EXPECT_EQ(p.encoder.width, 1280);  // encoder follows the source
  EXPECT_EQ(p.encoder.fps, 15);
  EXPECT_EQ(p.encoder.bitrateBps, 1'200'000u);
  ASSERT_EQ(p.sinks.size(), 3u);
  EXPECT_EQ(p.sinks[2].name, "slow");
  EXPECT_EQ(p.sinks[2].config.queueCapacity, 2u);
  EXPECT_EQ(p.sinks[2].config.perFrameDelay, 150ms);
}

TEST(Config, AllFieldsParse) {
  const auto cfg = parseConfig(R"({"pipelines": [{"name": "a",
      "source": {"sourceId": 7, "width": 640, "height": 480, "fps": 30, "pixelFormat": "I420", "poolSize": 6,
                 "pattern": "moving-texture", "timestampBarcode": true},
      "encoder": {"bitrateBps": 500000, "keyframeIntervalFrames": 60, "inputQueueCapacity": 3, "minKeyframeIntervalMs": 0},
      "sinks": [{"name": "s", "queueCapacity": 16, "perFrameDelayMs": 5}]}]})");
  ASSERT_TRUE(cfg) << cfg.error().message;
  const auto& p = cfg->pipelines[0];
  EXPECT_EQ(p.source.sourceId, 7u);
  EXPECT_EQ(p.source.pixelFormat, PixelFormat::I420);
  EXPECT_EQ(p.source.pattern, SyntheticPattern::MovingTexture);
  EXPECT_EQ(p.source.poolSize, 6u);
  EXPECT_TRUE(p.source.timestampBarcode);
  EXPECT_EQ(p.encoder.height, 480);
  EXPECT_EQ(p.encoder.keyframeIntervalFrames, 60);
  EXPECT_EQ(p.encoderNode.inputQueueCapacity, 3u);
  EXPECT_EQ(p.encoderNode.minKeyframeInterval, 0ms);
  EXPECT_EQ(p.sinks[0].config.queueCapacity, 16u);
  EXPECT_EQ(p.sinks[0].config.perFrameDelay, 5ms);
}

// Each case: a config with one bad field, and the path + reason the error must name.
struct BadCase {
  const char* json;
  const char* expected;
};

class ConfigRejects : public ::testing::TestWithParam<BadCase> {};

TEST_P(ConfigRejects, NamesTheField) {
  const auto cfg = parseConfig(GetParam().json);
  ASSERT_FALSE(cfg) << GetParam().json;
  EXPECT_EQ(cfg.error().code, ErrorCode::InvalidArgument);
  EXPECT_NE(cfg.error().message.find(GetParam().expected), std::string::npos) << cfg.error().message;
}

#define PIPE(extra) R"({"pipelines": [{"name": "a", "sinks": [{"name": "s"}])" extra "}]}"
INSTANTIATE_TEST_SUITE_P(
    Fields, ConfigRejects,
    ::testing::Values(
        BadCase{"{", "not valid JSON"}, BadCase{"[]", "<root>: must be an object"},
        BadCase{"{}", "pipelines: must be a non-empty array"}, BadCase{R"({"pipelines": []})", "pipelines: must be a non-empty array"},
        BadCase{R"({"pipelines": [], "extra": 1})", "extra: unknown field"},
        BadCase{R"({"pipelines": [{"sinks": [{"name": "s"}]}]})", "pipelines[0].name: is required"},
        BadCase{R"({"pipelines": [{"name": "", "sinks": [{"name": "s"}]}]})", "pipelines[0].name: must be a non-empty string"},
        BadCase{R"({"pipelines": [{"name": "a", "sinks": [{"name": "s"}]}, {"name": "a", "sinks": [{"name": "s"}]}]})",
                "pipelines[1].name: duplicate pipeline name"},
        BadCase{R"({"pipelines": [{"name": "a", "sinks": [{"name": "s"}]}, {"name": "b", "source": {"sourceId": 0}, "sinks": [{"name": "s"}]}]})",
                "pipelines[1].source.sourceId: duplicate sourceId 0"},
        BadCase{PIPE(R"(, "typo": 1)"), "pipelines[0].typo: unknown field"},
        BadCase{R"({"pipelines": [{"name": "a"}]})", "pipelines[0].sinks: must be a non-empty array"},
        BadCase{R"({"pipelines": [{"name": "a", "sinks": []}]})", "pipelines[0].sinks: must be a non-empty array"},
        BadCase{PIPE(R"(, "source": 3)"), "pipelines[0].source: must be an object"},
        BadCase{PIPE(R"(, "source": {"sourceId": -1})"), "pipelines[0].source.sourceId: must be in [0, 4294967295]"},
        BadCase{PIPE(R"(, "source": {"width": 2})"), "pipelines[0].source.width: must be in [4, 7680]"},
        BadCase{PIPE(R"(, "source": {"sourceId": 4294967296})"), "pipelines[0].source.sourceId: must be in"},
        BadCase{PIPE(R"(, "source": {"fps": true})"), "pipelines[0].source.fps: must be an integer"},
        BadCase{PIPE(R"(, "source": {"fps": null})"), "pipelines[0].source.fps: must be an integer"},
        BadCase{PIPE(R"(, "source": {"width": 7680, "height": 4320, "poolSize": 64})"), "pipelines[0].source.poolSize: frame pool would exceed"},
        BadCase{R"({"pipelines": [{"name": 5, "sinks": [{"name": "s"}]}]})", "pipelines[0].name: must be a non-empty string"},
        BadCase{R"({"pipelines": [{"name": "a/b", "sinks": [{"name": "s"}]}]})", "pipelines[0].name: must not contain '/'"},
        BadCase{R"({"pipelines": [{"name": "a", "sinks": [{"name": "h264"}]}]})", "pipelines[0].sinks[0].name: 'h264' is the encoder"},
        BadCase{R"({"pipelines": [3]})", "pipelines[0]: must be an object"},
        BadCase{R"({"pipelines": [{"name": "a", "sinks": {"name": "s"}}]})", "pipelines[0].sinks: must be a non-empty array"},
        BadCase{PIPE(R"(, "encoder": 3)"), "pipelines[0].encoder: must be an object"},
        BadCase{PIPE(R"(, "encoder": {"minKeyframeIntervalMs": 60001})"), "pipelines[0].encoder.minKeyframeIntervalMs: must be in"},
        BadCase{R"({"pipelines": [{"name": "a", "sinks": [{"name": "s", "queueCapacity": 1025}]}]})", "pipelines[0].sinks[0].queueCapacity: must be in"},
        BadCase{PIPE(R"(, "source": {"width": 641})"), "pipelines[0].source.width: must be even"},
        BadCase{PIPE(R"(, "source": {"height": 9999})"), "pipelines[0].source.height: must be in [4, 4320]"},
        BadCase{PIPE(R"(, "source": {"height": 481})"), "pipelines[0].source.height: must be even"},
        BadCase{PIPE(R"(, "source": {"fps": 0})"), "pipelines[0].source.fps: must be in [1, 240]"},
        BadCase{PIPE(R"(, "source": {"fps": 15.5})"), "pipelines[0].source.fps: must be an integer"},
        BadCase{PIPE(R"(, "source": {"fps": "15"})"), "pipelines[0].source.fps: must be an integer"},
        BadCase{PIPE(R"(, "source": {"poolSize": 0})"), "pipelines[0].source.poolSize: must be in [1, 64]"},
        BadCase{PIPE(R"(, "source": {"pixelFormat": "YUY2"})"), "pipelines[0].source.pixelFormat: must be one of: NV12, I420"},
        BadCase{PIPE(R"(, "source": {"pattern": "bars"})"), "pipelines[0].source.pattern: must be one of"},
        BadCase{PIPE(R"(, "source": {"timestampBarcode": 1})"), "pipelines[0].source.timestampBarcode: must be true or false"},
        BadCase{PIPE(R"(, "source": {"width": 320, "height": 240, "timestampBarcode": true})"),
                "pipelines[0].source.timestampBarcode: needs width >= 512"},
        BadCase{PIPE(R"(, "encoder": {"bitrateBps": 0})"), "pipelines[0].encoder.bitrateBps: must be in [10000, 100000000]"},
        BadCase{PIPE(R"(, "encoder": {"bitrateBps": 18446744073709551615})"), "pipelines[0].encoder.bitrateBps: must be in"},
        BadCase{PIPE(R"(, "encoder": {"keyframeIntervalFrames": 0})"), "pipelines[0].encoder.keyframeIntervalFrames: must be in [1, 3600]"},
        BadCase{PIPE(R"(, "encoder": {"inputQueueCapacity": 0})"), "pipelines[0].encoder.inputQueueCapacity: must be in [1, 64]"},
        BadCase{PIPE(R"(, "encoder": {"minKeyframeIntervalMs": -1})"), "pipelines[0].encoder.minKeyframeIntervalMs: must be in [0, 60000]"},
        BadCase{PIPE(R"(, "encoder": {"gop": 30})"), "pipelines[0].encoder.gop: unknown field"},
        BadCase{R"({"pipelines": [{"name": "a", "sinks": [{}]}]})", "pipelines[0].sinks[0].name: is required"},
        BadCase{R"({"pipelines": [{"name": "a", "sinks": [{"name": "s"}, {"name": "s"}]}]})", "pipelines[0].sinks[1].name: duplicate sink name"},
        BadCase{R"({"pipelines": [{"name": "a", "sinks": [{"name": "s", "queueCapacity": 0}]}]})",
                "pipelines[0].sinks[0].queueCapacity: must be in [1, 1024]"},
        BadCase{R"({"pipelines": [{"name": "a", "sinks": [{"name": "s", "perFrameDelayMs": -5}]}]})",
                "pipelines[0].sinks[0].perFrameDelayMs: must be in [0, 10000]"},
        BadCase{R"({"pipelines": [{"name": "a", "sinks": ["s"]}]})", "pipelines[0].sinks[0]: must be an object"}));

TEST(Config, MissingFileIsAnError) {
  const auto cfg = loadConfig("/nonexistent/eme.json");
  ASSERT_FALSE(cfg);
  EXPECT_NE(cfg.error().message.find("cannot read"), std::string::npos);
}

TEST(Config, DefaultsWhenSectionsOmitted) {
  const auto cfg = parseConfig(R"({"pipelines": [{"name": "a", "sinks": [{"name": "s"}]}]})");
  ASSERT_TRUE(cfg) << cfg.error().message;
  const auto& p = cfg->pipelines[0];
  EXPECT_EQ(p.source.width, SyntheticSourceConfig{}.width);
  EXPECT_EQ(p.encoder.bitrateBps, EncoderConfig{}.bitrateBps);
  EXPECT_EQ(p.encoderNode.inputQueueCapacity, EncoderNodeConfig{}.inputQueueCapacity);
  EXPECT_EQ(p.sinks[0].config.queueCapacity, mock::CountingSink::Config{}.queueCapacity);
}

// What the loader accepts, the nodes must accept: the smallest and the largest allowed values start cleanly.
TEST(Config, AcceptedExtremesStart) {
  for (const char* src : {R"({"width": 4, "height": 4, "fps": 1, "poolSize": 1})",
                          R"({"width": 7680, "height": 4320, "fps": 240, "poolSize": 1, "pixelFormat": "I420"})"}) {
    const auto cfg = parseConfig(std::string(R"({"pipelines": [{"name": "a", "source": )") + src +
                                 R"(, "sinks": [{"name": "s", "queueCapacity": 1}]}]})");
    ASSERT_TRUE(cfg) << cfg.error().message;
    MediaGraph g;
    auto& s = g.add<SyntheticVideoSource>("a", cfg->pipelines[0].source);
    EXPECT_TRUE(s.start()) << src;
    s.stop();
  }
}
