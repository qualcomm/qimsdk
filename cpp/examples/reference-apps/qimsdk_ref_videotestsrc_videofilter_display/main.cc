/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

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

  // Stream filters used in branch links.
  // They define specific stream characteristics from the supported options.
  auto vf1 = qti::VideoFilter().format("NV12").resolution(1920, 1080).framerate(30);
  auto vf2 = qti::VideoFilter().resolution(1280, 720);

  // Creates the pipeline, adds and links elements, and executes it.
  //
  // Linking is implicit and follows the order in which elements are added.
  Pipeline pipeline("video-pipeline");
  pipeline.add(src)
          .add_stream_filter("vf1", vf1)
          .add(transform)
          .add_stream_filter("vf2", vf2)
          .add(sink)
          .execute();
}

int main() {
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
