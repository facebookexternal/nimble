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
#include "dwio/nimble/velox/VeloxReader.h"

#include "velox/common/memory/MemoryPool.h"
#include "velox/dwio/common/tests/utils/BatchMaker.h"
#include "velox/exec/MemoryReclaimer.h"

#include <gtest/gtest.h>

namespace facebook::velox::nimble {
namespace {

class NimbleWriterTest : public testing::Test {
 protected:
  void SetUp() override {
    registerNimbleWriterFactory();
  }

  void TearDown() override {
    unregisterNimbleWriterFactory();
  }

  memory::MemoryPool& pool() {
    return *pool_;
  }

  memory::MemoryPool& rootPool() {
    return *aggrPool_;
  }

  auto factory() const {
    return dwio::common::getWriterFactory(dwio::common::FileFormat::NIMBLE);
  }

 private:
  std::shared_ptr<memory::MemoryPool> pool_ =
      memory::deprecatedAddDefaultLeafMemoryPool();
  std::shared_ptr<memory::MemoryPool> aggrPool_ =
      memory::deprecatedDefaultMemoryManager().addRootPool(
          "root_pool",
          memory::kMaxMemory,
          exec::MemoryReclaimer::create());
};

TEST_F(NimbleWriterTest, basic) {
  auto schema = ROW({
      {"i32", INTEGER()},
      {"string", VARCHAR()},
      {"struct", ROW({{"i32", INTEGER()}})},
  });
  auto expected = test::BatchMaker::createBatch(schema, 100, pool());
  std::string buf;
  dwio::common::WriterOptions options = {
      .schema = schema,
      .memoryPool = &rootPool(),
  };
  auto writer = factory()->createWriter(
      std::make_unique<dwio::common::WriteFileSink>(
          std::make_unique<InMemoryWriteFile>(&buf), "<in-memory>"),
      options);
  writer->write(expected);
  writer->close();
  InMemoryReadFile readFile(buf);
  facebook::nimble::VeloxReader reader(
      pool(),
      &readFile,
      std::make_shared<velox::dwio::common::ColumnSelector>(schema));
  VectorPtr actual;
  ASSERT_TRUE(reader.next(60, actual));
  ASSERT_EQ(actual->size(), 60);
  for (int i = 0; i < actual->size(); ++i) {
    ASSERT_TRUE(actual->equalValueAt(expected.get(), i, i));
  }
  ASSERT_TRUE(reader.next(60, actual));
  ASSERT_EQ(actual->size(), 40);
  for (int i = 0; i < actual->size(); ++i) {
    ASSERT_TRUE(actual->equalValueAt(expected.get(), i, 60 + i));
  }
  ASSERT_FALSE(reader.next(60, actual));
}

} // namespace
} // namespace facebook::velox::nimble
