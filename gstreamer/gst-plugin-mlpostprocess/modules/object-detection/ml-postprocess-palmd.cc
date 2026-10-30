/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include "ml-postprocess-palmd.h"

#include <algorithm>
#include <cmath>

static const float kNmsIntersectionThreshold = 0.5f;
static const float kDefaultThreshold = 0.7f;

static const std::vector<float> kAnchorStrides = {8, 16, 16, 16};
// Anchors per grid cell for each stride layer (aspect_ratios: [1.0] plus the
// interpolated-scale anchor MediaPipe's SsdAnchorsCalculator always emits).
static const uint32_t kAnchorsPerCell = 2;

// Enlarge the palm box to cover the whole hand and shift it towards the fingers.
static const float kHandScale = 2.6f;
static const float kHandOffset = -0.5f;

/* kModuleCaps
*
* Description of the supported caps and the type of the module.
*/
static const std::string kModuleCaps = R"(
{
  "type": "object-detection",
  "tensors": [
    {
      "format": ["FLOAT32"],
      "dimensions": [
        [1, 2016, 18],
        [1, 2016, 1]
      ]
    }
  ]
}
)";

Module::Module(LogCallback cb)
    : logger_(cb),
      threshold_(kDefaultThreshold) {

}


std::string Module::Caps() {

  return kModuleCaps;
}

float Module::IntersectionScore(const ObjectDetection &l_entry,
                                const ObjectDetection &r_entry) {

  // Figure out the width of the intersecting rectangle.
  // 1st: Find out the X axis coordinate of left most Top-Right point.
  float width = std::min(l_entry.right, r_entry.right);
  // 2nd: Find out the X axis coordinate of right most Top-Left point
  // and substract from the previously found value.
  width -= std::max(l_entry.left, r_entry.left);

  // Negative width means that there is no overlapping.
  if (width <= 0.0F)
    return 0.0F;

  // Figure out the height of the intersecting rectangle.
  // 1st: Find out the Y axis coordinate of bottom most Left-Top point.
  float height = std::min(l_entry.bottom, r_entry.bottom);
  // 2nd: Find out the Y axis coordinate of top most Left-Bottom point
  // and substract from the previously found value.
  height -= std::max(l_entry.top, r_entry.top);

  // Negative height means that there is no overlapping.
  if (height <= 0.0F)
    return 0.0F;

  // Calculate intersection area.
  float intersection = width * height;

  // Calculate the area of the 2 objects.
  float l_area = (l_entry.right - l_entry.left) * (l_entry.bottom - l_entry.top);
  float r_area = (r_entry.right - r_entry.left) * (r_entry.bottom - r_entry.top);

  // Intersection over Union score.
  return intersection / (l_area + r_area - intersection);
}

int32_t Module::NonMaxSuppression(const ObjectDetection &l_entry,
                                  const ObjectDetections &entries) {

  for (uint32_t idx = 0; idx < entries.size();  idx++) {
    ObjectDetection r_entry = entries[idx];

    // If labels do not match, continue with next list entry.
    if (l_entry.name != r_entry.name)
      continue;

    double score = IntersectionScore(l_entry, r_entry);

    // If the score is below the threshold, continue with next list entry.
    if (score <= kNmsIntersectionThreshold)
      continue;

    // If confidence of current box is higher, remove the old entry.
    if (l_entry.confidence > r_entry.confidence)
      return idx;

    // If confidence of current box is lower, don't add it to the list.
    if (l_entry.confidence <= r_entry.confidence)
      return -2;
  }

  // If this point is reached then add current box to the list;
  return -1;
}

void Module::ComputeTransformMatrix(ObjectDetection &entry,
                                    const Region& region) {

  auto find_by_name = [&entry](const std::string& name) -> Keypoint* {
    auto it = std::find_if(entry.landmarks->begin(), entry.landmarks->end(),
        [&name](const Keypoint& keypoint) { return keypoint.name == name; });

    return (it != entry.landmarks->end()) ? &(*it) : nullptr;
  };

  // The vector which is used for determining the angle of the hand.
  Keypoint* wrist = find_by_name("wrist_center");
  Keypoint* middle = find_by_name("middle_base");

  if ((nullptr == wrist) || (nullptr == middle)) {
    LOG(logger_, kLog, "Missing 'wrist_center' or 'middle_base', skipping!");
    return;
  }

  float width = entry.right - entry.left;
  float height = entry.bottom - entry.top;

  // Re-derive the landmarks into a common, isotropic pixel space (the input
  // tensor's region) so the rotation angle isn't skewed by a non-square box.
  wrist->x = (entry.left + wrist->x * width) * region.width;
  wrist->y = (entry.top + wrist->y * height) * region.height;
  middle->x = (entry.left + middle->x * width) * region.width;
  middle->y = (entry.top + middle->y * height) * region.height;

  float rotation = static_cast<float>(M_PI_2) -
      std::atan2(-(middle->y - wrist->y), middle->x - wrist->x);

  // Wraps an angle into [-pi, pi].
  rotation -= 2.0f * M_PI * std::floor((rotation + M_PI) / (2.0f * M_PI));

  float cosine = std::cos(rotation);
  float sine = std::sin(rotation);
  float pixheight = height * region.height;

  // Shift the rect centre towards the fingers (X offset is 0 for this model).
  float tx = -pixheight * kHandOffset * sine / region.width;
  float ty = pixheight * kHandOffset * cosine / region.height;

  // Pivot for scale/rotation: centre of the palm box, in relative coords.
  double cx = entry.left + (width / 2.0f);
  double cy = entry.top + (height / 2.0f);

  // Combined linear part (rotation . scale), pivoted on (cx, cy).
  double lx = kHandScale * cosine;
  double ly = kHandScale * sine;

  // cx/cy are normalized per-axis, so the pivot offset needs an aspect
  // correction to stay fixed under rotation on a non-square region.
  double aspect = static_cast<double>(region.height) / region.width;

  tx += ((1 - lx) * cx) + (ly * cy * aspect);
  ty += -(ly * cx / aspect) + ((1 - lx) * cy);

  // Scale the off-diagonal terms by aspect so the linear part stays
  // correct when region isn't square (relative axes have different scale).
  entry.xtraparams.emplace(Dictionary{
    {"rectangle-affine-matrix", std::vector<double>{
        lx, -ly * aspect, tx,
        ly / aspect, lx, ty,
        0.0, 0.0, 1.0}},
  });
}

bool Module::Configure(const std::string& labels_file,
                       const std::string& json_settings) {

  if (!labels_parser_.LoadFromFile(labels_file)) {
    LOG(logger_, kError, "Failed to parse labels");
    return false;
  }

  if (!json_settings.empty()) {
    auto root = JsonValue::Parse(json_settings);

    if (!root || root->GetType() != JsonType::Object)
      return false;

    threshold_ = root->GetNumber("confidence") / 100.0;
    LOG(logger_, kLog, "Threshold: %f", threshold_);

    auto landmarks = root->GetArray("landmarks");

    for (auto lmk : landmarks) {
      if (!lmk || lmk->GetType() != JsonType::Object)
        continue;

      std::map<uint32_t, std::string> names;

      for (auto name : lmk->GetArray("landmarks_names")) {
        names.emplace(name->GetNumber("id"), name->GetString("name"));
      }

      landmarks_.emplace(lmk->GetNumber("id"), names);
    }
  }

  return true;
}

bool Module::Process(const Tensors& tensors, Dictionary& mlparams,
                     std::any& output) {

  ObjectDetections& detections = std::any_cast<ObjectDetections&>(output);
  Region& region = std::any_cast<Region&>(mlparams["input-tensor-region"]);
  auto& resolution = std::any_cast<Resolution&>(mlparams["input-tensor-dimensions"]);

  if (anchors_.empty()) {
    for (size_t i = 0; i < kAnchorStrides.size(); i++) {
      for (uint32_t y = 0; y < resolution.height / kAnchorStrides[i]; y++) {
        for (uint32_t x = 0; x < resolution.width / kAnchorStrides[i]; x++) {
          float cx = (x + 0.5f) * kAnchorStrides[i];
          float cy = (y + 0.5f) * kAnchorStrides[i];

          for (uint32_t num = 0; num < kAnchorsPerCell; num++)
            anchors_.push_back({cx, cy});
        }
      }
    }
  }

  const float* bentries = reinterpret_cast<const float*>(tensors[0].data);
  const float* scores = reinterpret_cast<const float*>(tensors[1].data);
  uint32_t paxels = tensors[0].dimensions[1];
  uint32_t layers = tensors[0].dimensions[2];
  uint32_t n_landmarks = (layers - 4) / 2;

  for (uint32_t idx = 0; idx < paxels; idx++) {
    ObjectDetection entry;

    float confidence = 1 / (1 + std::exp(- scores[idx]));

    if (confidence < threshold_)
      continue;

    float cx = bentries[idx * layers] + anchors_[idx][0];
    float cy = bentries[(idx * layers) + 1] + anchors_[idx][1];
    float w = bentries[(idx * layers) + 2];
    float h = bentries[(idx * layers) + 3];

    entry.top =  cy - h / 2.0f;
    entry.left = cx - w / 2.0f;
    entry.bottom = entry.top + h;
    entry.right = entry.left + w;

    entry.top = (entry.top - region.y) / region.height;
    entry.bottom = (entry.bottom - region.y) / region.height;
    entry.left = (entry.left - region.x) / region.width;
    entry.right = (entry.right - region.x) / region.width;

    // Discard boxes which are completely outside the frame on X and/or Y.
    if (entry.left >= 1.0f || entry.right <= 0.0f ||
        entry.top >= 1.0f || entry.bottom <= 0.0f)
      continue;

    entry.confidence = confidence * 100;
    entry.name = labels_parser_.GetLabel(0);
    entry.color = labels_parser_.GetColor(0);

    // Non-Max Suppression (NMS) algorithm.
    int32_t nms = NonMaxSuppression(entry, detections);

    // If the NMS result is -2 don't add the prediction to the list.
    if (nms == (-2))
      continue;

    if (nms >= 0)
      detections.erase(detections.begin() + nms);

    LOG(logger_, kTrace, "Label: %s Confidence: %.2f Box[%f, %f, %f, %f]",
        entry.name.c_str(), entry.confidence, entry.left, entry.top,
        entry.right, entry.bottom);

    // Initialize the optional landmarks field.
    entry.landmarks.emplace(n_landmarks);

    // Get the names for the landmarks for current object class.
    const std::map<uint32_t, std::string>& names = landmarks_.at(0);

    for (uint32_t num = 0; num < n_landmarks; num++) {
      Keypoint& keypoint = entry.landmarks->at(num);

      keypoint.name = names.at(num);
      keypoint.confidence = entry.confidence;

      keypoint.x = bentries[(idx * layers) + 4 + (2 * num)] + anchors_[idx][0];
      keypoint.y = bentries[(idx * layers) + 4 + (2 * num) + 1] + anchors_[idx][1];

      keypoint.x = (keypoint.x - (cx - w / 2.0f)) / w;
      keypoint.y = (keypoint.y - (cy - h / 2.0f)) / h;

      LOG(logger_, kTrace, "Landmark: %d %s [%f %f] ", num,
          keypoint.name.c_str(), keypoint.x, keypoint.y);
    }

    ComputeTransformMatrix(entry, region);

    detections.push_back(entry);
  }

  return true;
}

IModule* NewModule(LogCallback logger) {
  return new Module(logger);
}
