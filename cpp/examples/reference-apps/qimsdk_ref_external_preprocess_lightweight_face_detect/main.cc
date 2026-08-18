/*
Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
SPDX-License-Identifier: BSD-3-Clause-Clear
*/

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include <qti/qimsdk.h>
#include <getopt.h>

using namespace qti;

namespace {

// Converts one NV12 blit into the model's [1, H, W, 1] uint8 grayscale input
// tensor by resizing the luma (Y) plane only.
//
// face_det_lite_w8a8.tflite takes raw uint8 luma, so there is no color
// conversion and no normalization step: the Y samples are copied through
// as-is. The NV12 chroma (UV) plane is therefore never read, and only the
// first plane is required.
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
bool convert_nv12_to_y8(const MLVideoImage& image,
                             const MLVideoBlit* blit,
                             MLTensor& tensor) {
  // Only the luma plane is consumed, so a single plane is enough. The channel
  // count is checked too: a 3-channel tensor would leave two thirds unwritten.
  if (tensor.type != MLTensorType::UInt8 || tensor.data == nullptr ||
      tensor.dimensions.size() != 4 || tensor.dimensions[0] != 1 ||
      tensor.dimensions[3] != 1 || image.format != "NV12" ||
      image.planes.empty() || image.width == 0 || image.height == 0) {
    std::cout << "preprocess failed: invalid input/output contract"
              << " | tensor.type=" << static_cast<int>(tensor.type)
              << "(expect UInt8)"
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

  // tensor.size is a byte count. For this uint8 tensor one element is one
  // byte, so the element count doubles as the required byte count.
  const size_t needed = static_cast<size_t>(out_h) * out_w;
  if (tensor.size < needed) {
    std::cout << "preprocess failed: output tensor too small"
              << " | tensor.size=" << tensor.size
              << " | needed=" << needed << std::endl;
    return false;
  }

  const auto* y_plane = static_cast<const uint8_t*>(image.planes[0].data);
  const int y_stride = image.planes[0].stride;

  if (!y_plane || y_stride <= 0) {
    std::cout << "preprocess failed: invalid NV12 luma plane"
              << " | y_plane=" << static_cast<const void*>(y_plane)
              << " | y_stride=" << y_stride << std::endl;
    return false;
  }

  // Letterbox padding: any destination area the blit does not cover stays 0.
  auto* dst_buf = static_cast<uint8_t*>(tensor.data);
  std::fill(dst_buf, dst_buf + needed, static_cast<uint8_t>(0));

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

  // Precompute the source column for each destination column once, so the
  // inner loop is a plain lookup instead of a multiply/divide per pixel.
  static thread_local std::vector<int> src_x_table;
  src_x_table.resize(dst_w);
  for (int x = 0; x < dst_w; ++x) {
    src_x_table[x] = std::min(src_w - 1, (x * src_w) / dst_w);
  }

  // y_plane lives in decoder/dma-buf memory, which is slow for the CPU to
  // read, especially with the strided/random access that scaling requires.
  // Pull each needed source row once with a single sequential read into a
  // local buffer, then sample from that (fast) buffer for the whole row.
  static thread_local std::vector<uint8_t> row_buf;
  row_buf.resize(src_w);
  int cached_src_y = -1;

  for (int y = 0; y < dst_h; ++y) {
    const int out_y = dst_y + y;
    const int src_y = std::min(src_h - 1, (y * src_h) / dst_h);

    if (src_y != cached_src_y) {
      std::copy(y_plane + src_y * y_stride,
                y_plane + src_y * y_stride + src_w,
                row_buf.begin());
      cached_src_y = src_y;
    }

    const size_t out_row = static_cast<size_t>(out_y) * out_w + dst_x;
    for (int x = 0; x < dst_w; ++x) {
      dst_buf[out_row + x] = row_buf[src_x_table[x]];
    }
  }

  return true;
}


static const std::string home_path =
    std::getenv("HOME") ? std::getenv("HOME") : "";

// Base path for sample assets (media/, models/, labels/ live under it).
// Set from the --model-base-path argument, or the default location.
static std::string model_base_path;

// Input source location, overridden via the --input-config argument.
static std::string input_config;

//  Example pipeline:
//
//    filesrc → qtdemux → h264parse → v4l2h264dec
//            → [videofilter:NV12] → tee name=t_split
//      t_split. → q0 → qtimetamux
//      t_split. → q1 → qtimlvconverter(custom callback)
//              → q2 → qtimltflite → q3 → qtimlpostprocess
//              → [textfilter] → q4 → qtimetamux
//      qtimetamux → oq1 → qtivoverlay → oq2 → waylandsink
//
//  The pipeline reads an MP4/H.264 file, decodes it through the hardware
//  decoder, runs a lightweight (quantized) face detector with external
//  preprocessing callback logic that resizes the NV12 luma plane into a
//  single-channel grayscale tensor, overlays the detected faces, and displays
//  the result through Wayland.

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
        std::cout << "[external-preprocess][face-detect] called: "
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

        const bool ok = convert_nv12_to_y8(
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
  inference.set("model", model_base_path + "/models/face_det_lite_w8a8.tflite");

  // Queues tensor outputs before postprocessing.
  Element q3("queue", "q3");

  // Decodes model output tensors into face-detection metadata.
  Element postprocess("qtimlpostprocess", "postprocess");
  postprocess.set("module", "qfd");
  postprocess.set("labels", model_base_path + "/labels/qfd-labels.json");

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
  auto vf = VideoFilter().format("NV12");
  auto mlf = TextFilter();

  // Creates the pipeline, adds and links elements, and executes it.
  //
  // Explicit linking is applied to branch the tee into the display and ML
  // metadata branches, and to merge them back at the metamux. The queue on
  // the display branch (q0) matters: without it both tee branches are served
  // by one thread and the ML branch stalls display output.
  Pipeline pipeline("ml-external-preprocess-lightweight-face-detect");
  pipeline.add(src)
          .add(demux)
          .add(parse)
          .add(decoder)
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

          .link("src", "demux", "parse", "decoder", "videofilter", "t_split")
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
  // Path for sample input assets; override via --input-config argument.
  input_config = home_path + "/Downloads/qimsdk_samples/media/face_sample.mp4";

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
