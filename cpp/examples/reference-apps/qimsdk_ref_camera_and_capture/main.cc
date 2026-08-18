/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <iostream>
#include <thread>
#include <cstdlib>
#include <string>

#include <qti/qimsdk.h>
#include <getopt.h>

using namespace qti;

static const std::string home_path =
    std::getenv("HOME") ? std::getenv("HOME") : "";

// Camera index, overridden via the --input-config argument.
static std::string input_config;

// Output sink location, overridden via the --output-config argument.
static std::string output_config;

//  Example pipeline:
//
//    source → [vf] → display → [if] → imagesink
//
//  The pipeline reads camera frames, runs ML inference and postprocessing,
//  and displays the result through Wayland.

void create_and_execute_pipeline() {

  // Captures frames from the camera source.
  Element source("qtiqmmfsrc", "source");
  source.set("camera", std::stoi(input_config));

  // Render video stream on display.
  //
  // async=false enforce state transition to ensure the buffers are returned on time.
  // sync=false disables strict rendering synchronization to the pipeline clock.
  // fullscreen=true renders the output fullscreen on the target display.
  Element display("waylandsink", "display");
  display.set("async", false);
  display.set("sync", false);
  display.set("fullscreen", true);

  // Writes output buffers to files.
  Element imagesink("multifilesink", "imagesink");
  imagesink.set("enable-last-sample", false);
  imagesink.set("location", output_config);

  // Stream filters used in branch links.
  // They define specific stream characteristics from the supported options.
  auto vf = VideoFilter().format("NV12").resolution(1920, 1080).framerate(30);
  auto image_filter = ImageFilter().format("jpeg").resolution(3840, 2160);

  // Creates the pipeline, adds and links elements, and executes it.
  //
  // Explicit linking is applied
  Pipeline pipeline("cam-pipeline");
  pipeline.add(source)
          .add_stream_filter("vf", vf)
          .add(display)
          .add_stream_filter("if", image_filter)
          .add(imagesink)
          .link("source", "vf", "display")
          .link("source", "if", "imagesink")
          .start();

  std::this_thread::sleep_for(std::chrono::seconds(2));

  auto cam = pipeline.get<CamSrc>("source");
  cam.image_capture();

  std::this_thread::sleep_for(std::chrono::seconds(2));

  pipeline.stop();
}

int main(int argc, char **argv) {
  if (home_path.empty()) {
    std::cerr << "Error: HOME environment variable is not set." << std::endl;
    return 1;
  }

  input_config = "0";
  output_config = home_path + "/Downloads/qimsdk_samples/media/image_%d.jpeg";

  const std::string default_input_config = input_config;
  const std::string default_output_config = output_config;

  static struct option long_options[] = {
    {"input-config", required_argument, 0, 'i'},
    {"output-config", required_argument, 0, 'o'},
    {"help", no_argument, 0, 'h'},
    {0, 0, 0, 0}
  };

  auto print_usage = [&](std::ostream &out) {
    out << "Usage: " << argv[0] << " [OPTIONS]\n"
        << "\n"
        << "Options:\n"
        << "  -i, --input-config VALUE    Input source configuration (camera number, device, or file path)\n"
        << "                               (default: " << default_input_config << ")\n"
        << "  -o, --output-config VALUE   Output file location\n"
        << "                               (default: " << default_output_config << ")\n"
        << "  -h, --help                  Show this help message and exit\n";
  };

  opterr = 0;  // Suppress getopt_long's own diagnostics; print_usage covers it.

  int option_index = 0;
  int c;
  while ((c = getopt_long(argc, argv, "i:o:h", long_options, &option_index)) != -1) {
    switch (c) {
      case 'i':
        input_config = optarg;
        break;
      case 'o':
        output_config = optarg;
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

  // Route GStreamer logs through the QIMSDK logger and enable debug output.
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
