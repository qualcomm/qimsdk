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

// Base path for sample assets (media/, models/, labels/ live under it).
// Set from the --model-base-path argument, or the default location.
static std::string model_base_path;

// Input source location, overridden via the --input-config argument.
static std::string input_config;

//  Example pipeline:
//
//    src → demux → parse → decoder → [videofilter] → mlbin1 → mlbin2 → overlay → display
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

  // Executes the ML model and attaches the results to the corresponding video frame.
  //
  // Configures the model, the hardware that executes it (delegate),
  // as well as the postprocessing algorithm and the label file.
  Element mlbin1("qtimlvideotflitebin", "mlbin1");
  mlbin1.set("inference-delegate", "external");
  mlbin1.set("inference-external-delegate-path", "libQnnTFLiteDelegate.so");
  mlbin1.set("inference-external-delegate-options", "QNNExternalDelegate,backend_type=htp;");
  mlbin1.set("inference-model", model_base_path + "/models/foot_track_net-person-foot-detection-w8a8.tflite");
  mlbin1.set("postprocess-module", "qpd");
  mlbin1.set("postprocess-labels", model_base_path + "/labels/foot_track_net.json");
  mlbin1.set("postprocess-settings", model_base_path + "/labels/foot_track_net_settings.json");

  // Executes the ML model and attaches the results to the corresponding video frame.
  //
  // Configures the model, the hardware that executes it (delegate),
  // as well as the postprocessing algorithm and the label file.
  Element mlbin2("qtimlvideotflitebin", "mlbin2");
  mlbin2.set("preprocess-mode", "roi-batch-cumulative");
  mlbin2.set("inference-delegate", "external");
  mlbin2.set("inference-external-delegate-path", "libQnnTFLiteDelegate.so");
  mlbin2.set("inference-external-delegate-options", "QNNExternalDelegate,backend_type=htp;");
  mlbin2.set("inference-model", model_base_path + "/models/gear_guard_net-ppe-detection-w8a8.tflite");
  mlbin2.set("postprocess-module", "yolov8");
  mlbin2.set("postprocess-labels", model_base_path + "/labels/gear_guard_net.json");

  // Renders ML metadata over the video frame.
  Element overlay("qtivoverlay", "overlay");

  // Render video stream on display.
  //
  // async=false enforce state transition to ensure the buffers are returned on time.
  // sync=true keeps rendering synchronized to the pipeline clock.
  // fullscreen=true renders the output fullscreen on the target display.
  Element display("waylandsink", "display");
  display.set("fullscreen", true);

  // Stream filters used in branch links.
  // They define specific stream characteristics from the supported options.
  auto videofilter = VideoFilter().format("NV12");

  // Creates the pipeline, adds and links elements, and executes it.
  //
  // Linking is implicit and follows the order in which elements are added.
  Pipeline pipeline("mlbin-pipeline");
  pipeline.add(src)
          .add(demux)
          .add(parse)
          .add(decoder)
          .add_stream_filter("videofilter", videofilter)
          .add(mlbin1)
          .add(mlbin2)
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
  // Path for sample input assets; override via --input-config argument.
  input_config = home_path + "/Downloads/qimsdk_samples/media/ppe_sample.mp4";

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
        << "  -i, --input-config PATH      Input media file path\n"
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
