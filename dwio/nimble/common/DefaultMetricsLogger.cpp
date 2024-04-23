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
#include "dwio/nimble/common/DefaultMetricsLogger.h"
#include <folly/json/json.h>
#include "dwio/nimble/common/Exceptions.h"

namespace facebook::nimble {

DefaultMetricsLogger::DefaultMetricsLogger(
    const std::string& ns,
    const std::string& table,
    const std::string& hostName,
    const std::string& clientId,
    std::string queryId)
    : ns_{ns},
      table_{table},
      hostName_{hostName},
      clientId_{clientId},
      queryId_{std::move(queryId)} {
  NIMBLE_DASSERT(!queryId_.empty(), "Empty query id passed in!");
}

void DefaultMetricsLogger::populateAccessorInfo(
    logger::XldbAlphaLogger& log) const {
  // Do not set the unset fields for better queries.
  if (LIKELY(!ns_.empty())) {
    log.setNS(ns_);
  }
  if (LIKELY(!table_.empty())) {
    log.setTable(table_);
  }
  if (LIKELY(!clientId_.empty())) {
    log.setClient(clientId_);
  }
  log.setQueryID(queryId_);
}

void DefaultMetricsLogger::logException(
    std::string_view operation,
    const std::string& errorMessage) const {
  logger::XldbAlphaLogger log;
  populateAccessorInfo(log);
  log.setOperationSV(operation);
  log.setError(errorMessage);
  LOG_VIA_LOGGER_ASYNC(log);
}

void DefaultMetricsLogger::logStripeLoad(
    const StripeLoadMetrics& metrics) const {
  logger::XldbAlphaLogger log;
  populateAccessorInfo(log);
  log.setOperationSV(kStripeLoadOperation);
  log.setCPUTime(metrics.cpuUsec);
  log.setWallTime(metrics.wallTimeUsec);
  log.setSerializedRunStats(folly::toJson(metrics.serialize()));
  LOG_VIA_LOGGER_ASYNC(log);
}

void DefaultMetricsLogger::logStripeFlush(
    const StripeFlushMetrics& metrics) const {
  logger::XldbAlphaLogger log;
  populateAccessorInfo(log);
  log.setOperationSV(kStripeFlushOperation);
  log.setCPUTime(metrics.flushCpuUsec);
  log.setWallTime(metrics.flushWallTimeUsec);
  log.setSerializedRunStats(folly::toJson(metrics.serialize()));
  LOG_VIA_LOGGER_ASYNC(log);
}

void DefaultMetricsLogger::logFileClose(const FileCloseMetrics& metrics) const {
  logger::XldbAlphaLogger log;
  populateAccessorInfo(log);
  log.setOperationSV(kFileCloseOperation);
  log.setCPUTime(metrics.totalFlushCpuUsec);
  log.setWallTime(metrics.totalFlushWallTimeUsec);
  log.setSerializedRunStats(folly::toJson(metrics.serialize()));
  LOG_VIA_LOGGER_ASYNC(log);
}

void DefaultMetricsLogger::logZstrongContext(const std::string& context) const {
  logger::XldbAlphaLogger log;
  populateAccessorInfo(log);
  log.setOperationSV(kZstrong);
  log.setSerializedDebugStats(context);
  LOG_VIA_LOGGER_ASYNC(log);
}

} // namespace facebook::nimble