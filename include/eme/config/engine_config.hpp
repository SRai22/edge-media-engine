#pragma once
// JSON engine configuration (EME-003). Every field is validated at load; errors name the field path,
// e.g. "pipelines[1].encoder.bitrateBps: must be > 0".

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "eme/core/types.hpp"
#include "eme/processing/encoder_node.hpp"
#include "eme/sources/synthetic_video_source.hpp"
#include "eme/testing/counting_sink.hpp"

namespace eme {

struct SinkEntry {
  std::string name;
  // shortcut: only the counting test sink is configurable; add a "type" field when real transports are config-driven.
  mock::CountingSink::Config config;
};

// One source -> one encoder -> N sinks. The encoder's size and rate follow the source.
struct PipelineConfig {
  std::string name;
  SyntheticSourceConfig source;
  EncoderConfig encoder;
  EncoderNodeConfig encoderNode;
  std::vector<SinkEntry> sinks;
};

struct EngineConfig {
  std::vector<PipelineConfig> pipelines;
};

// Unknown keys are rejected (typos must not silently fall back to defaults); missing keys take the defaults.
std::expected<EngineConfig, Error> parseConfig(std::string_view json);
std::expected<EngineConfig, Error> loadConfig(const std::filesystem::path& path);

}  // namespace eme
