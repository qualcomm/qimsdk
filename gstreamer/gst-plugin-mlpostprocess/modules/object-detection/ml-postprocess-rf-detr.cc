/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include "ml-postprocess-rf-detr.h"

#include <algorithm>
#include <cmath>

static const float kDefaultThreshold = 0.70;
// Non-maximum Suppression(NMS) threshold(50%), corresponding to 2/3 overlap.
static const float kNMSIntersectionTreshold = 0.5;

static const std::string kModuleCaps = R"(
{
  "type": "object-detection",
  "tensors": [
    {
      "format": ["FLOAT32"],
      "dimensions": [
        [1, [30, 3000], 4],
        [1, [30, 3000]],
        [1, [30, 3000]]
      ]
    }
  ]
}
)";

Module::Module(LogCallback cb)
    : logger_(cb),
      threshold_(kDefaultThreshold) {

}

Module::~Module() {

}

std::string Module::Caps() {

  return kModuleCaps;
}

bool Module::Configure(const std::string& labels, const std::string& settings) {

  if (!labels_parser_.LoadFromFile(labels)) {
    LOG(logger_, kError, "Failed to parse labels");
    return false;
  }

  if (!settings.empty()) {
    auto root = JsonValue::Parse(settings);

    if (!root || root->GetType() != JsonType::Object) return false;

    threshold_ = root->GetNumber("confidence") / 100.0;
    LOG(logger_, kLog, "Threshold: %f", threshold_);
  }

  return true;
}

bool Module::Process(const Tensors& tensors, Dictionary& mlparams,
                     std::any& output) {

  auto& detections = std::any_cast<ObjectDetections&>(output);
  auto& region = std::any_cast<Region&>(mlparams["input-tensor-region"]);

  const Tensor& tboxes = FindTensor(tensors, "boxes", 0);
  const Tensor& tscores = FindTensor(tensors, "logits", 1);
  const Tensor& tclasses = FindTensor(tensors, "classes", 2);

  uint32_t n_paxels = tboxes.dimensions[1];

  const float* bboxes = reinterpret_cast<const float*>(tboxes.data);
  const float* scores = reinterpret_cast<const float*>(tscores.data);
  const float* classes = reinterpret_cast<const float*>(tclasses.data);

  for (uint32_t idx = 0; idx < n_paxels; idx++) {
    float confidence = scores[idx];
    uint32_t class_idx = static_cast<uint32_t>(classes[idx]);

    // Discard results below the minimum score threshold.
    if (confidence < threshold_)
      continue;

    ObjectDetection entry;

    // "boxes" is normalized xyxy relative to the model's input tensor.
    entry.left = bboxes[idx * 4];
    entry.top = bboxes[idx * 4 + 1];
    entry.right = bboxes[idx * 4 + 2];
    entry.bottom = bboxes[idx * 4 + 3];

    // Adjust bounding box dimensions with extracted source tensor region.
    entry.top = (entry.top - region.y) / region.height;
    entry.bottom = (entry.bottom - region.y) / region.height;
    entry.left = (entry.left - region.x) / region.width;
    entry.right = (entry.right - region.x) / region.width;

    entry.confidence = confidence * 100.0f;
    entry.name = labels_parser_.GetLabel(class_idx);
    entry.color = labels_parser_.GetColor(class_idx);

    // Non-Max Suppression(NMS) algorithm.
    int32_t nms = NonMaxSuppression(entry, detections);

    // If the NMS result is -2 don't add the prediction to the list.
    if (nms == (-2))
      continue;

    LOG(logger_, kTrace, "Label: %s Confidence: %.2f Box[%f, %f, %f, %f]",
        entry.name.c_str(), entry.confidence, entry.left, entry.top,
        entry.right, entry.bottom);

    // If the NMS result is above -1 remove the entry with the nms index.
    if (nms >= 0)
      detections.erase(detections.begin() + nms);

    detections.emplace_back(std::move(entry));
  }

  return true;
}

const Tensor& Module::FindTensor(const Tensors& tensors,
                                 const std::string& name, uint32_t fallback) {

  for (const Tensor& tensor : tensors) {
    if (tensor.name == name) return tensor;
  }

  return tensors[fallback];
}

float Module::IntersectionScore(const ObjectDetection &l_object,
                                const ObjectDetection &r_object) {

  // Figure out the width of the intersecting rectangle.
  // 1st: Find out the X axis coordinate of left most Top-Right point.
  float width = std::min(l_object.right, r_object.right);
  // 2nd: Find out the X axis coordinate of right most Top-Left point
  // and substract from the previously found value.
  width -= std::max(l_object.left, r_object.left);

  // Negative width means that there is no overlapping.
  if (width <= 0.0F)
    return 0.0F;

  // Figure out the height of the intersecting rectangle.
  // 1st: Find out the Y axis coordinate of bottom most Left-Top point.
  float height = std::min(l_object.bottom, r_object.bottom);
  // 2nd: Find out the Y axis coordinate of top most Left-Bottom point
  // and substract from the previously found value.
  height -= std::max(l_object.top, r_object.top);

  // Negative height means that there is no overlapping.
  if (height <= 0.0F)
    return 0.0F;

  // Calculate intersection area.
  float intersection = width * height;

  // Calculate the area of the 2 objects.
  float l_area = (l_object.right - l_object.left) *
      (l_object.bottom - l_object.top);

  float r_area = (r_object.right - r_object.left) *
      (r_object.bottom - r_object.top);

  // Intersection over Union score.
  float iou = intersection / (l_area + r_area - intersection);

  // Intersection over the smaller box's area - catches a query that only
  // covers a part (torso/legs) of another query's box, which IoU alone
  // misses since the union stays dominated by the larger box's area.
  float iomin = intersection / std::min(l_area, r_area);

  return std::max(iou, iomin);
}

int32_t Module::NonMaxSuppression(const ObjectDetection &l_object,
                                  const ObjectDetections &objects) {

  for (uint32_t idx = 0; idx < objects.size();  idx++) {
    ObjectDetection r_object = objects[idx];

    // If labels do not match, continue with next list entry.
    if (l_object.name != r_object.name)
      continue;

    double score = IntersectionScore(l_object, r_object);

    // If the score is below the threshold, continue with next list entry.
    if (score <= kNMSIntersectionTreshold)
      continue;

    // If confidence of current box is higher, remove the old entry.
    if (l_object.confidence > r_object.confidence)
      return idx;

    // If confidence of current box is lower, don't add it to the list.
    if (l_object.confidence <= r_object.confidence)
      return -2;
  }

  // If this point is reached then add current box to the list;
  return -1;
}

IModule* NewModule(LogCallback logger) {

  return new Module(logger);
}
