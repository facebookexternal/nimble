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

#include "velox/dwio/common/WriterFactory.h"

namespace facebook::velox::nimble {

class NimbleWriterFactory : public dwio::common::WriterFactory {
 public:
  NimbleWriterFactory() : WriterFactory(dwio::common::FileFormat::NIMBLE) {}

  std::unique_ptr<dwio::common::Writer> createWriter(
      std::unique_ptr<dwio::common::FileSink> sink,
      const dwio::common::WriterOptions& options) override;
};

inline void registerNimbleWriterFactory() {
  dwio::common::registerWriterFactory(std::make_shared<NimbleWriterFactory>());
}

inline void unregisterNimbleWriterFactory() {
  dwio::common::unregisterWriterFactory(dwio::common::FileFormat::NIMBLE);
}

} // namespace facebook::velox::nimble
