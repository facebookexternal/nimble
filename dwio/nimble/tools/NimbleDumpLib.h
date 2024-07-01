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
#pragma once
#include <optional>
#include <ostream>

#include "dwio/nimble/encodings/Encoding.h"
#include "dwio/nimble/velox/VeloxReader.h"
#include "velox/common/file/File.h"

namespace facebook::nimble::tools {

class NimbleDumpLib {
 public:
  NimbleDumpLib(std::ostream& ostream, const std::string& file);

  void emitRichInfo();
  void emitInfo();
  void emitSchema(bool collapseFlatMap = true);
  void emitStripes(bool noHeader);
  void emitStreams(
      bool noHeader,
      bool flatmapKeys,
      std::optional<uint32_t> stripeId);
  void
  emitHistogram(bool topLevel, bool noHeader, std::optional<uint32_t> stripeId);
  void emitContent(uint32_t streamId, std::optional<uint32_t> stripeId);
  void emitBinary(
      std::function<std::unique_ptr<std::ostream>()> outputFactory,
      uint32_t streamId,
      uint32_t stripeId);
  void emitLayout(bool noHeader, bool compressed);

 private:
  std::shared_ptr<velox::memory::MemoryPool> pool_;
  std::shared_ptr<velox::ReadFile> file_;
  std::shared_ptr<TabletReader> tablet_;
  std::shared_ptr<VeloxReader> reader_;
  std::ostream& ostream_;
};
} // namespace facebook::nimble::tools
