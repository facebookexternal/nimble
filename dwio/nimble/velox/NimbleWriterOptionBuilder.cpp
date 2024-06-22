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

#include "dwio/nimble/velox/NimbleWriterOptionBuilder.h"

#include "dwio/nimble/common/Exceptions.h"
#include "dwio/nimble/velox/NimbleConfig.h"

#include <folly/compression/Compression.h>

#include <algorithm>
#include <iterator>
#include <string>

namespace facebook::dwio::api {
NimbleWriterOptionBuilder& NimbleWriterOptionBuilder::withSerdeParams(
    const std::shared_ptr<const velox::RowType>& schema,
    const std::map<std::string, std::string>& serdeParams) {
  auto config = nimble::Config::fromMap(serdeParams);
  if (config->get(nimble::Config::FLATTEN_MAP)) {
    auto flatMapCols = config->get(nimble::Config::MAP_FLAT_COLS);
    options_.flatMapColumns.clear();
    options_.flatMapColumns.reserve(flatMapCols.size());
    for (auto col : flatMapCols) {
      LOG(INFO) << fmt::format(
          "adding flat map column {} with schema width {}",
          col,
          schema->children().size());
      options_.flatMapColumns.insert(schema->nameOf(col));
    }
  }
  withDefaultFlushPolicy(config->get(nimble::Config::RAW_STRIPE_SIZE));

  auto batchReuseCols = config->get(nimble::Config::BATCH_REUSE_COLS);
  options_.dictionaryArrayColumns.clear();
  options_.dictionaryArrayColumns.reserve(batchReuseCols.size());
  for (const auto& colIdx : batchReuseCols) {
    options_.dictionaryArrayColumns.insert(schema->nameOf(colIdx));
  }

  auto deduplicatedMapCols = config->get(nimble::Config::DEDUPLICATED_COLS);
  options_.deduplicatedMapColumns.clear();
  options_.deduplicatedMapColumns.reserve(deduplicatedMapCols.size());
  for (const auto& colIdx : deduplicatedMapCols) {
    options_.deduplicatedMapColumns.insert(schema->nameOf(colIdx));
  }

  withDefaultInputGrowthPolicy(
      config->get(nimble::Config::INPUT_BUFFER_DEFAULT_GROWTH_CONFIGS));

  options_.compressionOptions.compressionAcceptRatio =
      config->get(nimble::Config::ENCODING_SELECTION_COMPRESSION_ACCEPT_RATIO);
  options_.compressionOptions.useVariableBitWidthCompressor =
      config->get(nimble::Config::ENABLE_ZSTRONG_VARIABLE_BITWIDTH_COMPRESSOR);
  options_.compressionOptions.internalCompressionLevel =
      config->get(nimble::Config::ZSTRONG_COMPRESSION_LEVEL);
  options_.compressionOptions.internalDecompressionLevel =
      config->get(nimble::Config::ZSTRONG_DECOMPRESSION_LEVEL);

  return withDefaultEncodingSelectionPolicy(
      config->get(nimble::Config::MANUAL_ENCODING_SELECTION_READ_FACTORS),
      options_.compressionOptions);
}

NimbleWriterOptionBuilder&
NimbleWriterOptionBuilder::withDefaultEncodingSelectionPolicy(
    std::vector<std::pair<nimble::EncodingType, float>> readFactors,
    nimble::CompressionOptions compressionOptions) {
  options_.encodingSelectionPolicyFactory =
      [encodingFactory =
           nimble::ManualEncodingSelectionPolicyFactory{
               std::move(readFactors), std::move(compressionOptions)}](
          nimble::DataType dataType)
      -> std::unique_ptr<nimble::EncodingSelectionPolicyBase> {
    return encodingFactory.createPolicy(dataType);
  };
  return *this;
}

NimbleWriterOptionBuilder&
NimbleWriterOptionBuilder::withDefaultEncodingSelectionPolicy(
    const std::string& readFactorConfig,
    nimble::CompressionOptions compressionOptions) {
  return withDefaultEncodingSelectionPolicy(
      nimble::ManualEncodingSelectionPolicyFactory::parseReadFactors(
          readFactorConfig),
      std::move(compressionOptions));
}

NimbleWriterOptionBuilder&
NimbleWriterOptionBuilder::withDefaultInputGrowthPolicy(
    std::map<uint64_t, float> rangedConfigs) {
  options_.inputGrowthPolicyFactory = [rangedConfigs =
                                           std::move(rangedConfigs)]() mutable
      -> std::unique_ptr<nimble::InputBufferGrowthPolicy> {
    return std::make_unique<nimble::DefaultInputBufferGrowthPolicy>(
        std::move(rangedConfigs));
  };
  return *this;
}

NimbleWriterOptionBuilder& NimbleWriterOptionBuilder::withFlatMapColumns(
    const std::vector<std::string>& flatMapCols) {
  options_.flatMapColumns.clear();
  for (const auto& col : flatMapCols) {
    options_.flatMapColumns.insert(col);
  }
  return *this;
}

NimbleWriterOptionBuilder&
NimbleWriterOptionBuilder::withDictionaryArrayColumns(
    const std::vector<std::string>& dictionaryArrayCols) {
  options_.dictionaryArrayColumns.clear();
  for (const auto& col : dictionaryArrayCols) {
    options_.dictionaryArrayColumns.insert(col);
  }
  return *this;
}
} // namespace facebook::dwio::api
