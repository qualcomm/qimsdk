/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>

#include <qti/qimsdk.h>

#include "qimsdk-test-utils.h"

using namespace qti;

namespace {

}  // namespace

bool decode_depth_estimation(const qti::MLFrame& frame,
                                  qti::MLDepthMaps& depthmaps,
                                  const std::vector<LabelEntry>& labels,
                                  const qti::MLParam& mlparams) {
  depthmaps.clear();

  auto indata = static_cast<const float *>(frame.tensors.front().data);
  uint32_t mlwidth = frame.tensors.front().dimensions[2];
  uint32_t mlheight = frame.tensors.front().dimensions[1];

  float source_width = 0.0f, source_height = 0.0f;
  qti::Region region = {};

  if (!mlparams.get("input-tensor-width", source_width) ||
      !mlparams.get("input-tensor-height", source_height) ||
      !mlparams.get("input-tensor-region", region)) {
    std::cerr << "Required mlparams not found for depth estimation parser."
              << std::endl;
    return false;
  }

  if (source_width <= 0.0f || source_height <= 0.0f) {
    std::cerr << "Invalid input tensor dimensions in mlparams." << std::endl;
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

  double mindepth = std::numeric_limits<double>::max();
  double maxdepth = std::numeric_limits<double>::min();

  qti::MLDepthMap depthmap;

  // Find the minimum and maximum depth values in the region mask.
  for (uint32_t row = top; row < bottom; row++) {
    for (uint32_t column = left; column < right; column++) {
      uint32_t idx = row * mlwidth + column;

      if (indata[idx] > maxdepth)
        maxdepth = indata[idx];

      if (indata[idx] < mindepth)
        mindepth = indata[idx];

      depthmap.values.push_back(indata[idx]);
    }
  }

  depthmap.n_rows = bottom - top;
  depthmap.n_columns = right - left;

  std::for_each(depthmap.values.cbegin(), depthmap.values.cend(),
      [&](const double& depth) {
        uint32_t id = std::numeric_limits<uint8_t>::max() *
                      ((depth - mindepth) / (maxdepth - mindepth));
        if (id < labels.size() && !labels[id].name.empty()) {
          depthmap.colors.push_back(labels[id].color);
        }
      });

  depthmaps.push_back(std::move(depthmap));

  std::cout << "[external-postprocess][depth_estimation] called: "
            << "tensors=" << frame.tensors.size()
            << ", params=" << mlparams.fields.size()
            << ", depths(out)=" << depthmaps.size()
            << ", decode_ok=true"
            << std::endl;
  return true;
}

static const std::string home_path =
    std::getenv("HOME") ? std::getenv("HOME") : "";

//  Example pipeline:
//
//    src → demux → parse → decoder → [vf] → split → q8 → composer → display
//                                                   └──→ q1 → preprocessing → q6 → inferencing → q7 → postprocessing → [mlf] → composer
//
//  The pipeline reads an MP4/H.264 file, decodes it through the hardware decoder,
//  runs external postprocessing callback logic and displays the result through Wayland.

void create_and_execute_pipeline() {

  // Reads the input media file as raw bytes.
  Element src("filesrc", "src");
  src.set("location", home_path + "/Downloads/qimsdk_samples/media/ai_demo_sample.mp4");

  // Extracts elementary streams from the MP4 container.
  Element demux("qtdemux", "demux");

  // Queues frames from tee into the ML branch.
  Element q1("queue", "q1");

  // Prepares the H.264 bitstream for the decoder.
  Element parse("h264parse", "parse");

  // Decodes the compressed H.264 stream into raw video frames.
  //
  // The I/O mode is configured to enforce DMA buffer usage,
  // avoiding unnecessary buffer copies.
  Element decoder("v4l2h264dec", "decoder");
  decoder.set("capture-io-mode", 4);
  decoder.set("output-io-mode", 4);

  auto vf = VideoFilter().format("NV12");

  // Splits decoded frames into display and ML branches.
  Element split("tee", "split");

  // Composites multiple input streams into a single output frame.
  Element composer("qtivcomposer", "composer");

  // Queues data between pipeline stages.
  Element q8("queue", "q8");

  // Render video stream on display.
  //
  // async=false enforce state transition to ensure the buffers are returned on time.
  // sync=true keeps rendering synchronized to the pipeline clock.
  // fullscreen=true renders the output fullscreen on the target display.
  Element display("waylandsink", "display");
  display.set("fullscreen", true);

  // Converts raw video frames into model input tensor format.
  Element preprocessing("qtimlvconverter", "preprocessing");

  // Queues data between pipeline stages.
  Element q6("queue", "q6");

  // Executes the ML model and attaches tensor outputs to each frame.
  Element inferencing("qtimltflite", "inferencing");
  inferencing.set("delegate", "external");
  inferencing.set("external-delegate-path", "libQnnTFLiteDelegate.so");
  inferencing.set("external-delegate-options", "QNNExternalDelegate,backend_type=htp;");
  inferencing.set("model", home_path + "/Downloads/qimsdk_samples/models/midas-tflite-w8a8.tflite");

  // Queues data between pipeline stages.
  Element q7("queue", "q7");

  // Read Labels used by the external postprocess callback for class decoding.
  static const std::vector<LabelEntry> labels =
      load_labels(home_path + "/Downloads/qimsdk_samples/labels/midas-v2-labels.json");

  // ML postprocessing element.
  MLPostprocess postprocessing("postprocessing");

  // ML postprocessing lambda function implementation.
  // Attach the callback for external postprocessing.
  postprocessing.set_handler(
      [](const MLFrame& frame, const MLParam& params,
         MLDepthMaps& depths) {
        return decode_depth_estimation(frame, depths, labels, params);
      });

  auto mlf = VideoFilter().format("RGBA");

  // Creates the pipeline, adds and links elements, and executes it.
  //
  // Explicit linking is applied
  Pipeline pipeline("midasv2_depth_estimation");

  pipeline.add(src)
          .add(demux)
          .add(parse)
          .add(decoder)
          .add_stream_filter("vf", vf)
          .add(split)
          .add(q1)
          .add(composer)
          .add(q8)
          .add(display)
          .add(preprocessing)
          .add(q6)
          .add(inferencing)
          .add(q7)
          .add(postprocessing)
          .add_stream_filter("mlf", mlf)
          .link("src", "demux", "parse", "decoder", "vf", "split")
          .link("split", "q8", "composer", "display")
          .link("split", "q1", "preprocessing", "q6", "inferencing", "q7", "postprocessing", "mlf", "composer");

  pipeline.get("composer").input(1).set("alpha", 0.5);

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
  } catch (const std::exception &ex) {
    std::cerr << "Exception: " << ex.what() << std::endl;
    return 1;
  }

  return 0;
}
