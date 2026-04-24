/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <cmath>
#include <iostream>
#include <utility>
#include <vector>
#include <cstdlib>

#include <qti/qimsdk.h>

#include "qimsdk-test-utils.h"

using namespace qti;

static const std::string home_path =
    std::getenv("HOME") ? std::getenv("HOME") : "";

namespace {

void TransformDimensions(MLDetection& box, const Region& region) {
  box.top = (box.top - region.y) / region.height;
  box.bottom = (box.bottom - region.y) / region.height;
  box.left = (box.left - region.x) / region.width;
  box.right = (box.right - region.x) / region.width;
}

float IntersectionScore(const MLDetection& l_box, const MLDetection& r_box) {
  float width = std::min(l_box.right, r_box.right);
  width -= std::max(l_box.left, r_box.left);

  if (width <= 0.0F)
    return 0.0F;

  float height = std::min(l_box.bottom, r_box.bottom);
  height -= std::max(l_box.top, r_box.top);

  if (height <= 0.0F)
    return 0.0F;

  float intersection = width * height;

  float l_area = (l_box.right - l_box.left) * (l_box.bottom - l_box.top);
  float r_area = (r_box.right - r_box.left) * (r_box.bottom - r_box.top);

  return intersection / (l_area + r_area - intersection);
}

int32_t NonMaxSuppression(const MLDetection& l_box,
                          const MLDetections& boxes) {
  const float kNMSIntersectionTreshold = 0.5;

  for (uint32_t idx = 0; idx < boxes.size(); idx++) {
    MLDetection r_box = boxes[idx];

    if (l_box.name != r_box.name)
      continue;

    double score = IntersectionScore(l_box, r_box);

    if (score <= kNMSIntersectionTreshold)
      continue;

    if (l_box.confidence > r_box.confidence)
      return idx;

    if (l_box.confidence <= r_box.confidence)
      return -2;
  }

  return -1;
}

bool decode_detection(const MLFrame& frame,
                      MLDetections& detections,
                      const MLParam& mlparams,
                      const std::vector<LabelEntry>& labels,
                      float confidence_threshold) {
  if (frame.tensors.empty()) {
    return false;
  }

  Region region;
  mlparams.get("input-tensor-region", region);

  uint32_t n_paxels = frame.tensors[0].dimensions[1];

  const float* bboxes = static_cast<const float*>(frame.tensors[0].data);
  const float* scores = static_cast<const float*>(frame.tensors[1].data);
  const float* classes = static_cast<const float*>(frame.tensors[2].data);

  for (uint32_t idx = 0; idx < n_paxels; idx++) {
    double confidence = scores[idx];
    uint32_t class_idx = static_cast<uint32_t>(classes[idx]);

    if (confidence < confidence_threshold)
      continue;

    MLDetection entry;
    entry.left = bboxes[idx * 4];
    entry.top = bboxes[idx * 4 + 1];
    entry.right = bboxes[idx * 4 + 2];
    entry.bottom = bboxes[idx * 4 + 3];

    std::cout << "Class: " << class_idx << " Confidence: " << confidence
              << " Box[" << entry.top << ", " << entry.left << ", "
              << entry.bottom << ", " << entry.right << "]" << std::endl;

    TransformDimensions(entry, region);

    if ((entry.top > 1.0) || (entry.left > 1.0) || (entry.bottom > 1.0) ||
        (entry.right > 1.0) || (entry.top < 0.0) || (entry.left < 0.0) ||
        (entry.bottom < 0.0) || (entry.right < 0.0))
      continue;

    entry.confidence = confidence * 100.0f;

    if (class_idx < labels.size() && !labels[class_idx].name.empty()) {
      entry.name = labels[class_idx].name;
      entry.color = labels[class_idx].color;
    }

    int32_t nms = NonMaxSuppression(entry, detections);

    if (nms == -2)
      continue;

    std::cout << "Label: " << entry.name << " Confidence: "
              << entry.confidence << " Box[" << entry.top << ", "
              << entry.left << ", " << entry.bottom << ", " << entry.right
              << "]" << std::endl;

    if (nms >= 0)
      detections.erase(detections.begin() + nms);

    detections.emplace_back(std::move(entry));
  }

  return true;
}

}  // namespace

void create_and_execute_pipeline() {
  Pipeline pipeline("mlbin-external-detection");

  static const std::vector<LabelEntry> labels =
      load_labels(home_path + "/Downloads/qimsdk_samples/labels/yolov8.json");

  MLVideoTFLiteBin mlbin("mlbin");
  mlbin
      .set("inference-delegate", "external",
           "inference-external-delegate-path", "libQnnTFLiteDelegate.so",
           "inference-external-delegate-options",
           "QNNExternalDelegate,backend_type=htp;",
           "inference-model", home_path + "/Downloads/qimsdk_samples/models/yolov8_det_quantized.tflite")
      .set_postprocess_handler(
          [&](const MLFrame& frame, const MLParam& params,
              MLDetections& detections) {
            return decode_detection(frame,
                                    detections,
                                    params,
                                    labels,
                                    /*confidence_threshold=*/0.70f);
          });

  pipeline
      .add("filesrc", "src", "location", home_path + "/Downloads/qimsdk_samples/media/ai_demo_sample.mp4")
      .add("qtdemux", "demux")
      .add("h264parse", "parse")
      .add("v4l2h264dec",
           "decoder",
           "output-io-mode", 4,
           "capture-io-mode", 4)
      .add_stream_filter("vf", VideoFilter().format("NV12"))
      .add(mlbin)
      .add("qtivoverlay", "overlay")
      .add("waylandsink", "display", "fullscreen", true);

  pipeline.execute();
}

int main() {
  if (home_path.empty()) {
    std::cerr << "Error: HOME environment variable is not set." << std::endl;
    return 1;
  }

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
