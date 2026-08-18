/*
Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
SPDX-License-Identifier: BSD-3-Clause-Clear
*/

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <getopt.h>

#include <qti/qimsdk.h>

using namespace qti;

namespace {

inline uint8_t clamp_u8(int value) {
  if (value < 0) return 0;
  if (value > 255) return 255;
  return static_cast<uint8_t>(value);
}

inline void nv12_to_rgb(uint8_t y, uint8_t u, uint8_t v,
                        float& r, float& g, float& b) {
  const int c = static_cast<int>(y) - 16;
  const int d = static_cast<int>(u) - 128;
  const int e = static_cast<int>(v) - 128;

  const int c_scaled = std::max(0, c) * 298;
  r = static_cast<float>(clamp_u8((c_scaled + 409 * e + 128) >> 8)) / 255.0f;
  g = static_cast<float>(clamp_u8((c_scaled - 100 * d - 208 * e + 128) >> 8)) / 255.0f;
  b = static_cast<float>(clamp_u8((c_scaled + 516 * d + 128) >> 8)) / 255.0f;
}


// Converts one NV12 blit into the model's [1, H, W, 3] float32 RGB input
// tensor, with values normalized to [0, 1] as palm_detection_full.tflite expects.
//
// Writes in place into `tensor.data`, which points directly at the preprocess
// element's output GstMLFrame block. The SDK re-validates the tensor's data
// pointer and size after the callback returns, so the block must be filled
// in place - repointing tensor.data at another buffer makes the frame fail.
//
// Scaling is nearest-neighbor, chosen to keep the example dependency-free.
// A model trained with bilinear or letterbox preprocessing will lose some
// accuracy here; match the model's training-time resize for best results.
//
// Returns false and logs the offending contract if the input image or output
// tensor does not match what this conversion supports.
bool convert_nv12_to_rgb(const MLVideoImage& image,
                             const MLVideoBlit* blit,
                             MLTensor& tensor) {
  if (tensor.type != MLTensorType::Float32 || tensor.data == nullptr ||
      tensor.dimensions.size() != 4 || tensor.dimensions[0] != 1 ||
      tensor.dimensions[3] != 3 || image.format != "NV12" ||
      image.planes.size() < 2 || image.width == 0 || image.height == 0) {
    std::cout << "preprocess failed: invalid input/output contract"
              << " | tensor.type=" << static_cast<int>(tensor.type)
              << " (expect Float32)"
              << " | tensor.data=" << tensor.data
              << " | tensor.dims.size=" << tensor.dimensions.size();
    if (!tensor.dimensions.empty()) {
      std::cout << " | dims=";
      for (size_t i = 0; i < tensor.dimensions.size(); ++i) {
        std::cout << tensor.dimensions[i]
                  << (i + 1 < tensor.dimensions.size() ? "x" : "");
      }
    }
    std::cout << " | image.format=" << image.format
              << " | image.planes=" << image.planes.size()
              << " | image.wh=" << image.width << "x" << image.height
              << std::endl;
    return false;
  }

  const uint32_t out_h = tensor.dimensions[1];
  const uint32_t out_w = tensor.dimensions[2];
  if (out_h == 0 || out_w == 0) {
    std::cout << "preprocess failed: zero output size"
              << " | out_wh=" << out_w << "x" << out_h << std::endl;
    return false;
  }

  // tensor.size is a byte count, so compare against bytes, not element count.
  // For the int8/uint8 examples the two happen to coincide; for float32 they
  // differ by 4x, and undersizing the check here would overflow the block.
  const size_t n_elements = static_cast<size_t>(out_h) * out_w * 3;
  const size_t needed = n_elements * sizeof(float);
  if (tensor.size < needed) {
    std::cout << "preprocess failed: output tensor too small"
              << " | tensor.size=" << tensor.size
              << " | needed=" << needed << std::endl;
    return false;
  }

  const auto* y_plane = static_cast<const uint8_t*>(image.planes[0].data);
  const auto* uv_plane = static_cast<const uint8_t*>(image.planes[1].data);
  const int y_stride = image.planes[0].stride;
  const int uv_stride = image.planes[1].stride;
  if (!y_plane || !uv_plane || y_stride <= 0 || uv_stride <= 0) {
    std::cout << "preprocess failed: invalid NV12 planes"
              << " | y_plane=" << static_cast<const void*>(y_plane)
              << " | uv_plane=" << static_cast<const void*>(uv_plane)
              << " | y_stride=" << y_stride
              << " | uv_stride=" << uv_stride << std::endl;
    return false;
  }

  // Letterbox padding: any destination area the blit does not cover stays 0.0.
  auto* dst_f32 = static_cast<float*>(tensor.data);
  std::fill(dst_f32, dst_f32 + n_elements, 0.0f);

  int dst_x = 0;
  int dst_y = 0;
  int dst_w = static_cast<int>(out_w);
  int dst_h = static_cast<int>(out_h);

  if (blit) {
    dst_x = std::max(0, blit->destination.x);
    dst_y = std::max(0, blit->destination.y);
    dst_w = std::max(1, std::min<int>(blit->destination.w, static_cast<int>(out_w) - dst_x));
    dst_h = std::max(1, std::min<int>(blit->destination.h, static_cast<int>(out_h) - dst_y));
  }

  const int src_w = static_cast<int>(image.width);
  const int src_h = static_cast<int>(image.height);

  for (int y = 0; y < dst_h; ++y) {
    const int out_y = dst_y + y;
    const int src_y = std::min(src_h - 1, (y * src_h) / dst_h);

    for (int x = 0; x < dst_w; ++x) {
      const int out_x = dst_x + x;
      const int src_x = std::min(src_w - 1, (x * src_w) / dst_w);

      const uint8_t yv = y_plane[src_y * y_stride + src_x];
      // NV12 interleaves U and V at half resolution, so a chroma pair starts
      // at an even column. On an odd-width frame the last column's pair would
      // read one byte past the visible row, so clamp the V index.
      const int uv_y = src_y / 2;
      const int uv_x = (src_x / 2) * 2;
      const uint8_t u = uv_plane[uv_y * uv_stride + uv_x];
      const uint8_t v =
          uv_plane[uv_y * uv_stride + std::min(uv_x + 1, src_w - 1)];

      float r = 0.0f;
      float g = 0.0f;
      float b = 0.0f;
      nv12_to_rgb(yv, u, v, r, g, b);

      const size_t out_idx = (static_cast<size_t>(out_y) * out_w + out_x) * 3;
      dst_f32[out_idx + 0] = r;
      dst_f32[out_idx + 1] = g;
      dst_f32[out_idx + 2] = b;
    }
  }

  return true;
}


static const std::string home_path =
    std::getenv("HOME") ? std::getenv("HOME") : "";

// Base path for sample assets (media/, models/, labels/ live under it).
// Set from the --model-base-path argument, or the default location.
static std::string model_base_path;

// Camera index, overridden via the --input-config argument.
static std::string input_config;

//  Example pipeline:
//
//    qtiqmmfsrc → [videofilter:NV12/1080p/30] → tee name=t_split
//      t_split. → q0 → qtimetamux
//      t_split. → q1 → qtimlvconverter(custom callback)
//              → q2 → qtimltflite → q3 → qtimlpostprocess
//              → [textfilter] → q4 → qtimetamux
//      qtimetamux → oq1 → qtivoverlay → oq2 → waylandsink
//
//  The pipeline captures camera frames, runs palm detection with external
//  preprocessing callback logic, overlays the detected palms, and displays
//  the result through Wayland.

void create_and_execute_pipeline() {

  // Captures frames from the camera source.
  CamSrc source("source");
  source.set("camera", std::stoi(input_config));

  // Splits decoded frames into display and ML branches.
  Element split("tee", "t_split");

  // Queues frames from tee into the display branch. Without this queue both
  // tee branches are served by one thread and the ML branch stalls display.
  Element q0("queue", "q0");

  // Queues frames from tee into the ML branch.
  Element q1("queue", "q1");

  // Converts raw video frames into model input tensor format.
  //
  // engine=none disables the internal preprocessing path in qtimlvconverter,
  // handing the conversion to the callback registered below.
  MLVConverter preprocessing("preprocess");
  preprocessing.set("engine", "none");

  // ML preprocessing lambda function implementation.
  // Attach the callback for external preprocessing.
  preprocessing.set_handler(
      [](const MLVideoBlits& blits, MLFrame& output) {
        std::cout << "[external-preprocess][palm-detect] called: "
                  << "blits=" << blits.entries.size()
                  << ", tensors=" << output.tensors.size() << std::endl;

        if (blits.entries.empty() || output.tensors.empty()) {
          std::cout << "preprocess failed: empty blits or tensors"
                    << " | blits=" << blits.entries.size()
                    << " | tensors=" << output.tensors.size() << std::endl;
          return false;
        }

        // Only the first blit is converted; this example assumes the
        // single-frame batching that qtimlvconverter uses by default.
        const MLVideoBlit& first_blit = blits.entries.front();
        const MLVideoImage& image = first_blit.image;
        if (image.planes.empty()) {
          std::cout << "preprocess failed: image has no planes"
                    << " | format=" << image.format
                    << " | wh=" << image.width << "x" << image.height << std::endl;
          return false;
        }

        const bool ok = convert_nv12_to_rgb(
            image, &first_blit, output.tensors.front());

        if (!ok) {
          std::cout << "preprocess failed: conversion routine failed" << std::endl;
        }
        return ok;
      });

  // Queues converted tensors before inference.
  Element q2("queue", "q2");

  // Executes the ML model and attaches tensor outputs to each frame.
  Element inference("qtimltflite", "inference");
  inference.set("delegate", "gpu");
  inference.set("model", model_base_path + "/models/palm_detection_full.tflite");

  // Queues tensor outputs before postprocessing.
  Element q3("queue", "q3");

  // Decodes model output tensors into palm-detection metadata.
  Element postprocess("qtimlpostprocess", "postprocess");
  postprocess.set("module", "palmd");
  postprocess.set("labels", model_base_path + "/labels/palmd_labels.json");
  postprocess.set("settings", model_base_path + "/labels/palmd_settings.json");

  // Queues metadata into the muxer's ML sink pad.
  Element q4("queue", "q4");

  // Merges metadata produced by the ML branch with original video frames.
  Element metamux("qtimetamux", "metamux");

  // Queues data between pipeline stages.
  Element oq1("queue", "oq1");

  // Renders ML metadata over the video frame.
  Element overlay("qtivoverlay", "overlay");

  // Queues data between pipeline stages.
  Element oq2("queue", "oq2");

  // Render video stream on display.
  //
  // fullscreen=true renders the output fullscreen on the target display.
  Element display("waylandsink", "display");
  display.set("fullscreen", true);

  // Stream filters used in branch links.
  // They define specific stream characteristics from the supported options.
  auto vf = VideoFilter().format("NV12").resolution(1920, 1080).framerate(30);
  auto mlf = TextFilter();

  // Creates the pipeline, adds and links elements, and executes it.
  //
  // Explicit linking is applied to branch the tee into the display and ML
  // metadata branches, and to merge them back at the metamux. The queue on
  // the display branch (q0) matters: without it both tee branches are served
  // by one thread and the ML branch stalls display output.
  Pipeline pipeline("ml-external-preprocess-palm-detect");
  pipeline.add(source)
          .add_stream_filter("videofilter", vf)
          .add(split)
          .add(q0)
          .add(q1)
          .add(preprocessing)
          .add(q2)
          .add(inference)
          .add(q3)
          .add(postprocess)
          .add_stream_filter("textfilter", mlf)
          .add(q4)
          .add(metamux)
          .add(oq1)
          .add(overlay)
          .add(oq2)
          .add(display)

          .link("source", "videofilter", "t_split")
          .link("t_split", "q0", "metamux")
          .link("t_split", "q1", "preprocess", "q2", "inference", "q3", "postprocess",
                "textfilter", "q4", "metamux")
          .link("metamux", "oq1", "overlay", "oq2", "display");

  pipeline.execute();
}

}  // namespace

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
