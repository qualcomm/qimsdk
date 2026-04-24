/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <iostream>
#include <cstdlib>

#include <qti/qimsdk.h>

using namespace qti;

static const std::string home_path =
    std::getenv("HOME") ? std::getenv("HOME") : "";

//  Example pipeline:
//
//    src → demux → [vf1] → parse → decoder → [vf2] → display
//
//  The pipeline creates and executes the configured media/ML graph.
//

void create_and_execute_pipeline() {

  // Reads a sequence of input files as stream data.
  Element src("multifilesrc", "src");
  src.set("location", home_path + "/Downloads/qimsdk_samples/media/ai_demo_sample.ts");
  src.set("stop-index", 0);

  // Extracts elementary streams from the transport stream.
  Element demux("tsdemux", "demux");

  // Prepares the H.264 bitstream for the decoder.
  Element parse("h264parse", "parse");

  // Decodes the compressed H.264 stream into raw video frames.
  //
  // The I/O mode is configured to enforce DMA buffer usage,
  // avoiding unnecessary buffer copies.
  Element decoder("v4l2h264dec", "decoder");
  decoder.set("output-io-mode", 4);
  decoder.set("capture-io-mode", 4);

  // Render video stream on display.
  //
  // async=false enforce state transition to ensure the buffers are returned on time.
  // sync=true keeps rendering synchronized to the pipeline clock.
  // fullscreen=true renders the output fullscreen on the target display.
  Element display("waylandsink", "display");
  display.set("fullscreen", true);

  auto vf1 = H264Filter().framerate(30);
  // Stream filters used in branch links.
  // They define specific stream characteristics from the supported options.
  auto vf2 = VideoFilter().format("NV12");

  // Creates the pipeline, adds and links elements, and executes it.
  //
  // Linking is implicit and follows the order in which elements are added.
  Pipeline pipeline("video-pipeline");
  pipeline.add(src)
          .add(demux)
          .add_stream_filter("vf1", vf1)
          .add(parse)
          .add(decoder)
          .add_stream_filter("vf2", vf2)
          .add(display);
  pipeline.start().wait().stop();
}

int main() {
  if (home_path.empty()) {
    std::cerr << "Error: HOME environment variable is not set." << std::endl;
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
