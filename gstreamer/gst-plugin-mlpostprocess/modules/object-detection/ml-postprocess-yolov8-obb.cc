/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include "ml-postprocess-yolov8-obb.h"

#include <algorithm>
#include <cmath>

#include <Eigen/Core>
#include <Eigen/Geometry>

static const float kDefaultThreshold = 0.70;
// Rotated IoU threshold for Non-Maximum Suppression.
static const float kNMSIntersectionTreshold = 0.5;

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
        [1, [1000, 20000], 4],
        [1, [1000, 20000], 1],
        [1, [1000, 20000]],
        [1, [1000, 20000]]
      ]
    }
  ]
}
)";

Module::Module(LogCallback cb)
    : logger_(cb),
      threshold_(kDefaultThreshold) {

}

const Tensor& Module::FindTensor(const Tensors& tensors,
                                 const std::string& name, uint32_t fallback) {

  for (const Tensor& tensor : tensors) {
    if (tensor.name == name) return tensor;
  }

  return tensors[fallback];
}

float Module::IntersectionScore(const ObjectCandidate &l_candidate,
                                const ObjectCandidate &r_candidate) {

  // Probabilistic IoU via Bhattacharyya distance between two 2D Gaussians.
  // See: arxiv.org/pdf/2106.06072v1.pdf
  static const float kEpsilon = 1e-7f;

  // Model each rotated box as a 2D Gaussian: Σ = R · diag(w²/12, h²/12) · Rᵀ
  // Variance w²/12 comes from a uniform distribution over [−w/2, w/2].
  auto covariance = [](const ObjectCandidate &box) -> Eigen::Matrix2f {
    Eigen::Rotation2Df rotation(box.angle);
    Eigen::Vector2f extents(box.w * box.w / 12.0f, box.h * box.h / 12.0f);

    return rotation.toRotationMatrix() * extents.asDiagonal() *
        rotation.toRotationMatrix().transpose();
  };

  Eigen::Matrix2f cov1 = covariance(l_candidate);
  Eigen::Matrix2f cov2 = covariance(r_candidate);

  // Extract symmetric 2×2 matrix components: [[a, c], [c, b]]
  float a1 = cov1(0, 0), b1 = cov1(1, 1), c1 = cov1(0, 1);
  float a2 = cov2(0, 0), b2 = cov2(1, 1), c2 = cov2(0, 1);

  float dx = l_candidate.cx - r_candidate.cx;
  float dy = l_candidate.cy - r_candidate.cy;

  // det(Σ₁ + Σ₂), used as the denominator in the Mahalanobis-like BD terms.
  float denom = (a1 + a2) * (b1 + b2) - (c1 + c2) * (c1 + c2) + kEpsilon;

  // t1 + t2: (1/4) · Δμᵀ (Σ₁+Σ₂)⁻¹ Δμ — penalises centre offset.
  float t1 = (((a1 + a2) * dy * dy) + ((b1 + b2) * dx * dx)) / denom * 0.25f;
  float t2 = ((c1 + c2) * (-dx) * dy) / denom * 0.5f;

  // t3: (1/2) · ln( det((Σ₁+Σ₂)/2) / √(det(Σ₁)·det(Σ₂)) ) — penalises shape mismatch.
  // std::max guards against negative values from floating-point rounding.
  float num3 = (a1 + a2) * (b1 + b2) - (c1 + c2) * (c1 + c2);
  float den3 = 4.0f * std::sqrt(std::max(a1 * b1 - c1 * c1, 0.0f) *
      std::max(a2 * b2 - c2 * c2, 0.0f)) + kEpsilon;
  float t3 = std::log(num3 / den3 + kEpsilon) * 0.5f;

  // Hellinger distance hd = √(1 − e^(−BD)); return similarity as 1 − hd.
  float bd = std::clamp(t1 + t2 + t3, kEpsilon, 100.0f);
  float hd = std::sqrt(1.0f - std::exp(-bd) + kEpsilon);

  return 1.0f - hd;
}

int32_t Module::NonMaxSuppression(const ObjectCandidate &candidate,
    const std::vector<ObjectCandidate> &candidates) {

  for (uint32_t idx = 0; idx < candidates.size(); idx++) {
    const ObjectCandidate &other = candidates[idx];

    // If classes do not match, continue with next list entry.
    if (candidate.idx != other.idx)
      continue;

    double score = IntersectionScore(candidate, other);

    // If the score is below the threshold, continue with next list entry.
    if (score <= kNMSIntersectionTreshold)
      continue;

    // If confidence of current box is higher, remove the old entry.
    if (candidate.confidence > other.confidence)
      return idx;

    // If confidence of current box is lower, don't add it to the list.
    if (candidate.confidence <= other.confidence)
      return -2;
  }

  // If this point is reached then add current box to the list;
  return -1;
}

std::string Module::Caps() {

  return kModuleCaps;
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
  }

  return true;
}

bool Module::Process(const Tensors& tensors, Dictionary& mlparams,
                     std::any& output) {

  ObjectDetections& detections = std::any_cast<ObjectDetections&>(output);
  Region& region = std::any_cast<Region&>(mlparams["input-tensor-region"]);

  // Match tensors by name, not position - fall back to declared order.
  const Tensor& tboxes = FindTensor(tensors, "boxes", 0);
  const Tensor& tangles = FindTensor(tensors, "angles", 1);
  const Tensor& tscores = FindTensor(tensors, "scores", 2);
  const Tensor& tclasses = FindTensor(tensors, "class_idx", 3);

  const float* boxes = reinterpret_cast<const float*>(tboxes.data);
  const float* angles = reinterpret_cast<const float*>(tangles.data);
  const float* scores = reinterpret_cast<const float*>(tscores.data);
  const float* classes = reinterpret_cast<const float*>(tclasses.data);

  uint32_t n_anchors = tboxes.dimensions[1];

  // Raw per-anchor candidates surviving threshold+NMS - ObjectDetection
  // entries (labels, colors, region-relative coords) are only built for
  // these once NMS has settled on the final set.
  std::vector<ObjectCandidate> candidates;

  for (uint32_t idx = 0; idx < n_anchors; idx++) {
    float confidence = scores[idx];

    if (confidence < threshold_)
      continue;

    uint32_t class_idx = static_cast<uint32_t>(classes[idx] + 0.5f);

    ObjectCandidate candidate{
        class_idx, confidence, angles[idx],
        boxes[idx * 4], boxes[idx * 4 + 1],
        boxes[idx * 4 + 2], boxes[idx * 4 + 3]
    };

    LOG(logger_, kTrace, "Class: %u Confidence: %.2f CX x CY[%f, %f] W x H: "
        "[%f, %f] Angle: %f", candidate.idx, candidate.confidence, candidate.cx,
        candidate.cy, candidate.w, candidate.h, candidate.angle);

    // NMS algorithm, using the rotated overlap score instead of an axis-aligned.
    int32_t nms = NonMaxSuppression(candidate, candidates);

    // If the NMS result is -2 don't add the prediction to the list.
    if (nms == (-2))
      continue;

    // If the NMS result is above -1 remove the entry with the nms index.
    if (nms >= 0)
      candidates.erase(candidates.begin() + nms);

    candidates.emplace_back(candidate);
  }

  for (const ObjectCandidate &candidate : candidates) {
    ObjectDetection entry;

    entry.top = candidate.cy - candidate.h / 2.0f;
    entry.left = candidate.cx - candidate.w / 2.0f;
    entry.bottom = entry.top + candidate.h;
    entry.right = entry.left + candidate.w;

    entry.top = (entry.top - region.y) / region.height;
    entry.bottom = (entry.bottom - region.y) / region.height;
    entry.left = (entry.left - region.x) / region.width;
    entry.right = (entry.right - region.x) / region.width;

    entry.confidence = candidate.confidence * 100.0f;
    entry.name = labels_parser_.GetLabel(candidate.idx);
    entry.color = labels_parser_.GetColor(candidate.idx);

    // Pure rotation pivoted on the box center.
    float cosine = std::cos(candidate.angle);
    float sine = std::sin(candidate.angle);

    // Pivot is the model's own decoded center, region-normalized - matches
    // this entry's own (unclipped) rectangle center.
    double cx = (candidate.cx - region.x) / region.width;
    double cy = (candidate.cy - region.y) / region.height;

    // cx/cy are normalized per-axis, so the pivot offset needs an aspect
    // correction to stay fixed under rotation on a non-square region.
    double aspect = static_cast<double>(region.height) / region.width;

    // Pure rotation pivoted on (cx, cy).
    double tx = ((1 - cosine) * cx) + (sine * cy * aspect);
    double ty = -(sine * cx / aspect) + ((1 - cosine) * cy);

    // Scale the off-diagonal terms by aspect so the linear part stays
    // correct when region isn't square (relative axes have different scale).
    entry.xtraparams.emplace(Dictionary{
      {"rectangle-affine-matrix", std::vector<double>{
          cosine, -sine * aspect, tx,
          sine / aspect, cosine, ty,
          0.0, 0.0, 1.0}},
    });

    LOG(logger_, kTrace, "Label: %s Confidence: %.2f Box[%f, %f, %f, %f]",
        entry.name.c_str(), entry.confidence, entry.top, entry.left,
        entry.bottom, entry.right);

    detections.emplace_back(std::move(entry));
  }

  return true;
}

IModule* NewModule(LogCallback logger) {

  return new Module(logger);
}
