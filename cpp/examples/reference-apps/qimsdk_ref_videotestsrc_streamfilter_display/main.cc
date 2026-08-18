/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <cstring>
#include <iostream>

#include <qti/qimsdk.h>

using namespace qti;

//  Example pipeline:
//
//    src → [vf1] → transform → [vf2] → sink
//
//  The pipeline creates and executes the configured media/ML graph.
//

void create_and_execute_pipeline() {

  // Generates synthetic test video frames.
  Element src("videotestsrc", "src");
  src.set("pattern", "ball");

  // Applies geometric transforms to video frames.
  Element transform("qtivtransform", "transform");
  transform.set("rotate", "180");

  // Render video stream on display.
  Element sink("waylandsink", "sink");
  sink.set("fullscreen", true);

  qti::StreamFilter vf1(
      "video/x-raw,format=NV12,width=1920,height=1080,framerate=30/1");
  qti::StreamFilter vf2("video/x-raw,width=1280,height=720");

  // Creates the pipeline, adds and links elements, and executes it.
  //
  // Linking is implicit and follows the order in which elements are added.
  Pipeline pipeline("video-pipeline-streamfilter-string");
  pipeline.add(src)
          .add_stream_filter("vf1", vf1)
          .add(transform)
          .add_stream_filter("vf2", vf2)
          .add(sink)
          .execute();
}

int main(int argc, char **argv) {
  auto print_usage = [&](std::ostream &out) {
    out << "Usage: " << argv[0] << " [OPTIONS]\n"
        << "\n"
        << "Options:\n"
        << "  -h, --help            Show this help message and exit\n";
  };

  if (argc == 2 && (std::strcmp(argv[1], "-h") == 0 || std::strcmp(argv[1], "--help") == 0)) {
    print_usage(std::cout);
    return 0;
  }

  if (argc != 1) {
    std::cerr << "Error: unexpected argument '" << argv[1] << "'\n\n";
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
