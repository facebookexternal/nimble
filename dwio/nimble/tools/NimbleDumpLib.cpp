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
#include <algorithm>
#include <fstream>
#include <locale>
#include <numeric>
#include <ostream>
#include <tuple>
#include <utility>

#include "common/strings/Zstd.h"
#include "dwio/common/filesystem/FileSystem.h"
#include "dwio/nimble/common/EncodingPrimitives.h"
#include "dwio/nimble/common/FixedBitArray.h"
#include "dwio/nimble/common/Types.h"
#include "dwio/nimble/encodings/EncodingFactory.h"
#include "dwio/nimble/encodings/EncodingLayout.h"
#include "dwio/nimble/tools/EncodingUtilities.h"
#include "dwio/nimble/tools/NimbleDumpLib.h"
#include "dwio/nimble/velox/EncodingLayoutTree.h"
#include "dwio/nimble/velox/VeloxReader.h"
#include "folly/cli/NestedCommandLineApp.h"

namespace facebook::nimble::tools {
#define BOLD "\033[1m"
#define RED "\033[31m"
#define GREEN "\033[32m"
#define YELLOW "\033[33m"
#define BLUE "\033[34m"
#define PURPLE "\033[35m"
#define CYAN "\033[36m"
#define RESET_COLOR "\033[0m"

namespace {

constexpr uint32_t kBufferSize = 1000;

struct GroupingKey {
  EncodingType encodingType;
  DataType dataType;
  std::optional<CompressionType> compressinType;
};

struct GroupingKeyCompare {
  size_t operator()(const GroupingKey& lhs, const GroupingKey& rhs) const {
    if (lhs.encodingType != rhs.encodingType) {
      return lhs.encodingType < rhs.encodingType;
    } else if (lhs.dataType != rhs.dataType) {
      return lhs.dataType < rhs.dataType;
    } else {
      return lhs.compressinType < rhs.compressinType;
    }
  }
};

struct EncodingHistogramValue {
  size_t count;
  size_t bytes;
};

enum HAlignment {
  LEFT,
  RIGHT,
};

class TableFormatter {
 public:
  TableFormatter(
      std::ostream& ostream,
      std::vector<std::tuple<
          std::string /* Title */,
          uint8_t /* Width */,
          HAlignment /* Horizontal Alignment */
          >> fields,
      bool noHeader = false)
      : ostream_{ostream}, fields_{std::move(fields)} {
    if (!noHeader) {
      ostream << YELLOW;
      for (const auto& field : fields_) {
        ostream << (std::get<2>(field) == HAlignment::RIGHT ? std::right
                                                            : std::left)
                << std::setw(std::get<1>(field) + 2) << std::get<0>(field);
      }
      ostream << RESET_COLOR << std::endl;
    }
  }

  void writeRow(const std::vector<std::string>& values) {
    assert(values.size() == fields_.size());
    for (auto i = 0; i < values.size(); ++i) {
      ostream_ << (std::get<2>(fields_[i]) == HAlignment::RIGHT ? std::right
                                                                : std::left)
               << std::setw(std::get<1>(fields_[i]) + 2) << values[i];
    }
    ostream_ << std::endl;
  }

 private:
  std::ostream& ostream_;
  std::vector<std::tuple<
      std::string /* Title */,
      uint8_t /* Width */,
      HAlignment /* Horizontal Alignment */
      >>
      fields_;
};

void traverseTablet(
    velox::memory::MemoryPool& memoryPool,
    const TabletReader& tabletReader,
    std::optional<int32_t> stripeIndex,
    std::function<void(uint32_t /* stripeId */)> stripeVisitor = nullptr,
    std::function<void(
        ChunkedStream& /*stream*/,
        uint32_t /*stripeId*/,
        uint32_t /* streamId*/)> streamVisitor = nullptr) {
  uint32_t startStripe = stripeIndex ? *stripeIndex : 0;
  uint32_t endStripe =
      stripeIndex ? *stripeIndex : tabletReader.stripeCount() - 1;
  for (uint32_t i = startStripe; i <= endStripe; ++i) {
    if (stripeVisitor) {
      stripeVisitor(i);
    }
    if (streamVisitor) {
      auto stripeIdentifier = tabletReader.getStripeIdentifier(i);
      std::vector<uint32_t> streamIdentifiers(
          tabletReader.streamCount(stripeIdentifier));
      std::iota(streamIdentifiers.begin(), streamIdentifiers.end(), 0);
      auto streams = tabletReader.load(
          stripeIdentifier,
          {streamIdentifiers.cbegin(), streamIdentifiers.cend()});
      for (uint32_t j = 0; j < streams.size(); ++j) {
        auto& stream = streams[j];
        if (stream) {
          InMemoryChunkedStream chunkedStream{memoryPool, std::move(stream)};
          streamVisitor(chunkedStream, i, j);
        }
      }
    }
  }
}

template <typename T>
void printScalarData(
    std::ostream& ostream,
    velox::memory::MemoryPool& pool,
    Encoding& stream,
    uint32_t rowCount) {
  nimble::Vector<T> buffer(&pool);
  nimble::Vector<char> nulls(&pool);
  buffer.resize(rowCount);
  nulls.resize((nimble::FixedBitArray::bufferSize(rowCount, 1)));
  nulls.zero_out();
  if (stream.isNullable()) {
    stream.materializeNullable(
        rowCount, buffer.data(), [&]() { return nulls.data(); });
  } else {
    stream.materialize(rowCount, buffer.data());
    nulls.fill(0xff);
  }
  for (uint32_t i = 0; i < rowCount; ++i) {
    if (stream.isNullable() && nimble::bits::getBit(i, nulls.data()) == 0) {
      ostream << "NULL" << std::endl;
    } else {
      ostream << folly::to<std::string>(buffer[i])
              << std::endl; // Have to use folly::to as Int8 was getting
                            // converted to char.
    }
  }
}

void printScalarType(
    std::ostream& ostream,
    velox::memory::MemoryPool& pool,
    Encoding& stream,
    uint32_t rowCount) {
  switch (stream.dataType()) {
#define CASE(KIND, cppType)                                    \
  case DataType::KIND: {                                       \
    printScalarData<cppType>(ostream, pool, stream, rowCount); \
    break;                                                     \
  }
    CASE(Int8, int8_t);
    CASE(Uint8, uint8_t);
    CASE(Int16, int16_t);
    CASE(Uint16, uint16_t);
    CASE(Int32, int32_t);
    CASE(Uint32, uint32_t);
    CASE(Int64, int64_t);
    CASE(Uint64, uint64_t);
    CASE(Float, float);
    CASE(Double, double);
    CASE(Bool, bool);
    CASE(String, std::string_view);
#undef CASE
    case DataType::Undefined: {
      NIMBLE_UNREACHABLE(
          fmt::format("Undefined type for stream: {}", stream.dataType()));
    }
  }
}

template <typename T>
auto commaSeparated(T value) {
  return fmt::format(std::locale("en_US.UTF-8"), "{:L}", value);
}

} // namespace

NimbleDumpLib::NimbleDumpLib(std::ostream& ostream, const std::string& file)
    : pool_{velox::memory::deprecatedAddDefaultLeafMemoryPool()},
      file_{dwio::file_system::FileSystem::openForRead(
          file,
          dwio::common::request::AccessDescriptorBuilder()
              .withClientId("nimble_dump")
              .build())},
      tablet_{std::make_shared<TabletReader>(*pool_, file_.get())},
      reader_{std::make_shared<VeloxReader>(*pool_, tablet_)},
      ostream_{ostream} {}

void NimbleDumpLib::emitRichInfo() {
  emitInfo();
  ostream_ << std::endl << BOLD << "Schema:" << RESET_COLOR << std::endl;
  emitSchema();
  ostream_ << std::endl << BOLD << "Stripes:" << RESET_COLOR << std::endl;
  emitStripes(false);
}

void NimbleDumpLib::emitInfo() {
  ostream_ << CYAN << "Nimble File " << RESET_COLOR << "Version "
           << tablet_->majorVersion() << "." << tablet_->minorVersion()
           << std::endl;
  ostream_ << "File Size: " << commaSeparated(tablet_->fileSize()) << std::endl;
  ostream_ << "Checksum: " << tablet_->checksum() << " ["
           << nimble::toString(tablet_->checksumType()) << "]" << std::endl;
  ostream_ << "Footer Compression: " << tablet_->footerCompressionType()
           << std::endl;
  ostream_ << "Footer Size: " << commaSeparated(tablet_->footerSize())
           << std::endl;
  ostream_ << "Stripe Count: " << commaSeparated(tablet_->stripeCount())
           << std::endl;
  ostream_ << "Row Count: " << commaSeparated(tablet_->tabletRowCount())
           << std::endl;

  auto& metadata = reader_->metadata();
  if (!metadata.empty()) {
    ostream_ << "Metadata:";
    for (const auto& pair : metadata) {
      ostream_ << std::endl << "  " << pair.first << ": " << pair.second;
    }
  }
  ostream_ << std::endl;
}

void NimbleDumpLib::emitSchema(bool collapseFlatMap) {
  auto emitOffsets = [](const Type& type) {
    std::string offsets;
    switch (type.kind()) {
      case Kind::Scalar: {
        offsets =
            folly::to<std::string>(type.asScalar().scalarDescriptor().offset());
        break;
      }
      case Kind::Array: {
        offsets =
            folly::to<std::string>(type.asArray().lengthsDescriptor().offset());
        break;
      }
      case Kind::Map: {
        offsets =
            folly::to<std::string>(type.asMap().lengthsDescriptor().offset());
        break;
      }
      case Kind::Row: {
        offsets =
            folly::to<std::string>(type.asRow().nullsDescriptor().offset());
        break;
      }
      case Kind::FlatMap: {
        offsets =
            folly::to<std::string>(type.asFlatMap().nullsDescriptor().offset());
        break;
      }
      case Kind::ArrayWithOffsets: {
        offsets = "o:" +
            folly::to<std::string>(
                      type.asArrayWithOffsets().offsetsDescriptor().offset()) +
            ",l:" +
            folly::to<std::string>(
                      type.asArrayWithOffsets().lengthsDescriptor().offset());
        break;
      }
      case Kind::SlidingWindowMap: {
        offsets = "o:" +
            folly::to<std::string>(
                      type.asSlidingWindowMap().offsetsDescriptor().offset()) +
            ",l:" +
            folly::to<std::string>(
                      type.asSlidingWindowMap().lengthsDescriptor().offset());
        break;
      }
    }

    return offsets;
  };

  bool skipping = false;
  SchemaReader::traverseSchema(
      reader_->schema(),
      [&](uint32_t level,
          const Type& type,
          const SchemaReader::NodeInfo& info) {
        auto parentType = info.parentType;
        if (parentType != nullptr && parentType->isFlatMap()) {
          auto childrenCount = parentType->asFlatMap().childrenCount();
          if (childrenCount > 2 && collapseFlatMap) {
            if (info.placeInSibling == 1) {
              ostream_ << std::string(
                              (std::basic_string<char>::size_type)level * 2,
                              ' ')
                       << "..." << std::endl;
              skipping = true;
            } else if (info.placeInSibling == childrenCount - 1) {
              skipping = false;
            }
          }
        }
        if (!skipping) {
          ostream_ << std::string(
                          (std::basic_string<char>::size_type)level * 2, ' ')
                   << "[" << emitOffsets(type) << "] " << info.name << " : ";
          if (type.isScalar()) {
            ostream_ << toString(type.kind()) << "<"
                     << toString(
                            type.asScalar().scalarDescriptor().scalarKind())
                     << ">" << std::endl;
          } else {
            ostream_ << toString(type.kind()) << std::endl;
          }
        }
      });
}

void NimbleDumpLib::emitStripes(bool noHeader) {
  TableFormatter formatter(
      ostream_,
      {{"Stripe Id", 7, LEFT},
       {"Stripe Offset", 15, RIGHT},
       {"Stripe Size", 15, RIGHT},
       {"Row Count", 10, RIGHT}},
      noHeader);
  traverseTablet(*pool_, *tablet_, std::nullopt, [&](uint32_t stripeIndex) {
    auto stripeIdentifier = tablet_->getStripeIdentifier(stripeIndex);
    auto sizes = tablet_->streamSizes(stripeIdentifier);
    auto stripeSize = std::accumulate(sizes.begin(), sizes.end(), 0UL);
    formatter.writeRow({
        folly::to<std::string>(stripeIndex),
        commaSeparated(tablet_->stripeOffset(stripeIndex)),
        commaSeparated(stripeSize),
        commaSeparated(tablet_->stripeRowCount(stripeIndex)),
    });
  });
}

void NimbleDumpLib::emitStreams(
    bool noHeader,
    bool streamLabels,
    std::optional<uint32_t> stripeId) {
  std::vector<std::tuple<std::string, uint8_t, HAlignment>> fields;
  fields.push_back({"Stripe Id", 11, LEFT});
  fields.push_back({"Stream Id", 11, LEFT});
  fields.push_back({"Stream Offset", 13, LEFT});
  fields.push_back({"Stream Size", 13, LEFT});
  fields.push_back({"Item Count", 13, LEFT});
  if (streamLabels) {
    fields.push_back({"Stream Label", 16, LEFT});
  }
  fields.push_back({"Type", 30, LEFT});

  TableFormatter formatter(ostream_, fields, noHeader);

  std::optional<StreamLabels> labels{};
  if (streamLabels) {
    labels.emplace(reader_->schema());
  }

  traverseTablet(
      *pool_,
      *tablet_,
      stripeId,
      nullptr /* stripeVisitor */,
      [&](ChunkedStream& stream, uint32_t stripeId, uint32_t streamId) {
        auto stripeIdentifier = tablet_->getStripeIdentifier(stripeId);
        uint32_t itemCount = 0;
        while (stream.hasNext()) {
          auto chunk = stream.nextChunk();
          itemCount += *reinterpret_cast<const uint32_t*>(chunk.data() + 2);
        }
        stream.reset();
        std::vector<std::string> values;
        values.push_back(folly::to<std::string>(stripeId));
        values.push_back(folly::to<std::string>(streamId));
        values.push_back(folly::to<std::string>(
            tablet_->streamOffsets(stripeIdentifier)[streamId]));
        values.push_back(folly::to<std::string>(
            tablet_->streamSizes(stripeIdentifier)[streamId]));
        values.push_back(folly::to<std::string>(itemCount));
        if (streamLabels) {
          auto it = values.emplace_back(labels->streamLabel(streamId));
        }
        values.push_back(getStreamInputLabel(stream));
        formatter.writeRow(values);
      });
}

void NimbleDumpLib::emitHistogram(
    bool topLevel,
    bool noHeader,
    std::optional<uint32_t> stripeId) {
  std::map<GroupingKey, EncodingHistogramValue, GroupingKeyCompare>
      encodingHistogram;
  const std::unordered_map<std::string, CompressionType> compressionMap{
      {toString(CompressionType::Uncompressed), CompressionType::Uncompressed},
      {toString(CompressionType::Zstd), CompressionType::Zstd},
      {toString(CompressionType::MetaInternal), CompressionType::MetaInternal},
  };
  traverseTablet(
      *pool_,
      *tablet_,
      stripeId,
      nullptr,
      [&](ChunkedStream& stream, auto /*stripeIndex*/, auto /*streamIndex*/) {
        while (stream.hasNext()) {
          traverseEncodings(
              stream.nextChunk(),
              [&](EncodingType encodingType,
                  DataType dataType,
                  uint32_t level,
                  uint32_t /* index */,
                  std::string /*nestedEncodingName*/,
                  std::unordered_map<EncodingPropertyType, EncodingProperty>
                      properties) {
                GroupingKey key{
                    .encodingType = encodingType, .dataType = dataType};
                const auto& compression =
                    properties.find(EncodingPropertyType::Compression);
                if (compression != properties.end()) {
                  key.compressinType =
                      compressionMap.at(compression->second.value);
                }
                auto& value = encodingHistogram[key];
                ++value.count;

                const auto& encodedSize =
                    properties.find(EncodingPropertyType::EncodedSize);
                if (encodedSize != properties.end()) {
                  value.bytes += folly::to<uint32_t>(encodedSize->second.value);
                }

                return !(topLevel && level == 1);
              });
        }
      });

  TableFormatter formatter(
      ostream_,
      {{"Encoding Type", 17, LEFT},
       {"Data Type", 13, LEFT},
       {"Compression", 15, LEFT},
       {"Instance Count", 15, RIGHT},
       {"Storage Bytes", 15, RIGHT}},
      noHeader);

  for (auto& [key, value] : encodingHistogram) {
    formatter.writeRow({
        toString(key.encodingType),
        toString(key.dataType),
        key.compressinType ? toString(*key.compressinType) : "",
        commaSeparated(value.count),
        commaSeparated(value.bytes),
    });
  }
}

void NimbleDumpLib::emitContent(
    uint32_t streamId,
    std::optional<uint32_t> stripeId) {
  uint32_t maxStreamCount;
  bool found = false;
  traverseTablet(*pool_, *tablet_, stripeId, [&](uint32_t stripeId) {
    auto stripeIdentifier = tablet_->getStripeIdentifier(stripeId);
    maxStreamCount =
        std::max(maxStreamCount, tablet_->streamCount(stripeIdentifier));
    if (streamId >= tablet_->streamCount(stripeIdentifier)) {
      return;
    }

    found = true;

    auto streams = tablet_->load(stripeIdentifier, std::vector{streamId});

    if (auto& stream = streams[0]) {
      InMemoryChunkedStream chunkedStream{*pool_, std::move(stream)};
      while (chunkedStream.hasNext()) {
        auto encoding =
            EncodingFactory::decode(*pool_, chunkedStream.nextChunk());
        uint32_t totalRows = encoding->rowCount();
        while (totalRows > 0) {
          auto currentReadSize = std::min(kBufferSize, totalRows);
          printScalarType(ostream_, *pool_, *encoding, currentReadSize);
          totalRows -= currentReadSize;
        }
      }
    }
  });

  if (!found) {
    throw folly::ProgramExit(
        -1,
        fmt::format(
            "Stream identifier {} is out of bound. Must be between 0 and {}\n",
            streamId,
            maxStreamCount));
  }
}

void NimbleDumpLib::emitBinary(
    std::function<std::unique_ptr<std::ostream>()> outputFactory,
    uint32_t streamId,
    uint32_t stripeId) {
  auto stripeIdentifier = tablet_->getStripeIdentifier(stripeId);
  if (streamId >= tablet_->streamCount(stripeIdentifier)) {
    throw folly::ProgramExit(
        -1,
        fmt::format(
            "Stream identifier {} is out of bound. Must be between 0 and {}\n",
            streamId,
            tablet_->streamCount(stripeIdentifier)));
  }

  auto streams = tablet_->load(stripeIdentifier, std::vector{streamId});

  if (auto& stream = streams[0]) {
    auto output = outputFactory();
    output->write(stream->getStream().data(), stream->getStream().size());
    output->flush();
  }
}

void traverseEncodingLayout(
    const std::optional<EncodingLayout>& node,
    const std::optional<EncodingLayout>& parentNode,
    uint32_t& nodeId,
    uint32_t parentId,
    uint32_t level,
    uint8_t childIndex,
    const std::function<void(
        const std::optional<EncodingLayout>&,
        const std::optional<EncodingLayout>&,
        uint32_t,
        uint32_t,
        uint32_t,
        uint8_t)>& visitor) {
  auto currentNodeId = nodeId;
  visitor(node, parentNode, currentNodeId, parentId, level, childIndex);

  if (node.has_value()) {
    for (int i = 0; i < node->childrenCount(); ++i) {
      traverseEncodingLayout(
          node->child(i), node, ++nodeId, currentNodeId, level + 1, i, visitor);
    }
  }
}

void traverseEncodingLayoutTree(
    const EncodingLayoutTree& node,
    const EncodingLayoutTree& parentNode,
    uint32_t& nodeId,
    uint32_t parentId,
    uint32_t level,
    uint8_t childIndex,
    const std::function<void(
        const EncodingLayoutTree&,
        const EncodingLayoutTree&,
        uint32_t,
        uint32_t,
        uint32_t,
        uint8_t)>& visitor) {
  auto currentNodeId = nodeId;
  visitor(node, parentNode, currentNodeId, parentId, level, childIndex);

  for (int i = 0; i < node.childrenCount(); ++i) {
    traverseEncodingLayoutTree(
        node.child(i), node, ++nodeId, currentNodeId, level + 1, i, visitor);
  }
}

std::string getEncodingLayoutLabel(
    const std::optional<nimble::EncodingLayout>& root) {
  std::string label;
  uint32_t currentLevel = 0;
  std::unordered_map<nimble::EncodingType, std::vector<std::string>>
      identifierNames{
          {nimble::EncodingType::Dictionary, {"Alphabet", "Indices"}},
          {nimble::EncodingType::MainlyConstant, {"IsCommon", "OtherValues"}},
          {nimble::EncodingType::Nullable, {"Data", "Nulls"}},
          {nimble::EncodingType::RLE, {"RunLengths", "RunValues"}},
          {nimble::EncodingType::SparseBool, {"Indices"}},
          {nimble::EncodingType::Trivial, {"Lengths"}},
      };

  auto getIdentifierName = [&](nimble::EncodingType encodingType,
                               uint8_t identifier) {
    auto it = identifierNames.find(encodingType);
    LOG(INFO) << (it == identifierNames.end()) << ", "
              << (it != identifierNames.end()
                      ? (int)(identifier >= it->second.size())
                      : -1);
    return it == identifierNames.end() || identifier >= it->second.size()
        ? "Unknown"
        : it->second[identifier];
  };

  uint32_t id = 0;
  traverseEncodingLayout(
      root,
      root,
      id,
      id,
      0,
      (uint8_t)0,
      [&](const std::optional<nimble::EncodingLayout>& node,
          const std::optional<nimble::EncodingLayout>& parentNode,
          uint32_t /* nodeId */,
          uint32_t /* parentId */,
          uint32_t level,
          uint8_t identifier) {
        if (!node.has_value()) {
          label += "N/A";
          return true;
        }

        if (level > currentLevel) {
          label += "[" +
              getIdentifierName(parentNode->encodingType(), identifier) + ":";

        } else if (level < currentLevel) {
          label += "]";
        }

        if (identifier > 0) {
          label += "," +
              getIdentifierName(parentNode->encodingType(), identifier) + ":";
        }

        currentLevel = level;

        label += toString(node->encodingType()) + "{" +
            toString(node->compressionType()) + "}";

        return true;
      });

  while (currentLevel-- > 0) {
    label += "]";
  }

  return label;
}

void NimbleDumpLib::emitLayout(bool noHeader, bool compressed) {
  auto size = file_->size();
  std::string buffer;
  buffer.resize(size);
  file_->pread(0, size, buffer.data());
  if (compressed) {
    std::string uncompressed;
    strings::zstdDecompress(buffer, &uncompressed);
    buffer = std::move(uncompressed);
  }

  auto layout = nimble::EncodingLayoutTree::create(buffer);

  TableFormatter formatter(
      ostream_,
      {
          {"Node Id", 11, LEFT},
          {"Parent Id", 11, LEFT},
          {"Node Type", 15, LEFT},
          {"Node Name", 17, LEFT},
          {"Encoding Layout", 20, LEFT},
      },
      noHeader);

  uint32_t id = 0;
  traverseEncodingLayoutTree(
      layout,
      layout,
      id,
      id,
      0,
      0,
      [&](const EncodingLayoutTree& node,
          const EncodingLayoutTree& /* parentNode */,
          uint32_t nodeId,
          uint32_t parentId,
          uint32_t /* level */,
          uint8_t /* identifier */) {
        auto identifiers = node.encodingLayoutIdentifiers();
        std::sort(identifiers.begin(), identifiers.end());

        std::string encodingLayout;
        for (auto identifier : identifiers) {
          if (!encodingLayout.empty()) {
            encodingLayout += "|";
          }
          encodingLayout += folly::to<std::string>(identifier) + ":" +
              getEncodingLayoutLabel(*node.encodingLayout(identifier));
        }

        formatter.writeRow(
            {folly::to<std::string>(nodeId),
             folly::to<std::string>(parentId),
             toString(node.schemaKind()),
             std::string(node.name()),
             encodingLayout});
      });
}

} // namespace facebook::nimble::tools
