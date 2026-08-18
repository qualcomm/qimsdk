/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <iostream>

#include <qti/qimsdk.h>
#include <getopt.h>

using namespace qti;

//  Example pipeline:
//
//    source → transform → [videofilter] → display
//
//  The pipeline reads frames from a USB (V4L2) camera, rotates them, restricts
//  the stream to NV12/1080p/30fps, and displays the result through Wayland.

void create_and_execute_pipeline(const std::string &device) {

  // Creates the pipeline, adds and links elements, and executes it.
  //
  // Elements are created on the fly as they are added, and linking is
  // implicit, following the order in which elements are added.
  //
  // sync=false disables strict rendering synchronization to the pipeline clock.
  // fullscreen=true renders the output fullscreen on the target display.
  Pipeline pipeline("usb-cam-pipeline");
  pipeline.add("v4l2src", "source", "device", device)
          .add("qtivtransform", "transform")
          .add_stream_filter("videofilter", VideoFilter().format("NV12").resolution(1920, 1080).framerate(30))
          .add("waylandsink", "display", "sync", false, "fullscreen", true)
          .execute();
}

int main(int argc, char **argv) {
  // Route GStreamer logs through the QIMSDK logger and enable debug output.
  qti::SetImsdkGstLogMode(qti::ImsdkGstLogMode::ImsdkLog);
  qti::SetImsdkLogLevel(qti::ImsdkLogLevel::Debug);

  // V4L2 device node to capture from, defaults to /dev/video2.
  std::string input_config;

  const std::string default_input_config = "/dev/video2";

  static struct option long_options[] = {
    {"input-config", required_argument, 0, 'i'},
    {"help", no_argument, 0, 'h'},
    {0, 0, 0, 0}
  };

  auto print_usage = [&](std::ostream &out) {
    out << "Usage: " << argv[0] << " [OPTIONS]\n"
        << "\n"
        << "Options:\n"
        << "  -i, --input-config DEVICE   V4L2 device node to capture from\n"
        << "                               (default: " << default_input_config << ")\n"
        << "  -h, --help                  Show this help message and exit\n";
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

  std::string device = !input_config.empty() ? input_config : default_input_config;

  try {
    create_and_execute_pipeline(device);
  } catch (const std::exception &ex) {
    std::cerr << "Exception: " << ex.what() << std::endl;
    return 1;
  }

  return 0;
}
