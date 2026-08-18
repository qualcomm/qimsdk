/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <iostream>
#include <string>
#include <getopt.h>

#include <qti/qimsdk.h>

using namespace qti;

// Camera index, overridden via the --input-config argument.
static std::string input_config;

void create_and_execute_pipeline() {
  Pipeline pipeline("cam-pipeline");
  pipeline.add("qtiqmmfsrc", "source", "camera", std::stoi(input_config))
          .add_stream_filter("videostream", VideoFilter().format("NV12").resolution(1920, 1080).framerate(30))
          .add("waylandsink", "display", "sync", false, "fullscreen", true)
          .execute();
}

int main(int argc, char **argv) {
  input_config = "0";

  const std::string default_input_config = input_config;

  static struct option long_options[] = {
    {"input-config", required_argument, 0, 'i'},
    {"help", no_argument, 0, 'h'},
    {0, 0, 0, 0}
  };

  auto print_usage = [&](std::ostream &out) {
    out << "Usage: " << argv[0] << " [OPTIONS]\n"
        << "\n"
        << "Options:\n"
        << "  -i, --input-config VALUE   Input source configuration (camera number, device, or file path)\n"
        << "                              (default: " << default_input_config << ")\n"
        << "  -h, --help                 Show this help message and exit\n";
  };

  opterr = 0;  // Suppress getopt_long's own diagnostics; print_usage covers it.

  int option_index = 0;
  int c;
  while ((c = getopt_long(argc, argv, "i:h", long_options, &option_index)) != -1) {
    switch (c) {
      case 'i':
        input_config = optarg;
        break;
      case 'h':
        print_usage(std::cout);
        return 0;
      case '?':
      default:
        print_usage(std::cerr);
        return 1;
    }
  }

  if (optind != argc) {
    std::cerr << "Error: unexpected argument '" << argv[optind] << "'\n\n";
    print_usage(std::cerr);
    return 1;
  }

  qti::SetImsdkGstLogMode(qti::ImsdkGstLogMode::ImsdkLog);
  qti::SetImsdkLogLevel(qti::ImsdkLogLevel::Debug);

  try {
    create_and_execute_pipeline();
  } catch (const std::exception &ex) {
    std::cerr << "Exception: " << ex.what() << std::endl;
    return 1;
  }

  return 0;
}
