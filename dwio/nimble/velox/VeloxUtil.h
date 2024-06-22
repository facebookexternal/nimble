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

#include "dwio/nimble/velox/VeloxReader.h"
#include "velox/vector/BaseVector.h"

namespace facebook::dwio::api {

// Make the vector as a clean reusable state, by resetting children size
// recursively for complex types.
void resetVector(const velox::VectorPtr& vector);

void populateFeatureSelector(
    const velox::dwio::common::ColumnSelector&,
    const std::unordered_map<uint32_t, std::vector<std::string>>& asStructMap,
    nimble::VeloxReadParams& outParams);

} // namespace facebook::dwio::api
