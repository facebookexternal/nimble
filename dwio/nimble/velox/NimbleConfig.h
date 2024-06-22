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
#pragma once

#include "dwio/nimble/common/Types.h"

#include "velox/common/config/Config.h"

namespace facebook::nimble {

class Config : public velox::common::ConfigBase<Config> {
 public:
  template <typename T>
  using Entry = velox::common::ConfigBase<Config>::Entry<T>;

  static Entry<bool> FLATTEN_MAP;
  static Entry<const std::vector<uint32_t>> MAP_FLAT_COLS;
  static Entry<const std::vector<uint32_t>> BATCH_REUSE_COLS;
  static Entry<const std::vector<uint32_t>> DEDUPLICATED_COLS;
  static Entry<uint64_t> RAW_STRIPE_SIZE;
  static Entry<const std::vector<std::pair<EncodingType, float>>>
      MANUAL_ENCODING_SELECTION_READ_FACTORS;
  static Entry<float> ENCODING_SELECTION_COMPRESSION_ACCEPT_RATIO;
  static Entry<uint32_t> ZSTRONG_COMPRESSION_LEVEL;
  static Entry<uint32_t> ZSTRONG_DECOMPRESSION_LEVEL;
  static Entry<bool> ENABLE_ZSTRONG_VARIABLE_BITWIDTH_COMPRESSOR;
  static Entry<const std::map<uint64_t, float>>
      INPUT_BUFFER_DEFAULT_GROWTH_CONFIGS;

  static std::shared_ptr<Config> fromMap(
      const std::map<std::string, std::string>& map) {
    auto ret = std::make_shared<Config>();
    ret->configs_.insert(map.cbegin(), map.cend());
    return ret;
  }
};

} // namespace facebook::nimble
