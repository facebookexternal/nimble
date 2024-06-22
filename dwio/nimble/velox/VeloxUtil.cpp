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

#include "dwio/nimble/velox/VeloxUtil.h"

#include "velox/dwio/common/exception/Exception.h"
#include "velox/vector/ComplexVector.h"
#include "velox/vector/FlatVector.h"

namespace facebook::dwio::api {

namespace {
template <velox::TypeKind K>
void resetVectorImpl(const velox::VectorPtr&) {}

void resetStringBuffer(const velox::VectorPtr& vector) {
  auto flat = vector->asFlatVector<velox::StringView>();
  DWIO_ENSURE(flat != nullptr);
  flat->clearStringBuffers();
}

template <>
void resetVectorImpl<velox::TypeKind::VARCHAR>(const velox::VectorPtr& vector) {
  resetStringBuffer(vector);
}

template <>
void resetVectorImpl<velox::TypeKind::VARBINARY>(
    const velox::VectorPtr& vector) {
  resetStringBuffer(vector);
}

template <>
void resetVectorImpl<velox::TypeKind::ARRAY>(const velox::VectorPtr& vector) {
  velox::ArrayVector* arrayVector = vector->as<velox::ArrayVector>();
  DWIO_ENSURE(arrayVector != nullptr);
  resetVector(arrayVector->elements());
}

template <>
void resetVectorImpl<velox::TypeKind::MAP>(const velox::VectorPtr& vector) {
  velox::MapVector* mapVector = vector->as<velox::MapVector>();
  DWIO_ENSURE(mapVector != nullptr);
  resetVector(mapVector->mapKeys());
  resetVector(mapVector->mapValues());
}

template <>
void resetVectorImpl<velox::TypeKind::ROW>(const velox::VectorPtr& vector) {
  velox::RowVector* rowVector = vector->as<velox::RowVector>();
  DWIO_ENSURE(rowVector != nullptr);
  std::for_each(
      rowVector->children().begin(), rowVector->children().end(), resetVector);
}
} // namespace

void resetVector(const velox::VectorPtr& vector) {
  if (vector == nullptr) {
    return;
  }
  vector->resetNulls();
  vector->resize(0);
  VELOX_DYNAMIC_TYPE_DISPATCH(resetVectorImpl, vector->typeKind(), vector);
}

namespace {

void populateFeatureSelectorForFlatMapAsStruct(
    const velox::dwio::common::ColumnSelector& selector,
    const std::unordered_map<uint32_t, std::vector<std::string>>& asStructMap,
    nimble::VeloxReadParams& params) {
  // asStructMap is using TypeWithId identifiers as the map key.
  // Nimble understands column names. Therefore, we perform id to name
  // conversion here.
  const auto typeWithId =
      velox::dwio::common::TypeWithId::create(selector.getSchema());
  const auto& names = typeWithId->type()->as<velox::TypeKind::ROW>().names();
  std::unordered_map<uint32_t, std::string_view> lookup;
  for (auto i = 0; i < names.size(); ++i) {
    lookup[typeWithId->childAt(i)->id()] = names[i];
  }
  for (const auto& pair : asStructMap) {
    auto it = lookup.find(pair.first);
    DWIO_ENSURE(
        it != lookup.end(), "Unable to resolve column name from node id.");
    params.readFlatMapFieldAsStruct.emplace(it->second);
    auto& featureSelector = params.flatMapFeatureSelector[it->second];
    featureSelector.mode = nimble::SelectionMode::Include;
    featureSelector.features.reserve(pair.second.size());
    for (const auto& feature : pair.second) {
      featureSelector.features.push_back(feature);
    }
  }
}

void populateOtherFeatureSelector(
    const velox::dwio::common::ColumnSelector& selector,
    nimble::VeloxReadParams& params) {
  if (selector.getProjection().empty()) {
    return;
  }
  constexpr char rejectPrefix = '!';
  for (const auto& filterNode : selector.getProjection()) {
    if (filterNode.expression.empty() ||
        params.readFlatMapFieldAsStruct.count(filterNode.name) != 0) {
      continue;
    }
    auto expressionJson = folly::parseJson(filterNode.expression);
    if (expressionJson.empty()) {
      continue;
    }
    auto& featureSelector = params.flatMapFeatureSelector[filterNode.name];
    featureSelector.features.reserve(expressionJson.size());
    auto exp = expressionJson[0].asString();
    DWIO_ENSURE(
        !exp.empty(),
        fmt::format("First feature in {} is empty", filterNode.name));
    bool isInclude = exp.front() != rejectPrefix;
    if (isInclude) {
      featureSelector.mode = nimble::SelectionMode::Include;
    } else {
      featureSelector.mode = nimble::SelectionMode::Exclude;
    }
    for (auto itor = expressionJson.begin(); itor != expressionJson.end();
         ++itor) {
      auto feature = itor->asString();
      DWIO_ENSURE(
          !feature.empty() && (feature.front() != rejectPrefix) == isInclude,
          fmt::format(
              "Empty or mixed included/excluded feature '{}' found "
              "for flatmap: {}.",
              feature,
              filterNode.name));
      featureSelector.features.emplace_back(
          isInclude ? feature : feature.substr(1));
    }
  }
}

} // namespace

void populateFeatureSelector(
    const velox::dwio::common::ColumnSelector& selector,
    const std::unordered_map<uint32_t, std::vector<std::string>>& asStructMap,
    nimble::VeloxReadParams& params) {
  if (!asStructMap.empty()) {
    populateFeatureSelectorForFlatMapAsStruct(selector, asStructMap, params);
  }
  populateOtherFeatureSelector(selector, params);
}

} // namespace facebook::dwio::api
