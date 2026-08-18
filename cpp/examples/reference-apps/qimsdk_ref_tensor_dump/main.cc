/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <iostream>
#include <cstdlib>

#include <qti/qimsdk.h>
#include <getopt.h>

using namespace qti;

static const std::string home_path =
    std::getenv("HOME") ? std::getenv("HOME") : "";

// Input source location, overridden via the --input-config argument.
static std::string input_config;

// Output sink location, overridden via the --output-config argument.
static std::string output_config;

//  Example pipeline:
//
//    src → demux → parse → decoder → [vf] → ml_preprocessing → [tf] → sink
//
//  The pipeline reads an MP4/H.264 file, decodes it through the hardware decoder,
//  overlays detected objects, and displays the result through Wayland.

void create_and_execute_pipeline() {

  // Reads the input media file as raw bytes.
  Element src("filesrc", "src");
  src.set("location", input_config);

  // Extracts elementary streams from the MP4 container.
  Element demux("qtdemux", "demux");

  // Prepares the H.264 bitstream for the decoder.
  Element parse("h264parse", "parse");

  // Decodes the compressed H.264 stream into raw video frames.
  //
  // The I/O mode is configured to enforce DMA buffer usage,
  // avoiding unnecessary buffer copies.
  Element decoder("v4l2h264dec", "decoder");
  decoder.set("output-io-mode", 4);
  decoder.set("capture-io-mode", 4);

  // Converts raw video frames into model input tensor format.
  Element ml_preprocessing("qtimlvconverter", "ml-preprocessing");

  // Writes output buffers to files.
  Element sink("multifilesink", "sink");
  sink.set("max-files", 1);
  sink.set("location", output_config);

  // Stream filters used in branch links.
  // They define specific stream characteristics from the supported options.
  auto vf = VideoFilter().format("NV12");
  auto tf = TensorFilter().type("UINT8").dimensions(1,520,520,3);

  // Creates the pipeline, adds and links elements, and executes it.
  //
  // Linking is implicit and follows the order in which elements are added.
  Pipeline pipeline("ml-pipeline");
  pipeline.add(src)
          .add(demux)
          .add(parse)
          .add(decoder)
          .add_stream_filter("vf", vf)
          .add(ml_preprocessing)
          .add_stream_filter("tf", tf)
          .add(sink);
  pipeline.start().wait().stop();
}

int main(int argc, char **argv) {
  if (home_path.empty()) {
    std::cerr << "Error: HOME environment variable is not set." << std::endl;
    return 1;
  }

  input_config = home_path + "/Downloads/qimsdk_samples/media/ai_demo_sample.mp4";
  output_config = home_path + "/Downloads/qimsdk_samples/media/tensor_520_520.rgb";

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
