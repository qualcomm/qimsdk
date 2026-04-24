/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include <qti/qimsdk.h>

#include "qimsdk-test-utils.h"

using namespace qti;

// Layer index at which the object score resides.
static const uint32_t kScoreIdx = 4;
// Layer index from which the class labels begin.
static const uint32_t kClassesIdx = 5;

// Non-maximum Suppression (NMS) threshold (50%), corresponding to 2/3 overlap.
static const float kNMSIntersectionTreshold = 0.5;

namespace {

float IntersectionScore(const MLDetection &l_box,
                        const MLDetection &r_box) {

  // Figure out the width of the intersecting rectangle.
  // 1st: Find out the X axis coordinate of left most Top-Right point.
  float width = std::min(l_box.right, r_box.right);
  // 2nd: Find out the X axis coordinate of right most Top-Left point
  // and substract from the previously found value.
  width -= std::max(l_box.left, r_box.left);

  // Negative width means that there is no overlapping.
  if (width <= 0.0F)
    return 0.0F;

  // Figure out the height of the intersecting rectangle.
  // 1st: Find out the Y axis coordinate of bottom most Left-Top point.
  float height = std::min(l_box.bottom, r_box.bottom);
  // 2nd: Find out the Y axis coordinate of top most Left-Bottom point
  // and substract from the previously found value.
  height -= std::max(l_box.top, r_box.top);

  // Negative height means that there is no overlapping.
  if (height <= 0.0F)
    return 0.0F;

  // Calculate intersection area.
  float intersection = width * height;

  // Calculate the area of the 2 objects.
  float l_area = (l_box.right - l_box.left) * (l_box.bottom - l_box.top);
  float r_area = (r_box.right - r_box.left) * (r_box.bottom - r_box.top);

  // Intersection over Union score.
  return intersection / (l_area + r_area - intersection);
}

void TransformDimensions(MLDetection &box,
                         const Region& region) {

  box.top = (box.top - region.y) / region.height;
  box.bottom = (box.bottom - region.y) / region.height;
  box.left = (box.left - region.x) / region.width;
  box.right = (box.right - region.x) / region.width;
}

int32_t TensorCompareValues(const float *data,
    const uint32_t& l_idx, const uint32_t& r_idx) {

  return ((float*)data)[l_idx] > ((float*)data)[r_idx] ? 1 :
     ((float*)data)[l_idx] < ((float*)data)[r_idx] ? -1 : 0;
}

int32_t NonMaxSuppression(const MLDetection &l_box,
                                  const MLDetections &boxes) {

  for (uint32_t idx = 0; idx < boxes.size();  idx++) {
    MLDetection r_box = boxes[idx];

    // If labels do not match, continue with next list entry.
    if (l_box.name != r_box.name)
      continue;

    float score = IntersectionScore(l_box, r_box);

    // If the score is below the threshold, continue with next list entry.
    if (score <= kNMSIntersectionTreshold)
      continue;

    // If confidence of current box is higher, remove the old entry.
    if (l_box.confidence > r_box.confidence)
      return idx;

    // If confidence of current box is lower, don't add it to the list.
    if (l_box.confidence <= r_box.confidence)
      return -2;
  }

  // If this point is reached then add current box to the list;
  return -1;
}

bool parse_monoblock_tensors (const MLFrame& frame,
                             MLDetections& detections,
                             const MLParam& mlparams,
                             const std::vector<LabelEntry>& labels,
                             float confidence_threshold) {
  Region region = {};
  int nms = -1;
  float bbox[4] = { 0, };
  float source_width = 0.0f, source_height = 0.0f;

  const float* data = static_cast<const float*>(frame.tensors[0].data);

  uint32_t n_paxels = frame.tensors[0].dimensions[1];
  uint32_t n_layers = frame.tensors[0].dimensions[2];

  uint32_t idx = 0;

  if (!mlparams.get("input-tensor-width", source_width) ||
      !mlparams.get("input-tensor-height", source_height) ||
      !mlparams.get("input-tensor-region", region)) {
    std::cerr << "Required mlparams not found for monoblock parser." << std::endl;
    return false;
  }

  if (region.width == 0 || region.height == 0) {
    std::cerr << "Invalid input-tensor-region dimensions." << std::endl;
    return false;
  }

  for (uint32_t num = 0; num < n_paxels; num++, idx += n_layers) {
    MLDetection entry;

    float score = data[idx + kScoreIdx];

    if (score < confidence_threshold)
      continue;

    uint32_t id = idx + kClassesIdx;

    for (uint32_t m = (idx + kClassesIdx + 1);  m < (idx + n_layers); m++)
      id = (TensorCompareValues(data, m, id) > 0) ? m : id;

    float confidence = data[id];

    confidence *= score;

    if (confidence < confidence_threshold)
      continue;

    bbox[0] = data[idx];
    bbox[1] = data[idx + 1];
    bbox[2] = data[idx + 2];
    bbox[3] = data[idx + 3];

    entry.top = (bbox[1] - (bbox[3] / 2)) * source_height;
    entry.left = (bbox[0] - (bbox[2] / 2)) * source_width;
    entry.bottom = (bbox[1] + (bbox[3] / 2)) * source_height;
    entry.right = (bbox[0] + (bbox[2] / 2)) * source_width;

    TransformDimensions(entry, region);

    // Keep dimensions within the region.
    entry.top = std::max(entry.top,static_cast<float>(region.y));
    entry.left = std::max(entry.left, static_cast<float>(region.x));
    entry.bottom = std::min(entry.bottom,
        static_cast<float>((region.y + region.height)));
    entry.right = std::min(entry.right,
        static_cast<float>((region.x + region.width)));

    if (id - (idx + kClassesIdx) < labels.size() &&
        !labels[id - (idx + kClassesIdx)].name.empty()) {
      entry.name  = labels[id - (idx + kClassesIdx)].name;
      entry.color = labels[id - (idx + kClassesIdx)].color;
    }
    entry.confidence = confidence * 100.0f;

    nms = NonMaxSuppression(entry, detections);

    if (nms == (-2))
      continue;

    if (nms >= 0)
      detections.erase(detections.begin() + nms);

    detections.emplace_back(std::move(entry));
  }

  return true;
}

}  // namespace

bool decode_detection(const MLFrame& frame,
                      MLDetections& detections,
                      const MLParam& mlparams,
                      const std::vector<LabelEntry>& labels,
                      float confidence_threshold) {
  detections.clear();

  bool ok = true;
  if (frame.tensors.size() == 1) {
    parse_monoblock_tensors(frame, detections, mlparams, labels,
                            confidence_threshold);
  } else {
    std::cerr << "Ml frame with unsupported post-processing procedure!";
    ok = false;
  }

  std::cout << "[external-postprocess][detection] called: "
            << "tensors=" << frame.tensors.size()
            << ", params=" << mlparams.fields.size()
            << ", detections(out)=" << detections.size()
            << ", decode_ok=" << (ok ? "true" : "false");
  if (!detections.empty()) {
    std::cout << ", top1='" << detections.front().name
              << "'@" << detections.front().confidence;
  }
  std::cout << std::endl;

  return ok;
}

static const std::string home_path =
    std::getenv("HOME") ? std::getenv("HOME") : "";

//  Example pipeline:
//
//    src → demux → parse → decoder → [vf] → split → q1 → preprocessing → q2 → inferencing → q3 → postprocessing → [mlf] → mlmuxer → q4 → overlay → display
//
//  The pipeline reads an MP4/H.264 file, decodes it through the hardware decoder,
//  runs external postprocessing callback logic and displays the result through Wayland.

void create_and_execute_pipeline() {

  // Reads the input media file as raw bytes.
  Element src("filesrc", "src");
  src.set("location", home_path + "/Downloads/qimsdk_samples/media/ai_demo_sample.mp4");

  // Extracts elementary streams from the MP4 container.
  Element demux("qtdemux", "demux");

  // Queues data between pipeline stages.

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
  Element split("tee", "split");

  // Queues frames from tee into the ML branch.
  Element q1("queue", "q1");

  // Converts raw video frames into model input tensor format.
  Element preprocessing("qtimlvconverter", "preprocessing");

  // Queues converted tensors before inference.
  Element q2("queue", "q2");

  // Executes the ML model and attaches tensor outputs to each frame.
  Element inferencing("qtimltflite", "inferencing");
  inferencing.set("delegate", "external");
  inferencing.set("external-delegate-path", "libQnnTFLiteDelegate.so");
  inferencing.set("external-delegate-options", "QNNExternalDelegate,backend_type=htp;");
  inferencing.set("model", home_path + "/Downloads/qimsdk_samples/models/yolov5m-320x320-int8.tflite");

  // Queues tensors between inference and postprocessing.
  Element q3("queue", "q3");

  // Merges metadata produced by the ML branch with original video frames.
  Element mlmuxer("qtimetamux", "mlmuxer");

  // Queues data between pipeline stages.
  Element q4("queue", "q4");

  // Renders ML metadata over the video frame.
  Element overlay("qtivoverlay", "overlay");

  // Render video stream on display.
  //
  // async=false enforce state transition to ensure the buffers are returned on time.
  // sync=true keeps rendering synchronized to the pipeline clock.
  // fullscreen=true renders the output fullscreen on the target display.
  Element display("waylandsink", "display");
  display.set("fullscreen", true);

  // Read Labels used by the external postprocess callback for class decoding.
  static const std::vector<LabelEntry> labels =
      load_labels(home_path + "/Downloads/qimsdk_samples/labels/yolov5m.json");

  // ML postprocessing element.
  MLPostprocess postprocessing("postprocessing");

  // ML postprocessing lambda function implementation.
  // Attach the callback for external postprocessing.
  postprocessing.set_handler(
      [](const MLFrame& frame, const MLParam& params,
         MLDetections& detections) {
        return decode_detection(frame, detections, params, labels,
                                /*confidence_threshold=*/0.70f);
      });

  // Stream filters used in branch links.
  // They define specific stream characteristics from the supported options.
  auto vf = VideoFilter().format("NV12");
  auto mlf = TextFilter();

  // Creates the pipeline, adds and links elements, and executes it.
  //
  // Explicit linking is applied
  Pipeline pipeline("ml-external-detection");

  pipeline.add(src)
        .add(demux)
        .add(parse)
        .add(decoder)
        .add_stream_filter("vf", vf)
        .add(split)
        .add(q1)
        .add(preprocessing)
        .add(q2)
        .add(inferencing)
        .add(q3)
        .add(postprocessing)
        .add_stream_filter("mlf", mlf)
        .add(mlmuxer)
        .add(q4)
        .add(overlay)
        .add(display)
        .link("src", "demux", "parse", "decoder", "vf", "split")
        .link("split", "mlmuxer")
        .link("split", "q1", "preprocessing", "q2", "inferencing", "q3", "postprocessing", "mlf", "mlmuxer", "q4", "overlay", "display");

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
