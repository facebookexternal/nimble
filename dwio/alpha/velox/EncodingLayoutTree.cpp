// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#include "dwio/alpha/velox/EncodingLayoutTree.h"
#include "dwio/alpha/common/EncodingPrimitives.h"
#include "dwio/alpha/common/Exceptions.h"

namespace facebook::alpha {

namespace {

constexpr uint32_t kMinBufferSize = 8;

std::pair<EncodingLayoutTree, uint32_t> createInternal(std::string_view tree) {
  // Layout:
  // 1 byte: Schema Kind
  // 2 byte: Name length
  // X bytes: Name bytes
  // 1 byte: Stream encoding layout count
  // Repeat next for "Stream encoding layout count" times:
  // 1 byte: Stream identifier
  // 2 byte: Encoding layout length
  // Y bytes: Encoding layout bytes
  // End repeat
  // 4 byte: Children count
  // Z bytes: Children

  ALPHA_CHECK(
      tree.size() >= kMinBufferSize,
      "Invalid captured encoding tree. Buffer too small.");

  auto pos = tree.data();
  auto schemaKind = encoding::read<uint8_t, Kind>(pos);
  auto nameLength = encoding::read<uint16_t>(pos);

  ALPHA_CHECK(
      tree.size() >= nameLength + kMinBufferSize,
      "Invalid captured encoding tree. Buffer too small.");

  std::string_view name{pos, nameLength};
  pos += nameLength;

  auto encodingLayoutCount = encoding::read<uint8_t>(pos);
  std::unordered_map<EncodingLayoutTree::StreamIdentifier, EncodingLayout>
      encodingLayouts;
  encodingLayouts.reserve(encodingLayoutCount);
  for (auto i = 0; i < encodingLayoutCount; ++i) {
    ALPHA_CHECK(
        tree.size() - (pos - tree.data()) >= 3,
        "Invalid captured encoding tree. Buffer too small.");
    auto streamIdentifier = encoding::read<uint8_t>(pos);
    auto encodingLength = encoding::read<uint16_t>(pos);

    ALPHA_CHECK(
        tree.size() - (pos - tree.data()) >= encodingLength,
        "Invalid captured encoding tree. Buffer too small.");

    auto layout = EncodingLayout::create({pos, encodingLength});
    encodingLayouts.insert({streamIdentifier, std::move(layout.first)});
    pos += layout.second;

    ALPHA_CHECK(
        layout.second == encodingLength,
        "Invalid captured encoding tree. Encoding size mismatch.");
  }

  auto childrenCount = encoding::read<uint32_t>(pos);
  uint32_t offset = pos - tree.data();
  std::vector<EncodingLayoutTree> children;
  children.reserve(childrenCount);
  for (auto i = 0; i < childrenCount; ++i) {
    auto encodingLayoutTree = createInternal(tree.substr(offset));
    offset += encodingLayoutTree.second;
    children.push_back(std::move(encodingLayoutTree.first));
  }

  return {
      {schemaKind,
       std::move(encodingLayouts),
       std::string{name},
       std::move(children)},
      offset};
}

} // namespace

EncodingLayoutTree::EncodingLayoutTree(
    Kind schemaKind,
    std::unordered_map<StreamIdentifier, EncodingLayout> encodingLayouts,
    std::string name,
    std::vector<EncodingLayoutTree> children)
    : schemaKind_{schemaKind},
      encodingLayouts_{std::move(encodingLayouts)},
      name_{std::move(name)},
      children_{std::move(children)} {
  ALPHA_CHECK(
      encodingLayouts_.size() < std::numeric_limits<uint8_t>::max(),
      "Too many encoding layout streams.");
}

uint32_t EncodingLayoutTree::serialize(std::span<char> output) const {
  ALPHA_CHECK(
      output.size() >= kMinBufferSize + name_.size(),
      "Captured encoding buffer too small.");

  auto pos = output.data();
  alpha::encoding::write(schemaKind_, pos);
  alpha::encoding::write<uint16_t>(name_.size(), pos);
  if (!name_.empty()) {
    alpha::encoding::writeBytes(name_, pos);
  }

  alpha::encoding::write<uint8_t>(encodingLayouts_.size(), pos);
  for (const auto& pair : encodingLayouts_) {
    uint32_t encodingSize = 0;
    alpha::encoding::write<StreamIdentifier>(pair.first, pos);
    encodingSize = pair.second.serialize(
        output.subspan(pos - output.data() + sizeof(uint16_t)));
    alpha::encoding::write<uint16_t>(encodingSize, pos);
    pos += encodingSize;
  }

  alpha::encoding::write<uint32_t>(children_.size(), pos);

  for (auto i = 0; i < children_.size(); ++i) {
    pos += children_[i].serialize(output.subspan(pos - output.data()));
  }

  return pos - output.data();
}

EncodingLayoutTree EncodingLayoutTree::create(std::string_view tree) {
  return std::move(createInternal(tree).first);
}

Kind EncodingLayoutTree::schemaKind() const {
  return schemaKind_;
}

const EncodingLayout* FOLLY_NULLABLE EncodingLayoutTree::encodingLayout(
    EncodingLayoutTree::StreamIdentifier identifier) const {
  auto it = encodingLayouts_.find(identifier);
  if (it == encodingLayouts_.end()) {
    return nullptr;
  }
  return &it->second;
}

const std::string& EncodingLayoutTree::name() const {
  return name_;
}

uint32_t EncodingLayoutTree::childrenCount() const {
  return children_.size();
}

const EncodingLayoutTree& EncodingLayoutTree::child(uint32_t index) const {
  ALPHA_DCHECK(
      index < childrenCount(),
      "Encoding layout tree child index is out of range.");

  return children_[index];
}

std::vector<EncodingLayoutTree::StreamIdentifier>
EncodingLayoutTree::encodingLayoutIdentifiers() const {
  std::vector<EncodingLayoutTree::StreamIdentifier> identifiers;
  identifiers.reserve(encodingLayouts_.size());
  std::transform(
      encodingLayouts_.cbegin(),
      encodingLayouts_.cend(),
      std::back_inserter(identifiers),
      [](const auto& pair) { return pair.first; });

  return identifiers;
}

} // namespace facebook::alpha
