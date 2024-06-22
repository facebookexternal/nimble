/*
 * Copyright (c) Meta Platforms, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "dwio/nimble/velox/NimbleConfig.h"

#include "dwio/nimble/encodings/EncodingSelectionPolicy.h"

#include <folly/json/dynamic.h>
#include <folly/json/json.h>

DEFINE_string(
    nimble_selection_read_factors,
    "Constant=1.0;Trivial=0.5;FixedBitWidth=0.9;MainlyConstant=1.0;SparseBool=1.0;Dictionary=1.0;RLE=1.0;Varint=1.0",
    "Encoding selection read factors, in the format: "
    "<EncodingName>=<FactorFloatValue>;<EncodingName>=<FactorFloatValue>;...");

DEFINE_double(
    nimble_selection_compression_accept_ratio,
    0.97,
    "Encoding selection compression accept ratio.");

DEFINE_bool(
    nimble_zstrong_enable_variable_bit_width_compressor,
    false,
    "Enable zstrong variable bit width compressor at write time. Transparent at read time.");

DEFINE_string(
    nimble_writer_input_buffer_default_growth_config,
    "{\"32\":4.0,\"512\":1.414,\"4096\":1.189}",
    "Default growth config for writer input buffers, each entry in the format of {range_start,growth_factor}");

namespace facebook::nimble {
namespace {
template <typename T>
std::vector<T> parseVector(const std::string& str) {
  std::vector<T> result;
  if (!str.empty()) {
    std::vector<folly::StringPiece> pieces;
    folly::split(',', str, pieces, true);
    for (auto& p : pieces) {
      const auto& trimmedCol = folly::trimWhitespace(p);
      if (!trimmedCol.empty()) {
        result.push_back(folly::to<T>(trimmedCol));
      }
    }
  }
  return result;
}

std::map<uint64_t, float> parseGrowthConfigMap(const std::string& str) {
  std::map<uint64_t, float> ret;
  NIMBLE_CHECK(!str.empty(), "Can't supply an empty growth config.");
  folly::dynamic json = folly::parseJson(str);
  for (const auto& pair : json.items()) {
    auto [_, inserted] = ret.emplace(
        folly::to<uint64_t>(pair.first.asString()), pair.second.asDouble());
    NIMBLE_CHECK(
        inserted, fmt::format("Duplicate key: {}.", pair.first.asString()));
  }
  return ret;
}
} // namespace

/* static */ Config::Entry<bool> Config::FLATTEN_MAP("orc.flatten.map", false);

/* static */ Config::Entry<const std::vector<uint32_t>> Config::MAP_FLAT_COLS(
    "orc.map.flat.cols",
    {},
    [](const std::vector<uint32_t>& val) { return folly::join(",", val); },
    [](const std::string& /* key */, const std::string& val) {
      return parseVector<uint32_t>(val);
    });

/* static */ Config::Entry<const std::vector<uint32_t>>
    Config::DEDUPLICATED_COLS(
        "alpha.map.deduplicated.cols",
        {},
        [](const std::vector<uint32_t>& val) { return folly::join(",", val); },
        [](const std::string& /* key */, const std::string& val) {
          return parseVector<uint32_t>(val);
        });

/* static */ Config::Entry<const std::vector<uint32_t>>
    Config::BATCH_REUSE_COLS(
        "alpha.dictionaryarray.cols",
        {},
        [](const std::vector<uint32_t>& val) { return folly::join(",", val); },
        [](const std::string& /* key */, const std::string& val) {
          return parseVector<uint32_t>(val);
        });

/* static */ Config::Entry<uint64_t> Config::RAW_STRIPE_SIZE(
    "alpha.raw.stripe.size",
    512L * 1024L * 1024L);

/* static */ Config::Entry<const std::vector<std::pair<EncodingType, float>>>
    Config::MANUAL_ENCODING_SELECTION_READ_FACTORS(
        "alpha.encodingselection.read.factors",
        ManualEncodingSelectionPolicyFactory::parseReadFactors(
            FLAGS_nimble_selection_read_factors),
        [](const std::vector<std::pair<EncodingType, float>>& val) {
          std::vector<std::string> encodingFactorStrings;
          std::transform(
              val.cbegin(),
              val.cend(),
              std::back_inserter(encodingFactorStrings),
              [](const auto& readFactor) {
                return fmt::format(
                    "{}={}", toString(readFactor.first), readFactor.second);
              });
          return folly::join(";", encodingFactorStrings);
        },
        [](const std::string& /* key */, const std::string& val) {
          return ManualEncodingSelectionPolicyFactory::parseReadFactors(val);
        });

/* static */ Config::Entry<float>
    Config::ENCODING_SELECTION_COMPRESSION_ACCEPT_RATIO(
        "alpha.encodingselection.compression.accept.ratio",
        FLAGS_nimble_selection_compression_accept_ratio);

/* static */ Config::Entry<uint32_t> Config::ZSTRONG_COMPRESSION_LEVEL(
    "alpha.zstrong.compression.level",
    4);

/* static */ Config::Entry<uint32_t> Config::ZSTRONG_DECOMPRESSION_LEVEL(
    "alpha.zstrong.decompression.level",
    2);

/* static */ Config::Entry<bool>
    Config::ENABLE_ZSTRONG_VARIABLE_BITWIDTH_COMPRESSOR(
        "alpha.zstrong.enable.variable.bit.width.compressor",
        FLAGS_nimble_zstrong_enable_variable_bit_width_compressor);

/* static */ Config::Entry<const std::map<uint64_t, float>>
    Config::INPUT_BUFFER_DEFAULT_GROWTH_CONFIGS(
        "alpha.writer.input.buffer.default.growth.configs",
        parseGrowthConfigMap(
            FLAGS_nimble_writer_input_buffer_default_growth_config),
        [](const std::map<uint64_t, float>& val) {
          folly::dynamic obj = folly::dynamic::object;
          for (const auto& [rangeStart, growthFactor] : val) {
            obj[folly::to<std::string>(rangeStart)] = growthFactor;
          }
          return folly::toJson(obj);
        },
        [](const std::string& /* key */, const std::string& val) {
          return parseGrowthConfigMap(val);
        });
} // namespace facebook::nimble
