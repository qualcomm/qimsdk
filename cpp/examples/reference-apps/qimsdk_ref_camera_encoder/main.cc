/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <iostream>
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
//    source -> [vf] -> encoder -> parser -> muxer -> sink
//
//  The pipeline reads camera frames, encodes them with H.264, muxes into MP4,
//  and writes the output to disk.

void create_and_execute_pipeline() {

  // Captures frames from the camera source.
  Element source("qtiqmmfsrc", "source");
  source.set("camera", std::stoi(input_config));

  // Encodes raw video frames into H.264 stream.
  Element encoder("v4l2h264enc", "encoder");
  encoder.set("output-io-mode", "dmabuf-import");
  encoder.set("capture-io-mode", "dmabuf");

  // Parses H.264 bitstream for downstream muxing.
  Element parser("h264parse", "parser");

  // Muxes encoded stream into MP4 container.
  Element muxer("mp4mux", "muxer");

  // Writes output stream to a file.
  Element sink("filesink", "sink");
  sink.set("location", output_config);

  // Stream filters used in branch links.
  // They define specific stream characteristics from the supported options.
  auto vf = VideoFilter().format("NV12").resolution(1920, 1080).framerate(30);

  // Creates the pipeline, adds and links elements, and executes it.
  //
  // Explicit linking is applied.
  Pipeline pipeline("cam-encoder-pipeline");
  pipeline.add(source)
          .add_stream_filter("vf", vf)
          .add(encoder)
          .add(parser)
          .add(muxer)
          .add(sink)
          .eos(true)
          .execute();
}

int main(int argc, char **argv) {
  if (home_path.empty()) {
    std::cerr << "Error: HOME environment variable is not set." << std::endl;
    return 1;
  }

  input_config = "0";
  output_config = home_path + "/Downloads/qimsdk_samples/media/encoder_output.mp4";

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
