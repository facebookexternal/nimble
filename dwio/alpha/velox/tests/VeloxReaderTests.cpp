// Copyright 2004-present Facebook. All Rights Reserved.

#include <folly/executors/CPUThreadPoolExecutor.h>
#include <gtest/gtest.h>
#include <optional>
#include <string_view>

#include "dwio/alpha/common/Buffer.h"
#include "dwio/alpha/common/Types.h"
#include "dwio/alpha/common/Vector.h"
#include "dwio/alpha/common/tests/AlphaFileWriter.h"
#include "dwio/alpha/common/tests/TestUtils.h"
#include "dwio/alpha/velox/SchemaUtils.h"
#include "dwio/alpha/velox/VeloxReader.h"
#include "dwio/alpha/velox/VeloxWriter.h"
#include "velox/dwio/common/ColumnSelector.h"
#include "velox/dwio/common/tests/utils/BatchMaker.h"
#include "velox/type/Type.h"
#include "velox/vector/BaseVector.h"
#include "velox/vector/ComplexVector.h"
#include "velox/vector/DecodedVector.h"
#include "velox/vector/NullsBuilder.h"
#include "velox/vector/SelectivityVector.h"
#include "velox/vector/fuzzer/VectorFuzzer.h"
#include "velox/vector/tests/utils/VectorMaker.h"

using namespace ::facebook;

namespace {
auto rootPool = velox::memory::deprecatedDefaultMemoryManager().addRootPool(
    "velox_reader_tests");
auto leafPool = rootPool -> addLeafChild("leaf");
struct VeloxMapGeneratorConfig {
  std::shared_ptr<const velox::RowType> rowType;
  velox::TypeKind keyType;
  std::string stringKeyPrefix = "test_";
  uint32_t maxSizeForMap = 10;
  unsigned long seed = folly::Random::rand32();
  bool hasNulls = true;
};

// Generates a batch of MpaVector Data
class VeloxMapGenerator {
 public:
  VeloxMapGenerator(
      velox::memory::MemoryPool* pool,
      VeloxMapGeneratorConfig config)
      : pool_{pool}, config_{config}, rng_(config_.seed), buffer_(*pool) {
    LOG(INFO) << "seed: " << config_.seed;
  }

  velox::VectorPtr generateBatch(velox::vector_size_t batchSize) {
    auto offsets = velox::allocateOffsets(batchSize, pool_);
    auto rawOffsets = offsets->template asMutable<velox::vector_size_t>();
    auto sizes = velox::allocateSizes(batchSize, pool_);
    auto rawSizes = sizes->template asMutable<velox::vector_size_t>();
    velox::vector_size_t childSize = 0;
    for (auto i = 0; i < batchSize; ++i) {
      rawOffsets[i] = childSize;
      auto length = folly::Random::rand32(rng_) % (config_.maxSizeForMap + 1);
      rawSizes[i] = length;
      childSize += length;
    }

    // create keys
    auto keys = generateKeys(batchSize, childSize, rawSizes);
    auto offset = 0;
    // encode keys
    if (folly::Random::oneIn(2, rng_)) {
      offset = 0;
      auto indices = velox::AlignedBuffer::allocate<velox::vector_size_t>(
          childSize, pool_);
      auto rawIndices = indices->asMutable<velox::vector_size_t>();
      for (auto i = 0; i < batchSize; ++i) {
        auto mapSize = rawSizes[i];
        for (auto j = 0; j < mapSize; ++j) {
          rawIndices[offset + j] = offset + mapSize - j - 1;
        }
        offset += mapSize;
      }
      keys = velox::BaseVector::wrapInDictionary(
          nullptr, indices, childSize, keys);
    }

    velox::VectorFuzzer fuzzer(
        {
            .vectorSize = static_cast<size_t>(childSize),
            .nullRatio = 0.1,
            .stringLength = 20,
            .stringVariableLength = true,
            .containerLength = 5,
            .containerVariableLength = true,
            .dictionaryHasNulls = config_.hasNulls,
        },
        pool_,
        config_.seed);

    // Generate a random null vector.
    velox::NullsBuilder builder{batchSize, pool_};
    if (config_.hasNulls) {
      for (auto i = 0; i < batchSize; ++i) {
        if (folly::Random::oneIn(10, rng_)) {
          builder.setNull(i);
        }
      }
    }
    auto nulls = builder.build();
    std::vector<velox::VectorPtr> children;
    for (auto& featureColumn : config_.rowType->children()) {
      velox::VectorPtr map = std::make_shared<velox::MapVector>(
          pool_,
          featureColumn,
          nulls,
          batchSize,
          offsets,
          sizes,
          keys,
          fuzzer.fuzz(featureColumn->asMap().valueType()));
      // Encode map
      if (folly::Random::oneIn(2, rng_)) {
        map = fuzzer.fuzzDictionary(map);
      }
      children.push_back(map);
    }

    return std::make_shared<velox::RowVector>(
        pool_, config_.rowType, nullptr, batchSize, std::move(children));
  }

  std::mt19937& rng() {
    return rng_;
  }

 private:
  std::shared_ptr<velox::BaseVector> generateKeys(
      velox::vector_size_t batchSize,
      velox::vector_size_t childSize,
      velox::vector_size_t* rawSizes) {
    switch (config_.keyType) {
#define SCALAR_CASE(veloxKind, cppType)                                    \
  case velox::TypeKind::veloxKind: {                                       \
    auto keys =                                                            \
        velox::BaseVector::create(velox::veloxKind(), childSize, pool_);   \
    auto rawKeyValues = keys->asFlatVector<cppType>()->mutableRawValues(); \
    auto offset = 0;                                                       \
    for (auto i = 0; i < batchSize; ++i) {                                 \
      for (auto j = 0; j < rawSizes[i]; ++j) {                             \
        rawKeyValues[offset++] = folly::to<cppType>(j);                    \
      }                                                                    \
    }                                                                      \
    return keys;                                                           \
  }
      SCALAR_CASE(TINYINT, int8_t)
      SCALAR_CASE(SMALLINT, int16_t)
      SCALAR_CASE(INTEGER, int32_t)
      SCALAR_CASE(BIGINT, int64_t)

#undef SCALAR_CASE
      case velox::TypeKind::VARCHAR: {
        auto keys =
            velox::BaseVector::create(velox::VARCHAR(), childSize, pool_);
        auto flatVector = keys->asFlatVector<velox::StringView>();
        auto offset = 0;
        for (auto i = 0; i < batchSize; ++i) {
          for (auto j = 0; j < rawSizes[i]; ++j) {
            auto key = config_.stringKeyPrefix + folly::to<std::string>(j);
            flatVector->set(
                offset++, {key.data(), static_cast<int32_t>(key.size())});
          }
        }
        return keys;
      }
      default:
        ALPHA_NOT_SUPPORTED("Unsupported Key Type");
    }
  }
  velox::memory::MemoryPool* pool_;
  VeloxMapGeneratorConfig config_;
  std::mt19937 rng_;
  alpha::Buffer buffer_;
};

template <typename T>
void fillKeysVector(
    velox::VectorPtr& vector,
    velox::vector_size_t offset,
    T& key) {
  auto flatVectorMutable =
      static_cast<velox::FlatVector<T>&>(*vector).mutableRawValues();
  flatVectorMutable[offset] = key;
}

template <typename T>
std::string getStringKey(T key) {
  return folly::to<std::string>(key);
}

template <>
std::string getStringKey(velox::StringView key) {
  return std::string(key);
}

// utility function to convert an input Map velox::VectorPtr to outVector if
// isKeyPresent
template <typename T>
void filterFlatMap(
    const velox::VectorPtr& vector,
    velox::VectorPtr& outVector,
    std::function<bool(std::string& key)> isKeyPresent) {
  auto mapVector = vector->as<velox::MapVector>();
  auto offsets = mapVector->rawOffsets();
  auto sizes = mapVector->rawSizes();
  auto keysVector = mapVector->mapKeys()->asFlatVector<T>();
  auto valuesVector = mapVector->mapValues();

  if (outVector == nullptr) {
    outVector = velox::BaseVector::create(
        vector->type(), vector->size(), vector->pool());
  }
  auto resultVector = outVector->as<velox::MapVector>();
  auto newKeysVector = resultVector->mapKeys();
  velox::VectorPtr newValuesVector = velox::BaseVector::create(
      mapVector->mapValues()->type(), 0, mapVector->pool());
  auto* offsetsPtr = resultVector->mutableOffsets(vector->size())
                         ->asMutable<velox::vector_size_t>();
  auto* lengthsPtr = resultVector->mutableSizes(vector->size())
                         ->asMutable<velox::vector_size_t>();
  newKeysVector->resize(keysVector->size());
  newValuesVector->resize(valuesVector->size());
  resultVector->setNullCount(vector->size());

  velox::vector_size_t offset = 0;
  for (velox::vector_size_t index = 0; index < mapVector->size(); ++index) {
    offsetsPtr[index] = offset;
    if (!mapVector->isNullAt(index)) {
      resultVector->setNull(index, false);
      for (velox::vector_size_t i = offsets[index];
           i < offsets[index] + sizes[index];
           ++i) {
        auto keyValue = keysVector->valueAtFast(i);
        auto&& stringKeyValue = getStringKey(keyValue);
        if (isKeyPresent(stringKeyValue)) {
          fillKeysVector(newKeysVector, offset, keyValue);
          newValuesVector->copy(valuesVector.get(), offset, i, 1);
          ++offset;
        }
      }
    } else {
      resultVector->setNull(index, true);
    }
    lengthsPtr[index] = offset - offsetsPtr[index];
  }

  newKeysVector->resize(offset, false);
  newValuesVector->resize(offset, false);
  resultVector->setKeysAndValues(
      std::move(newKeysVector), std::move(newValuesVector));
}

// compare two map vector, where expected map will be converted a new vector
// based on isKeyPresent Functor
template <typename T>
void compareFlatMapAsFilteredMap(
    velox::VectorPtr expected,
    velox::VectorPtr actual,
    std::function<bool(std::string&)> isKeyPresent) {
  auto flat = velox::BaseVector::create(
      expected->type(), expected->size(), expected->pool());
  flat->copy(expected.get(), 0, 0, expected->size());
  auto expectedRow = flat->as<velox::RowVector>();
  auto actualRow = actual->as<velox::RowVector>();
  EXPECT_EQ(expectedRow->childrenSize(), actualRow->childrenSize());
  for (auto i = 0; i < expectedRow->childrenSize(); ++i) {
    velox::VectorPtr outVector;
    filterFlatMap<T>(expectedRow->childAt(i), outVector, isKeyPresent);
    for (int j = 0; j < outVector->size(); j++) {
      ASSERT_TRUE(outVector->equalValueAt(actualRow->childAt(i).get(), j, j))
          << "Content mismatch at index " << j
          << "\nReference: " << outVector->toString(j)
          << "\nResult: " << actualRow->childAt(i)->toString(j);
    }
  }
}

std::unique_ptr<alpha::VeloxReader> getReaderForLifeCycleTest(
    const std::shared_ptr<const velox::RowType> schema,
    int32_t batchSize,
    std::mt19937& rng,
    alpha::VeloxWriterOptions writerOptions = {},
    alpha::VeloxReadParams readParams = {}) {
  std::function<bool(velox::vector_size_t)> isNullAt =
      [](velox::vector_size_t i) { return i % 2 == 0; };

  auto vector = velox::test::BatchMaker::createBatch(
      schema, batchSize, *leafPool, rng, isNullAt);
  auto file = alpha::test::createAlphaFile(*rootPool, vector, writerOptions);

  std::unique_ptr<velox::InMemoryReadFile> readFile =
      std::make_unique<velox::InMemoryReadFile>(file);

  std::shared_ptr<alpha::Tablet> tablet =
      std::make_shared<alpha::Tablet>(*leafPool, std::move(readFile));
  auto selector = std::make_shared<velox::dwio::common::ColumnSelector>(schema);
  std::unique_ptr<alpha::VeloxReader> reader =
      std::make_unique<alpha::VeloxReader>(
          *leafPool, tablet, std::move(selector), readParams);
  return reader;
}

template <typename TData, typename TRequested>
void verifyUpcastedScalars(
    const velox::VectorPtr& expected,
    uint32_t& idxInExpected,
    const velox::VectorPtr& result,
    uint32_t readSize) {
  ASSERT_TRUE(expected->isScalar() && result->isScalar());
  auto flatExpected = expected->asFlatVector<TData>();
  auto flatResult = result->asFlatVector<TRequested>();
  for (uint32_t i = 0; i < result->size(); ++i) {
    EXPECT_EQ(expected->isNullAt(idxInExpected), result->isNullAt(i))
        << "Unexpected null status. index: " << i << ", readSize: " << readSize;
    if (!result->isNullAt(i)) {
      if constexpr (
          alpha::isIntegralType<TData>() || alpha::isBoolType<TData>()) {
        EXPECT_EQ(
            static_cast<TRequested>(flatExpected->valueAtFast(idxInExpected)),
            flatResult->valueAtFast(i))
            << "Unexpected value. index: " << i << ", readSize: " << readSize;
      } else {
        EXPECT_DOUBLE_EQ(
            static_cast<TRequested>(flatExpected->valueAtFast(idxInExpected)),
            flatResult->valueAtFast(i))
            << "Unexpected value. index: " << i << ", readSize: " << readSize;
      }
    }
    ++idxInExpected;
  }
}

size_t streamsReadCount(
    velox::memory::MemoryPool& pool,
    velox::ReadFile* readFile,
    const std::vector<alpha::testing::Chunk>& chunks) {
  // Assumed for the algorithm
  VELOX_CHECK_EQ(false, readFile->shouldCoalesce());
  alpha::Tablet tablet(pool, readFile);
  VELOX_CHECK_GE(tablet.stripeCount(), 1);
  auto offsets = tablet.streamOffsets(0);
  std::unordered_set<uint32_t> streamOffsets;
  LOG(INFO) << "Number of streams: " << offsets.size();
  for (auto offset : offsets) {
    LOG(INFO) << "Stream offset: " << offset;
    streamOffsets.insert(offset);
  }
  size_t readCount = 0;
  auto fileSize = readFile->size();
  for (const auto [offset, size] : chunks) {
    // This is to prevent the case when the file is too small, then entire file
    // is read from 0 to the end. It can also happen that we don't read from 0
    // to the end, but just the last N bytes (a big block at the end). If that
    // read coincidently starts at the beginning of a stream, I may think that
    // I'm reading a stream. So I'm also guarding against it.
    if (streamOffsets.contains(offset) && (offset + size) != fileSize) {
      ++readCount;
    }
  }
  return readCount;
}

} // namespace

class VeloxReaderTests : public ::testing::TestWithParam<bool> {
 protected:
  void SetUp() override {
    pool_ = leafPool;
  }

  std::shared_ptr<velox::memory::MemoryPool> pool_;
};

TEST_F(VeloxReaderTests, DontReadUnselectedColumnsFromFile) {
  auto type = velox::ROW({
      {"tinyint_val", velox::TINYINT()},
      {"smallint_val", velox::SMALLINT()},
      {"int_val", velox::INTEGER()},
      {"long_val", velox::BIGINT()},
      {"float_val", velox::REAL()},
      {"double_val", velox::DOUBLE()},
      {"string_val", velox::VARCHAR()},
      {"array_val", velox::ARRAY(velox::BIGINT())},
      {"map_val", velox::MAP(velox::INTEGER(), velox::BIGINT())},
  });

  int batchSize = 100;
  auto seed = folly::Random::rand32();
  LOG(INFO) << "seed: " << seed;
  auto selectedColumnNames =
      std::vector<std::string>{"tinyint_val", "double_val"};
  auto vector = velox::test::BatchMaker::createBatch(
      type, batchSize, *pool_, nullptr, seed);
  auto file = alpha::test::createAlphaFile(*rootPool, vector);

  uint32_t readSize = 1;
  alpha::testing::InMemoryTrackableReadFile readFile(file);
  // We want to check stream by stream if they are being read
  readFile.setShouldCoalesce(false);

  auto selector = std::make_shared<velox::dwio::common::ColumnSelector>(
      std::dynamic_pointer_cast<const velox::RowType>(vector->type()),
      selectedColumnNames);
  alpha::VeloxReader reader(*pool_, &readFile, std::move(selector));

  velox::VectorPtr result;
  reader.next(readSize, result);

  auto chunks = readFile.chunks();

  for (auto [offset, size] : chunks) {
    LOG(INFO) << "Stream read: " << offset;
  }

  EXPECT_EQ(
      streamsReadCount(*pool_, &readFile, chunks), selectedColumnNames.size());
}

TEST_F(VeloxReaderTests, DontReadUnprojectedFeaturesFromFile) {
  auto type = velox::ROW({
      {"float_features", velox::MAP(velox::INTEGER(), velox::REAL())},
  });
  auto rowType = std::dynamic_pointer_cast<const velox::RowType>(type);

  int batchSize = 500;
  auto seed = folly::Random::rand32();

  VeloxMapGeneratorConfig generatorConfig{
      .rowType = rowType,
      .keyType = velox::TypeKind::INTEGER,
      .maxSizeForMap = 10,
      .seed = seed,
      .hasNulls = false,
  };

  VeloxMapGenerator generator(pool_.get(), generatorConfig);
  auto vector = generator.generateBatch(batchSize);

  alpha::VeloxWriterOptions writerOptions;
  writerOptions.flatMapColumns.insert("float_features");

  auto file =
      alpha::test::createAlphaFile(*rootPool, vector, std::move(writerOptions));

  facebook::alpha::testing::InMemoryTrackableReadFile readFile(file);
  // We want to check stream by stream if they are being read
  readFile.setShouldCoalesce(false);

  auto selector = std::make_shared<velox::dwio::common::ColumnSelector>(
      std::dynamic_pointer_cast<const velox::RowType>(vector->type()));

  alpha::VeloxReadParams params;
  params.readFlatMapFieldAsStruct.insert("float_features");
  auto& selectedFeatures =
      params.flatMapFeatureSelector["float_features"].features;
  std::mt19937 rng(seed);
  for (int i = 0; i < generatorConfig.maxSizeForMap; ++i) {
    if (folly::Random::oneIn(2, rng)) {
      selectedFeatures.push_back(folly::to<std::string>(i));
    }
  }
  // Features list can't be empty.
  if (selectedFeatures.empty()) {
    selectedFeatures = {folly::to<std::string>(
        folly::Random::rand32(generatorConfig.maxSizeForMap))};
  }

  LOG(INFO) << "Selected features (" << selectedFeatures.size()
            << ") :" << folly::join(", ", selectedFeatures);

  alpha::VeloxReader reader(*pool_, &readFile, std::move(selector), params);

  uint32_t readSize = 1000;
  velox::VectorPtr result;
  reader.next(readSize, result);

  auto selectedFeaturesSet = std::unordered_set<std::string>(
      selectedFeatures.cbegin(), selectedFeatures.cend());

  // We have those streams: Row, FlatMap, N*(Values + inMap)
  // Row: Empty stream. Not read.
  // FlatMap: Empty if !hasNulls
  // N: Number of features
  // Values: Empty if all rows are null (if inMap all false)
  // inMap: Non-empty
  //
  // Therefore the formula is: 0 + 0 + N*(Values*any(inMap) + inMap)
  ASSERT_FALSE(generatorConfig.hasNulls);
  int expectedNonEmptyStreamsCount = 0; // 0 if !hasNulls
  auto rowResult = result->as<velox::RowVector>();
  ASSERT_EQ(rowResult->childrenSize(), 1); // FlatMap
  auto flatMap = rowResult->childAt(0)->as<velox::RowVector>();

  for (int feature = 0; feature < flatMap->childrenSize(); ++feature) {
    // Each feature will have at least inMap stream
    ++expectedNonEmptyStreamsCount;
    if (selectedFeaturesSet.contains(
            flatMap->type()->asRow().nameOf(feature))) {
      auto columnResult = flatMap->childAt(feature);
      for (int row = 0; row < columnResult->size(); ++row) {
        // Values stream for this column will only exist if there's at least
        // one element inMap in this column (if not all rows are null at either
        // row level or element level)
        if (!flatMap->isNullAt(row) && !columnResult->isNullAt(row)) {
          ++expectedNonEmptyStreamsCount;
          // exit row iteration, we know that there's at least one element
          break;
        }
      }
    }
  }

  auto chunks = readFile.chunks();

  LOG(INFO) << "Total streams read: " << chunks.size();
  for (auto [offset, size] : chunks) {
    LOG(INFO) << "Stream read: " << offset;
  }

  EXPECT_EQ(
      streamsReadCount(*pool_, &readFile, chunks),
      expectedNonEmptyStreamsCount);
}

TEST_F(VeloxReaderTests, ReadComplexData) {
  auto type = velox::ROW({
      {"tinyint_val", velox::TINYINT()},
      {"smallint_val", velox::SMALLINT()},
      {"int_val", velox::INTEGER()},
      {"long_val", velox::BIGINT()},
      {"float_val", velox::REAL()},
      {"double_val", velox::DOUBLE()},
      {"bool_val", velox::BOOLEAN()},
      {"string_val", velox::VARCHAR()},
      {"array_val", velox::ARRAY(velox::BIGINT())},
      {"map_val", velox::MAP(velox::INTEGER(), velox::BIGINT())},
      {"struct_val",
       velox::ROW({
           {"float_val", velox::REAL()},
           {"double_val", velox::DOUBLE()},
       })},
      {"nested_val",
       velox::MAP(
           velox::INTEGER(),
           velox::ROW({
               {"float_val", velox::REAL()},
               {"array_val",
                velox::ARRAY(velox::MAP(velox::INTEGER(), velox::BIGINT()))},
           }))},
  });

  auto typeUpcast = velox::ROW({
      {"tinyint_val", velox::SMALLINT()},
      {"smallint_val", velox::INTEGER()},
      {"int_val", velox::BIGINT()},
      {"long_val", velox::BIGINT()},
      {"float_val", velox::DOUBLE()},
      {"double_val", velox::DOUBLE()},
      {"bool_val", velox::INTEGER()},
      {"string_val", velox::VARCHAR()},
      {"array_val", velox::ARRAY(velox::BIGINT())},
      {"map_val", velox::MAP(velox::INTEGER(), velox::BIGINT())},
      {"struct_val",
       velox::ROW({
           {"float_val", velox::REAL()},
           {"double_val", velox::DOUBLE()},
       })},
      {"nested_val",
       velox::MAP(
           velox::INTEGER(),
           velox::ROW({
               {"float_val", velox::REAL()},
               {"array_val",
                velox::ARRAY(velox::MAP(velox::INTEGER(), velox::BIGINT()))},
           }))},
  });

  // Note: Batch size of 5, with current BatchMaker implementation, creates a
  // non nullable row column. Batch size 1234, creates a nullable row column.
  for (int batchSize : {5, 1234}) {
    auto vector = velox::test::BatchMaker::createBatch(type, batchSize, *pool_);
    auto file = alpha::test::createAlphaFile(*rootPool, vector);

    for (bool upcast : {false, true}) {
      for (uint32_t readSize : {1, 2, 5, 7, 20, 100, 555, 2000}) {
        velox::InMemoryReadFile readFile(file);
        auto selector = std::make_shared<velox::dwio::common::ColumnSelector>(
            std::dynamic_pointer_cast<const velox::RowType>(
                upcast ? typeUpcast : vector->type()));
        alpha::VeloxReader reader(*pool_, &readFile, std::move(selector));

        velox::vector_size_t rowIndex = 0;
        std::vector<uint32_t> childRowIndices(
            vector->as<velox::RowVector>()->childrenSize(), 0);
        velox::VectorPtr result;
        while (reader.next(readSize, result)) {
          ASSERT_EQ(result->type()->kind(), velox::TypeKind::ROW);
          if (upcast) {
            verifyUpcastedScalars<int8_t, int16_t>(
                vector->as<velox::RowVector>()->childAt(0),
                childRowIndices[0],
                result->as<velox::RowVector>()->childAt(0),
                readSize);
            verifyUpcastedScalars<int16_t, int32_t>(
                vector->as<velox::RowVector>()->childAt(1),
                childRowIndices[1],
                result->as<velox::RowVector>()->childAt(1),
                readSize);
            verifyUpcastedScalars<int32_t, int64_t>(
                vector->as<velox::RowVector>()->childAt(2),
                childRowIndices[2],
                result->as<velox::RowVector>()->childAt(2),
                readSize);
            verifyUpcastedScalars<int64_t, int64_t>(
                vector->as<velox::RowVector>()->childAt(3),
                childRowIndices[3],
                result->as<velox::RowVector>()->childAt(3),
                readSize);
            verifyUpcastedScalars<float, double>(
                vector->as<velox::RowVector>()->childAt(4),
                childRowIndices[4],
                result->as<velox::RowVector>()->childAt(4),
                readSize);
            verifyUpcastedScalars<double, double>(
                vector->as<velox::RowVector>()->childAt(5),
                childRowIndices[5],
                result->as<velox::RowVector>()->childAt(5),
                readSize);
            verifyUpcastedScalars<bool, int32_t>(
                vector->as<velox::RowVector>()->childAt(6),
                childRowIndices[6],
                result->as<velox::RowVector>()->childAt(6),
                readSize);
          } else {
            for (velox::vector_size_t i = 0; i < result->size(); ++i) {
              ASSERT_TRUE(vector->equalValueAt(result.get(), rowIndex, i))
                  << "Content mismatch at index " << rowIndex
                  << "\nReference: " << vector->toString(rowIndex)
                  << "\nResult: " << result->toString(i);

              ++rowIndex;
            }
          }
        }
      }
    }
  }
}

TEST_F(VeloxReaderTests, Lifetime) {
  velox::StringView s{"012345678901234567890123456789"};
  std::vector<velox::StringView> strings{s, s, s, s, s};
  std::vector<std::vector<velox::StringView>> stringsOfStrings{
      strings, strings, strings, strings, strings};
  velox::test::VectorMaker vectorMaker{pool_.get()};
  auto vector = vectorMaker.rowVector(
      {vectorMaker.flatVector<int32_t>({1, 2, 3, 4, 5}),
       vectorMaker.flatVector(strings),
       vectorMaker.arrayVector<velox::StringView>(stringsOfStrings),
       vectorMaker.mapVector<int32_t, velox::StringView>(
           5,
           /*sizeAt*/ [](auto row) { return row; },
           /*keyAt*/ [](auto row) { return row; },
           /*valueAt*/
           [&s](auto /* row */) { return s; }),
       vectorMaker.rowVector(
           /* childNames */ {"a", "b"},
           /* children */
           {vectorMaker.flatVector<float>({1., 2., 3., 4., 5.}),
            vectorMaker.flatVector(strings)})});

  velox::VectorPtr result;
  {
    auto file = alpha::test::createAlphaFile(*rootPool, vector);
    velox::InMemoryReadFile readFile(file);
    auto selector = std::make_shared<velox::dwio::common::ColumnSelector>(
        std::dynamic_pointer_cast<const velox::RowType>(vector->type()));
    alpha::VeloxReader reader(*pool_, &readFile, std::move(selector));

    ASSERT_TRUE(reader.next(vector->size(), result));
    ASSERT_FALSE(reader.next(vector->size(), result));
  }

  // At this point, the reader is dropped, so the vector should be
  // self-contained and doesn't rely on the reader state.

  ASSERT_EQ(vector->size(), result->size());
  ASSERT_EQ(result->type()->kind(), velox::TypeKind::ROW);

  for (int32_t i = 0; i < result->size(); ++i) {
    ASSERT_TRUE(vector->equalValueAt(result.get(), i, i))
        << "Content mismatch at index " << i
        << "\nReference: " << vector->toString(i)
        << "\nResult: " << result->toString(i);
  }
}

TEST_F(VeloxReaderTests, AllValuesNulls) {
  velox::test::VectorMaker vectorMaker{pool_.get()};
  auto vector = vectorMaker.rowVector(
      {vectorMaker.flatVectorNullable<int32_t>(
           {std::nullopt, std::nullopt, std::nullopt}),
       vectorMaker.flatVectorNullable<double>(
           {std::nullopt, std::nullopt, std::nullopt}),
       velox::BaseVector::createNullConstant(
           velox::ROW({{"foo", velox::INTEGER()}}), 3, pool_.get()),
       velox::BaseVector::createNullConstant(
           velox::MAP(velox::INTEGER(), velox::BIGINT()), 3, pool_.get()),
       velox::BaseVector::createNullConstant(
           velox::ARRAY(velox::INTEGER()), 3, pool_.get())});

  auto projectedType = velox::ROW({
      {"c0", velox::INTEGER()},
      {"c1", velox::DOUBLE()},
      {"c2", velox::ROW({{"foo", velox::INTEGER()}})},
      {"c3", velox::MAP(velox::INTEGER(), velox::BIGINT())},
      {"c4", velox::ARRAY(velox::INTEGER())},
  });
  velox::VectorPtr result;
  {
    alpha::VeloxWriterOptions options;
    options.flatMapColumns.insert("c3");
    options.dictionaryArrayColumns.insert("c4");
    auto file = alpha::test::createAlphaFile(*rootPool, vector, options);
    velox::InMemoryReadFile readFile(file);

    alpha::VeloxReadParams params;
    params.readFlatMapFieldAsStruct.insert("c3");
    params.flatMapFeatureSelector.insert({"c3", {{"1"}}});
    auto selector =
        std::make_shared<velox::dwio::common::ColumnSelector>(projectedType);
    alpha::VeloxReader reader(*pool_, &readFile, selector, params);

    ASSERT_TRUE(reader.next(vector->size(), result));
    ASSERT_FALSE(reader.next(vector->size(), result));
  }

  // At this point, the reader is dropped, so the vector should be
  // self-contained and doesn't rely on the reader state.

  ASSERT_EQ(vector->size(), result->size());
  auto& vectorType = result->type();
  ASSERT_EQ(vectorType->kind(), velox::TypeKind::ROW);
  ASSERT_EQ(vectorType->size(), projectedType->size());
  ASSERT_EQ(vectorType->childAt(3)->kind(), velox::TypeKind::ROW);
  ASSERT_EQ(vectorType->childAt(4)->kind(), velox::TypeKind::ARRAY);

  auto resultRow = result->as<velox::RowVector>();
  for (int32_t i = 0; i < result->size(); ++i) {
    for (auto j = 0; j < vectorType->size(); ++j) {
      ASSERT_TRUE(resultRow->childAt(j)->isNullAt(i));
    }
  }
}

TEST_F(VeloxReaderTests, StringBuffers) {
  // Creating a string column with 10 identical strings.
  // We will perform 2 reads of 5 rows each, and compare the string buffers
  // generated.
  // Note: all strings are long enough to force Velox to store them in string
  // buffers instead of inlining them.
  std::string s{"012345678901234567890123456789"};
  std::vector<std::string> column{s, s, s, s, s, s, s, s, s, s};
  velox::test::VectorMaker vectorMaker{pool_.get()};
  auto vector = vectorMaker.rowVector({vectorMaker.flatVector(column)});

  velox::VectorPtr result;
  auto file = alpha::test::createAlphaFile(*rootPool, vector);
  velox::InMemoryReadFile readFile(file);
  auto selector = std::make_shared<velox::dwio::common::ColumnSelector>(
      std::dynamic_pointer_cast<const velox::RowType>(vector->type()));
  alpha::VeloxReader reader(*pool_, &readFile, std::move(selector));

  ASSERT_TRUE(reader.next(5, result));

  ASSERT_EQ(5, result->size());
  ASSERT_EQ(result->type()->kind(), velox::TypeKind::ROW);
  auto rowVector = result->as<velox::RowVector>();
  ASSERT_EQ(1, rowVector->childrenSize());
  const auto& buffers1 = rowVector->childAt(0)
                             ->as<velox::FlatVector<velox::StringView>>()
                             ->stringBuffers();
  EXPECT_LE(
      1,
      rowVector->childAt(0)
          ->as<velox::FlatVector<velox::StringView>>()
          ->stringBuffers()
          .size());

  // Capture string buffer size after first batch read
  auto bufferSizeFirst = std::accumulate(
      buffers1.begin(), buffers1.end(), 0, [](int sum, const auto& buffer) {
        return sum + buffer->size();
      });

  ASSERT_TRUE(reader.next(5, result));
  rowVector = result->as<velox::RowVector>();
  ASSERT_EQ(1, rowVector->childrenSize());
  const auto& buffers2 = rowVector->childAt(0)
                             ->as<velox::FlatVector<velox::StringView>>()
                             ->stringBuffers();

  ASSERT_EQ(5, result->size());
  EXPECT_LE(
      1,
      rowVector->childAt(0)
          ->as<velox::FlatVector<velox::StringView>>()
          ->stringBuffers()
          .size());

  // Capture string buffer size after second batch read. Since both batched
  // contain exactly the same strings ,batch sizes should match.
  auto bufferSizeSecond = std::accumulate(
      buffers2.begin(), buffers2.end(), 0, [](int sum, const auto& buffer) {
        return sum + buffer->size();
      });

  EXPECT_EQ(bufferSizeFirst, bufferSizeSecond);
}

TEST_F(VeloxReaderTests, NullVectors) {
  velox::test::VectorMaker vectorMaker{pool_.get()};

  // In the following table, the first 5 rows contain nulls and the last 5
  // rows don't.
  auto vector = vectorMaker.rowVector(
      {vectorMaker.flatVectorNullable<int32_t>(
           {1, 2, std::nullopt, 4, 5, 6, 7, 8, 9, 10}),
       vectorMaker.flatVectorNullable<velox::StringView>(
           {"1", std::nullopt, "3", "4", "5", "6", "7", "8", "9", "10"}),
       vectorMaker.arrayVectorNullable<double>(
           {std::vector<std::optional<double>>{1.0, 2.2, std::nullopt},
            {},
            std::nullopt,
            std::vector<std::optional<double>>{1.1, 2.0},
            {},
            std::vector<std::optional<double>>{6.1},
            std::vector<std::optional<double>>{7.1},
            std::vector<std::optional<double>>{8.1},
            std::vector<std::optional<double>>{9.1},
            std::vector<std::optional<double>>{10.1}}),
       vectorMaker.mapVector<int32_t, int64_t>(
           10,
           /*sizeAt*/ [](auto row) { return row; },
           /*keyAt*/ [](auto row) { return row; },
           /*valueAt*/
           [](auto row) { return row; },
           /*isNullAt*/ [](auto row) { return row < 5 && row % 2 == 0; }),
       vectorMaker.rowVector(
           /* childNames */ {"a", "b"},
           /* children */
           {vectorMaker.flatVector<int32_t>({1, 2, 3, 4, 5, 6, 7, 8, 9, 10}),
            vectorMaker.flatVector<double>(
                {1.1, 2.2, 3.3, 4.4, 5.5, 6.6, 7.7, 8.8, 9.9, 10.10})})});
  vector->childAt(4)->setNull(2, true); // Set null in row vector

  auto file = alpha::test::createAlphaFile(*rootPool, vector);
  velox::InMemoryReadFile readFile(file);
  auto selector = std::make_shared<velox::dwio::common::ColumnSelector>(
      std::dynamic_pointer_cast<const velox::RowType>(vector->type()));

  alpha::VeloxReader reader(*pool_, &readFile, std::move(selector));

  velox::VectorPtr result;

  // When reader is reading the first 5 rows, it should find null entries and
  // vectors should indicate that nulls exist.
  ASSERT_TRUE(reader.next(5, result));
  ASSERT_EQ(5, result->size());
  ASSERT_EQ(velox::TypeKind::ROW, result->type()->kind());

  auto rowVector = result->as<velox::RowVector>();

  ASSERT_EQ(5, rowVector->childrenSize());
  EXPECT_TRUE(rowVector->childAt(0)->mayHaveNulls());
  EXPECT_TRUE(rowVector->childAt(1)->mayHaveNulls());
  EXPECT_TRUE(rowVector->childAt(2)->mayHaveNulls());
  EXPECT_TRUE(rowVector->childAt(3)->mayHaveNulls());
  EXPECT_TRUE(rowVector->childAt(4)->mayHaveNulls());

  for (int32_t i = 0; i < result->size(); ++i) {
    ASSERT_TRUE(vector->equalValueAt(result.get(), i, i))
        << "Content mismatch at index " << i
        << "\nReference: " << vector->toString(i)
        << "\nResult: " << result->toString(i);
  }

  // When reader is reading the last 5 rows, it should identify that no null
  // exist and optimize vectors to efficiently indicate that.
  ASSERT_TRUE(reader.next(5, result));
  rowVector = result->as<velox::RowVector>();

  EXPECT_FALSE(rowVector->childAt(0)->mayHaveNulls());
  EXPECT_FALSE(rowVector->childAt(1)->mayHaveNulls());
  EXPECT_FALSE(rowVector->childAt(2)->mayHaveNulls());
  EXPECT_FALSE(rowVector->childAt(3)->mayHaveNulls());
  EXPECT_FALSE(rowVector->childAt(4)->mayHaveNulls());

  for (int32_t i = 0; i < result->size(); ++i) {
    ASSERT_TRUE(vector->equalValueAt(result.get(), i + 5, i))
        << "Content mismatch at index " << i + 5
        << "\nReference: " << vector->toString(i + 5)
        << "\nResult: " << result->toString(i);
  }

  ASSERT_FALSE(reader.next(1, result));
}

bool vectorEquals(
    const velox::VectorPtr& expected,
    const velox::VectorPtr& actual,
    velox::vector_size_t index) {
  return expected->equalValueAt(actual.get(), index, index);
};

template <typename T = int32_t>
void writeAndVerify(
    std::mt19937& rng,
    velox::memory::MemoryPool& pool,
    const velox::RowTypePtr& type,
    std::function<velox::VectorPtr(const velox::RowTypePtr&)> generator,
    std::function<bool(
        const velox::VectorPtr&,
        const velox::VectorPtr&,
        velox::vector_size_t)> validator,
    size_t count,
    alpha::VeloxWriterOptions writerOptions = {},
    alpha::VeloxReadParams readParams = {},
    std::function<bool(std::string&)> isKeyPresent = nullptr,
    std::function<void(const velox::VectorPtr&)> comparator = nullptr,
    bool multiSkip = false,
    bool checkMemoryLeak = false) {
  std::string file;
  auto writeFile = std::make_unique<velox::InMemoryWriteFile>(&file);
  alpha::FlushDecision decision;
  writerOptions.enableChunking = true;
  writerOptions.flushPolicyFactory = [&]() {
    return std::make_unique<alpha::LambdaFlushPolicy>(
        [&](auto&) { return decision; });
  };

  std::vector<velox::VectorPtr> expected;
  alpha::VeloxWriter writer(
      *rootPool, type, std::move(writeFile), std::move(writerOptions));
  bool perBatchFlush = folly::Random::oneIn(2, rng);
  for (auto i = 0; i < count; ++i) {
    auto vector = generator(type);
    int32_t rowIndex = 0;
    while (rowIndex < vector->size()) {
      decision = alpha::FlushDecision::None;
      auto batchSize = vector->size() - rowIndex;
      // Randomly produce chunks
      if (!comparator && folly::Random::oneIn(2)) {
        batchSize = folly::Random::rand32(0, batchSize, rng) + 1;
        decision = alpha::FlushDecision::Chunk;
      }
      if ((perBatchFlush || folly::Random::oneIn(5, rng)) &&
          (rowIndex + batchSize == vector->size())) {
        decision = alpha::FlushDecision::Stripe;
      }
      writer.write(vector->slice(rowIndex, batchSize));
      rowIndex += batchSize;
    }
    expected.push_back(vector);
  }
  writer.close();

  velox::InMemoryReadFile readFile(file);
  auto selector = std::make_shared<velox::dwio::common::ColumnSelector>(type);
  // new pool with to limit already used memory and with tracking enabled
  auto leakDetectPool =
      facebook::velox::memory::deprecatedDefaultMemoryManager().addRootPool(
          "memory_leak_detect");
  auto readerPool = leakDetectPool->addLeafChild("reader_pool");

  alpha::VeloxReader reader(*readerPool.get(), &readFile, selector, readParams);
  if (folly::Random::oneIn(2, rng)) {
    LOG(INFO) << "using executor";
    readParams.decodingExecutor =
        std::make_shared<folly::CPUThreadPoolExecutor>(1);
  }

  auto rootTypeFromSchema = convertToVeloxType(*reader.schema());
  EXPECT_EQ(*type, *rootTypeFromSchema)
      << "Expected: " << type->toString()
      << ", actual: " << rootTypeFromSchema->toString();

  velox::VectorPtr result;
  velox::vector_size_t numIncrements = 0, prevMemory = 0;
  for (auto i = 0; i < expected.size(); ++i) {
    auto& current = expected.at(i);
    ASSERT_TRUE(reader.next(current->size(), result));
    ASSERT_EQ(result->size(), current->size());
    if (comparator) {
      comparator(result);
    }
    if (isKeyPresent) {
      compareFlatMapAsFilteredMap<T>(current, result, isKeyPresent);
    } else {
      for (auto j = 0; j < result->size(); ++j) {
        ASSERT_TRUE(validator(current, result, j))
            << "Content mismatch at index " << j << " at count " << i
            << "\nReference: " << current->toString(j)
            << "\nResult: " << result->toString(j);
      }
    }

    // validate skip
    if (i % 2 == 0) {
      alpha::VeloxReader reader1(pool, &readFile, selector, readParams);
      alpha::VeloxReader reader2(pool, &readFile, selector, readParams);
      auto rowCount = expected.at(0)->size();
      velox::vector_size_t remaining = rowCount;
      uint32_t skipCount = 0;
      do {
        auto toSkip = folly::Random::rand32(1, remaining, rng);
        velox::VectorPtr result1;
        velox::VectorPtr result2;
        reader1.next(toSkip, result1);
        reader2.skipRows(toSkip);
        remaining -= toSkip;

        if (remaining > 0) {
          auto toRead = folly::Random::rand32(1, remaining, rng);
          reader1.next(toRead, result1);
          reader2.next(toRead, result2);

          ASSERT_EQ(result1->size(), result2->size());

          for (auto j = 0; j < result1->size(); ++j) {
            ASSERT_TRUE(vectorEquals(result1, result2, j))
                << "Content mismatch at index " << j
                << " skipCount  = " << skipCount << " remaining = " << remaining
                << " to read = " << toRead
                << "\nReference: " << result1->toString(j)
                << "\nResult: " << result2->toString(j);
          }

          remaining -= toRead;
        }
        skipCount += 1;
      } while (multiSkip && remaining > 0);
    }

    // validate memory usage
    if (readerPool->currentBytes() > prevMemory) {
      numIncrements++;
    }
    prevMemory = readerPool->currentBytes();
  }
  ASSERT_FALSE(reader.next(1, result));
  if (checkMemoryLeak) {
    EXPECT_LE(numIncrements, expected.size() / 2);
  }
}

TEST_P(VeloxReaderTests, FuzzSimple) {
  bool multithreaded = GetParam();

  auto type = velox::ROW({
      {"bool_val", velox::BOOLEAN()},
      {"byte_val", velox::TINYINT()},
      {"short_val", velox::SMALLINT()},
      {"int_val", velox::INTEGER()},
      {"long_val", velox::BIGINT()},
      {"float_val", velox::REAL()},
      {"double_val", velox::DOUBLE()},
      {"string_val", velox::VARCHAR()},
      {"binary_val", velox::VARBINARY()},
      // {"ts_val", velox::TIMESTAMP()},
  });
  auto rowType = std::dynamic_pointer_cast<const velox::RowType>(type);
  auto seed = folly::Random::rand32();
  LOG(INFO) << "seed: " << seed;

  alpha::VeloxWriterOptions writerOptions;
  if (multithreaded) {
    writerOptions.parallelEncoding = true;
    writerOptions.parallelWriting = true;
    writerOptions.parallelExecutor =
        std::make_shared<folly::CPUThreadPoolExecutor>(
            std::thread::hardware_concurrency(),
            std::thread::hardware_concurrency());
  }

  // Small batches creates more edge cases.
  size_t batchSize = 10;
  velox::VectorFuzzer noNulls(
      {
          .vectorSize = batchSize,
          .nullRatio = 0,
          .stringLength = 20,
          .stringVariableLength = true,
      },
      pool_.get(),
      seed);

  velox::VectorFuzzer hasNulls{
      {
          .vectorSize = batchSize,
          .nullRatio = 0.05,
          .stringLength = 10,
          .stringVariableLength = true,
      },
      pool_.get(),
      seed};

  auto iterations = 20;
  auto batches = 20;
  std::mt19937 rng{seed};
  for (auto i = 0; i < iterations; ++i) {
    writeAndVerify(
        rng,
        *pool_,
        rowType,
        [&](auto& type) { return noNulls.fuzzInputRow(type); },
        vectorEquals,
        batches,
        writerOptions);
    writeAndVerify(
        rng,
        *pool_,
        rowType,
        [&](auto& type) { return hasNulls.fuzzInputRow(type); },
        vectorEquals,
        batches,
        writerOptions);
  }
}

TEST_P(VeloxReaderTests, FuzzComplex) {
  bool multithreaded = GetParam();

  auto type = velox::ROW({
      {"array", velox::ARRAY(velox::REAL())},
      {"dict_array", velox::ARRAY(velox::REAL())},
      {"map", velox::MAP(velox::INTEGER(), velox::DOUBLE())},
      {"row",
       velox::ROW({
           {"a", velox::REAL()},
           {"b", velox::INTEGER()},
       })},
      {"nested",
       velox::ARRAY(velox::ROW({
           {"a", velox::INTEGER()},
           {"b", velox::MAP(velox::REAL(), velox::REAL())},
       }))},
      {"nested_map_array1",
       velox::MAP(velox::INTEGER(), velox::ARRAY(velox::REAL()))},
      {"nested_map_array2",
       velox::MAP(velox::INTEGER(), velox::ARRAY(velox::INTEGER()))},
  });
  auto rowType = std::dynamic_pointer_cast<const velox::RowType>(type);
  auto seed = folly::Random::rand32();
  LOG(INFO) << "seed: " << seed;

  alpha::VeloxWriterOptions writerOptions;
  writerOptions.dictionaryArrayColumns.insert("nested_map_array1");
  writerOptions.dictionaryArrayColumns.insert("nested_map_array2");
  writerOptions.dictionaryArrayColumns.insert("dict_array");

  if (multithreaded) {
    writerOptions.parallelEncoding = true;
    writerOptions.parallelWriting = true;
    writerOptions.parallelExecutor =
        std::make_shared<folly::CPUThreadPoolExecutor>(
            std::thread::hardware_concurrency(),
            std::thread::hardware_concurrency());
  }

  // Small batches creates more edge cases.
  size_t batchSize = 10;
  velox::VectorFuzzer noNulls(
      {
          .vectorSize = batchSize,
          .nullRatio = 0,
          .stringLength = 20,
          .stringVariableLength = true,
          .containerLength = 5,
          .containerVariableLength = true,
      },
      pool_.get(),
      seed);

  velox::VectorFuzzer hasNulls{
      {
          .vectorSize = batchSize,
          .nullRatio = 0.05,
          .stringLength = 10,
          .stringVariableLength = true,
          .containerLength = 5,
          .containerVariableLength = true,
      },
      pool_.get(),
      seed};

  auto iterations = 20;
  auto batches = 20;
  std::mt19937 rng{seed};
  for (auto i = 0; i < iterations; ++i) {
    writeAndVerify(
        rng,
        *pool_.get(),
        rowType,
        [&](auto& type) { return noNulls.fuzzInputRow(type); },
        vectorEquals,
        batches,
        writerOptions);
    writeAndVerify(
        rng,
        *pool_,
        rowType,
        [&](auto& type) { return hasNulls.fuzzInputRow(type); },
        vectorEquals,
        batches,
        writerOptions);
  }
}

TEST_P(VeloxReaderTests, ArrayWithOffsets) {
  bool multithreaded = GetParam();

  auto type = velox::ROW({
      {"dictionaryArray", velox::ARRAY(velox::INTEGER())},
  });
  auto rowType = std::dynamic_pointer_cast<const velox::RowType>(type);
  auto seed = folly::Random::rand32();
  LOG(INFO) << "seed: " << seed;

  alpha::VeloxWriterOptions writerOptions;
  writerOptions.dictionaryArrayColumns.insert("dictionaryArray");
  if (multithreaded) {
    writerOptions.parallelEncoding = true;
    writerOptions.parallelWriting = true;
    writerOptions.parallelExecutor =
        std::make_shared<folly::CPUThreadPoolExecutor>(
            std::thread::hardware_concurrency(),
            std::thread::hardware_concurrency());
  }

  velox::test::VectorMaker vectorMaker{pool_.get()};

  auto iterations = 20;
  auto batches = 20;
  std::mt19937 rng{seed};
  int expectedNumArrays = 0;
  bool checkMemoryLeak = true;

  auto compare = [&](const velox::VectorPtr& vector) {
    ASSERT_EQ(
        vector->wrappedVector()
            ->as<velox::RowVector>()
            ->childAt(0)
            ->loadedVector()
            ->wrappedVector()
            ->size(),
        expectedNumArrays);
  };
  for (auto i = 0; i < iterations; ++i) {
    expectedNumArrays = 1;
    writeAndVerify(
        rng,
        *pool_.get(),
        rowType,
        [&](auto& /*type*/) {
          return vectorMaker.rowVector(
              {"dictionaryArray"},
              {
                  vectorMaker.arrayVector<int32_t>({{1, 2}}),
              });
        },
        vectorEquals,
        batches,
        writerOptions,
        {},
        nullptr,
        compare,
        false,
        checkMemoryLeak);
    writeAndVerify(
        rng,
        *pool_.get(),
        rowType,
        [&](auto& /*type*/) {
          return vectorMaker.rowVector(
              {"dictionaryArray"},
              {
                  vectorMaker.arrayVector<int32_t>({{1, 2}, {1, 2}}),
              });
        },
        vectorEquals,
        batches,
        writerOptions,
        {},
        nullptr,
        compare,
        false,
        checkMemoryLeak);
    writeAndVerify(
        rng,
        *pool_.get(),
        rowType,
        [&](auto& /*type*/) {
          return vectorMaker.rowVector(
              {"dictionaryArray"},
              {
                  vectorMaker.arrayVector<int32_t>({{}}),
              });
        },
        vectorEquals,
        batches,
        writerOptions,
        {},
        nullptr,
        compare,
        false,
        checkMemoryLeak);
    writeAndVerify(
        rng,
        *pool_.get(),
        rowType,
        [&](auto& /*type*/) {
          return vectorMaker.rowVector(
              {"dictionaryArray"},
              {
                  vectorMaker.arrayVector<int32_t>({{}, {}, {}}),
              });
        },
        vectorEquals,
        batches,
        writerOptions,
        {},
        nullptr,
        compare,
        false,
        checkMemoryLeak);

    expectedNumArrays = 3;
    writeAndVerify(
        rng,
        *pool_.get(),
        rowType,
        [&](auto& /*type*/) {
          return vectorMaker.rowVector(
              {"dictionaryArray"},
              {
                  vectorMaker.arrayVector<int32_t>(
                      {{1, 2}, {1, 2}, {2, 3}, {5, 6, 7}, {5, 6, 7}}),
              });
        },
        vectorEquals,
        batches,
        writerOptions,
        {},
        nullptr,
        compare,
        false,
        checkMemoryLeak);
    writeAndVerify(
        rng,
        *pool_.get(),
        rowType,
        [&](auto& /*type*/) {
          return vectorMaker.rowVector(
              {"dictionaryArray"},
              {
                  vectorMaker.arrayVector<int32_t>(
                      {{1, 2}, {1, 2}, {2, 3}, {}, {}}),
              });
        },
        vectorEquals,
        batches,
        writerOptions,
        {},
        nullptr,
        compare,
        false,
        checkMemoryLeak);
    writeAndVerify(
        rng,
        *pool_.get(),
        rowType,
        [&](auto& /*type*/) {
          return vectorMaker.rowVector(
              {"dictionaryArray"},
              {
                  vectorMaker.arrayVector<int32_t>(
                      {{}, {}, {2, 3}, {5, 6, 7}, {5, 6, 7}}),
              });
        },
        vectorEquals,
        batches,
        writerOptions,
        {},
        nullptr,
        compare,
        false,
        checkMemoryLeak);
    writeAndVerify(
        rng,
        *pool_.get(),
        rowType,
        [&](auto& /*type*/) {
          return vectorMaker.rowVector(
              {"dictionaryArray"},
              {
                  vectorMaker.arrayVector<int32_t>(
                      {{1, 2}, {1, 2}, {}, {5, 6, 7}, {5, 6, 7}}),
              });
        },
        vectorEquals,
        batches,
        writerOptions,
        {},
        nullptr,
        compare,
        false,
        checkMemoryLeak);

    expectedNumArrays = 4;
    writeAndVerify(
        rng,
        *pool_.get(),
        rowType,
        [&](auto& /*type*/) {
          return vectorMaker.rowVector(
              {"dictionaryArray"},
              {
                  vectorMaker.arrayVector<int32_t>(
                      {{1, 3}, {1, 2}, {}, {5, 6, 7}, {5, 6, 7}}),
              });
        },
        vectorEquals,
        batches,
        writerOptions,
        {},
        nullptr,
        compare,
        false,
        checkMemoryLeak);

    expectedNumArrays = 5;
    writeAndVerify(
        rng,
        *pool_.get(),
        rowType,
        [&](auto& /*type*/) {
          return vectorMaker.rowVector(
              {"dictionaryArray"},
              {
                  // The middle element is a 0 length element and not null
                  vectorMaker.arrayVector<int32_t>(
                      {{1, 3}, {1, 2}, {}, {1, 2}, {5, 6, 7}}),
              });
        },
        vectorEquals,
        batches,
        writerOptions,
        {},
        nullptr,
        compare,
        false,
        checkMemoryLeak);

    writeAndVerify(
        rng,
        *pool_.get(),
        rowType,
        [&](auto& /*type*/) {
          return vectorMaker.rowVector(
              {"dictionaryArray"},
              {
                  // The middle element is a 0 length element and not null
                  vectorMaker.arrayVector<int32_t>(
                      {{1, 3}, {1, 2}, {}, {1, 2}, {5, 6, 7}}),
              });
        },
        vectorEquals,
        batches,
        writerOptions,
        {},
        nullptr,
        compare,
        false,
        checkMemoryLeak);
  }
}

TEST_P(VeloxReaderTests, ArrayWithOffsetsNullable) {
  bool multithreaded = GetParam();

  auto type = velox::ROW({
      {"dictionaryArray", velox::ARRAY(velox::INTEGER())},
  });
  auto rowType = std::dynamic_pointer_cast<const velox::RowType>(type);
  auto seed = folly::Random::rand32();
  LOG(INFO) << "seed: " << seed;

  alpha::VeloxWriterOptions writerOptions;
  writerOptions.dictionaryArrayColumns.insert("dictionaryArray");
  if (multithreaded) {
    writerOptions.parallelEncoding = true;
    writerOptions.parallelWriting = true;
    writerOptions.parallelExecutor =
        std::make_shared<folly::CPUThreadPoolExecutor>(
            std::thread::hardware_concurrency(),
            std::thread::hardware_concurrency());
  }
  velox::test::VectorMaker vectorMaker{pool_.get()};

  auto iterations = 20;
  auto batches = 20;
  std::mt19937 rng{seed};
  int expectedNumArrays = 0;
  bool checkMemoryLeak = true;

  auto compare = [&](const velox::VectorPtr& vector) {
    ASSERT_EQ(
        vector->wrappedVector()
            ->as<velox::RowVector>()
            ->childAt(0)
            ->loadedVector()
            ->wrappedVector()
            ->size(),
        expectedNumArrays);
  };
  for (auto i = 0; i < iterations; ++i) {
    expectedNumArrays = 1;
    writeAndVerify(
        rng,
        *pool_.get(),
        rowType,
        [&](auto& /*type*/) {
          return vectorMaker.rowVector(
              {"dictionaryArray"},
              {
                  vectorMaker.arrayVectorNullable<int32_t>({{}, std::nullopt}),
              });
        },
        vectorEquals,
        batches,
        writerOptions,
        {},
        nullptr,
        compare,
        false,
        checkMemoryLeak);

    writeAndVerify(
        rng,
        *pool_.get(),
        rowType,
        [&](auto& /*type*/) {
          return vectorMaker.rowVector(
              {"dictionaryArray"},
              {
                  vectorMaker.arrayVectorNullable<int32_t>({std::nullopt}),
              });
        },
        vectorEquals,
        batches,
        writerOptions,
        {},
        nullptr,
        compare,
        false,
        checkMemoryLeak);

    expectedNumArrays = 2;
    writeAndVerify(
        rng,
        *pool_.get(),
        rowType,
        [&](auto& /*type*/) {
          return vectorMaker.rowVector(
              {"dictionaryArray"},
              {
                  vectorMaker.arrayVectorNullable<int32_t>({
                      std::vector<std::optional<int32_t>>{1, 2, std::nullopt},
                      {},
                      std::vector<std::optional<int32_t>>{1, 2, std::nullopt},
                      std::nullopt,
                      std::vector<std::optional<int32_t>>{1, 2, std::nullopt},
                      std::vector<std::optional<int32_t>>{1, 2, std::nullopt},
                      std::vector<std::optional<int32_t>>{1, 2},
                  }),
              });
        },
        vectorEquals,
        batches,
        writerOptions,
        {},
        nullptr,
        compare,
        false,
        checkMemoryLeak);

    expectedNumArrays = 2;
    writeAndVerify(
        rng,
        *pool_.get(),
        rowType,
        [&](auto& /*type*/) {
          return vectorMaker.rowVector(
              {"dictionaryArray"},
              {
                  vectorMaker.arrayVectorNullable<int32_t>({
                      std::vector<std::optional<int32_t>>{1, 3},
                      std::vector<std::optional<int32_t>>{1, 2},
                      {},
                      std::vector<std::optional<int32_t>>{1, 2},
                      std::nullopt,
                      std::vector<std::optional<int32_t>>{1, 2},
                  }),
              });
        },
        vectorEquals,
        batches,
        writerOptions,
        {},
        nullptr,
        compare,
        false,
        checkMemoryLeak);

    expectedNumArrays = 1;
    writeAndVerify(
        rng,
        *pool_.get(),
        rowType,
        [&](auto& /*type*/) {
          return vectorMaker.rowVector(
              {"dictionaryArray"},
              {
                  vectorMaker.arrayVectorNullable<int32_t>({
                      std::vector<std::optional<int32_t>>{1, 2},
                      std::vector<std::optional<int32_t>>{1, 2},
                      {},
                      std::nullopt,
                      std::vector<std::optional<int32_t>>{1, 2},
                  }),
              });
        },
        vectorEquals,
        batches,
        writerOptions,
        {},
        nullptr,
        compare,
        false,
        checkMemoryLeak);

    expectedNumArrays = 1;
    writeAndVerify(
        rng,
        *pool_.get(),
        rowType,
        [&](auto& /*type*/) {
          return vectorMaker.rowVector(
              {"dictionaryArray"},
              {
                  vectorMaker.arrayVectorNullable<int32_t>({
                      std::vector<std::optional<int32_t>>{1, 2},
                      std::vector<std::optional<int32_t>>{1, 2},
                      std::vector<std::optional<int32_t>>{1, 2},
                      {},
                      std::nullopt,
                  }),
              });
        },
        vectorEquals,
        batches,
        writerOptions,
        {},
        nullptr,
        compare,
        false,
        checkMemoryLeak);

    expectedNumArrays = 1;
    writeAndVerify(
        rng,
        *pool_.get(),
        rowType,
        [&](auto& /*type*/) {
          return vectorMaker.rowVector(
              {"dictionaryArray"},
              {
                  vectorMaker.arrayVectorNullable<int32_t>({
                      {},
                      std::nullopt,
                      std::vector<std::optional<int32_t>>{1, 2},
                      std::vector<std::optional<int32_t>>{1, 2},
                      std::vector<std::optional<int32_t>>{1, 2},
                  }),
              });
        },
        vectorEquals,
        batches,
        writerOptions,
        {},
        nullptr,
        compare,
        false,
        checkMemoryLeak);
  }
}

TEST_P(VeloxReaderTests, ArrayWithOffsetsMultiskips) {
  bool multithreaded = GetParam();

  auto type = velox::ROW({
      {"dictionaryArray", velox::ARRAY(velox::INTEGER())},
  });
  auto rowType = std::dynamic_pointer_cast<const velox::RowType>(type);
  auto seed = folly::Random::rand32();
  LOG(INFO) << "seed: " << seed;

  alpha::VeloxWriterOptions writerOptions;
  writerOptions.dictionaryArrayColumns.insert("dictionaryArray");
  if (multithreaded) {
    writerOptions.parallelEncoding = true;
    writerOptions.parallelWriting = true;
    writerOptions.parallelExecutor =
        std::make_shared<folly::CPUThreadPoolExecutor>(
            std::thread::hardware_concurrency(),
            std::thread::hardware_concurrency());
  }
  velox::test::VectorMaker vectorMaker{pool_.get()};

  auto iterations = 50;
  auto batches = 20;
  std::mt19937 rng{seed};
  int expectedNumArrays = 0;
  bool checkMemoryLeak = true;

  auto compare = [&](const velox::VectorPtr& vector) {
    ASSERT_EQ(
        vector->wrappedVector()
            ->as<velox::RowVector>()
            ->childAt(0)
            ->loadedVector()
            ->wrappedVector()
            ->size(),
        expectedNumArrays);
  };

  auto strideVector = [&](const std::vector<std::vector<int32_t>>& vector) {
    std::vector<std::vector<int32_t>> stridedVector;

    for (auto const& vec : vector) {
      for (uint32_t idx = 0; idx < folly::Random::rand32(1, 5, rng); ++idx) {
        stridedVector.push_back(vec);
      }
    }
    return stridedVector;
  };

  for (auto i = 0; i < iterations; ++i) {
    expectedNumArrays = 6;
    writeAndVerify(
        rng,
        *pool_.get(),
        rowType,
        [&](auto& /*type*/) {
          return vectorMaker.rowVector(
              {"dictionaryArray"},
              {
                  // The middle element is a 0 length element and not null
                  vectorMaker.arrayVector<int32_t>(strideVector(
                      {{1, 2}, {1, 2, 3}, {}, {1, 2, 3}, {}, {4, 5, 6, 7}})),
              });
        },
        vectorEquals,
        batches,
        writerOptions,
        {},
        nullptr,
        compare,
        true,
        checkMemoryLeak);

    expectedNumArrays = 3;
    writeAndVerify(
        rng,
        *pool_.get(),
        rowType,
        [&](auto& /*type*/) {
          return vectorMaker.rowVector(
              {"dictionaryArray"},
              {
                  vectorMaker.arrayVectorNullable<int32_t>({
                      std::vector<std::optional<int32_t>>{1, 2},
                      std::vector<std::optional<int32_t>>{1, 2, 3},
                      std::nullopt,
                      std::vector<std::optional<int32_t>>{1, 2, 3},
                      std::nullopt,
                      std::vector<std::optional<int32_t>>{4, 5, 6, 7},
                  }),
              });
        },
        vectorEquals,
        batches,
        writerOptions,
        {},
        nullptr,
        compare,
        true,
        checkMemoryLeak);
  }
}

// convert map to struct
template <typename T = int32_t>
bool compareFlatMap(
    const velox::VectorPtr& expected,
    const velox::VectorPtr& actual,
    velox::vector_size_t index) {
  auto mapVector = expected->as<velox::MapVector>();
  auto offsets = mapVector->rawOffsets();
  auto sizes = mapVector->rawSizes();
  auto keysVector = mapVector->mapKeys()->asFlatVector<T>();
  auto valuesVector = mapVector->mapValues();

  auto structVector = actual->as<velox::RowVector>();
  folly::F14FastMap<std::string, velox::vector_size_t> columnOffsets(
      structVector->childrenSize());
  for (auto i = 0; i < structVector->childrenSize(); ++i) {
    columnOffsets[structVector->type()->asRow().nameOf(i)] = i;
  }

  std::unordered_set<std::string> keys;
  if (!mapVector->isNullAt(index)) {
    for (auto i = offsets[index]; i < offsets[index] + sizes[index]; ++i) {
      auto key = keysVector->valueAtFast(i);
      keys.insert(folly::to<std::string>(key));
      if (!valuesVector->equalValueAt(
              structVector->childAt(columnOffsets[folly::to<std::string>(key)])
                  .get(),
              i,
              index)) {
        return false;
      }
    }
  }
  // missing keys should be null
  for (const auto& columnOffset : columnOffsets) {
    if (keys.count(folly::to<std::string>(columnOffset.first)) == 0 &&
        !structVector->childAt(columnOffset.second)->isNullAt(index)) {
      return false;
    }
  }

  return true;
}

template <typename T = int32_t>
bool compareFlatMaps(
    const velox::VectorPtr& expected,
    const velox::VectorPtr& actual,
    velox::vector_size_t index) {
  auto flat = velox::BaseVector::create(
      expected->type(), expected->size(), expected->pool());
  flat->copy(expected.get(), 0, 0, expected->size());
  auto expectedRow = flat->as<velox::RowVector>();
  auto actualRow = actual->as<velox::RowVector>();
  EXPECT_EQ(expectedRow->childrenSize(), actualRow->childrenSize());
  for (auto i = 0; i < expectedRow->childrenSize(); ++i) {
    auto columnType = actualRow->type()->childAt(i);
    if (columnType->kind() != velox::TypeKind::ROW) {
      return false;
    }
    if (!compareFlatMap<T>(
            expectedRow->childAt(i), actualRow->childAt(i), index)) {
      return false;
    }
  }
  return true;
}

template <typename T>
void testFlatMapNullValues() {
  auto type = velox::ROW(
      {{"fld", velox::MAP(velox::INTEGER(), velox::CppToType<T>::create())}});

  std::string file;
  auto writeFile = std::make_unique<velox::InMemoryWriteFile>(&file);

  facebook::alpha::VeloxWriterOptions writerOptions;
  writerOptions.flatMapColumns.insert("fld");

  alpha::VeloxWriter writer(
      *rootPool, type, std::move(writeFile), std::move(writerOptions));

  facebook::velox::test::VectorMaker vectorMaker(leafPool.get());
  auto values = vectorMaker.flatVectorNullable<T>(
      {std::nullopt, std::nullopt, std::nullopt});
  auto keys = vectorMaker.flatVector<int32_t>({1, 2, 3});
  auto vector = vectorMaker.rowVector(
      {"fld"}, {vectorMaker.mapVector({0, 1, 2}, keys, values)});

  writer.write(vector);
  writer.close();

  alpha::VeloxReadParams readParams;
  velox::InMemoryReadFile readFile(file);
  auto selector = std::make_shared<velox::dwio::common::ColumnSelector>(type);
  alpha::VeloxReader reader(
      *leafPool, &readFile, std::move(selector), readParams);

  velox::VectorPtr output;
  auto size = 3;
  reader.next(size, output);
  for (auto i = 0; i < size; ++i) {
    EXPECT_TRUE(vectorEquals(vector, output, i));
  }
}

TEST_F(VeloxReaderTests, FlatMapNullValues) {
  testFlatMapNullValues<int8_t>();
  testFlatMapNullValues<int16_t>();
  testFlatMapNullValues<int32_t>();
  testFlatMapNullValues<int64_t>();
  testFlatMapNullValues<float>();
  testFlatMapNullValues<double>();
  testFlatMapNullValues<velox::StringView>();
}

TEST_P(VeloxReaderTests, FlatMapToStruct) {
  bool multithreaded = GetParam();

  auto floatFeatures = velox::MAP(velox::INTEGER(), velox::REAL());
  auto idListFeatures =
      velox::MAP(velox::INTEGER(), velox::ARRAY(velox::BIGINT()));
  auto idScoreListFeatures =
      velox::MAP(velox::INTEGER(), velox::MAP(velox::BIGINT(), velox::REAL()));
  auto rowColumn = velox::MAP(
      velox::INTEGER(),
      velox::ROW({{"a", velox::INTEGER()}, {"b", velox::REAL()}}));

  auto type = velox::ROW({
      {"float_features", floatFeatures},
      {"id_list_features", idListFeatures},
      {"id_score_list_features", idScoreListFeatures},
      {"row_column", rowColumn},
  });
  auto rowType = std::dynamic_pointer_cast<const velox::RowType>(type);

  VeloxMapGeneratorConfig generatorConfig{
      .rowType = rowType,
      .keyType = velox::TypeKind::INTEGER,
      .maxSizeForMap = 10};
  VeloxMapGenerator generator(pool_.get(), generatorConfig);

  alpha::VeloxWriterOptions writerOptions;
  writerOptions.flatMapColumns.insert("float_features");
  writerOptions.flatMapColumns.insert("id_list_features");
  writerOptions.flatMapColumns.insert("id_score_list_features");
  writerOptions.flatMapColumns.insert("row_column");
  if (multithreaded) {
    writerOptions.parallelEncoding = true;
    writerOptions.parallelWriting = true;
    writerOptions.parallelExecutor =
        std::make_shared<folly::CPUThreadPoolExecutor>(
            std::thread::hardware_concurrency(),
            std::thread::hardware_concurrency());
  }

  alpha::VeloxReadParams params;
  params.readFlatMapFieldAsStruct.insert("float_features");
  params.readFlatMapFieldAsStruct.insert("id_list_features");
  params.readFlatMapFieldAsStruct.insert("id_score_list_features");
  params.readFlatMapFieldAsStruct.insert("row_column");
  for (auto i = 0; i < 10; ++i) {
    params.flatMapFeatureSelector["float_features"].features.push_back(
        folly::to<std::string>(i));
    params.flatMapFeatureSelector["id_list_features"].features.push_back(
        folly::to<std::string>(i));
    params.flatMapFeatureSelector["id_score_list_features"].features.push_back(
        folly::to<std::string>(i));
    params.flatMapFeatureSelector["row_column"].features.push_back(
        folly::to<std::string>(i));
  }

  auto iterations = 20;
  auto batches = 10;
  for (auto i = 0; i < iterations; ++i) {
    writeAndVerify(
        generator.rng(),
        *pool_,
        rowType,
        [&](auto&) { return generator.generateBatch(10); },
        compareFlatMaps<int32_t>,
        batches,
        writerOptions,
        params);
  }
}

TEST_P(VeloxReaderTests, FlatMapToStructForComplexType) {
  bool multithreaded = GetParam();
  auto rowColumn = velox::MAP(
      velox::INTEGER(),
      velox::ROW(
          {{"a", velox::INTEGER()},
           {"b", velox::MAP(velox::INTEGER(), velox::ARRAY(velox::REAL()))}}));

  auto type = velox::ROW({
      {"row_column", rowColumn},
  });
  auto rowType = std::dynamic_pointer_cast<const velox::RowType>(type);

  VeloxMapGeneratorConfig generatorConfig{
      .rowType = rowType,
      .keyType = velox::TypeKind::INTEGER,
      .maxSizeForMap = 10};
  VeloxMapGenerator generator(pool_.get(), generatorConfig);

  alpha::VeloxWriterOptions writerOptions;
  writerOptions.flatMapColumns.insert("row_column");
  if (multithreaded) {
    writerOptions.parallelEncoding = true;
    writerOptions.parallelWriting = true;
    writerOptions.parallelExecutor =
        std::make_shared<folly::CPUThreadPoolExecutor>(
            std::thread::hardware_concurrency(),
            std::thread::hardware_concurrency());
  }

  alpha::VeloxReadParams params;
  params.readFlatMapFieldAsStruct.insert("row_column");
  for (auto i = 0; i < 10; ++i) {
    params.flatMapFeatureSelector["row_column"].features.push_back(
        folly::to<std::string>(i));
  }

  auto iterations = 20;
  auto batches = 10;
  for (auto i = 0; i < iterations; ++i) {
    writeAndVerify(
        generator.rng(),
        *pool_,
        rowType,
        [&](auto&) { return generator.generateBatch(10); },
        compareFlatMaps<int32_t>,
        batches,
        writerOptions,
        params);
  }
}

TEST_P(VeloxReaderTests, StringKeyFlatMapAsStruct) {
  bool multithreaded = GetParam();

  auto stringKeyFeatures = velox::MAP(velox::VARCHAR(), velox::REAL());
  auto type = velox::ROW({
      {"string_key_feature", stringKeyFeatures},
  });
  auto rowType = std::dynamic_pointer_cast<const velox::RowType>(type);

  alpha::VeloxWriterOptions writerOptions;
  writerOptions.flatMapColumns.insert("string_key_feature");
  if (multithreaded) {
    writerOptions.parallelEncoding = true;
    writerOptions.parallelWriting = true;
    writerOptions.parallelExecutor =
        std::make_shared<folly::CPUThreadPoolExecutor>(
            std::thread::hardware_concurrency(),
            std::thread::hardware_concurrency());
  }

  VeloxMapGeneratorConfig generatorConfig{
      .rowType = rowType,
      .keyType = velox::TypeKind::VARCHAR,
      .maxSizeForMap = 10,
      .stringKeyPrefix = "testKeyString_",
  };
  VeloxMapGenerator generator(pool_.get(), generatorConfig);

  alpha::VeloxReadParams params;
  params.readFlatMapFieldAsStruct.emplace("string_key_feature");
  for (auto i = 0; i < 10; ++i) {
    params.flatMapFeatureSelector["string_key_feature"].features.push_back(
        "testKeyString_" + folly::to<std::string>(i));
  }

  auto iterations = 10;
  auto batches = 1;
  for (auto i = 0; i < iterations; ++i) {
    writeAndVerify(
        generator.rng(),
        *pool_,
        rowType,
        [&](auto&) { return generator.generateBatch(10); },
        compareFlatMaps<velox::StringView>,
        batches,
        writerOptions,
        params);
  }

  iterations = 20;
  batches = 10;
  for (auto i = 0; i < iterations; ++i) {
    writeAndVerify(
        generator.rng(),
        *pool_,
        rowType,
        [&](auto&) { return generator.generateBatch(10); },
        compareFlatMaps<velox::StringView>,
        batches,
        writerOptions,
        params);
  }
}

TEST_P(VeloxReaderTests, FlatMapAsMapEncoding) {
  bool multithreaded = GetParam();

  auto floatFeatures = velox::MAP(velox::INTEGER(), velox::REAL());
  auto idListFeatures =
      velox::MAP(velox::INTEGER(), velox::ARRAY(velox::BIGINT()));
  auto idScoreListFeatures =
      velox::MAP(velox::INTEGER(), velox::MAP(velox::BIGINT(), velox::REAL()));
  auto type = velox::ROW({
      {"float_features", floatFeatures},
      {"id_list_features", idListFeatures},
      {"id_score_list_features", idScoreListFeatures},
  });
  auto rowType = std::dynamic_pointer_cast<const velox::RowType>(type);
  VeloxMapGeneratorConfig generatorConfig{
      .rowType = rowType,
      .keyType = velox::TypeKind::INTEGER,
  };
  VeloxMapGenerator generator(pool_.get(), generatorConfig);

  alpha::VeloxWriterOptions writerOptions;
  writerOptions.flatMapColumns.emplace("float_features");
  writerOptions.flatMapColumns.emplace("id_list_features");
  writerOptions.flatMapColumns.emplace("id_score_list_features");
  if (multithreaded) {
    writerOptions.parallelEncoding = true;
    writerOptions.parallelWriting = true;
    writerOptions.parallelExecutor =
        std::make_shared<folly::CPUThreadPoolExecutor>(
            std::thread::hardware_concurrency(),
            std::thread::hardware_concurrency());
  }

  // Verify the flatmap read without feature selection they are read as
  // MapEncoding
  alpha::VeloxReadParams params;
  auto iterations = 10;
  auto batches = 10;
  for (auto i = 0; i < iterations; ++i) {
    writeAndVerify(
        generator.rng(),
        *pool_,
        rowType,
        [&](auto&) { return generator.generateBatch(10); },
        vectorEquals,
        batches,
        writerOptions,
        params);
  }

  for (auto i = 0; i < 10; ++i) {
    params.flatMapFeatureSelector["float_features"].features.push_back(
        folly::to<std::string>(i));
    params.flatMapFeatureSelector["id_list_features"].features.push_back(
        folly::to<std::string>(i));
    params.flatMapFeatureSelector["id_score_list_features"].features.push_back(
        folly::to<std::string>(i));
  }

  for (auto i = 0; i < iterations; ++i) {
    writeAndVerify(
        generator.rng(),
        *pool_,
        rowType,
        [&](auto&) { return generator.generateBatch(10); },
        vectorEquals,
        batches,
        writerOptions,
        params);
  }

  {
    // Selecting only odd values column from flat map
    params.flatMapFeatureSelector.clear();
    for (auto i = 0; i < 10; ++i) {
      if (i % 2 == 1) {
        params.flatMapFeatureSelector["float_features"].features.push_back(
            folly::to<std::string>(i));
        params.flatMapFeatureSelector["id_list_features"].features.push_back(
            folly::to<std::string>(i));
        params.flatMapFeatureSelector["id_score_list_features"]
            .features.push_back(folly::to<std::string>(i));
      }
    }

    std::unordered_set<std::string> floatFeaturesLookup{
        params.flatMapFeatureSelector["float_features"].features.begin(),
        params.flatMapFeatureSelector["float_features"].features.end()};
    std::unordered_set<std::string> idListFeaturesLookup{
        params.flatMapFeatureSelector["id_list_features"].features.begin(),
        params.flatMapFeatureSelector["id_list_features"].features.end()};
    std::unordered_set<std::string> idScoreListFeaturesLookup{
        params.flatMapFeatureSelector["id_score_list_features"]
            .features.begin(),
        params.flatMapFeatureSelector["id_score_list_features"].features.end()};
    auto isKeyPresent = [&](std::string& key) {
      return floatFeaturesLookup.find(key) != floatFeaturesLookup.end() ||
          idListFeaturesLookup.find(key) != idListFeaturesLookup.end() ||
          idScoreListFeaturesLookup.find(key) !=
          idScoreListFeaturesLookup.end();
    };
    for (auto i = 0; i < iterations; ++i) {
      writeAndVerify(
          generator.rng(),
          *pool_,
          rowType,
          [&](auto&) { return generator.generateBatch(10); },
          vectorEquals,
          batches,
          writerOptions,
          params,
          isKeyPresent);
    }
  }

  {
    // Exclude odd values column from flat map
    params.flatMapFeatureSelector.clear();
    std::unordered_set<std::string> floatFeaturesLookup;
    std::unordered_set<std::string> idListFeaturesLookup;
    std::unordered_set<std::string> idScoreListFeaturesLookup;

    params.flatMapFeatureSelector["float_features"].mode =
        alpha::SelectionMode::Exclude;
    params.flatMapFeatureSelector["id_list_features"].mode =
        alpha::SelectionMode::Exclude;
    params.flatMapFeatureSelector["id_score_list_features"].mode =
        alpha::SelectionMode::Exclude;
    for (auto i = 0; i < 10; ++i) {
      std::string iStr = folly::to<std::string>(i);
      if (i % 2 == 1) {
        params.flatMapFeatureSelector["float_features"].features.push_back(
            iStr);
        params.flatMapFeatureSelector["id_list_features"].features.push_back(
            iStr);
        params.flatMapFeatureSelector["id_score_list_features"]
            .features.push_back(iStr);
      } else {
        floatFeaturesLookup.insert(iStr);
        idListFeaturesLookup.insert(iStr);
        idScoreListFeaturesLookup.insert(iStr);
      }
    }

    auto isKeyPresent = [&](std::string& key) {
      return floatFeaturesLookup.find(key) != floatFeaturesLookup.end() ||
          idListFeaturesLookup.find(key) != idListFeaturesLookup.end() ||
          idScoreListFeaturesLookup.find(key) !=
          idScoreListFeaturesLookup.end();
    };
    for (auto i = 0; i < iterations; ++i) {
      writeAndVerify(
          generator.rng(),
          *pool_,
          rowType,
          [&](auto&) { return generator.generateBatch(10); },
          vectorEquals,
          batches,
          writerOptions,
          params,
          isKeyPresent);
    }
  }
}

TEST_F(VeloxReaderTests, StringKeyFlatMapAsMapEncoding) {
  auto stringKeyFeatures = velox::MAP(velox::VARCHAR(), velox::REAL());
  auto type = velox::ROW({
      {"string_key_feature", stringKeyFeatures},
  });
  auto rowType = std::dynamic_pointer_cast<const velox::RowType>(type);

  VeloxMapGeneratorConfig generatorConfig{
      .rowType = rowType,
      .keyType = velox::TypeKind::VARCHAR,
      .stringKeyPrefix = "testKeyString_",
  };
  VeloxMapGenerator generator(pool_.get(), generatorConfig);

  alpha::VeloxWriterOptions writerOptions;
  writerOptions.flatMapColumns.insert("string_key_feature");

  alpha::VeloxReadParams params;
  // Selecting only keys with even index to it
  for (auto i = 0; i < 10; ++i) {
    if (i % 2 == 0) {
      params.flatMapFeatureSelector["string_key_feature"].features.push_back(
          "testKeyString_" + folly::to<std::string>(i));
    }
  }

  std::unordered_set<std::string> stringKeyFeature{
      params.flatMapFeatureSelector["string_key_feature"].features.begin(),
      params.flatMapFeatureSelector["string_key_feature"].features.end()};

  auto isKeyPresent = [&](std::string& key) {
    return stringKeyFeature.find(key) != stringKeyFeature.end();
  };

  auto iterations = 10;

  // Keeping the batchCount 1 produces the case where flatmap readers nulls
  // column is empty, as decodedmap produce the mayHavenulls as false
  auto batches = 1;
  for (auto i = 0; i < iterations; ++i) {
    writeAndVerify<velox::StringView>(
        generator.rng(),
        *pool_,
        rowType,
        [&](auto&) { return generator.generateBatch(10); },
        nullptr, /* for key present use a fix function */
        batches,
        writerOptions,
        params,
        isKeyPresent);
  }

  iterations = 20;
  batches = 10;
  for (auto i = 0; i < iterations; ++i) {
    writeAndVerify<velox::StringView>(
        generator.rng(),
        *pool_,
        rowType,
        [&](auto&) { return generator.generateBatch(10); },
        nullptr, /* for key present use a fix function */
        batches,
        writerOptions,
        params,
        isKeyPresent);
  }
}

class TestAlphaReaderFactory {
 public:
  TestAlphaReaderFactory(
      velox::memory::MemoryPool& memoryPool,
      std::vector<velox::VectorPtr> vectors,
      const alpha::VeloxWriterOptions& writerOptions = {})
      : memoryPool_(memoryPool) {
    file_ = std::make_unique<velox::InMemoryReadFile>(
        alpha::test::createAlphaFile(*rootPool, vectors, writerOptions));
    type_ = std::dynamic_pointer_cast<const velox::RowType>(vectors[0]->type());
  }

  alpha::VeloxReader createReader(alpha::VeloxReadParams params = {}) {
    auto selector =
        std::make_shared<velox::dwio::common::ColumnSelector>(type_);
    return alpha::VeloxReader(
        this->memoryPool_, file_.get(), std::move(selector), params);
  }

  alpha::Tablet createTablet() {
    return alpha::Tablet(memoryPool_, file_.get());
  }

 private:
  std::unique_ptr<velox::InMemoryReadFile> file_;
  std::shared_ptr<const velox::RowType> type_;
  velox::memory::MemoryPool& memoryPool_;
};

std::vector<velox::VectorPtr> createSkipSeekVectors(
    velox::memory::MemoryPool& pool,
    const std::vector<int>& rowsPerStripe) {
  auto seed = folly::Random::rand32();
  LOG(INFO) << "seed: " << seed;
  std::mt19937 rng{seed};

  std::vector<velox::VectorPtr> vectors(rowsPerStripe.size());
  velox::test::VectorMaker vectorMaker{&pool};

  for (auto i = 0; i < rowsPerStripe.size(); ++i) {
    std::string s;
    vectors[i] = vectorMaker.rowVector(
        {"a", "b", "dictionaryArray"},
        {vectorMaker.flatVector<int32_t>(
             rowsPerStripe[i],
             /* valueAt */
             [&rng](auto /* row */) { return folly::Random::rand32(rng); },
             /* isNullAt */ [](auto row) { return row % 2 == 1; }),
         vectorMaker.flatVector<velox::StringView>(
             rowsPerStripe[i],
             /* valueAt */
             [&s, &rng](auto /* row */) {
               s = "arf_" + folly::to<std::string>(folly::Random::rand32(rng));
               return velox::StringView(s.data(), s.size());
             },
             /* isNullAt */ [](auto row) { return row % 2 == 1; }),
         vectorMaker.arrayVector<int32_t>(
             rowsPerStripe[i],
             /* sizeAt */
             [](auto /* row */) { return 1; },
             /* valueAt */
             [](auto row) {
               /* duplicated values to check cache usage */
               return row / 4;
             })});
  }

  return vectors;
}

void readAndVerifyContent(
    alpha::VeloxReader& reader,
    std::vector<velox::VectorPtr> expectedVectors,
    uint32_t rowsToRead,
    uint32_t expectedNumberOfRows,
    uint32_t expectedStripe,
    velox::vector_size_t expectedRowInStripe) {
  velox::VectorPtr result;
  EXPECT_TRUE(reader.next(rowsToRead, result));
  ASSERT_EQ(result->type()->kind(), velox::TypeKind::ROW);
  velox::RowVector* rowVec = result->as<velox::RowVector>();
  ASSERT_EQ(rowVec->childAt(0)->type()->kind(), velox::TypeKind::INTEGER);
  ASSERT_EQ(rowVec->childAt(1)->type()->kind(), velox::TypeKind::VARCHAR);
  ASSERT_EQ(rowVec->childAt(2)->type()->kind(), velox::TypeKind::ARRAY);
  const int curRows = result->size();
  ASSERT_EQ(curRows, expectedNumberOfRows);
  ASSERT_LT(expectedStripe, expectedVectors.size());
  auto& expected = expectedVectors[expectedStripe];

  for (velox::vector_size_t i = 0; i < curRows; ++i) {
    if (!expected->equalValueAt(result.get(), i + expectedRowInStripe, i)) {
      ASSERT_TRUE(
          expected->equalValueAt(result.get(), i + expectedRowInStripe, i))
          << "Content mismatch at index " << i
          << "\nReference: " << expected->toString(i + expectedRowInStripe)
          << "\nResult: " << result->toString(i);
    }
  }
}

TEST_P(VeloxReaderTests, ReaderSeekTest) {
  bool multithreaded = GetParam();

  // Generate an Alpha file with 3 stripes and 10 rows each
  auto vectors = createSkipSeekVectors(*pool_, {10, 10, 10});
  alpha::VeloxWriterOptions writerOptions;
  writerOptions.dictionaryArrayColumns.insert("dictionaryArray");
  if (multithreaded) {
    writerOptions.parallelEncoding = true;
    writerOptions.parallelWriting = true;
    writerOptions.parallelExecutor =
        std::make_shared<folly::CPUThreadPoolExecutor>(
            std::thread::hardware_concurrency(),
            std::thread::hardware_concurrency());
  }

  TestAlphaReaderFactory readerFactory(*pool_, vectors, writerOptions);
  auto reader = readerFactory.createReader();

  auto rowResult = reader.skipRows(0);
  EXPECT_EQ(0, rowResult);
  rowResult = reader.seekToRow(0);
  EXPECT_EQ(0, rowResult);

  // [Stripe# 0, Current Pos: 0] seek to 1 position
  rowResult = reader.seekToRow(1);
  EXPECT_EQ(rowResult, 1);
  readAndVerifyContent(
      reader,
      vectors,
      /* rowsToRead */ 1,
      /* expectedNumberOfRows */ 1,
      /* expectedStripe */ 0,
      /* expectedRowInStripe */ 1);

  // [Stripe# 0, Current Pos: 2] seek to 5 position
  rowResult = reader.seekToRow(5);
  // [Stripe# 0, Current Pos: 5] seeks start from rowIdx 0
  EXPECT_EQ(rowResult, 5);
  readAndVerifyContent(
      reader,
      vectors,
      /* rowsToRead */ 6,
      /* expectedNumberOfRows */ 5,
      /* expectedStripe */ 0,
      /* expectedRowInStripe */ 5);

  // [Stripe# 0, Current Pos: 10] seek to 10 position
  rowResult = reader.seekToRow(10);
  // [Stripe# 1, Current Pos: 0]
  EXPECT_EQ(rowResult, 10);
  readAndVerifyContent(
      reader,
      vectors,
      /* rowsToRead */ 10,
      /* expectedNumberOfRows */ 10,
      /* expectedStripe */ 1,
      /* expectedRowInStripe */ 0);

  // [Stripe# 2, Current Pos: 0]
  rowResult = reader.seekToRow(29);
  // [Stripe# 2, Current Pos: 9]
  EXPECT_EQ(rowResult, 29);
  readAndVerifyContent(
      reader,
      vectors,
      /* rowsToRead */ 2,
      /* expectedNumberOfRows */ 1,
      /* expectedStripe */ 2,
      /* expectedRowInStripe */ 9);

  // seek past
  {
    rowResult = reader.seekToRow(32);
    // Seeks with rows >= totalRows in Alpha file, seeks to lastRow
    EXPECT_EQ(rowResult, 30);
    velox::VectorPtr result;
    EXPECT_FALSE(reader.next(1, result));
  }
}

TEST_P(VeloxReaderTests, ReaderSkipTest) {
  bool multithreaded = GetParam();

  // Generate an Alpha file with 3 stripes and 10 rows each
  auto vectors = createSkipSeekVectors(*pool_, {10, 10, 10});
  alpha::VeloxWriterOptions writerOptions;
  writerOptions.dictionaryArrayColumns.insert("dictionaryArray");
  if (multithreaded) {
    writerOptions.parallelEncoding = true;
    writerOptions.parallelWriting = true;
    writerOptions.parallelExecutor =
        std::make_shared<folly::CPUThreadPoolExecutor>(
            std::thread::hardware_concurrency(),
            std::thread::hardware_concurrency());
  }

  TestAlphaReaderFactory readerFactory(*pool_, vectors, writerOptions);
  auto reader = readerFactory.createReader();

  // Current position in Comments below represent the position in stripe
  // [Stripe# 0, Current Pos: 0], After skip [Stripe# 0, Current Pos: 1]
  auto rowResult = reader.skipRows(1);
  EXPECT_EQ(rowResult, 1);
  // readAndVerifyContent() moves the rowPosition in reader
  readAndVerifyContent(
      reader,
      vectors,
      /* rowsToRead */ 1,
      /* expectedNumberOfRows */ 1,
      /* expectedStripe */ 0,
      /* expectedRowInStripe */ 1);

  // [Stripe# 0, Current Pos: 2], After skip [Stripe# 0, Current Pos: 7]
  rowResult = reader.skipRows(5);
  EXPECT_EQ(rowResult, 5);
  // reader don't read across stripe so expectedRow is 3
  readAndVerifyContent(
      reader,
      vectors,
      /* rowsToRead */ 4,
      /* expectedNumberOfRows */ 3,
      /* expectedStripe */ 0,
      /* expectedRowInStripe */ 7);

  // [Stripe# 1, Current Pos: 0], After skip [Stripe# 2, Current Pos: 0]
  rowResult = reader.skipRows(10);
  EXPECT_EQ(rowResult, 10);
  readAndVerifyContent(
      reader,
      vectors,
      /* rowsToRead */ 1,
      /* expectedNumberOfRows */ 1,
      /* expectedStripe */ 2,
      /* expectedRowInStripe */ 0);

  // [Stripe# 2, Current Pos: 1], After skip [Stripe# 2, Current Pos: 9]
  rowResult = reader.skipRows(8);
  EXPECT_EQ(rowResult, 8);
  // reader don't read across stripe so expectedRow is 3
  readAndVerifyContent(
      reader,
      vectors,
      /* rowsToRead */ 2,
      /* expectedNumberOfRows */ 1,
      /* expectedStripe */ 2,
      /* expectedRowInStripe */ 9);

  {
    // [Stripe# 3, Current Pos: 0], Reached EOF
    rowResult = reader.skipRows(5);
    EXPECT_EQ(rowResult, 0);
    velox::VectorPtr result;
    EXPECT_FALSE(reader.next(1, result));
  }

  // Try to seek to start and test skip
  rowResult = reader.seekToRow(0);
  EXPECT_EQ(0, rowResult);
  // [Stripe# 0, Current Pos: 0], After skip [Stripe# 1, Current Pos: 2]
  rowResult = reader.skipRows(12);
  EXPECT_EQ(rowResult, 12);
  readAndVerifyContent(
      reader,
      vectors,
      /* rowsToRead */ 10,
      /* expectedNumberOfRows */ 8,
      /* expectedStripe */ 1,
      /* expectedRowInStripe */ 2);

  // Test continuous skip calls and then readandVerify
  reader.seekToRow(0);
  // [Stripe# 0, Current Pos: 0], After skip [Stripe# 1, Current Pos: 0]
  for (int i = 0; i < 10; ++i) {
    rowResult = reader.skipRows(1);
    EXPECT_EQ(rowResult, 1);
  }
  readAndVerifyContent(
      reader,
      vectors,
      /* rowsToRead */ 1,
      /* expectedNumberOfRows */ 1,
      /* expectedStripe */ 1,
      /* expectedRowInStripe */ 0);

  // Continuous skip calls across stripe
  // [Stripe# 1, Current Pos: 1], After skip [Stripe# 2, Current Pos: 9]
  for (int i = 0; i < 6; ++i) {
    rowResult = reader.skipRows(3);
  }
  readAndVerifyContent(
      reader,
      vectors,
      /* rowsToRead */ 2,
      /* expectedNumberOfRows */ 1,
      /* expectedStripe */ 2,
      /* expectedRowInStripe */ 9);

  {
    // Current position: EOF
    velox::VectorPtr result;
    EXPECT_FALSE(reader.next(1, result));
  }

  // Read the data(This also move the reader state) follow by skips and verify
  reader.seekToRow(0);
  for (int i = 0; i < 11; ++i) {
    readAndVerifyContent(
        reader,
        vectors,
        /* rowsToRead */ 1,
        /* expectedNumberOfRows */ 1,
        /* expectedStripe */ (i / 10),
        /* expectedRowInStripe */ (i % 10));
  }
  // [Stripe# 1, Current Pos: 1], After skip [Stripe# 1, Current Pos: 6]
  rowResult = reader.skipRows(5);
  EXPECT_EQ(rowResult, 5);
  readAndVerifyContent(
      reader,
      vectors,
      /* rowsToRead */ 5,
      /* expectedNumberOfRows */ 4,
      /* expectedStripe */ 1,
      /* expectedRowInStripe */ 6);

  {
    // verify the skip to more rows then file have
    reader.seekToRow(0);
    // [Stripe# 0, Current Pos: 0], After skip EOF
    rowResult = reader.skipRows(32);
    EXPECT_EQ(30, rowResult);
    velox::VectorPtr result;
    EXPECT_FALSE(reader.next(1, result));

    reader.seekToRow(0);
    // [Stripe# 0, Current Pos: 0], After skip [Stripe# 2, Current Pos: 2]
    rowResult = reader.skipRows(22);
    EXPECT_EQ(22, rowResult);
    readAndVerifyContent(
        reader,
        vectors,
        /* rowsToRead */ 9,
        /* expectedNumberOfRows */ 8,
        /* expectedStripe */ 2,
        /* expectedRowInStripe */ 2);
    EXPECT_FALSE(reader.next(1, result));
  }
}

TEST_P(VeloxReaderTests, ReaderSkipSingleStripeTest) {
  bool multithreaded = GetParam();

  // Generate an Alpha file with 1 stripe and 12 rows
  auto vectors = createSkipSeekVectors(*pool_, {12});
  alpha::VeloxWriterOptions writerOptions;
  writerOptions.dictionaryArrayColumns.insert("dictionaryArray");
  if (multithreaded) {
    writerOptions.parallelEncoding = true;
    writerOptions.parallelWriting = true;
    writerOptions.parallelExecutor =
        std::make_shared<folly::CPUThreadPoolExecutor>(
            std::thread::hardware_concurrency(),
            std::thread::hardware_concurrency());
  }

  TestAlphaReaderFactory readerFactory(*pool_, vectors, writerOptions);
  auto reader = readerFactory.createReader();

  // Current position in Comments below represent the position in stripe
  // [Stripe# 0, Current Pos: 0], After skip [Stripe# 0, Current Pos: 1]
  auto rowResult = reader.skipRows(1);
  EXPECT_EQ(rowResult, 1);
  readAndVerifyContent(
      reader,
      vectors,
      /* rowsToRead */ 12,
      /* expectedNumberOfRows */ 11,
      /* expectedStripe */ 0,
      /* expectedRowInStripe */ 1);

  // Current pos : EOF, try to read skip past it
  {
    rowResult = reader.skipRows(13);
    EXPECT_EQ(rowResult, 0);
    rowResult = reader.skipRows(1);
    EXPECT_EQ(rowResult, 0);
    velox::VectorPtr result;
    EXPECT_FALSE(reader.next(1, result));
  }

  // Seek to position 2 and then skip 11 rows to reach EOF
  rowResult = reader.seekToRow(2);
  EXPECT_EQ(rowResult, 2);
  rowResult = reader.skipRows(11);
  EXPECT_EQ(rowResult, 10);
  {
    velox::VectorPtr result;
    EXPECT_FALSE(reader.next(1, result));
  }

  // Seek to 0 and skip 13 rows
  rowResult = reader.seekToRow(0);
  EXPECT_EQ(rowResult, 0);
  rowResult = reader.skipRows(13);
  EXPECT_EQ(rowResult, 12);
  {
    velox::VectorPtr result;
    EXPECT_FALSE(reader.next(1, result));
  }
}

TEST_P(VeloxReaderTests, ReaderSeekSingleStripeTest) {
  bool multithreaded = GetParam();

  // Generate an Alpha file with 1 stripes and 11 rows
  auto vectors = createSkipSeekVectors(*pool_, {11});
  alpha::VeloxWriterOptions writerOptions;
  writerOptions.dictionaryArrayColumns.insert("dictionaryArray");
  if (multithreaded) {
    writerOptions.parallelEncoding = true;
    writerOptions.parallelWriting = true;
    writerOptions.parallelExecutor =
        std::make_shared<folly::CPUThreadPoolExecutor>(
            std::thread::hardware_concurrency(),
            std::thread::hardware_concurrency());
  }

  TestAlphaReaderFactory readerFactory(*pool_, vectors, writerOptions);
  auto reader = readerFactory.createReader();

  // Current position in Comments below represent the position in stripe
  // [Stripe# 0, Current Pos: 0], After skip [Stripe# 0, Current Pos: 5]
  auto rowResult = reader.seekToRow(5);
  EXPECT_EQ(rowResult, 5);
  readAndVerifyContent(
      reader,
      vectors,
      /* rowsToRead */ 12,
      /* expectedNumberOfRows */ 6,
      /* expectedStripe */ 0,
      /* expectedRowInStripe */ 5);

  // Current pos : EOF, try to read skip past it
  {
    rowResult = reader.seekToRow(15);
    EXPECT_EQ(rowResult, 11);
    velox::VectorPtr result;
    EXPECT_FALSE(reader.next(1, result));
    rowResult = reader.seekToRow(10000);
    EXPECT_EQ(rowResult, 11);
    EXPECT_FALSE(reader.next(1, result));
  }
}

TEST_F(VeloxReaderTests, ReaderSkipUnevenStripesTest) {
  // Generate an Alpha file with 4 stripes
  auto vectors = createSkipSeekVectors(*pool_, {12, 15, 25, 18});
  alpha::VeloxWriterOptions writerOptions;
  writerOptions.dictionaryArrayColumns.insert("dictionaryArray");

  TestAlphaReaderFactory readerFactory(*pool_, vectors, writerOptions);
  auto reader = readerFactory.createReader();

  // Current position in Comments below represent the position in stripe
  // [Stripe# 0, Current Pos: 0], After skip [Stripe# 2, Current Pos: 8]
  auto rowResult = reader.skipRows(35);
  EXPECT_EQ(rowResult, 35);
  readAndVerifyContent(
      reader,
      vectors,
      /* rowsToRead */ 12,
      /* expectedNumberOfRows */ 12,
      /* expectedStripe */ 2,
      /* expectedRowInStripe */ 8);

  // [Stripe# 2, Current Pos: 20], After skip EOF
  {
    rowResult = reader.skipRows(25);
    EXPECT_EQ(rowResult, 23);
    velox::VectorPtr result;
    EXPECT_FALSE(reader.next(1, result));
  }
}

template <typename T>
void getFieldDefaultValue(alpha::Vector<T>& input, uint32_t index) {
  static_assert(T() == 0, "Default Constructor value is not zero initialized");
  input[index] = T();
}

template <>
void getFieldDefaultValue(alpha::Vector<std::string>& input, uint32_t index) {
  input[index] = std::string();
}

template <typename T>
void verifyDefaultValue(T valueToBeUpdatedWith, T defaultValue, int32_t size) {
  alpha::Vector<T> testData(leafPool.get(), size);
  for (int i = 0; i < testData.size(); ++i) {
    getFieldDefaultValue<T>(testData, i);
    ASSERT_EQ(testData[i], defaultValue);
    testData[i] = valueToBeUpdatedWith;
    getFieldDefaultValue<T>(testData, i);
    ASSERT_EQ(testData[i], defaultValue);
  }
}

// this test is created to keep an eye on the default value for T() for
// primitive type. Recently it came to our notice that T() does zero
// initialize the value for optimized builds. T() we have used a bit in the
// code to zero out the result. This is a dummy test to fail fast if it is not
// zero initialized for primitive types
TEST_F(VeloxReaderTests, TestPrimitiveFieldDefaultValue) {
  verifyDefaultValue<velox::vector_size_t>(2, 0, 10);
  verifyDefaultValue<int8_t>(2, 0, 30);
  verifyDefaultValue<uint8_t>(2, 0, 30);
  verifyDefaultValue<int16_t>(2, 0, 30);
  verifyDefaultValue<uint16_t>(2, 0, 30);
  verifyDefaultValue<int64_t>(2, 0, 30);
  verifyDefaultValue<uint64_t>(2, 0, 30);
  verifyDefaultValue<uint32_t>(2, 0.0, 30);
  verifyDefaultValue<float>(2.1, 0.0, 30);
  verifyDefaultValue<bool>(true, false, 30);
  verifyDefaultValue<double>(3.2, 0.0, 30);
  verifyDefaultValue<std::string>("test", "", 30);
}

struct RangeTestParams {
  uint64_t rangeStart;
  uint64_t rangeEnd;

  // Tuple arguments: rowsToRead, expectedNumberOfRows, expectedStripe,
  // expectedRowInStripe
  std::vector<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>> expectedReads;

  // Tuple arguments: seekToRow, expectedSeekResult
  std::vector<std::tuple<uint32_t, uint32_t>> expectedSeeks;

  // Tuple arguments: skipRows, expectedSkipResult
  std::vector<std::tuple<uint32_t, uint32_t>> expectedSkips;
};

TEST_P(VeloxReaderTests, RangeReads) {
  bool multithreaded = GetParam();

  // Generate an Alpha file with 4 stripes
  auto vectors = createSkipSeekVectors(*pool_, {10, 15, 25, 9});
  alpha::VeloxWriterOptions writerOptions;
  writerOptions.dictionaryArrayColumns.insert("dictionaryArray");
  if (multithreaded) {
    writerOptions.parallelEncoding = true;
    writerOptions.parallelWriting = true;
    writerOptions.parallelExecutor =
        std::make_shared<folly::CPUThreadPoolExecutor>(
            std::thread::hardware_concurrency(),
            std::thread::hardware_concurrency());
  }

  TestAlphaReaderFactory readerFactory(*pool_, vectors, writerOptions);

  auto test = [&readerFactory, &vectors](RangeTestParams params) {
    auto reader = readerFactory.createReader(alpha::VeloxReadParams{
        .fileRangeStartOffset = params.rangeStart,
        .fileRangeEndOffset = params.rangeEnd});

    for (const auto& expectedRead : params.expectedReads) {
      readAndVerifyContent(
          reader,
          vectors,
          /* rowsToRead */ std::get<0>(expectedRead),
          /* expectedNumberOfRows */ std::get<1>(expectedRead),
          /* expectedStripe */ std::get<2>(expectedRead),
          /* expectedRowInStripe */ std::get<3>(expectedRead));
    }

    {
      velox::VectorPtr result;
      EXPECT_FALSE(reader.next(1, result));
    }

    for (const auto& expectedSeek : params.expectedSeeks) {
      EXPECT_EQ(
          std::get<1>(expectedSeek),
          reader.seekToRow(std::get<0>(expectedSeek)));
    }

    reader.seekToRow(0);
    for (const auto& expectedSkip : params.expectedSkips) {
      EXPECT_EQ(
          std::get<1>(expectedSkip),
          reader.skipRows(std::get<0>(expectedSkip)));
    }

    reader.seekToRow(0);
    for (const auto& expectedRead : params.expectedReads) {
      readAndVerifyContent(
          reader,
          vectors,
          /* rowsToRead */ std::get<0>(expectedRead),
          /* expectedNumberOfRows */ std::get<1>(expectedRead),
          /* expectedStripe */ std::get<2>(expectedRead),
          /* expectedRowInStripe */ std::get<3>(expectedRead));
    }

    {
      velox::VectorPtr result;
      EXPECT_FALSE(reader.next(1, result));
    }
  };

  // Try to read all data in the file. Since we cover the entire file (end is
  // bigger than the file size), we expect to be able to read all lines.
  LOG(INFO) << "--> Range covers the entire file";
  LOG(INFO) << "File:     |--s0--|--s1--|--s2--|--s3--|";
  LOG(INFO) << "Range:    |---------------------------------|";
  LOG(INFO) << "Expected: |--s0--|--s1--|--s2--|--s3--|";
  test({
      .rangeStart = 0,
      .rangeEnd = 100'000'000,

      // Reads stop at stripe boundaries, so we need to invoke several reads
      // to read the entire file.
      .expectedReads =
          {{30, 10, 0, 0}, {30, 15, 1, 0}, {30, 25, 2, 0}, {30, 9, 3, 0}},

      // Seeks should be allowed to anywhere in this file (rows 0 to 59)
      .expectedSeeks =
          {{0, 0},
           {5, 5},
           {10, 10},
           {15, 15},
           {25, 25},
           {30, 30},
           {45, 45},
           {50, 50},
           {55, 55},
           {59, 59},
           {60, 59}},

      // Skips should cover the entire file (59 rows)
      .expectedSkips = {{0, 0}, {10, 10}, {20, 20}, {30, 29}, {1, 0}},
  });

  // Test a range covering only the first stripe.
  // Using range starting at 0 guarantees we cover the first stripe.
  // Since first stripe is much greater than 1 byte, using range ending at 1,
  // guarantees we don't cover any other stripe other than the fisrt stripe.
  LOG(INFO) << "--> Range covers beginning of first stripe";
  LOG(INFO) << "File:     |--s0--|--s1--|--s2--|--s3--|";
  LOG(INFO) << "Range:    |---|";
  LOG(INFO) << "Expected: |--s0--|";
  test({
      .rangeStart = 0,
      .rangeEnd = 1,

      // Reads should only find rows in stripe 0.
      .expectedReads = {{5, 5, 0, 0}, {10, 5, 0, 5}},

      // Seeks should be allowed to access rows in  first stripe only (rows 0
      // to 10)
      .expectedSeeks =
          {{0, 0}, {5, 5}, {10, 10}, {15, 10}, {30, 10}, {59, 10}, {60, 10}},

      // Skips should cover first stripe only (59 rows)
      .expectedSkips = {{0, 0}, {5, 5}, {10, 5}, {1, 0}},
  });

  auto tablet = readerFactory.createTablet();

  // Test a range starting somewhere in the first stripe (but not at zero
  // offset) to exactly the end of the first stripe. This should be resolved
  // to zero stripes.
  LOG(INFO) << "--> Range covers end of stripe 0";
  LOG(INFO) << "File:     |--s0--|--s1--|--s2--|--s3--|";
  LOG(INFO) << "Range:       |---|";
  LOG(INFO) << "Expected: <empty>>";
  test({
      .rangeStart = 1,
      .rangeEnd = tablet.stripeOffset(1),

      // No read should succeed, as we have zero stripes to read from
      .expectedReads = {},

      // All seeks should be ignored
      .expectedSeeks =
          {{0, 0}, {5, 0}, {10, 0}, {15, 0}, {30, 0}, {59, 0}, {60, 0}},

      // All skips should be ignored
      .expectedSkips = {{0, 0}, {5, 0}, {59, 0}},
  });

  // Test a range starting somewhere in stripe 0 (but not at zero) to somwhere
  // in stripe 1. This should resolve to only stripe 1.
  LOG(INFO) << "--> Range covers beginning of stripe 1";
  LOG(INFO) << "File:     |--s0--|--s1--|--s2--|--s3--|";
  LOG(INFO) << "Range:       |------|";
  LOG(INFO) << "Expected:        |--s1--|";

  test({
      .rangeStart = 1,
      .rangeEnd = tablet.stripeOffset(1) + 1,

      // Reads should all resolve to stripe 1
      .expectedReads = {{5, 5, 1, 0}, {20, 10, 1, 5}},

      // Seeks should succeed if they are in range [10, 25). Otherwise, they
      // should return the edges of stripe 1.
      .expectedSeeks =
          {{0, 10},
           {5, 10},
           {10, 10},
           {15, 15},
           {25, 25},
           {26, 25},
           {59, 25},
           {60, 25}},

      // Skips should allow skipping only 15 rows (number of rows in stripe 1)
      .expectedSkips = {{0, 0}, {5, 5}, {11, 10}, {1, 0}},
  });

  // Test a range starting exactly on stripe 2 to somwhere
  // in stripe 2. This should resolve to only stripe 2.
  LOG(INFO) << "--> Range starts at beginning of stripe 2";
  LOG(INFO) << "File:     |--s0--|--s1--|--s2--|--s3--|";
  LOG(INFO) << "Range:           |---|";
  LOG(INFO) << "Expected:        |--s1--|";
  test({
      .rangeStart = tablet.stripeOffset(1),
      .rangeEnd = tablet.stripeOffset(1) + 1,

      // Reads should all resolve to stripe 1
      .expectedReads = {{5, 5, 1, 0}, {20, 10, 1, 5}},

      // Seeks should succeed if they are in range [10, 25). Otherwise, they
      // should return the edges of stripe 1.
      .expectedSeeks =
          {{0, 10},
           {5, 10},
           {10, 10},
           {15, 15},
           {25, 25},
           {26, 25},
           {59, 25},
           {60, 25}},

      // Skips should allow skipping only 15 rows (number of rows in stripe 1)
      .expectedSkips = {{0, 0}, {5, 5}, {11, 10}, {1, 0}},
  });

  // Test a range spanning multiple stripes. We'll start somewhere in stripe 0
  // and end somewhere in stripe 2. This should resolve to stripes 1 and 2.
  LOG(INFO) << "--> Range spans stripes (0, 1 ,2)";
  LOG(INFO) << "File:     |--s0--|--s1--|--s2--|--s3--|";
  LOG(INFO) << "Range:        |------------|";
  LOG(INFO) << "Expected:        |--s1--|--s2--|";
  test({
      .rangeStart = tablet.stripeOffset(1) - 1,
      .rangeEnd = tablet.stripeOffset(2) + 1,

      // Reads should all resolve to stripes 1 and 2 (rows [15 to 50)).
      // Reads stop at stripe boundaries, so we need to invoke several reads
      // to continue to the next stripe.
      .expectedReads =
          {{5, 5, 1, 0}, {20, 10, 1, 5}, {20, 20, 2, 0}, {20, 5, 2, 20}},

      // Seeks should succeed if they are in range [10, 50). Otherwise, they
      // should return the edges of stripe 1 and 2.
      .expectedSeeks =
          {{0, 10},
           {5, 10},
           {10, 10},
           {15, 15},
           {25, 25},
           {26, 26},
           {49, 49},
           {50, 50},
           {59, 50},
           {60, 50}},

      // Skips should allow skipping only 40 rows (number of rows in stripes 1
      // and 2)
      .expectedSkips = {{0, 0}, {5, 5}, {11, 11}, {23, 23}, {2, 1}, {1, 0}},
  });

  // Test a range spanning multiple stripes. We'll start at the beginning of
  // stripe 1 and end somewhere in stripe 3. This should resolve to stripes 1,
  // 2 and 3.
  LOG(INFO) << "--> Range spans stripes (1 ,2, 3)";
  LOG(INFO) << "File:     |--s0--|--s1--|--s2--|--s3--|";
  LOG(INFO) << "Range:           |----------------|";
  LOG(INFO) << "Expected:        |--s1--|--s2--|--s3--|";
  test({
      .rangeStart = tablet.stripeOffset(1),
      .rangeEnd = tablet.stripeOffset(3) + 1,

      // Reads should all resolve to stripes 1, 2 and 3 (rows [15 to 59)).
      // Reads stop at stripe boundaries, so we need to invoke several reads
      // to continue to the next stripe.
      .expectedReads =
          {{5, 5, 1, 0},
           {20, 10, 1, 5},
           {20, 20, 2, 0},
           {20, 5, 2, 20},
           {20, 9, 3, 0}},

      // Seeks should succeed if they are in range [10, 59). Otherwise, they
      // should return the edges of stripe 1 and 3.
      .expectedSeeks =
          {{0, 10},
           {5, 10},
           {10, 10},
           {15, 15},
           {25, 25},
           {26, 26},
           {49, 49},
           {50, 50},
           {59, 59},
           {60, 59}},

      // Skips should allow skipping only 49 rows (number of rows in stripes 1
      // to 3)
      .expectedSkips = {{0, 0}, {5, 5}, {11, 11}, {32, 32}, {2, 1}, {1, 0}},
  });

  // Test last stripe.
  LOG(INFO) << "--> Range covers stripe 3";
  LOG(INFO) << "File:     |--s0--|--s1--|--s2--|--s3--|";
  LOG(INFO) << "Range:                         |----------|";
  LOG(INFO) << "Expected:                      |--s3--|";
  test({
      .rangeStart = tablet.stripeOffset(3),
      .rangeEnd = 100'000'000,

      // Reads should all resolve to stripe 3 (rows 50 to 59).
      .expectedReads = {{5, 5, 3, 0}, {5, 4, 3, 5}},

      // Seeks should succeed if they are in range [50, 59). Otherwise, they
      // should return the edges of stripe 3.
      .expectedSeeks =
          {{0, 50},
           {10, 50},
           {15, 50},
           {26, 50},
           {49, 50},
           {50, 50},
           {59, 59},
           {60, 59}},

      // Skips should allow skipping only 9 rows (number of rows in stripe 3)
      .expectedSkips = {{0, 0}, {5, 5}, {5, 4}, {1, 0}},
  });
}

TEST_F(VeloxReaderTests, TestScalarFieldLifeCycle) {
  auto testScalarFieldLifeCycle =
      [](const std::shared_ptr<const velox::RowType> schema,
         int32_t batchSize,
         std::mt19937& rng) {
        velox::VectorPtr result;
        auto reader = getReaderForLifeCycleTest(schema, 4 * batchSize, rng);
        EXPECT_TRUE(reader->next(batchSize, result));
        // Hold the reference to values Buffers
        auto child = result->as<velox::RowVector>()->childAt(0);
        velox::BaseVector* rowPtr = result.get();
        velox::Buffer* rawNulls = child->nulls().get();
        velox::BufferPtr values = child->values();
        // Reset the child so that it can be reused
        child.reset();
        EXPECT_TRUE(reader->next(batchSize, result));
        child = result->as<velox::RowVector>()->childAt(0);
        EXPECT_EQ(rawNulls, child->nulls().get());
        EXPECT_NE(values.get(), child->values().get());
        EXPECT_EQ(rowPtr, result.get());

        // Hold the reference to NULL buffer
        velox::BufferPtr nulls = child->nulls();
        velox::Buffer* rawValues = child->values().get();
        child.reset();
        EXPECT_TRUE(reader->next(batchSize, result));
        child = result->as<velox::RowVector>()->childAt(0);
        EXPECT_NE(nulls.get(), child->nulls().get());
        EXPECT_EQ(rawValues, child->values().get());
        EXPECT_EQ(rowPtr, result.get());

        rawNulls = nulls.get();
        // Hold reference to child ScalarVector and it should use another
        // ScalarVector along with childBuffers
        EXPECT_TRUE(reader->next(batchSize, result));
        auto child1 = result->as<velox::RowVector>()->childAt(0);
        EXPECT_NE(child, child1);
        EXPECT_EQ(rowPtr, result.get());
        // after VectorPtr is reset its buffer also reset
        EXPECT_NE(rawNulls, child1->nulls().get());
        EXPECT_NE(rawValues, child1->values().get());
      };

  auto seed = folly::Random::rand32();
  LOG(INFO) << "seed: " << seed;
  std::mt19937 rng{seed};
  std::vector<std::shared_ptr<const velox::RowType>> types = {
      velox::ROW({{"tinyInt", velox::TINYINT()}}),
      velox::ROW({{"smallInt", velox::SMALLINT()}}),
      velox::ROW({{"int", velox::INTEGER()}}),
      velox::ROW({{"bigInt", velox::BIGINT()}}),
      velox::ROW({{"Real", velox::REAL()}}),
      velox::ROW({{"Double", velox::DOUBLE()}}),
      velox::ROW({{"VARCHAR", velox::VARCHAR()}}),
  };
  for (auto& type : types) {
    LOG(INFO) << "Field Type: " << type->nameOf(0);
    for (int i = 0; i < 10; ++i) {
      testScalarFieldLifeCycle(type, 10, rng);
    }
  }
}

TEST_F(VeloxReaderTests, TestArrayFieldLifeCycle) {
  auto seed = folly::Random::rand32();
  LOG(INFO) << "seed: " << seed;
  std::mt19937 rng{seed};
  auto type = velox::ROW({{"arr_val", velox::ARRAY(velox::BIGINT())}});
  auto testArrayFieldLifeCycle =
      [](const std::shared_ptr<const velox::RowType> type,
         int32_t batchSize,
         std::mt19937& rng) {
        velox::VectorPtr result;
        auto reader = getReaderForLifeCycleTest(type, 4 * batchSize, rng);
        EXPECT_TRUE(reader->next(batchSize, result));
        // Hold the reference to internal Buffers and element doesn't change
        auto child = std::dynamic_pointer_cast<velox::ArrayVector>(
            result->as<velox::RowVector>()->childAt(0));
        velox::BaseVector* childPtr = child.get();
        velox::BaseVector* rowPtr = result.get();
        velox::Buffer* rawNulls = child->nulls().get();
        velox::Buffer* rawSizes = child->sizes().get();
        velox::BufferPtr offsets = child->offsets();
        auto elementsPtr = child->elements().get();
        child.reset();
        EXPECT_TRUE(reader->next(batchSize, result));
        child = std::dynamic_pointer_cast<velox::ArrayVector>(
            result->as<velox::RowVector>()->childAt(0));

        EXPECT_EQ(rawNulls, child->nulls().get());
        EXPECT_EQ(rawSizes, child->sizes().get());
        EXPECT_NE(offsets, child->offsets());
        EXPECT_EQ(elementsPtr, child->elements().get());
        EXPECT_EQ(rowPtr, result.get());

        // Hold the reference to Elements vector, other buffer should be
        // reused
        auto elements = child->elements();
        velox::Buffer* rawOffsets = child->offsets().get();
        childPtr = child.get();
        child.reset();
        EXPECT_TRUE(reader->next(batchSize, result));
        child = std::dynamic_pointer_cast<velox::ArrayVector>(
            result->as<velox::RowVector>()->childAt(0));
        EXPECT_EQ(rawNulls, child->nulls().get());
        EXPECT_EQ(rawSizes, child->sizes().get());
        EXPECT_EQ(rawOffsets, child->offsets().get());
        EXPECT_NE(elements, child->elements());
        EXPECT_EQ(childPtr, child.get());
        EXPECT_EQ(rowPtr, result.get());

        // Don't release the Child Array vector to row vector, all the buffers
        // in array should not be resused.
        elementsPtr = child->elements().get();
        EXPECT_TRUE(reader->next(batchSize, result));
        auto child1 = std::dynamic_pointer_cast<velox::ArrayVector>(
            result->as<velox::RowVector>()->childAt(0));
        EXPECT_NE(rawNulls, child1->nulls().get());
        EXPECT_NE(rawSizes, child1->sizes().get());
        EXPECT_NE(rawOffsets, child1->offsets().get());
        EXPECT_NE(elementsPtr, child1->elements().get());
        EXPECT_NE(childPtr, child1.get());
        EXPECT_EQ(rowPtr, result.get());
      };
  for (int i = 0; i < 10; ++i) {
    testArrayFieldLifeCycle(type, 10, rng);
  }
}

TEST_F(VeloxReaderTests, TestMapFieldLifeCycle) {
  auto testMapFieldLifeCycle =
      [](const std::shared_ptr<const velox::RowType> type,
         int32_t batchSize,
         std::mt19937& rng) {
        velox::VectorPtr result;
        auto reader = getReaderForLifeCycleTest(type, 5 * batchSize, rng);
        EXPECT_TRUE(reader->next(batchSize, result));
        // Hold the reference to internal Buffers and element doesn't change
        auto child = std::dynamic_pointer_cast<velox::MapVector>(
            result->as<velox::RowVector>()->childAt(0));
        velox::BaseVector* childPtr = child.get();
        velox::BaseVector* rowPtr = result.get();
        velox::Buffer* rawNulls = child->nulls().get();
        velox::BufferPtr sizes = child->sizes();
        velox::Buffer* rawOffsets = child->offsets().get();
        auto keysPtr = child->mapKeys().get();
        auto valuesPtr = child->mapValues().get();
        child.reset();
        EXPECT_TRUE(reader->next(batchSize, result));
        child = std::dynamic_pointer_cast<velox::MapVector>(
            result->as<velox::RowVector>()->childAt(0));

        EXPECT_EQ(rawNulls, child->nulls().get());
        EXPECT_NE(sizes, child->sizes());
        EXPECT_EQ(rawOffsets, child->offsets().get());
        EXPECT_EQ(keysPtr, child->mapKeys().get());
        EXPECT_EQ(valuesPtr, child->mapValues().get());
        EXPECT_EQ(rowPtr, result.get());

        // Hold the reference to keys vector, other buffer should be reused
        auto mapKeys = child->mapKeys();
        velox::Buffer* rawSizes = child->sizes().get();
        childPtr = child.get();
        child.reset();
        EXPECT_TRUE(reader->next(batchSize, result));
        child = std::dynamic_pointer_cast<velox::MapVector>(
            result->as<velox::RowVector>()->childAt(0));
        EXPECT_EQ(rawNulls, child->nulls().get());
        EXPECT_EQ(rawSizes, child->sizes().get());
        EXPECT_EQ(rawOffsets, child->offsets().get());
        EXPECT_NE(mapKeys, child->mapKeys());
        EXPECT_EQ(valuesPtr, child->mapValues().get());
        EXPECT_EQ(childPtr, child.get());
        EXPECT_EQ(rowPtr, result.get());

        // Hold the reference to values vector, other buffer should be reused
        keysPtr = child->mapKeys().get();
        auto mapValues = child->mapValues();
        childPtr = child.get();
        child.reset();
        EXPECT_TRUE(reader->next(batchSize, result));
        child = std::dynamic_pointer_cast<velox::MapVector>(
            result->as<velox::RowVector>()->childAt(0));
        EXPECT_EQ(rawNulls, child->nulls().get());
        EXPECT_EQ(rawSizes, child->sizes().get());
        EXPECT_EQ(rawOffsets, child->offsets().get());
        EXPECT_EQ(keysPtr, child->mapKeys().get());
        EXPECT_NE(mapValues, child->mapValues());
        EXPECT_EQ(childPtr, child.get());
        EXPECT_EQ(rowPtr, result.get());

        // Don't release the Child map vector to row vector, all the buffers
        // in array should not be resused.
        valuesPtr = child->mapValues().get();
        EXPECT_TRUE(reader->next(batchSize, result));
        auto child1 = std::dynamic_pointer_cast<velox::MapVector>(
            result->as<velox::RowVector>()->childAt(0));
        EXPECT_NE(rawNulls, child1->nulls().get());
        EXPECT_NE(rawSizes, child1->sizes().get());
        EXPECT_NE(rawOffsets, child1->offsets().get());
        EXPECT_NE(keysPtr, child1->mapKeys().get());
        EXPECT_NE(valuesPtr, child1->mapValues().get());
        EXPECT_NE(childPtr, child1.get());
        EXPECT_EQ(rowPtr, result.get());
      };
  auto seed = folly::Random::rand32();
  LOG(INFO) << "seed: " << seed;
  std::mt19937 rng{seed};
  auto type =
      velox::ROW({{"map_val", velox::MAP(velox::INTEGER(), velox::REAL())}});
  for (int i = 0; i < 10; ++i) {
    testMapFieldLifeCycle(type, 10, rng);
    testMapFieldLifeCycle(type, 10, rng);
  }
}

TEST_F(VeloxReaderTests, TestFlatMapAsMapFieldLifeCycle) {
  auto testFlatMapFieldLifeCycle =
      [](const std::shared_ptr<const velox::RowType> type,
         int32_t batchSize,
         std::mt19937& rng) {
        velox::VectorPtr result;
        alpha::VeloxWriterOptions writeOptions;
        writeOptions.flatMapColumns.insert("flat_map");
        auto reader =
            getReaderForLifeCycleTest(type, 5 * batchSize, rng, writeOptions);
        EXPECT_TRUE(reader->next(batchSize, result));
        // Hold the reference to internal Buffers and element doesn't change
        auto child = std::dynamic_pointer_cast<velox::MapVector>(
            result->as<velox::RowVector>()->childAt(0));
        velox::BaseVector* childPtr = child.get();
        velox::BaseVector* rowPtr = result.get();
        velox::Buffer* rawNulls = child->nulls().get();
        velox::BufferPtr sizes = child->sizes();
        velox::Buffer* rawOffsets = child->offsets().get();
        auto keysPtr = child->mapKeys().get();
        child.reset();
        EXPECT_TRUE(reader->next(batchSize, result));
        child = std::dynamic_pointer_cast<velox::MapVector>(
            result->as<velox::RowVector>()->childAt(0));

        EXPECT_EQ(rawNulls, child->nulls().get());
        EXPECT_NE(sizes, child->sizes());
        EXPECT_EQ(rawOffsets, child->offsets().get());
        EXPECT_EQ(keysPtr, child->mapKeys().get());
        EXPECT_EQ(rowPtr, result.get());

        // Hold the reference to keys vector, other buffer should be reused
        auto mapKeys = child->mapKeys();
        velox::Buffer* rawSizes = child->sizes().get();
        childPtr = child.get();
        child.reset();
        EXPECT_TRUE(reader->next(batchSize, result));
        child = std::dynamic_pointer_cast<velox::MapVector>(
            result->as<velox::RowVector>()->childAt(0));
        EXPECT_EQ(rawNulls, child->nulls().get());
        EXPECT_EQ(rawSizes, child->sizes().get());
        EXPECT_EQ(rawOffsets, child->offsets().get());
        EXPECT_NE(mapKeys, child->mapKeys());
        EXPECT_EQ(childPtr, child.get());
        EXPECT_EQ(rowPtr, result.get());

        // Don't release the Child map vector to row vector, all the buffers
        // in array should not be resused.
        EXPECT_TRUE(reader->next(batchSize, result));
        auto child1 = std::dynamic_pointer_cast<velox::MapVector>(
            result->as<velox::RowVector>()->childAt(0));
        EXPECT_NE(rawNulls, child1->nulls().get());
        EXPECT_NE(rawSizes, child1->sizes().get());
        EXPECT_NE(rawOffsets, child1->offsets().get());
        EXPECT_NE(keysPtr, child1->mapKeys().get());
        EXPECT_NE(childPtr, child1.get());
        EXPECT_EQ(rowPtr, result.get());
      };
  auto seed = folly::Random::rand32();
  LOG(INFO) << "seed: " << seed;
  std::mt19937 rng{seed};
  auto type =
      velox::ROW({{"flat_map", velox::MAP(velox::INTEGER(), velox::REAL())}});
  for (int i = 0; i < 10; ++i) {
    testFlatMapFieldLifeCycle(type, 10, rng);
    testFlatMapFieldLifeCycle(type, 10, rng);
  }
}

TEST_F(VeloxReaderTests, TestRowFieldLifeCycle) {
  auto testRowFieldLifeCycle =
      [](const std::shared_ptr<const velox::RowType> type,
         int32_t batchSize,
         std::mt19937& rng) {
        velox::VectorPtr result;
        auto reader = getReaderForLifeCycleTest(type, 5 * batchSize, rng);
        EXPECT_TRUE(reader->next(batchSize, result));
        // Hold the reference to internal Buffers and element doesn't change
        auto child = std::dynamic_pointer_cast<velox::RowVector>(
            result->as<velox::RowVector>()->childAt(0));
        velox::BaseVector* childPtr = child.get();
        velox::BaseVector* rowPtr = result.get();
        velox::BufferPtr nulls = child->nulls();
        velox::BaseVector* childPtrAtIdx0 = child->childAt(0).get();
        velox::BaseVector* childPtrAtIdx1 = child->childAt(1).get();
        child.reset();
        EXPECT_TRUE(reader->next(batchSize, result));
        child = std::dynamic_pointer_cast<velox::RowVector>(
            result->as<velox::RowVector>()->childAt(0));

        EXPECT_NE(nulls, child->nulls());
        EXPECT_EQ(childPtrAtIdx0, child->childAt(0).get());
        EXPECT_EQ(childPtrAtIdx1, child->childAt(1).get());
        EXPECT_EQ(rowPtr, result.get());

        // Hold the reference to one of child vector, sibling should not
        // change
        auto childAtIdx0 = child->childAt(0);
        velox::Buffer* rawNulls = child->nulls().get();
        childPtr = child.get();
        child.reset();
        EXPECT_TRUE(reader->next(batchSize, result));
        child = std::dynamic_pointer_cast<velox::RowVector>(
            result->as<velox::RowVector>()->childAt(0));
        EXPECT_EQ(rawNulls, child->nulls().get());
        EXPECT_NE(childAtIdx0, child->childAt(0));
        EXPECT_EQ(childPtrAtIdx1, child->childAt(1).get());
        EXPECT_EQ(childPtr, child.get());
        EXPECT_EQ(rowPtr, result.get());

        // Don't release the Child-row vector to row vector, all the buffers
        // in array should not be resused.
        EXPECT_TRUE(reader->next(batchSize, result));
        auto child1 = std::dynamic_pointer_cast<velox::RowVector>(
            result->as<velox::RowVector>()->childAt(0));
        EXPECT_NE(rawNulls, child1->nulls().get());
        EXPECT_NE(child->childAt(0), child1->childAt(0));
        EXPECT_NE(child->childAt(1), child1->childAt(1));
        EXPECT_NE(childPtr, child1.get());
        EXPECT_EQ(rowPtr, result.get());
      };

  auto type = velox::ROW(
      {{"row_val",
        velox::ROW(
            {{"a", velox::INTEGER()}, {"b", velox::ARRAY(velox::BIGINT())}})}});
  auto seed = folly::Random::rand32();
  LOG(INFO) << "seed: " << seed;
  std::mt19937 rng{seed};
  for (int i = 0; i < 10; ++i) {
    testRowFieldLifeCycle(type, 10, rng);
    testRowFieldLifeCycle(type, 10, rng);
  }
}

namespace {
void testVeloxTypeFromAlphaSchema(
    velox::memory::MemoryPool& memoryPool,
    alpha::VeloxWriterOptions writerOptions,
    const velox::RowVectorPtr& vector) {
  const auto& veloxRowType =
      std::dynamic_pointer_cast<const velox::RowType>(vector->type());
  auto file = alpha::test::createAlphaFile(*rootPool, vector, writerOptions);
  auto inMemFile = velox::InMemoryReadFile(file);

  alpha::VeloxReader veloxReader(
      memoryPool,
      &inMemFile,
      std::make_shared<velox::dwio::common::ColumnSelector>(veloxRowType));
  const auto& veloxTypeResult = convertToVeloxType(*veloxReader.schema());

  EXPECT_EQ(*veloxRowType, *veloxTypeResult)
      << "Expected: " << veloxRowType->toString()
      << ", actual: " << veloxTypeResult->toString();
}
} // namespace

TEST_F(VeloxReaderTests, VeloxTypeFromAlphaSchema) {
  auto type = velox::ROW({
      {"tinyint_val", velox::TINYINT()},
      {"smallint_val", velox::SMALLINT()},
      {"int_val", velox::INTEGER()},
      {"long_val", velox::BIGINT()},
      {"float_val", velox::REAL()},
      {"double_val", velox::DOUBLE()},
      {"binary_val", velox::VARBINARY()},
      {"string_val", velox::VARCHAR()},
      {"array_val", velox::ARRAY(velox::BIGINT())},
      {"map_val", velox::MAP(velox::INTEGER(), velox::BIGINT())},
      {"struct_val",
       velox::ROW({
           {"float_val", velox::REAL()},
           {"double_val", velox::DOUBLE()},
       })},
      {"nested_map_row_val",
       velox::MAP(
           velox::INTEGER(),
           velox::ROW({
               {"float_val", velox::REAL()},
               {"array_val",
                velox::ARRAY(velox::MAP(velox::INTEGER(), velox::BIGINT()))},
           }))},
      {"dictionary_array_val", velox::ARRAY(velox::BIGINT())},
  });

  const auto& vector = std::dynamic_pointer_cast<velox::RowVector>(
      velox::test::BatchMaker::createBatch(type, 100, *pool_));
  alpha::VeloxWriterOptions writerOptions;
  writerOptions.flatMapColumns.insert("nested_map_row_val");
  writerOptions.dictionaryArrayColumns.insert("dictionary_array_val");
  testVeloxTypeFromAlphaSchema(*pool_, writerOptions, vector);
}

TEST_F(VeloxReaderTests, VeloxTypeFromAlphaSchemaEmptyFlatMap) {
  velox::test::VectorMaker vectorMaker{pool_.get()};
  uint32_t numRows = 5;
  auto vector = vectorMaker.rowVector(
      {"col_0", "col_1"},
      {
          vectorMaker.flatVector<int32_t>(
              numRows,
              [](velox::vector_size_t row) { return 1000 + row; },
              [](velox::vector_size_t row) { return row == 1; }),
          vectorMaker.mapVector<velox::StringView, int32_t>(
              numRows,
              /*sizeAt*/
              [](velox::vector_size_t /* mapRow */) { return 0; },
              /*keyAt*/
              [](velox::vector_size_t /* mapRow */,
                 velox::vector_size_t /* row */) { return ""; },
              /*valueAt*/
              [](velox::vector_size_t /* mapRow */,
                 velox::vector_size_t /* row */) { return 0; },
              /*isNullAt*/
              [](velox::vector_size_t /* mapRow */) { return true; }),
      });
  alpha::VeloxWriterOptions writerOptions;
  writerOptions.flatMapColumns.insert("col_1");
  testVeloxTypeFromAlphaSchema(*pool_, writerOptions, vector);
}

TEST_F(VeloxReaderTests, MissingMetadata) {
  velox::test::VectorMaker vectorMaker{pool_.get()};
  auto vector =
      vectorMaker.rowVector({vectorMaker.flatVector<int32_t>({1, 2, 3})});

  alpha::VeloxWriterOptions options;
  auto file = alpha::test::createAlphaFile(*rootPool, vector, options);
  alpha::testing::InMemoryTrackableReadFile readFile(file);

  alpha::VeloxReader reader(*pool_, &readFile);
  {
    readFile.resetChunks();
    const auto& metadata = reader.metadata();
    // Default metadata injects at least one entry
    ASSERT_LE(1, metadata.size());
    EXPECT_EQ(1, readFile.chunks().size());
  }

  {
    // Metadata is loaded lazily, so reading again just to be sure all is
    // well.
    readFile.resetChunks();
    const auto& metadata = reader.metadata();
    ASSERT_LE(1, metadata.size());
    EXPECT_EQ(0, readFile.chunks().size());
  }
}

TEST_F(VeloxReaderTests, WithMetadata) {
  velox::test::VectorMaker vectorMaker{pool_.get()};
  auto vector =
      vectorMaker.rowVector({vectorMaker.flatVector<int32_t>({1, 2, 3})});

  alpha::VeloxWriterOptions options{
      .metadata = {{"key 1", "value 1"}, {"key 2", "value 2"}},
  };
  auto file = alpha::test::createAlphaFile(*rootPool, vector, options);
  alpha::testing::InMemoryTrackableReadFile readFile(file);

  alpha::VeloxReader reader(*pool_, &readFile);

  {
    readFile.resetChunks();
    auto metadata = reader.metadata();
    ASSERT_EQ(2, metadata.size());
    ASSERT_TRUE(metadata.contains("key 1"));
    ASSERT_TRUE(metadata.contains("key 2"));
    ASSERT_EQ("value 1", metadata["key 1"]);
    ASSERT_EQ("value 2", metadata["key 2"]);

    EXPECT_EQ(1, readFile.chunks().size());
  }

  {
    // Metadata is loaded lazily, so reading again just to be sure all is
    // well.
    readFile.resetChunks();
    auto metadata = reader.metadata();
    ASSERT_EQ(2, metadata.size());
    ASSERT_TRUE(metadata.contains("key 1"));
    ASSERT_TRUE(metadata.contains("key 2"));
    ASSERT_EQ("value 1", metadata["key 1"]);
    ASSERT_EQ("value 2", metadata["key 2"]);

    EXPECT_EQ(0, readFile.chunks().size());
  }
}

TEST_F(VeloxReaderTests, InaccurateSchemaWithSelection) {
  // Some compute engines (e.g. Presto) sometimes don't have the full schema
  // to pass into the reader (if column projection is used). The reader needs
  // the schema in order to correctly construct the output vector. However,
  // for unprojected columns, the reader just need to put a placeholder null
  // column (so ordinals will work as expected), and the actual column type
  // doesn't matter. In this case, we expect the compute engine to construct a
  // column selector, with dummy nodes in the schema for the unprojected
  // columns. This test verifies that the reader handles this correctly.
  velox::test::VectorMaker vectorMaker{pool_.get()};
  auto vector = vectorMaker.rowVector(
      {"int1", "int2", "string", "double", "row1", "row2", "int3", "int4"},
      {vectorMaker.flatVector<int32_t>({11, 12, 13, 14, 15}),
       vectorMaker.flatVector<int32_t>({21, 22, 23, 24, 25}),
       vectorMaker.flatVector({"s1", "s2", "s3", "s4", "s5"}),
       vectorMaker.flatVector<double>({1.1, 2.2, 3.3, 4.4, 5.5}),
       vectorMaker.rowVector(
           /* childNames */ {"a1", "b1"},
           /* children */
           {vectorMaker.flatVector<int32_t>({111, 112, 113, 114, 115}),
            vectorMaker.flatVector({"s111", "s112", "s113", "s114", "s115"})}),
       vectorMaker.rowVector(
           /* childNames */ {"a2", "b2"},
           /* children */
           {vectorMaker.flatVector<int32_t>({211, 212, 213, 214, 215}),
            vectorMaker.flatVector({"s211", "s212", "s213", "s214", "s215"})}),
       vectorMaker.flatVector<int32_t>({31, 32, 33, 34, 35}),
       vectorMaker.flatVector<int32_t>({41, 42, 43, 44, 45})});

  velox::VectorPtr result;
  {
    auto file = alpha::test::createAlphaFile(*rootPool, vector);
    velox::InMemoryReadFile readFile(file);
    auto inaccurateType = velox::ROW({
        {"c1", velox::VARCHAR()},
        {"c2", velox::INTEGER()},
        {"c3", velox::VARCHAR()},
        {"c4", velox::VARCHAR()},
        {"c5", velox::VARCHAR()},
        {"c6", velox::ROW({velox::INTEGER(), velox::VARCHAR()})},
        {"c7", velox::INTEGER()},
        // We didn't add the last column on purpose, to test that the reader
        // can handle smaller schemas.
    });

    std::unordered_set<uint64_t> projected{1, 2, 5, 6};
    auto selector = std::make_shared<velox::dwio::common::ColumnSelector>(
        inaccurateType,
        std::vector<uint64_t>{projected.begin(), projected.end()});
    alpha::VeloxReader reader(*pool_, &readFile, std::move(selector));

    ASSERT_TRUE(reader.next(vector->size(), result));
    const auto& rowResult = result->as<velox::RowVector>();
    ASSERT_EQ(inaccurateType->size(), rowResult->childrenSize());
    for (auto i = 0; i < rowResult->childrenSize(); ++i) {
      const auto& child = rowResult->childAt(i);
      if (projected.count(i) == 0) {
        ASSERT_EQ(rowResult->childAt(i), nullptr);
      } else {
        ASSERT_EQ(5, child->size());
        for (auto j = 0; j < child->size(); ++j) {
          ASSERT_FALSE(child->isNullAt(j));
          ASSERT_TRUE(child->equalValueAt(vector->childAt(i).get(), j, j));
        }
      }
    }
    ASSERT_FALSE(reader.next(vector->size(), result));
  }
}

INSTANTIATE_TEST_CASE_P(
    VeloxReaderTestsMultiThreaded,
    VeloxReaderTests,
    ::testing::Values(false, true));