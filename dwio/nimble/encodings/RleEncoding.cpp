// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#include "dwio/nimble/encodings/RleEncoding.h"

namespace facebook::nimble {

RLEEncoding<bool>::RLEEncoding(
    velox::memory::MemoryPool& memoryPool,
    std::string_view data)
    : internal::RLEEncodingBase<bool, RLEEncoding<bool>>(memoryPool, data) {
  initialValue_ = *reinterpret_cast<const bool*>(
      internal::RLEEncodingBase<bool, RLEEncoding<bool>>::getValuesStart());
  NIMBLE_CHECK(
      (internal::RLEEncodingBase<bool, RLEEncoding<bool>>::getValuesStart() +
       1) == data.end(),
      "Unexpected run length encoding end");
  internal::RLEEncodingBase<bool, RLEEncoding<bool>>::reset();
}

bool RLEEncoding<bool>::nextValue() {
  value_ = !value_;
  return !value_;
}

void RLEEncoding<bool>::resetValues() {
  value_ = initialValue_;
}

} // namespace facebook::nimble
