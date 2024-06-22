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

#include "dwio/nimble/velox/NimbleWriter.h"

#include "dwio/nimble/velox/NimbleWriterOptionBuilder.h"
#include "dwio/nimble/velox/VeloxWriter.h"

#include "velox/exec/MemoryReclaimer.h"

namespace facebook::velox::nimble {

namespace {

class NimbleWriterAdapter : public dwio::common::Writer {
 public:
  NimbleWriterAdapter(
      memory::MemoryPool& memoryPool,
      const velox::TypePtr& schema,
      std::unique_ptr<WriteFile> file,
      facebook::nimble::VeloxWriterOptions options)
      : writer_(memoryPool, schema, std::move(file), std::move(options)) {}

  void write(const VectorPtr& data) override {
    writer_.write(data);
  }

  void flush() override {
    writer_.flush();
  }

  void close() override {
    writer_.close();
  }

  void abort() override {}

 private:
  facebook::nimble::VeloxWriter writer_;
};

} // namespace

std::unique_ptr<dwio::common::Writer> NimbleWriterFactory::createWriter(
    std::unique_ptr<dwio::common::FileSink> sink,
    const dwio::common::WriterOptions& options) {
  // TODO: Pass the sink directly to writer.
  auto* sinkWrapper = dynamic_cast<dwio::common::WriteFileSink*>(sink.get());
  VELOX_CHECK_NOT_NULL(
      sinkWrapper, "Expected WriteFileSink, got {}", typeid(*sink).name());

  return std::make_unique<NimbleWriterAdapter>(
      *options.memoryPool,
      options.schema,
      sinkWrapper->toWriteFile(),
      facebook::dwio::api::NimbleWriterOptionBuilder()
          .withSerdeParams(asRowType(options.schema), options.serdeParameters)
          .withReclaimerFactory(
              []() { return exec::MemoryReclaimer::create(); })
          .withSpillConfig(options.spillConfig)
          .build());
}

} // namespace facebook::velox::nimble
