/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>
#include <cstdlib>

#include <qti/qimsdk.h>

#include "qimsdk-test-utils.h"

using namespace qti;
static const std::string home_path =
    std::getenv("HOME") ? std::getenv("HOME") : "";

namespace {

bool segmentation_callback(const MLFrame& frame,
                           const MLParam& params,
                           MLSegmentations& segmentations) {
  std::cout << "[external-postprocess][segmentation] called: tensors="
            << frame.tensors.size() << std::endl;

  if (frame.tensors.empty() || !frame.tensors.front().data)
    return false;

  // Read Labels used by the external postprocess callback for class decoding.
  static const std::vector<LabelEntry> labels =
      load_labels(home_path + "/Downloads/qimsdk_samples/labels/dv3-argmax-labels.json");

  const auto& tensor = frame.tensors.front();
  if (tensor.type != MLTensorType::Float32)
    return false;

  const auto* indata = static_cast<const float*>(tensor.data);

  uint32_t mlwidth = tensor.dimensions[2];
  uint32_t mlheight = tensor.dimensions[1];
  // The 4th tensor dimension represents multiple the class scores per paxel.
  uint32_t n_scores = (tensor.dimensions.size() != 4) ?
      1 : tensor.dimensions[3];

  float source_width = 0.0f;
  float source_height = 0.0f;
  if (!params.get("input-tensor-width", source_width) ||
      !params.get("input-tensor-height", source_height) ||
      source_width <= 0.0f || source_height <= 0.0f) {
    return false;
  }

  Region region;
  if (!params.get("input-tensor-region", region)) {
    return false;
  }

  // Scale factors for avoiding using division and instead use multiplication.
  float wscale = static_cast<float>(mlwidth) / source_width;
  float hscale = static_cast<float>(mlheight) / source_height;

  // Recalculate region dimensions to the tensor resolution being processed.
  uint32_t left = std::lround(region.x * wscale);
  uint32_t top = std::lround(region.y * hscale);
  uint32_t right = std::lround((region.x + region.width) * wscale);
  uint32_t bottom = std::lround((region.y + region.height) * hscale);

  right = std::min<uint32_t>(right, mlwidth);
  bottom = std::min<uint32_t>(bottom, mlheight);

  if (left >= right || top >= bottom)
    return false;

  MLSegmentation segmentation;
  segmentation.n_rows = bottom - top;
  segmentation.n_columns = right - left;

  const uint32_t out_size = segmentation.n_rows * segmentation.n_columns;
  segmentation.labels.assign(out_size, "unknown");
  segmentation.colors.assign(out_size, 0x0000000F);

  for (uint32_t row = top; row < bottom; row++) {
    const uint32_t out_row = (row - top) * segmentation.n_columns;
    const uint32_t row_base = row * mlwidth * n_scores;

    for (uint32_t column = left; column < right; column++) {
      const uint32_t pix_base = row_base + (column * n_scores);
      uint32_t id = 0;

      if (n_scores == 1) {
        id = static_cast<uint32_t>(indata[pix_base]);
      } else {
        uint32_t best = pix_base;
        for (uint32_t num = pix_base + 1; num < pix_base + n_scores; num++)
          best = (indata[num] > indata[best]) ? num : best;
        id = best - pix_base;
      }

      if (id < labels.size() && !labels[id].name.empty()) {
        const uint32_t outidx = out_row + (column - left);
        segmentation.labels[outidx] = labels[id].name;
        segmentation.colors[outidx] = labels[id].color;
      }
    }
  }

  segmentations.push_back(std::move(segmentation));
  return true;
}

}  // namespace

//  Example pipeline:
//
//    src → demux → parse → decoder → [vf] → split → q11 → composer → display
//                                                         └──→ q6 → preprocessing → q7 → inferencing → q8 → postprocessing → [mlf] → composer
//
//  The pipeline reads an MP4/H.264 file, decodes it through the hardware decoder,
//  runs external postprocessing callback logic and displays the result through Wayland.

void create_and_execute_pipeline() {

  // ML postprocessing element.
  MLPostprocess postprocessing("postprocessing");
  // Attach the callback for external postprocessing.
  postprocessing.set_handler(
      [](const MLFrame& frame, const MLParam& params,
         MLSegmentations& segmentations) {
        return segmentation_callback(frame, params, segmentations);
      });
  // Reads the input media file as raw bytes.
  Element src("filesrc", "src");
  src.set("location", home_path + "/Downloads/qimsdk_samples/media/ai_demo_sample.mp4");

  // Extracts elementary streams from the MP4 container.
  Element demux("qtdemux", "demux");


  // Prepares the H.264 bitstream for the decoder.
  Element parse("h264parse", "parse");

  // Decodes the compressed H.264 stream into raw video frames.
  //
  // The I/O mode is configured to enforce DMA buffer usage,
  // avoiding unnecessary buffer copies.
  Element decoder("v4l2h264dec", "decoder");
  decoder.set("capture-io-mode", 4);
  decoder.set("output-io-mode", 4);

  // Splits decoded frames into display and ML branches.
  Element split("tee", "split");

  // Composites multiple input streams into a single output frame.
  Element composer("qtivcomposer", "composer");

  // Queues data between pipeline stages.
  Element q11("queue", "q11");

  // Render video stream on display.
  //
  // async=false enforce state transition to ensure the buffers are returned on time.
  // sync=true keeps rendering synchronized to the pipeline clock.
  // fullscreen=true renders the output fullscreen on the target display.
  Element display("waylandsink", "display");
  display.set("fullscreen", true);

  // Queues data between pipeline stages.
  Element q6("queue", "q6");

  // Converts raw video frames into model input tensor format.
  Element preprocessing("qtimlvconverter", "preprocessing");

  // Queues data between pipeline stages.
  Element q7("queue", "q7");

  // Executes the ML model and attaches tensor outputs to each frame.
  Element inferencing("qtimltflite", "inferencing");
  inferencing.set("delegate", "external");
  inferencing.set("external-delegate-path", "libQnnTFLiteDelegate.so");
  inferencing.set("external-delegate-options", "QNNExternalDelegate,backend_type=htp;");
  inferencing.set("model", home_path + "/Downloads/qimsdk_samples/models/dv3_argmax_int32.tflite");

  // Queues data between pipeline stages.
  Element q8("queue", "q8");

  // Stream filters used in branch links.
  // They define specific stream characteristics from the supported options.
  auto vf = VideoFilter().format("NV12");
  auto mlf = VideoFilter().format("RGBA");

  // Creates the pipeline, adds and links elements, and executes it.
  //
  // Explicit linking is applied
  Pipeline pipeline("ml-external-segmentation");
  pipeline
      .add(src)
      .add(demux)
      .add(parse)
      .add(decoder)
      .add_stream_filter("vf", vf)
      .add(split)
      .add(composer)
      .add(q11)
      .add(display)
      .add(q6)
      .add(preprocessing)
      .add(q7)
      .add(inferencing)
      .add(q8)
      .add(postprocessing)
      .add_stream_filter("mlf", mlf)
      .link("src", "demux", "parse", "decoder", "vf", "split")
      .link("split", "q11", "composer", "display")
      .link("split", "q6", "preprocessing", "q7", "inferencing", "q8",
            "postprocessing", "mlf", "composer");

  pipeline.execute();
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
  } catch (const std::exception& ex) {
    std::cerr << "Exception: " << ex.what() << std::endl;
    return 1;
  }

  return 0;
}
