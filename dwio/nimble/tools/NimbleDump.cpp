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
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>

#include "common/base/BuildInfo.h"
#include "common/init/light.h"
#include "dwio/nimble/tools/NimbleDumpLib.h"
#include "folly/Singleton.h"
#include "folly/cli/NestedCommandLineApp.h"
#include "velox/common/base/StatsReporter.h"

using namespace ::facebook;
namespace po = ::boost::program_options;

template <typename T>
std::optional<T> getOptional(const po::variable_value& val) {
  return val.empty() ? std::nullopt : std::optional<T>(val.as<T>());
}

int main(int argc, char* argv[]) {
  // Disable GLOG
  FLAGS_minloglevel = 5;

  auto init = init::InitFacebookLight{
      &argc, &argv, folly::InitOptions().useGFlags(false)};

  std::string version{BuildInfo::getBuildTimeISO8601()};
  if (!version.empty()) {
    auto buildRevision = BuildInfo::getBuildRevision();
    if (buildRevision && buildRevision[0] != '\0') {
      version += folly::to<std::string>(" [", buildRevision, "]");
    }
  }

  folly::NestedCommandLineApp app{"", version};
  int style = po::command_line_style::default_style;
  style &= ~po::command_line_style::allow_guessing;
  app.setOptionStyle(static_cast<po::command_line_style::style_t>(style));

  po::positional_options_description positionalArgs;
  positionalArgs.add("file", /*max_count*/ 1);

  app.addCommand(
         "info",
         "<file>",
         "Print file information",
         "Prints file information.",
         [](const po::variables_map& options,
            const std::vector<std::string>& /*args*/) {
           nimble::tools::NimbleDumpLib{
               std::cout, options["file"].as<std::string>()}
               .emitRichInfo();
         },
         positionalArgs)
      .add_options()(
          "file",
          po::value<std::string>()->required(),
          "Nimble file path. Can be a local path or a Warm Storage path.");

  app.addCommand(
         "schema",
         "<file>",
         "Print file schema",
         "Prints the file schema tree.",
         [](const po::variables_map& options,
            const std::vector<std::string>& /*args*/) {
           nimble::tools::NimbleDumpLib{
               std::cout, options["file"].as<std::string>()}
               .emitSchema(!options["full"].as<bool>());
         },
         positionalArgs)
      // clang-format off
        .add_options()
            (
                "file",
                po::value<std::string>()->required(),
                "Nimble file path. Can be a local path or a Warm Storage path."
            )(
                "full,f",
                po::bool_switch()->default_value(false),
                "Emit full flat map schemas. Default is to collapse flat map schemas."
            );
  // clang-format on

  app.addCommand(
         "stripes",
         "<file>",
         "Print stripe information",
         "Prints detailed stripe information.",
         [](const po::variables_map& options,
            const std::vector<std::string>& /*args*/) {
           nimble::tools::NimbleDumpLib{
               std::cout, options["file"].as<std::string>()}
               .emitStripes(options["no_header"].as<bool>());
         },
         positionalArgs)
      // clang-format off
        .add_options()
        (
            "file",
            po::value<std::string>()->required(),
            "Nimble file path. Can be a local path or a Warm Storage path."
        )(
            "no_header,n",
            po::bool_switch()->default_value(false),
            "Don't print column names. Default is to include column names."
        );
  // clang-format on

  app.addCommand(
         "streams",
         "<file>",
         "Print stream information",
         "Prints detailed stream information.",
         [](const po::variables_map& options,
            const std::vector<std::string>& /*args*/) {
           nimble::tools::NimbleDumpLib{
               std::cout, options["file"].as<std::string>()}
               .emitStreams(
                   options["no_header"].as<bool>(),
                   options["labels"].as<bool>(),
                   getOptional<uint32_t>(options["stripe"]));
         },
         positionalArgs)
      // clang-format off
        .add_options()
            (
                "file",
                po::value<std::string>()->required(),
                "Nimble file path. Can be a local path or a Warm Storage path."
            )(
                "stripe,s",
                po::value<uint32_t>(),
                "Limit output to a single stripe with the provided stripe id. "
                "Default is to print streams for all stripes."
            )(
                "no_header,n",
                po::bool_switch()->default_value(false),
                "Don't print column names. Default is to include column names."
            )(
                "labels,l",
                po::bool_switch()->default_value(false),
                "Include stream labels. Lables provide a readable path from the "
                "root node to the stream, as they appear in the schema tree."
            );
  // clang-format on

  app.addCommand(
         "histogram",
         "<file>",
         "Print encoding histogram",
         "Prints encoding histogram, counting how many times each encoding "
         "appears in the file.",
         [](const po::variables_map& options,
            const std::vector<std::string>& /*args*/) {
           nimble::tools::NimbleDumpLib{
               std::cout, options["file"].as<std::string>()}
               .emitHistogram(
                   options["root_only"].as<bool>(),
                   options["no_header"].as<bool>(),
                   getOptional<uint32_t>(options["stripe"]));
         },
         positionalArgs)
      // clang-format off
        .add_options()
            (
                "file",
                po::value<std::string>()->required(),
                "Nimble file path. Can be a local path or a Warm Storage path."
            )(
                "stripe,s",
                po::value<uint32_t>(),
                "Limit analysis to a single stripe with the provided stripe id. "
                "Default is to analyze encodings in all stripes."
            )(
                "root_only,r",
                po::bool_switch()->default_value(false),
                "Include only root (top level) encodings in histogram. "
                "Default is to analyze full encoding trees."
            )(
                "no_header,n",
                po::bool_switch()->default_value(false),
                "Don't print column names. Default is to include column names."
            );
  // clang-format on

  app.addCommand(
         "content",
         "<file>",
         "Print the content of a stream",
         "Prints the materialized content (actual values) of a stream.",
         [](const po::variables_map& options,
            const std::vector<std::string>& /*args*/) {
           nimble::tools::NimbleDumpLib{
               std::cout, options["file"].as<std::string>()}
               .emitContent(
                   options["stream"].as<uint32_t>(),
                   getOptional<uint32_t>(options["stripe"]));
         },
         positionalArgs)
      // clang-format off
        .add_options()
            (
                "file",
                po::value<std::string>()->required(),
                "Nimble file path. Can be a local path or a Warm Storage path."
            )(
                "stream",
                po::value<uint32_t>()->required(),
                "The content of this stream id will be emitted."
            )(
                "stripe",
                po::value<uint32_t>(),
                "Limit output to a single stripe with the provided stripe id. "
                "Default is to output stream content across in all stripes."
            );
  // clang-format on

  app.addCommand(
         "binary",
         "<file>",
         "Dumps stream binary content",
         "Dumps stream binary content to a file.",
         [](const po::variables_map& options,
            const std::vector<std::string>& /*args*/) {
           nimble::tools::NimbleDumpLib{
               std::cout, options["file"].as<std::string>()}
               .emitBinary(
                   [path = options["output"].as<std::string>()]() {
                     return std::make_unique<std::ofstream>(
                         path,
                         std::ios::out | std::ios::binary | std::ios::trunc);
                   },
                   options["stream"].as<uint32_t>(),
                   options["stripe"].as<uint32_t>());
         },
         positionalArgs)
      // clang-format off
        .add_options()
            (
                "file",
                po::value<std::string>()->required(),
                "Nimble file path. Can be a local path or a Warm Storage path."
            )(
                "output,o",
                po::value<std::string>()->required(),
                "Output file path."
            )(
                "stream",
                po::value<uint32_t>()->required(),
                "The content of this stream id will be dumped to the output file."
            )(
                "stripe",
                po::value<uint32_t>()->required(),
                "Dumps the stream from this stripe id."
            );
  // clang-format on

  app.addCommand(
         "layout",
         "<file>",
         "Dumps layout file",
         "Dumps captured layout file content.",
         [](const po::variables_map& options,
            const std::vector<std::string>& /*args*/) {
           nimble::tools::NimbleDumpLib{
               std::cout, options["file"].as<std::string>()}
               .emitLayout(
                   options["no_header"].as<bool>(),
                   !options["uncompressed"].as<bool>());
         },
         positionalArgs)
      // clang-format off
          .add_options()
              (
                  "file",
                  po::value<std::string>()->required(),
                  "Encoding layout file path."
              )(
                "no_header,n",
                po::bool_switch()->default_value(false),
                "Don't print column names. Default is to include column names."
              )(
                "uncompressed,u",
                po::bool_switch()->default_value(false),
                "Is the layout file uncompressed. Default is false, which means "
                "the layout file is compressed."
              );
  // clang-format on

  app.addAlias("i", "info");
  app.addAlias("b", "binary");
  app.addAlias("c", "content");

  return app.run(argc, argv);
}

// Initialize dummy Velox stats reporter
folly::Singleton<facebook::velox::BaseStatsReporter> reporter([]() {
  return new facebook::velox::DummyStatsReporter();
});
