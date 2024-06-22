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

#include "dwio/nimble/velox/NimbleReader.h"

#include "dwio/nimble/tablet/Constants.h"
#include "dwio/nimble/velox/VeloxUtil.h"

namespace facebook::velox::nimble {

namespace {

const std::vector<std::string> kPreloadOptionalSections = {
    std::string(facebook::nimble::kSchemaSection)};

class NimbleRowReader : public dwio::common::RowReader {
 public:
  NimbleRowReader(
      std::unique_ptr<facebook::nimble::VeloxReader> reader,
      const std::shared_ptr<common::ScanSpec>& scanSpec)
      : reader_(std::move(reader)), scanSpec_(scanSpec) {
    reader_->loadStripeIfAny();
  }

  int64_t nextRowNumber() override {
    VELOX_NYI();
  }

  int64_t nextReadSize(uint64_t /*size*/) override {
    VELOX_NYI();
  }

  uint64_t next(
      uint64_t size,
      VectorPtr& result,
      const dwio::common::Mutation* mutation) override {
    TypePtr resultType;
    VectorPtr rawResult;
    if (result) {
      resultType = result->type();
      rawResult = std::move(rawVectorForBatchReader(*result));
      result.reset();
    }
    if (!reader_->next(size, rawResult)) {
      if (rawResult) {
        result = BaseVector::create(resultType, 0, &reader_->memoryPool());
        rawVectorForBatchReader(*result) = std::move(rawResult);
      }
      return 0;
    }
    auto scanned = rawResult->size();
    result = projectColumns(rawResult, *scanSpec_, mutation);
    rawVectorForBatchReader(*result) = std::move(rawResult);
    return scanned;
  }

  void updateRuntimeStats(
      dwio::common::RuntimeStatistics& /*stats*/) const override {
    // No-op for non-selective reader.
  }

  void resetFilterCaches() override {
    // No-op for non-selective reader.
  }

  std::optional<size_t> estimatedRowSize() const override {
    return std::optional(reader_->estimatedRowSize());
  }

 private:
  std::unique_ptr<facebook::nimble::VeloxReader> reader_;
  std::shared_ptr<common::ScanSpec> scanSpec_;

  static VectorPtr& rawVectorForBatchReader(BaseVector& vector) {
    auto* rowVector = vector.as<RowVector>();
    VELOX_CHECK_NOT_NULL(rowVector);
    return rowVector->rawVectorForBatchReader();
  }
};

class NimbleReader : public dwio::common::Reader {
 public:
  NimbleReader(
      const dwio::common::ReaderOptions& options,
      const std::shared_ptr<ReadFile>& readFile)
      : options_(options),
        readFile_(readFile),
        tabletReader_(std::make_shared<facebook::nimble::TabletReader>(
            options.getMemoryPool(),
            readFile_.get(),
            kPreloadOptionalSections)) {
    if (!options_.getFileSchema()) {
      facebook::nimble::VeloxReader tmpReader(
          options.getMemoryPool(), tabletReader_);
      options_.setFileSchema(tmpReader.type());
    }
  }

  std::optional<uint64_t> numberOfRows() const override {
    return tabletReader_->tabletRowCount();
  }

  std::unique_ptr<dwio::common::ColumnStatistics> columnStatistics(
      uint32_t /*index*/) const override {
    // TODO
    return nullptr;
  }

  const RowTypePtr& rowType() const override {
    return options_.getFileSchema();
  }

  const std::shared_ptr<const dwio::common::TypeWithId>& typeWithId()
      const override {
    if (!typeWithId_) {
      typeWithId_ = dwio::common::TypeWithId::create(rowType());
    }
    return typeWithId_;
  }

  std::unique_ptr<dwio::common::RowReader> createRowReader(
      const dwio::common::RowReaderOptions& options) const override {
    facebook::nimble::VeloxReadParams params;
    params.fileRangeStartOffset = options.getOffset();
    params.fileRangeEndOffset = options.getLimit();
    params.decodingExecutor = options.getDecodingExecutor();
    auto selector = options.getSelector();
    if (!selector) {
      selector = std::make_shared<dwio::common::ColumnSelector>(rowType());
    }
    facebook::dwio::api::populateFeatureSelector(
        *selector, options.getMapColumnIdAsStruct(), params);
    auto reader = std::make_unique<facebook::nimble::VeloxReader>(
        options_.getMemoryPool(),
        tabletReader_,
        std::move(selector),
        std::move(params));
    return std::make_unique<NimbleRowReader>(
        std::move(reader), options.getScanSpec());
  }

 private:
  dwio::common::ReaderOptions options_;
  std::shared_ptr<ReadFile> readFile_;
  std::shared_ptr<facebook::nimble::TabletReader> tabletReader_;
  mutable std::shared_ptr<const dwio::common::TypeWithId> typeWithId_;
};

} // namespace

std::unique_ptr<dwio::common::Reader> NimbleReaderFactory::createReader(
    std::unique_ptr<dwio::common::BufferedInput> input,
    const dwio::common::ReaderOptions& options) {
  return std::make_unique<NimbleReader>(options, input->getReadFile());
}

void registerNimbleReaderFactory() {
  dwio::common::registerReaderFactory(std::make_shared<NimbleReaderFactory>());
}

void unregisterNimbleReaderFactory() {
  dwio::common::unregisterReaderFactory(dwio::common::FileFormat::NIMBLE);
}

} // namespace facebook::velox::nimble
