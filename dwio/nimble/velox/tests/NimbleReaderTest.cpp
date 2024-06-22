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
#include "dwio/nimble/common/tests/NimbleFileWriter.h"
#include "dwio/nimble/velox/VeloxReader.h"

#include "velox/dwio/common/tests/utils/BatchMaker.h"
#include "velox/vector/tests/utils/VectorMaker.h"

#include <gtest/gtest.h>

namespace facebook::velox::nimble {
namespace {

class NimbleReaderTest : public testing::Test {
 protected:
  void SetUp() override {
    registerNimbleReaderFactory();
  }

  void TearDown() override {
    unregisterNimbleReaderFactory();
  }

  memory::MemoryPool& pool() {
    return *pool_;
  }

  memory::MemoryPool& rootPool() {
    return *rootPool_;
  }

 private:
  std::shared_ptr<memory::MemoryPool> rootPool_ =
      memory::deprecatedDefaultMemoryManager().addRootPool();
  std::shared_ptr<memory::MemoryPool> pool_ = rootPool_->addLeafChild("leaf");
};

void setScanSpec(const Type& type, dwio::common::RowReaderOptions& options) {
  auto spec = std::make_shared<common::ScanSpec>("root");
  spec->addAllChildFields(type);
  options.setScanSpec(spec);
}

TEST_F(NimbleReaderTest, basic) {
  auto type = ROW({{"i32", INTEGER()}});
  const auto numRows = 100;
  auto vec = test::BatchMaker::createBatch(type, numRows, pool());
  auto file = facebook::nimble::test::createNimbleFile(rootPool(), vec);
  auto readFile = std::make_shared<InMemoryReadFile>(file);
  auto factory =
      dwio::common::getReaderFactory(dwio::common::FileFormat::NIMBLE);
  auto input = std::make_unique<dwio::common::BufferedInput>(readFile, pool());
  dwio::common::ReaderOptions options(&pool());
  auto reader = factory->createReader(std::move(input), options);
  EXPECT_EQ(reader->numberOfRows(), numRows);
  EXPECT_EQ(*reader->rowType(), *type);

  dwio::common::RowReaderOptions rowOptions;
  setScanSpec(*type, rowOptions);
  auto rowReader = reader->createRowReader(rowOptions);
  EXPECT_TRUE(rowReader->estimatedRowSize().has_value());
  EXPECT_EQ(rowReader->estimatedRowSize().value(), 4);

  VectorPtr result;
  ASSERT_EQ(rowReader->next(30, result), 30);
  EXPECT_EQ(rowReader->estimatedRowSize().value(), 4);
  for (int i = 0; i < 30; ++i) {
    EXPECT_TRUE(result->equalValueAt(vec.get(), i, i));
  }
  ASSERT_EQ(rowReader->next(71, result), 70);
  EXPECT_EQ(
      rowReader->estimatedRowSize().value(),
      ::facebook::nimble::VeloxReader::kConservativeEstimatedRowSize);
  for (int i = 0; i < 70; ++i) {
    EXPECT_TRUE(result->equalValueAt(vec.get(), i, 30 + i));
  }
}

TEST_F(NimbleReaderTest, columnSelector) {
  auto type =
      ROW({"int_col", "double_col", "string_col"},
          {INTEGER(), DOUBLE(), VARCHAR()});
  auto vec = test::BatchMaker::createBatch(type, 100, pool());
  auto file = facebook::nimble::test::createNimbleFile(rootPool(), vec);
  auto readFile = std::make_shared<InMemoryReadFile>(file);
  auto factory =
      dwio::common::getReaderFactory(dwio::common::FileFormat::NIMBLE);
  auto input = std::make_unique<dwio::common::BufferedInput>(readFile, pool());
  dwio::common::ReaderOptions options(&pool());
  auto reader = factory->createReader(std::move(input), options);
  EXPECT_EQ(reader->numberOfRows(), 100);
  EXPECT_EQ(*reader->rowType(), *type);

  dwio::common::RowReaderOptions rowOptions;
  auto spec = std::make_shared<common::ScanSpec>("<root>");
  spec->addField("string_col", 0);
  spec->addField("double_col", 1);
  rowOptions.setScanSpec(spec);

  auto columnSelector = std::make_shared<dwio::common::ColumnSelector>(
      type, std::vector<std::string>{"string_col", "double_col"});
  rowOptions.select(columnSelector);

  auto rowReader = reader->createRowReader(rowOptions);
  VectorPtr result;

  ASSERT_EQ(rowReader->next(50, result), 50);
  ASSERT_EQ(result->type()->size(), 2);
  auto* rowVec = result->as<RowVector>();
  ASSERT_TRUE(rowVec);
  ASSERT_EQ(rowVec->children().size(), 2);
  auto* stringCol = rowVec->childAt(0).get();
  auto* doubleCol = rowVec->childAt(1).get();
  ASSERT_EQ(rowVec->childAt(0)->type(), VARCHAR());
  ASSERT_EQ(rowVec->childAt(0)->size(), 50);
  ASSERT_EQ(rowVec->childAt(1)->type(), DOUBLE());
  ASSERT_EQ(rowVec->childAt(1)->size(), 50);

  ASSERT_EQ(rowReader->next(50, result), 50);
  ASSERT_EQ(result->type()->size(), 2);
  rowVec = result->as<RowVector>();
  ASSERT_TRUE(rowVec);
  ASSERT_EQ(rowVec->children().size(), 2);
  ASSERT_EQ(rowVec->childAt(0).get(), stringCol);
  ASSERT_EQ(rowVec->childAt(1).get(), doubleCol);
  ASSERT_EQ(rowVec->childAt(0)->type(), VARCHAR());
  ASSERT_EQ(rowVec->childAt(0)->size(), 50);
  ASSERT_EQ(rowVec->childAt(1)->type(), DOUBLE());
  ASSERT_EQ(rowVec->childAt(1)->size(), 50);
}

TEST_F(NimbleReaderTest, filter) {
  test::VectorMaker maker(&pool());
  auto data = maker.flatVector<int64_t>(10, folly::identity);
  auto vec = maker.rowVector({data, data});
  auto file = facebook::nimble::test::createNimbleFile(rootPool(), vec);
  auto readFile = std::make_shared<InMemoryReadFile>(file);
  auto factory =
      dwio::common::getReaderFactory(dwio::common::FileFormat::NIMBLE);
  auto input = std::make_unique<dwio::common::BufferedInput>(readFile, pool());
  dwio::common::ReaderOptions options(&pool());
  auto reader = factory->createReader(std::move(input), options);
  dwio::common::RowReaderOptions rowOptions;
  auto spec = std::make_shared<common::ScanSpec>("<root>");
  spec->getOrCreateChild(common::Subfield("c0"))
      ->setFilter(common::createBigintValues({2, 3, 5, 7}, false));
  spec->addField("c1", 0);
  rowOptions.setScanSpec(spec);
  auto rowReader = reader->createRowReader(rowOptions);
  VectorPtr result;
  ASSERT_EQ(rowReader->next(6, result), 6);
  ASSERT_EQ(result->size(), 3);
  auto* rowVec = result->as<RowVector>();
  ASSERT_TRUE(rowVec);
  ASSERT_EQ(rowVec->children().size(), 1);
  auto* c1 = rowVec->childAt(0).get();
  ASSERT_EQ(c1->encoding(), VectorEncoding::Simple::DICTIONARY);
  c1 = c1->valueVector().get();
  ASSERT_EQ(rowReader->next(1, result), 1);
  ASSERT_EQ(result->size(), 0);
  ASSERT_EQ(rowReader->next(4, result), 3);
  ASSERT_EQ(result->size(), 1);
  rowVec = result->as<RowVector>();
  ASSERT_TRUE(rowVec);
  ASSERT_EQ(rowVec->children().size(), 1);
  auto* c1Prime = rowVec->childAt(0).get();
  ASSERT_EQ(c1Prime->encoding(), VectorEncoding::Simple::DICTIONARY);
  ASSERT_EQ(c1Prime->valueVector().get(), c1);
}

TEST_F(NimbleReaderTest, flatMap) {
  test::VectorMaker maker(&pool());
  auto mapVec = maker.mapVector<int32_t, float>(
      20,
      [](vector_size_t) { return 5; },
      [](vector_size_t row) { return row % 5; },
      [](vector_size_t row) { return row; });
  auto vec = maker.rowVector({"float_features"}, {mapVec});
  facebook::nimble::VeloxWriterOptions writerOptions;
  writerOptions.flatMapColumns.insert("float_features");
  auto file =
      facebook::nimble::test::createNimbleFile(rootPool(), vec, writerOptions);
  auto readFile = std::make_shared<InMemoryReadFile>(file);
  auto factory =
      dwio::common::getReaderFactory(dwio::common::FileFormat::NIMBLE);
  auto input = std::make_unique<dwio::common::BufferedInput>(readFile, pool());
  dwio::common::ReaderOptions options(&pool());
  auto reader = factory->createReader(std::move(input), options);
  ASSERT_EQ(*reader->rowType(), *vec->type());
  {
    dwio::common::RowReaderOptions rowOptions;
    setScanSpec(*vec->type(), rowOptions);
    auto rowReader = reader->createRowReader(rowOptions);
    VectorPtr result;
    ASSERT_EQ(rowReader->next(20, result), 20);
    ASSERT_EQ(*result->type(), *vec->type());
    for (int i = 0; i < 20; ++i) {
      EXPECT_TRUE(result->equalValueAt(vec.get(), i, i));
    }
  }
  {
    auto floatFeaturesType = ROW(
        {{"0", REAL()},
         {"1", REAL()},
         {"2", REAL()},
         {"3", REAL()},
         {"4", REAL()}});
    dwio::common::RowReaderOptions rowOptions;
    setScanSpec(*ROW({"float_features"}, {floatFeaturesType}), rowOptions);
    rowOptions.setFlatmapNodeIdsAsStruct({{1, {"0", "1", "2", "3", "4"}}});
    auto rowReader = reader->createRowReader(rowOptions);
    VectorPtr result;
    ASSERT_EQ(rowReader->next(20, result), 20);
    ASSERT_EQ(result->type()->size(), 1);
    ASSERT_EQ(result->type()->childAt(0)->kind(), TypeKind::ROW);
    auto* floatFeatures = result->as<RowVector>()->childAt(0)->as<RowVector>();
    ASSERT_EQ(*floatFeatures->type(), *floatFeaturesType);
    for (int i = 0; i < 20; ++i) {
      for (int j = 0; j < 5; ++j) {
        EXPECT_EQ(
            floatFeatures->childAt(j)->asFlatVector<float>()->valueAt(i),
            j + 5 * i);
      }
    }
  }
}

void testSubfieldPruningUsingFeatureSelector(
    bool flatmap,
    memory::MemoryPool& leafPool,
    memory::MemoryPool& rootPool) {
  test::VectorMaker maker(&leafPool);
  auto vec = maker.rowVector(
      {"float_features"},
      {
          maker.mapVector<int32_t, float>(
              5,
              [](vector_size_t) { return 5; },
              [](vector_size_t row) { return row % 5; },
              [](vector_size_t row) { return row; }),
      });
  auto rowType = asRowType(vec->type());
  facebook::nimble::VeloxWriterOptions writerOptions;
  if (flatmap) {
    writerOptions.flatMapColumns.insert("float_features");
  }
  auto file =
      facebook::nimble::test::createNimbleFile(rootPool, vec, writerOptions);
  auto readFile = std::make_shared<InMemoryReadFile>(file);
  auto factory =
      dwio::common::getReaderFactory(dwio::common::FileFormat::NIMBLE);
  auto input =
      std::make_unique<dwio::common::BufferedInput>(readFile, leafPool);
  dwio::common::ReaderOptions options(&leafPool);
  auto reader = factory->createReader(std::move(input), options);
  auto spec = std::make_shared<common::ScanSpec>("root");
  spec->addAllChildFields(*vec->type());
  // No effect on bulk reader but will be used by selective reader.
  spec->childByName("float_features")
      ->childByName(common::ScanSpec::kMapKeysFieldName)
      ->setFilter(common::createBigintValues({1, 3}, false));
  dwio::common::RowReaderOptions rowOptions;
  rowOptions.setScanSpec(spec);
  rowOptions.select(std::make_shared<dwio::common::ColumnSelector>(
      rowType, std::vector<std::string>({"float_features#[1,3]"})));
  auto rowReader = reader->createRowReader(rowOptions);
  VectorPtr result;
  ASSERT_EQ(rowReader->next(vec->size(), result), vec->size());
  VectorPtr expected;
  if (flatmap) {
    expected = maker.rowVector(
        {"float_features"},
        {
            maker.mapVector<int32_t, float>(
                vec->size(),
                [](vector_size_t) { return 2; },
                [](vector_size_t i, vector_size_t j) { return 1 + 2 * j; },
                [](vector_size_t i, vector_size_t j) {
                  return 5 * i + 1 + 2 * j;
                }),
        });
  } else {
    expected = vec;
  }
  ASSERT_EQ(result->size(), expected->size());
  for (int i = 0; i < result->size(); ++i) {
    ASSERT_TRUE(result->equalValueAt(expected.get(), i, i))
        << result->toString(0, result->size());
  }
  ASSERT_EQ(rowReader->next(vec->size(), result), 0);
}

TEST_F(NimbleReaderTest, subfieldPruningFlatMap) {
  testSubfieldPruningUsingFeatureSelector(true, pool(), rootPool());
}

TEST_F(NimbleReaderTest, subfieldPruningMap) {
  testSubfieldPruningUsingFeatureSelector(false, pool(), rootPool());
}

} // namespace
} // namespace facebook::velox::nimble
