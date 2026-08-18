/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <iostream>
#include <thread>
#include <cstdlib>

#include <qti/qimsdk.h>

using namespace qti;

static const std::string home_path =
    std::getenv("HOME") ? std::getenv("HOME") : "";

//  Example pipeline:
//
//    source → [vf] → display → [if] → imagesink
//
//  The pipeline reads camera frames, runs ML inference and postprocessing,
//  and displays the result through Wayland.

void create_and_execute_pipeline() {

  // Captures frames from the camera source.
  Element source("qtiqmmfsrc", "source");

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
  imagesink.set("location", home_path + "/Downloads/qimsdk_samples/media/image_%d.jpeg");

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
