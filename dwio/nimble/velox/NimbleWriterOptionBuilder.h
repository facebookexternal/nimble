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

#include "dwio/nimble/encodings/EncodingSelectionPolicy.h"
#include "dwio/nimble/velox/EncodingLayoutTree.h"
#include "dwio/nimble/velox/VeloxWriterOptions.h"

#include "velox/common/memory/MemoryArbitrator.h"

#include <memory>

namespace facebook::dwio::api {
class NimbleWriterOptionBuilder {
 public:
  NimbleWriterOptionBuilder& withDefaultFlushPolicy(uint64_t rawStripeSize) {
    options_.flushPolicyFactory = [rawStripeSize]() {
      // The threshold of raw stripe size buffered before encoding stripes.
      return std::make_unique<nimble::RawStripeSizeFlushPolicy>(rawStripeSize);
    };
    return *this;
  }

  NimbleWriterOptionBuilder& withFlushPolicyFactory(
      std::function<std::unique_ptr<nimble::FlushPolicy>()>
          flushPolicyFactory) {
    options_.flushPolicyFactory = flushPolicyFactory;
    return *this;
  }

  NimbleWriterOptionBuilder& withDefaultEncodingSelectionPolicy(
      std::vector<std::pair<nimble::EncodingType, float>> readFactors,
      nimble::CompressionOptions compressionOptions);

  NimbleWriterOptionBuilder& withDefaultEncodingSelectionPolicy(
      const std::string& readFactorConfig,
      nimble::CompressionOptions compressionOptions);

  NimbleWriterOptionBuilder& withDefaultInputGrowthPolicy(
      std::map<uint64_t, float> rangedConfigs);

  // TODO: This is the integration point for nimble config.
  NimbleWriterOptionBuilder& withSerdeParams(
      const std::shared_ptr<const velox::RowType>& schema,
      const std::map<std::string, std::string>& serdeParams);

  NimbleWriterOptionBuilder& withFlatMapColumns(
      const std::vector<std::string>& flatMapCols);

  NimbleWriterOptionBuilder& withDictionaryArrayColumns(
      const std::vector<std::string>& dictionaryArrayCols);

  NimbleWriterOptionBuilder& withFeatureReordering(
      std::vector<std::tuple<size_t, std::vector<int64_t>>> featureReordering) {
    options_.featureReordering.emplace(std::move(featureReordering));
    return *this;
  }

  NimbleWriterOptionBuilder& withMetricsLogger(
      const std::shared_ptr<nimble::MetricsLogger>& metricsLogger) {
    options_.metricsLogger = metricsLogger;
    return *this;
  }

  NimbleWriterOptionBuilder& withEncodingLayoutTree(
      nimble::EncodingLayoutTree encodingLayoutTree) {
    options_.encodingLayoutTree.emplace(std::move(encodingLayoutTree));
    return *this;
  }

  NimbleWriterOptionBuilder& withReclaimerFactory(
      std::function<std::unique_ptr<velox::memory::MemoryReclaimer>()>
          reclaimerFactory) {
    options_.reclaimerFactory = std::move(reclaimerFactory);
    return *this;
  }

  NimbleWriterOptionBuilder& withSpillConfig(
      const velox::common::SpillConfig* spillConfig) {
    options_.spillConfig = spillConfig;
    return *this;
  }

  nimble::VeloxWriterOptions build() {
    return options_;
  }

 private:
  nimble::VeloxWriterOptions options_;
};

} // namespace facebook::dwio::api
