/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <iostream>
#include <cstdlib>
#include <string>
#include <getopt.h>

#include <qti/qimsdk.h>

using namespace qti;

static const std::string home_path =
    std::getenv("HOME") ? std::getenv("HOME") : "";

// Base path for sample assets (media/, models/, labels/ live under it).
// Set from the --model-base-path argument, or the default location.
static std::string model_base_path;

// Camera index, overridden via the --input-config argument.
static std::string input_config;

//  Example pipeline:
//
//    source → [videofilter] → qtimlvideotflitebin → qtivoverlay → waylandsink
//
//  The pipeline captures camera frames, runs YOLOv8 inference and postprocessing
//  in the ML bin, overlays detected objects, and displays the result.

void create_and_execute_pipeline() {

  // Captures frames from the camera source.
  Element source("qtiqmmfsrc", "source");
  source.set("camera", std::stoi(input_config));

  // Executes ML inference and postprocessing.
  //
  // Configures the model, the execution delegate and postprocessing labels.
  Element mlbin("qtimlvideotflitebin", "mlbin");
  mlbin.set("inference-delegate", "external");
  mlbin.set("inference-external-delegate-path", "libQnnTFLiteDelegate.so");
  mlbin.set("inference-external-delegate-options", "QNNExternalDelegate,backend_type=htp;");
  mlbin.set("inference-model", model_base_path + "/models/yolov8_det_quantized.tflite");
  mlbin.set("postprocess-module", "yolov8");
  mlbin.set("postprocess-labels", model_base_path + "/labels/yolov8.json");

  // Renders ML metadata over the video frame.
  Element overlay("qtivoverlay", "overlay");

  // Render video stream on display.
  //
  // async=false enforce state transition to ensure the buffers are returned on time.
  // sync=false disables strict rendering synchronization to the pipeline clock.
  Element display("waylandsink", "display");
  display.set("sync", false);
  display.set("fullscreen", true);

  // Stream filters used in branch links.
  // They define specific stream characteristics from the supported options.
  auto videofilter = qti::VideoFilter().format("NV12").resolution(1920, 1080).framerate(30);

  // Creates the pipeline, adds and links elements, and executes it.
  //
  // Linking is implicit and follows the order in which elements are added.
  Pipeline pipeline("camera-yolov8-mlbin-pipeline");
  pipeline.add(source)
          .add_stream_filter("videofilter", videofilter)
          .add(mlbin)
          .add(overlay)
          .add(display)
          .execute();
}

int main(int argc, char **argv) {
  if (home_path.empty()) {
    std::cerr << "Error: HOME environment variable is not set." << std::endl;
    return 1;
  }

  // Base path for sample assets; override via --model-base-path argument.
  model_base_path = home_path + "/Downloads/qimsdk_samples";
  input_config = "0";

  const std::string default_model_base_path = model_base_path;
  const std::string default_input_config = input_config;

  static struct option long_options[] = {
    {"model-base-path", required_argument, 0, 'm'},
    {"input-config", required_argument, 0, 'i'},
    {"help", no_argument, 0, 'h'},
    {0, 0, 0, 0}
  };

  auto print_usage = [&](std::ostream &out) {
    out << "Usage: " << argv[0] << " [OPTIONS]\n"
        << "\n"
        << "Options:\n"
        << "  -m, --model-base-path PATH   Base path for models and labels\n"
        << "                                (default: " << default_model_base_path << ")\n"
        << "  -i, --input-config VALUE     Input source configuration (camera number, device, or file path)\n"
        << "                                (default: " << default_input_config << ")\n"
        << "  -h, --help                   Show this help message and exit\n";
  };

  opterr = 0;  // Suppress getopt_long's own diagnostics; print_usage covers it.

  int option_index = 0;
  int c;
  while ((c = getopt_long(argc, argv, "m:i:h", long_options, &option_index)) != -1) {
    switch (c) {
      case 'm':
        model_base_path = optarg;
        break;
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
