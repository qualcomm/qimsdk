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
//    source → [videostream] → tee name=split
//      split. → qtimetamux
//      split. → q2 → qtimlvconverter → q3 → qtimltflite → q4
//             → qtimlpostprocess → [mlf:text] → qtimetamux → q5 → qtivoverlay → waylandsink
//
//  The pipeline reads camera frames, runs YOLOv8 inference and postprocessing,
//  overlays detected objects, and displays the result through Wayland.

void create_and_execute_pipeline() {

  // Captures frames from the camera source.
  Element source("qtiqmmfsrc", "source");
  source.set("camera", std::stoi(input_config));

  // Splits decoded frames into display and ML branches.
  Element split("tee", "split");

  // Queues converted tensors before inference.
  Element q2("queue", "q2");

  // Converts raw video frames into model input tensor format.
  Element preprocessing("qtimlvconverter", "preprocessing");

  // Queues converted tensors before inference.
  Element q3("queue", "q3");

  // Executes the ML model and attaches tensor outputs to each frame.
  //
  // Configures the model and the hardware delegate used for execution.
  Element inferencing("qtimltflite", "inferencing");
  inferencing.set("delegate", "external");
  inferencing.set("external-delegate-path", "libQnnTFLiteDelegate.so");
  inferencing.set("external-delegate-options", "QNNExternalDelegate,backend_type=htp;");
  inferencing.set("model", model_base_path + "/models/yolov8_det_quantized.tflite");

  // Decodes model output tensors into metadata for downstream overlay.
  Element postprocessing("qtimlpostprocess", "postprocessing");
  postprocessing.set("results", 5);
  postprocessing.set("module", "yolov8");
  postprocessing.set("labels", model_base_path + "/labels/yolov8.json");
  postprocessing.set("settings", "{\"confidence\": 70.0}");

  // Merges metadata produced by the ML branch with original video frames.
  Element mlmuxer("qtimetamux", "mlmuxer");

  // Queues data between pipeline stages.
  Element q5("queue", "q5");

  // Renders ML metadata over the video frame.
  Element overlay("qtivoverlay", "overlay");

  // Render video stream on display.
  //
  // async=false enforce state transition to ensure the buffers are returned on time.
  // sync=false disables strict rendering synchronization to the pipeline clock.
  // fullscreen=true renders the output fullscreen on the target display.
  Element display("waylandsink", "display");
  display.set("sync", false);
  display.set("fullscreen", true);

  // Stream filters used in branch links.
  // They define specific stream characteristics from the supported options.
  auto videostream = qti::VideoFilter().format("NV12").resolution(1920, 1080).framerate(30);
  auto mlf = TextFilter();

  // Creates the pipeline, adds and links elements, and executes it.
  //
  // Explicit linking is applied
  Pipeline pipeline("ml-cam-pipeline");
  pipeline.add(source)
          .add_stream_filter("videostream", videostream)
          .add(split)
          .add(q2)
          .add(preprocessing)
          .add(q3)
          .add(inferencing)
          .add("queue", "q4")
          .add(postprocessing)
          .add_stream_filter("mlf", mlf)
          .add(mlmuxer)
          .add(q5)
          .add(overlay)
          .add(display)
          .link("split", "mlmuxer")
          .link("source", "videostream", "split", "q2", "preprocessing", "q3", "inferencing", "q4", "postprocessing", "mlf", "mlmuxer", "q5", "overlay", "display")
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
