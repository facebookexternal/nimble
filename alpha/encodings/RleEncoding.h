// (c) Facebook, Inc. and its affiliates. Confidential and proprietary.

#pragma once

#include <array>
#include <span>
#include <type_traits>
#include "dwio/alpha/common/Bits.h"
#include "dwio/alpha/common/Buffer.h"
#include "dwio/alpha/common/EncodingPrimitives.h"
#include "dwio/alpha/common/EncodingType.h"
#include "dwio/alpha/common/FixedBitArray.h"
#include "dwio/alpha/common/Rle.h"
#include "dwio/alpha/common/Vector.h"
#include "dwio/alpha/encodings/Encoding.h"
#include "dwio/alpha/encodings/EncodingFactoryNew.h"
#include "dwio/alpha/encodings/EncodingIdentifier.h"
#include "dwio/alpha/encodings/EncodingSelection.h"

// Holds data in RLE format. Run lengths are bit packed, and the run values
// are stored trivially.
//
// Note: we might want to recursively use the encoding factory to encode the
// run values. This recursive use can lead to great compression, but also
// tends to slow things down, particularly write speed.

namespace facebook::alpha {

namespace internal {

// Base case covers the datatype-independent functionality. We use the CRTP
// to avoid having to use virtual functions (namely on
// RLEEncodingBase::RunValue).
// Data layout is:
//   Encoding::kPrefixSize bytes: standard Encoding data
//   4 bytes: runs size
//   X bytes: runs encoding bytes
template <typename T, typename RLEEncoding>
class RLEEncodingBase
    : public TypedEncoding<T, typename TypeTraits<T>::physicalType> {
 public:
  using cppDataType = T;
  using physicalType = typename TypeTraits<T>::physicalType;

  RLEEncodingBase(velox::memory::MemoryPool& memoryPool, std::string_view data)
      : TypedEncoding<T, physicalType>(memoryPool, data),
        materializedRunLengths_{EncodingFactory::decode(
            memoryPool,
            {data.data() + Encoding::kPrefixSize + 4,
             *reinterpret_cast<const uint32_t*>(
                 data.data() + Encoding::kPrefixSize)})} {}

  void reset() {
    materializedRunLengths_.reset();
    derived().resetValues();
    copiesRemaining_ = materializedRunLengths_.nextValue();
    currentValue_ = nextValue();
  }

  void skip(uint32_t rowCount) final {
    uint32_t rowsLeft = rowCount;
    // TODO: We should have skip blocks.
    while (rowsLeft) {
      if (rowsLeft < copiesRemaining_) {
        copiesRemaining_ -= rowsLeft;
        return;
      } else {
        rowsLeft -= copiesRemaining_;
        copiesRemaining_ = materializedRunLengths_.nextValue();
        currentValue_ = nextValue();
      }
    }
  }

  void materialize(uint32_t rowCount, void* buffer) final {
    uint32_t rowsLeft = rowCount;
    physicalType* output = static_cast<physicalType*>(buffer);
    while (rowsLeft) {
      if (rowsLeft < copiesRemaining_) {
        std::fill(output, output + rowsLeft, currentValue_);
        copiesRemaining_ -= rowsLeft;
        return;
      } else {
        std::fill(output, output + copiesRemaining_, currentValue_);
        output += copiesRemaining_;
        rowsLeft -= copiesRemaining_;
        copiesRemaining_ = materializedRunLengths_.nextValue();
        currentValue_ = nextValue();
      }
    }
  }

  static std::string_view encode(
      EncodingSelection<physicalType>& selection,
      std::span<const physicalType> values,
      Buffer& buffer) {
    const uint32_t valueCount = values.size();
    Vector<uint32_t> runLengths(&buffer.getMemoryPool());
    Vector<physicalType> runValues(&buffer.getMemoryPool());
    rle::computeRuns(values, &runLengths, &runValues);

    Buffer tempBuffer{buffer.getMemoryPool()};
    std::string_view serializedRunLengths =
        selection.template encodeNested<uint32_t>(
            EncodingIdentifiers::RunLength::RunLengths, runLengths, tempBuffer);

    std::string_view serializedRunValues =
        getSerializedRunValues(selection, runValues, tempBuffer);

    const uint32_t encodingSize = Encoding::kPrefixSize + 4 +
        serializedRunLengths.size() + serializedRunValues.size();
    char* reserved = buffer.reserve(encodingSize);
    char* pos = reserved;
    Encoding::serializePrefix(
        EncodingType::RLE, TypeTraits<T>::dataType, valueCount, pos);
    encoding::writeString(serializedRunLengths, pos);
    encoding::writeBytes(serializedRunValues, pos);
    ALPHA_DASSERT(pos - reserved == encodingSize, "Encoding size mismatch.");
    return {reserved, encodingSize};
  }

  const char* getValuesStart() {
    return this->data_.data() + Encoding::kPrefixSize + 4 +
        *reinterpret_cast<const uint32_t*>(
               this->data_.data() + Encoding::kPrefixSize);
  }

  RLEEncoding& derived() {
    return *static_cast<RLEEncoding*>(this);
  }
  physicalType nextValue() {
    return derived().nextValue();
  }
  static std::string_view getSerializedRunValues(
      EncodingSelection<physicalType>& selection,
      const Vector<physicalType>& runValues,
      Buffer& buffer) {
    return RLEEncoding::getSerializedRunValues(selection, runValues, buffer);
  }

  uint32_t copiesRemaining_ = 0;
  physicalType currentValue_;
  detail::BufferedEncoding<uint32_t, 32> materializedRunLengths_;
};

} // namespace internal

// Handles the numeric cases. Bools and strings are templated below.
// Data layout is:
// RLEEncodingBase bytes
// 4 * sizeof(physicalType) bytes: run values
template <typename T>
class RLEEncoding final : public internal::RLEEncodingBase<T, RLEEncoding<T>> {
  using physicalType = typename TypeTraits<T>::physicalType;

 public:
  explicit RLEEncoding(
      velox::memory::MemoryPool& memoryPool,
      std::string_view data);

  physicalType nextValue();
  void resetValues();
  static std::string_view getSerializedRunValues(
      EncodingSelection<physicalType>& selection,
      const Vector<physicalType>& runValues,
      Buffer& buffer) {
    return selection.template encodeNested<physicalType>(
        EncodingIdentifiers::RunLength::RunValues, runValues, buffer);
  }

 private:
  detail::BufferedEncoding<physicalType, 32> values_;
};

// For the bool case we know the values will alternative between true
// and false, so in addition to the run lengths we need only store
// whether the first value is true or false.
// RLEEncodingBase bytes
// 1 byte: whether first row is true
template <>
class RLEEncoding<bool> final
    : public internal::RLEEncodingBase<bool, RLEEncoding<bool>> {
 public:
  RLEEncoding(velox::memory::MemoryPool& memoryPool, std::string_view data);

  bool nextValue();
  void resetValues();
  static std::string_view getSerializedRunValues(
      EncodingSelection<bool>& /* selection */,
      const Vector<bool>& runValues,
      Buffer& buffer) {
    char* reserved = buffer.reserve(sizeof(char));
    *reserved = runValues[0];
    return {reserved, 1};
  }

 private:
  bool initialValue_;
  bool value_;
};

//
// End of public API. Implementations follow.
//

template <typename T>
RLEEncoding<T>::RLEEncoding(
    velox::memory::MemoryPool& memoryPool,
    std::string_view data)
    : internal::RLEEncodingBase<T, RLEEncoding<T>>(memoryPool, data),
      values_{EncodingFactory::decode(
          memoryPool,
          {internal::RLEEncodingBase<T, RLEEncoding<T>>::getValuesStart(),
           static_cast<size_t>(
               data.end() -
               internal::RLEEncodingBase<T, RLEEncoding<T>>::
                   getValuesStart())})} {
  internal::RLEEncodingBase<T, RLEEncoding<T>>::reset();
}

template <typename T>
typename RLEEncoding<T>::physicalType RLEEncoding<T>::nextValue() {
  return values_.nextValue();
}

template <typename T>
void RLEEncoding<T>::resetValues() {
  values_.reset();
}

} // namespace facebook::alpha
