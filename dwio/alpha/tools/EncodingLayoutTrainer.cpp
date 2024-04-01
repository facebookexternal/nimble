// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#include "dwio/alpha/tools/EncodingLayoutTrainer.h"
#include <dwio/alpha/common/Vector.h>
#include <optional>
#include <string_view>
#include "common/strings/Zstd.h"
#include "dwio/alpha/common/Exceptions.h"
#include "dwio/alpha/common/Types.h"
#include "dwio/alpha/encodings/EncodingFactoryNew.h"
#include "dwio/alpha/encodings/EncodingLayoutCapture.h"
#include "dwio/alpha/velox/ChunkedStream.h"
#include "dwio/alpha/velox/VeloxReader.h"
#include "dwio/api/AlphaWriterOptionBuilder.h"
#include "fbjava/datainfra-metastore/api/if/gen-cpp2/hive_metastore_types.h"
#include "thrift/lib/cpp/protocol/TBase64Utils.h"
#include "thrift/lib/cpp2/protocol/CompactProtocol.h"
#include "velox/dwio/common/ExecutorBarrier.h"

namespace facebook::alpha::tools {

namespace {

template <typename T>
class TrainingNode {
 public:
  TrainingNode(
      std::string name,
      std::unique_ptr<T>&& state,
      std::vector<std::unique_ptr<TrainingNode<T>>>&& children)
      : name_{std::move(name)},
        state_{std::move(state)},
        children_{std::move(children)} {}

  const std::string& name() const {
    return name_;
  }

  T& state() const {
    return *state_;
  }

  const std::vector<std::unique_ptr<TrainingNode<T>>>& children() const {
    return children_;
  }

 private:
  std::string name_;
  std::unique_ptr<T> state_;
  std::vector<std::unique_ptr<TrainingNode<T>>> children_;
};

template <typename T>
T deserialize(const std::string& source) {
  T result;
  auto compressed = apache::thrift::protocol::base64Decode(source);
  std::string uncompressed;
  if (!strings::zstdDecompress(compressed->moveToFbString(), &uncompressed)) {
    throw std::runtime_error(
        fmt::format("Unable to decompress data: {}", source));
  }
  apache::thrift::CompactSerializer::deserialize(uncompressed, result);
  return result;
}

struct State {
  explicit State(const Type& type) : type{type} {}

  const Type& type;
  std::unordered_map<EncodingLayoutTree::StreamIdentifier, EncodingLayout>
      encodingLayouts;
  std::mutex mutex;
};

std::unique_ptr<TrainingNode<State>> createTrainingTree(
    const Type& type,
    const std::function<
        void(const StreamDescriptor&, std::function<void(EncodingLayout&&)>)>&
        train,
    const std::string& name = "") {
  auto state = std::make_unique<State>(type);
  std::vector<std::unique_ptr<TrainingNode<State>>> children;

#define _ASYNC_TRAIN(descriptor, identifier)                         \
  train(descriptor, [state = state.get()](EncodingLayout&& layout) { \
    std::lock_guard lock{state->mutex};                              \
    state->encodingLayouts.insert({                                  \
        EncodingLayoutTree::StreamIdentifiers::identifier,           \
        std::move(layout),                                           \
    });                                                              \
  });

  switch (type.kind()) {
    case Kind::Scalar: {
      _ASYNC_TRAIN(type.asScalar().scalarDescriptor(), Scalar::ScalarStream);
      break;
    }
    case Kind::Array: {
      auto& array = type.asArray();
      _ASYNC_TRAIN(array.lengthsDescriptor(), Array::LengthsStream);
      children.reserve(1);
      children.emplace_back(createTrainingTree(*array.elements(), train));
      break;
    }
    case Kind::Map: {
      auto& map = type.asMap();
      _ASYNC_TRAIN(map.lengthsDescriptor(), Map::LengthsStream);
      children.reserve(2);
      children.emplace_back(createTrainingTree(*map.keys(), train));
      children.emplace_back(createTrainingTree(*map.values(), train));
      break;
    }
    case Kind::Row: {
      auto& row = type.asRow();
      _ASYNC_TRAIN(row.nullsDescriptor(), Row::NullsStream);
      children.reserve(row.childrenCount());
      for (auto i = 0; i < row.childrenCount(); ++i) {
        children.emplace_back(createTrainingTree(*row.childAt(i), train));
      }
      break;
    }
    case Kind::FlatMap: {
      auto& map = type.asFlatMap();
      _ASYNC_TRAIN(map.nullsDescriptor(), FlatMap::NullsStream);
      children.reserve(map.childrenCount());
      for (auto i = 0; i < map.childrenCount(); ++i) {
        children.emplace_back(
            createTrainingTree(*map.childAt(i), train, map.nameAt(i)));
      }
      break;
    }
    case Kind::ArrayWithOffsets: {
      auto& array = type.asArrayWithOffsets();
      _ASYNC_TRAIN(array.offsetsDescriptor(), ArrayWithOffsets::OffsetsStream);
      _ASYNC_TRAIN(array.lengthsDescriptor(), ArrayWithOffsets::LengthsStream);
      children.reserve(1);
      children.emplace_back(createTrainingTree(*array.elements(), train));
      break;
    }
  }

  return std::make_unique<TrainingNode<State>>(
      name, std::move(state), std::move(children));
}

class PreloadedStreamLoader : public StreamLoader {
 public:
  explicit PreloadedStreamLoader(std::string_view stream) : stream_{stream} {}
  const std::string_view getStream() const override {
    return {stream_.data(), stream_.size()};
  }

 private:
  const std::string_view stream_;
};

template <typename T>
EncodingLayout trainEncoding(
    velox::memory::MemoryPool& memoryPool,
    const VeloxWriterOptions& options,
    const std::vector<std::string_view>& streams) {
  // Train on a single schema node. Load all data from all stripes and perform
  // basic encoding selection

  std::vector<alpha::Vector<T>> chunks;
  std::vector<std::unique_ptr<Encoding>> encodings;
  uint64_t rowCount = 0;
  for (const auto& stream : streams) {
    InMemoryChunkedStream chunkedStream{
        memoryPool, std::make_unique<PreloadedStreamLoader>(stream)};
    while (chunkedStream.hasNext()) {
      auto chunk = chunkedStream.nextChunk();
      auto encoding = alpha::EncodingFactory::decode(memoryPool, chunk);
      Vector<T> data{&memoryPool};
      data.resize(encoding->rowCount());
      encoding->materialize(encoding->rowCount(), data.data());
      rowCount += encoding->rowCount();
      chunks.push_back(std::move(data));
      encodings.push_back(std::move(encoding));
    }
  }

  Vector<T> data{&memoryPool};
  data.reserve(rowCount);
  for (const auto& chunk : chunks) {
    for (const auto& item : chunk) {
      data.push_back(item);
    }
  }

  auto policy = std::unique_ptr<EncodingSelectionPolicy<T>>(
      static_cast<EncodingSelectionPolicy<T>*>(
          options.encodingSelectionPolicyFactory(TypeTraits<T>::dataType)
              .release()));
  Buffer buffer{memoryPool};
  std::string_view encoding;
  encoding = alpha::EncodingFactory::encode<T>(std::move(policy), data, buffer);
  return EncodingLayoutCapture::capture(encoding);
}

EncodingLayoutTree toEncodingLayoutTree(const TrainingNode<State>& node) {
  auto& state = node.state();
  std::vector<EncodingLayoutTree> children;
  children.reserve(node.children().size());
  for (const auto& child : node.children()) {
    children.push_back(toEncodingLayoutTree(*child));
  }
  return {
      state.type.kind(),
      std::move(state.encodingLayouts),
      node.name(),
      std::move(children),
  };
}

} // namespace

EncodingLayoutTrainer::EncodingLayoutTrainer(
    velox::memory::MemoryPool& memoryPool,
    std::vector<std::string_view> files,
    std::string serializedSerde)
    : memoryPool_{memoryPool},
      files_{std::move(files)},
      serializedSerde_{std::move(serializedSerde)} {
  ALPHA_CHECK(!files_.empty(), "No files provided to train on");
}

EncodingLayoutTree EncodingLayoutTrainer::train(folly::Executor& executor) {
  // Initial "training" implementation is very basic.
  // It loads a single file, and for each schema node (stream), it loads all
  // data from the file and performs encoding selection on it.
  //
  // Future versions will:
  // * Support multiple files
  // * Verify encoding selection stability across files/stripes.
  // * Perform better encoding selection (brute forcing, etc.)
  // * Measure read/write performance
  // * Support different cost functions

  // One file for now
  ALPHA_CHECK(files_.size() == 1, "Only supporting single file training.");
  auto& file = files_.front();

  LOG(INFO) << "Opening file " << file;

  std::unique_ptr<velox::LocalReadFile> readFile =
      std::make_unique<velox::LocalReadFile>(file);

  std::shared_ptr<alpha::Tablet> tablet =
      std::make_shared<alpha::Tablet>(memoryPool_, std::move(readFile));
  std::unique_ptr<alpha::VeloxReader> reader =
      std::make_unique<alpha::VeloxReader>(memoryPool_, tablet);

  std::vector<std::vector<std::unique_ptr<StreamLoader>>> stripeStreams;
  stripeStreams.reserve(tablet->stripeCount());
  for (auto i = 0; i < tablet->stripeCount(); ++i) {
    std::vector<uint32_t> identifiers;
    identifiers.resize(tablet->streamCount(i));
    std::iota(identifiers.begin(), identifiers.end(), 0);
    stripeStreams.push_back(tablet->load(i, identifiers));
  }

  dwio::api::AlphaWriterOptionBuilder optionBuilder;
  if (!serializedSerde_.empty()) {
    optionBuilder.withSerdeParams(
        reader->getType(),
        deserialize<Apache::Hadoop::Hive::SerDeInfo>(serializedSerde_)
            .get_parameters());
  }
  auto options = optionBuilder.build();

  LOG(INFO) << "Training parameters: CompressionAcceptRatio = "
            << options.compressionOptions.compressionAcceptRatio
            << ", Zstrong.CompressionLevel = "
            << options.compressionOptions.zstrongCompressionLevel
            << ", Zstrong.DecompressionLevel = "
            << options.compressionOptions.zstrongDecompressionLevel;

  velox::dwio::common::ExecutorBarrier barrier{executor};
  // Traverse schema. For each node, load all data and capture basic encoding
  // selection on data.
  auto taskTree = createTrainingTree(
      *reader->schema(),
      [&](const StreamDescriptor& descriptor,
          std::function<void(EncodingLayout &&)> setLayout) {
        barrier.add([&, setLayout = std::move(setLayout)]() {
          std::vector<std::string_view> streams;
          for (auto& stripeStream : stripeStreams) {
            const auto offset = descriptor.offset();
            if (stripeStream.size() > offset && stripeStream[offset]) {
              streams.push_back(stripeStream[offset]->getStream());
            }
          }

#define _SCALAR_CASE(kind, dataType)                                   \
  case ScalarKind::kind:                                               \
    setLayout(trainEncoding<dataType>(memoryPool_, options, streams)); \
    break;

          switch (descriptor.scalarKind()) {
            _SCALAR_CASE(Int8, int8_t);
            _SCALAR_CASE(UInt8, uint8_t);
            _SCALAR_CASE(Int16, int16_t);
            _SCALAR_CASE(UInt16, uint16_t);
            _SCALAR_CASE(Int32, int32_t);
            _SCALAR_CASE(UInt32, uint32_t);
            _SCALAR_CASE(Int64, int64_t);
            _SCALAR_CASE(UInt64, uint64_t);
            _SCALAR_CASE(Float, float);
            _SCALAR_CASE(Double, double);
            _SCALAR_CASE(Bool, bool);
            _SCALAR_CASE(String, std::string_view);
            _SCALAR_CASE(Binary, std::string_view);
            case ScalarKind::Undefined:
              ALPHA_UNREACHABLE("Scalar kind cannot be undefined.");
          }

#undef _SCALAR_KIND
        });
      });

  barrier.waitAll();

  return toEncodingLayoutTree(*taskTree);
}

} // namespace facebook::alpha::tools
