/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <qti/qimsdk.h>

#include "qimsdk-test-utils.h"

using namespace qti;

static const std::vector<float> kAnchorSizes = {8, 16, 16, 16};

static const float kNmsIntersectionThreshold = 0.5f;

constexpr uint32_t kExpectedTensors = 4;

using namespace qti;

namespace {

struct KeypointLinkIds {
  uint32_t s_kp_id;
  uint32_t d_kp_id;

  KeypointLinkIds()
      : s_kp_id(0), d_kp_id(0) {};

  KeypointLinkIds(uint32_t s_kp_id, uint32_t d_kp_id)
      : s_kp_id(s_kp_id), d_kp_id(d_kp_id) {};
};

float IntersectionScore(const MLDetection &l_box,
                        const MLDetection &r_box) {

  // Figure out the width of the intersecting rectangle.
  // 1st: Find out the X axis coordinate of left most Top-Right point.
  float width = fmin(l_box.right, r_box.right);
  // 2nd: Find out the X axis coordinate of right most Top-Left point
  // and substract from the previously found value.
  width -= fmax(l_box.left, r_box.left);

  // Negative width means that there is no overlapping.
  if (width <= 0.0F)
    return 0.0F;

  // Figure out the height of the intersecting rectangle.
  // 1st: Find out the Y axis coordinate of bottom most Left-Top point.
  float height = fmin (l_box.bottom, r_box.bottom);
  // 2nd: Find out the Y axis coordinate of top most Left-Bottom point
  // and substract from the previously found value.
  height -= fmax (l_box.top, r_box.top);

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

int32_t NonMaxSuppression(const MLDetection &l_box,
                          const MLDetections &boxes) {

  for (uint32_t idx = 0; idx < boxes.size();  idx++) {
    MLDetection r_box = boxes[idx];

    // If labels do not match, continue with next list entry.
    if (l_box.name != r_box.name)
      continue;

    double score = IntersectionScore (l_box, r_box);

    // If the score is below the threshold, continue with next list entry.
    if (score <= kNmsIntersectionThreshold)
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

void TransformDimensions(MLDetection &box,
                         const Region& region) {

  box.top = (box.top - region.y) / region.height;
  box.bottom = (box.bottom - region.y) / region.height;
  box.left = (box.left - region.x) / region.width;
  box.right = (box.right - region.x) / region.width;
}

}  // namespace

bool decode_object_detection(const MLFrame& frame,
                      const MLParam& mlparams,
                      const std::vector<LabelEntry>& labels,
                      MLDetections& detections,
                      float confidence_threshold) {
  detections.clear();

  float source_width = 0.0f;
  float source_height = 0.0f;
  if (!mlparams.get("input-tensor-width", source_width) ||
      !mlparams.get("input-tensor-height", source_height) ||
      source_width <= 0.0f || source_height <= 0.0f) {
    return false;
  }

  Region region;
  if (!mlparams.get("input-tensor-region", region)) {
    return false;
  }

  std::vector<std::array<float, 2>> anchors;
  std::vector<MLKeypoint> landmarks(7);

  landmarks[0].name = "wrist_center";
  landmarks[1].name = "index_base";
  landmarks[2].name = "middle_base";
  landmarks[3].name = "ring_base";
  landmarks[4].name = "pinky_base";
  landmarks[5].name = "palm_center";
  landmarks[6].name = "thumb_base";

  if (anchors.empty()) {
    for (size_t i = 0; i < kAnchorSizes.size(); i++) {
      for (uint32_t y = 0; y < source_height / kAnchorSizes[i]; y++) {
        for (uint32_t x = 0; x < source_width / kAnchorSizes[i]; x++) {
          float cx = (x + 0.5f) * kAnchorSizes[i];
          float cy = (y + 0.5f) * kAnchorSizes[i];
          anchors.push_back({cx, cy});
        }
      }
    }
  }

  const float* bboxes = static_cast<const float*>(frame.tensors[0].data);
  const float* scores = static_cast<const float*>(frame.tensors[1].data);
  uint32_t paxels = frame.tensors[0].dimensions[1];
  uint32_t layers = frame.tensors[0].dimensions[2];
  uint32_t n_landmarks = (layers - 4) / 2;

  for (uint32_t idx = 0; idx < paxels; idx++) {
    MLDetection entry;

    float confidence = 1 / (1 + expf(- scores[idx]));

    if (confidence < confidence_threshold)
      continue;

    float cx = bboxes[idx * layers] + anchors[idx / 2][0];
    float cy = bboxes[(idx * layers) + 1] + anchors[idx / 2][1];
    float w = bboxes[(idx * layers) + 2];
    float h = bboxes[(idx * layers) + 3];

    entry.top =  cy - h / 2.0f;
    entry.left = cx - w / 2.0f;
    entry.bottom = entry.top + h;
    entry.right = entry.left + w;

    entry.left = std::max(entry.left, (float)region.x);
    entry.top = std::max(entry.top, (float)region.y);
    entry.right = std::min(entry.right, (float) (region.x + region.width));
    entry.bottom = std::min(entry.bottom, (float) (region.y + region.height));

    TransformDimensions(entry, region);

    entry.confidence = confidence * 100;
    entry.name = labels[0].name;
    entry.color = labels[0].color;

    // Non-Max Suppression (NMS) algorithm.
    int32_t nms = NonMaxSuppression(entry, detections);

    // If the NMS result is -2 don't add the prediction to the list.
    if (nms == (-2))
      continue;

    if (nms >= 0)
      detections.erase(detections.begin() + nms);

    for (uint32_t num = 0; num < n_landmarks; num++) {
      MLKeypoint lmk;
      lmk.name = landmarks[num].name;

      lmk.x = bboxes[(idx * layers) + 4 + (2 * num)] + anchors[idx / 2][0];
      lmk.y = bboxes[(idx * layers) + 4 + (2 * num) + 1] + anchors[idx / 2][1];

      lmk.x = (lmk.x - (cx - w / 2.0f)) / w;
      lmk.y = (lmk.y - (cy - h / 2.0f)) / h;

      lmk.x = std::min(std::max(lmk.x, 0.0f), 1.0f);
      lmk.y = std::min(std::max(lmk.y, 0.0f), 1.0f);

      entry.landmarks.emplace_back(lmk);

      std::cout << "Landmark: " << num
                << " " << lmk.name
                << " [" << lmk.x
                << ", " << lmk.y
                << "]" << std::endl;
    }

    detections.push_back(entry);
  }

  return true;
}

void KeypointTransformCoordinates(MLKeypoint& keypoint,
                                  const Region& region) {

  keypoint.x = (keypoint.x - region.x) / region.width;
  keypoint.y = (keypoint.y - region.y) / region.height;
}

bool decode_pose_estimation(const MLFrame& frame,
                                          const std::vector<LabelEntry>& labels,
                                          const MLParam& mlparams,
                                          MLPoses& poses,
                                          float confidence_threshold) {
  poses.clear();

  if (frame.tensors[0].dimensions[1] != frame.tensors[3].dimensions[1]) {
    std::cout << "Second dimension of first and third tensor must be equal: "
              << frame.tensors[0].dimensions[1]
              << " != " << frame.tensors[3].dimensions[1];
    return false;
  }

  std::vector<KeypointLinkIds> connections = {
    {0,  17},
    {1,  0},
    {2,  1},
    {3,  2},
    {4,  3},
    {5,  0},
    {6,  5},
    {7,  6},
    {8,  7},
    {9,  5},
    {10, 9},
    {11, 10},
    {12, 11},
    {13, 9},
    {14, 13},
    {15, 14},
    {16, 15},
    {17, 13},
    {18, 17},
    {19, 18},
    {20, 19}
  };

  Region region;
  if (!mlparams.get("input-tensor-region", region)) {
    return false;
  }

  const float* coordinates = static_cast<const float*>(frame.tensors[0].data);
  const float* scores = static_cast<const float*>(frame.tensors[1].data);

  // There are 3 coordinates per point - x, y, z
  uint32_t n_keypoints = frame.tensors[0].dimensions[1] / 3;
  float confidence = scores[0];

  if (confidence < confidence_threshold)
    return true;

  MLPose entry;
  entry.confidence = confidence;

  entry.keypoints.resize(n_keypoints);

  for (uint32_t idx = 0; idx < n_keypoints; idx++) {
    MLKeypoint& kp = entry.keypoints[idx];

    kp.x = coordinates[3 * idx];
    kp.y = coordinates[3 * idx + 1];

    if (idx < labels.size() && !labels[idx].name.empty()) {
      kp.name = labels[idx].name;
      kp.color = labels[idx].color;
    }

    kp.confidence = confidence * 100;

    KeypointTransformCoordinates(kp, region);

    kp.x = std::min(std::max(kp.x,(float)0),(float)1);
    kp.y = std::min(std::max(kp.y,(float)0),(float)1);
  }

  std::vector<MLKeypointLink> links;
    for (uint32_t num = 0; num < connections.size(); num++) {
      KeypointLinkIds& lk = connections[num];
      links.push_back(MLKeypointLink {
                      entry.keypoints[lk.s_kp_id],
                      entry.keypoints[lk.d_kp_id]
                    });
  }

  entry.links = std::move(links);

  poses.push_back(entry);

  return true;
}

bool decode_tensor(const MLFrame& frame,
                        const MLParam& mlparams,
                        MLFrame& output) {
    std::cout << "[external-postprocess][tensor] called: tensors="
            << frame.tensors.size() << std::endl;

  if (frame.tensors.size() != kExpectedTensors) {
    std::cerr << "[GR] Expected " << kExpectedTensors << " tensors, got "
              << frame.tensors.size() << std::endl;
    return false;
  }

  const auto& landmarks2d = frame.tensors[0];
  const auto& handedness = frame.tensors[2];
  const auto& landmarks3d = frame.tensors[3];

  if (landmarks2d.dimensions.size() != 2 ||
      landmarks2d.dimensions.back() != 63) {
    std::cerr << "[GR] Invalid 2D landmarks shape" << std::endl;
    return false;
  }

  if (landmarks3d.dimensions.size() != 2 ||
      landmarks3d.dimensions.back() != 63) {
    std::cerr << "[GR] Invalid 3D landmarks shape" << std::endl;
    return false;
  }

  if (landmarks2d.dimensions.front() != landmarks3d.dimensions.front()) {
    std::cerr << "[GR] Batch mismatch between 2D and 3D landmarks" << std::endl;
    return false;
  }

  if (output.tensors.size() < 3) {
    std::cerr << "[GR] Output tensor count mismatch: expected at least 3, got "
              << output.tensors.size() << std::endl;
    return false;
  }

  // Tensor 0: 2D landmarks [B, 63] -> [B, 21, 3]
  auto& out_landmarks2d = output.tensors[0];
  out_landmarks2d.type = landmarks2d.type;
  out_landmarks2d.dimensions = landmarks2d.dimensions;
  if (landmarks2d.dimensions.size() == 2 && landmarks2d.dimensions.back() == 63) {
    const uint32_t batch = landmarks2d.dimensions.front();
    out_landmarks2d.dimensions = {batch, 21, 3};
  }
  if (out_landmarks2d.size != landmarks2d.size || out_landmarks2d.data == nullptr) {
    std::cerr << "[GR] Output tensor[0] size mismatch: expected "
              << landmarks2d.size << ", got " << out_landmarks2d.size
              << std::endl;
    return false;
  }
  if (landmarks2d.size > 0 && landmarks2d.data != nullptr &&
      out_landmarks2d.data != landmarks2d.data) {
    std::memcpy(out_landmarks2d.data, landmarks2d.data, landmarks2d.size);
  }

  // Tensor 1: handedness (shape/type preserved)
  auto& out_handedness = output.tensors[1];
  out_handedness.type = handedness.type;
  out_handedness.dimensions = handedness.dimensions;
  if (out_handedness.size != handedness.size || out_handedness.data == nullptr) {
    std::cerr << "[GR] Output tensor[1] size mismatch: expected "
              << handedness.size << ", got " << out_handedness.size
              << std::endl;
    return false;
  }
  if (handedness.size > 0 && handedness.data != nullptr &&
      out_handedness.data != handedness.data) {
    std::memcpy(out_handedness.data, handedness.data, handedness.size);
  }

  // Tensor 2: 3D landmarks [B, 63] -> [B, 21, 3]
  auto& out_landmarks3d = output.tensors[2];
  out_landmarks3d.type = landmarks3d.type;
  out_landmarks3d.dimensions = landmarks3d.dimensions;
  if (landmarks3d.dimensions.size() == 2 && landmarks3d.dimensions.back() == 63) {
    const uint32_t batch = landmarks3d.dimensions.front();
    out_landmarks3d.dimensions = {batch, 21, 3};
  }
  if (out_landmarks3d.size != landmarks3d.size || out_landmarks3d.data == nullptr) {
    std::cerr << "[GR] Output tensor[2] size mismatch: expected "
              << landmarks3d.size << ", got " << out_landmarks3d.size
              << std::endl;
    return false;
  }
  if (landmarks3d.size > 0 && landmarks3d.data != nullptr &&
      out_landmarks3d.data != landmarks3d.data) {
    std::memcpy(out_landmarks3d.data, landmarks3d.data, landmarks3d.size);
  }

  return true;
}

bool decode_image_classification(const MLFrame& frame,
                                      const std::vector<LabelEntry>& labels,
                                      MLClassifications& classifications,
                                      float confidence_threshold) {
  classifications.clear();

  double confidence = 0.0;

  uint32_t n_inferences = frame.tensors[0].dimensions[1];

  const float *data = static_cast<const float*>(frame.tensors[0].data);

  // Fill the prediction table.
  for (uint32_t idx = 0; idx < n_inferences; ++idx) {
    confidence = data[idx];

    // Discard results with confidence below the set threshold.
    if (confidence < confidence_threshold)
      continue;

    MLClassification entry;
    entry.confidence = confidence;

    if (idx < labels.size() && !labels[idx].name.empty()) {
      entry.name = labels[idx].name;
      entry.color = labels[idx].color;
    }

    classifications.emplace_back(std::move(entry));
  }

  return true;
}

static const std::string home_path =
    std::getenv("HOME") ? std::getenv("HOME") : "";

//  Example pipeline:
//
//    source → [vf] → t_split_1
//      ├─→ metamux_1 → q22 → metatransform → t_split_2
//      └─→ q5 → stage_01_preproc → q6 → stage_01_inference → q7 → stage_01_postproc → [tf1] → metamux_1
//
//    t_split_2
//      ├─→ metamux_2 → q23 → overlay → display
//      └─→ q14 → stage_02_preproc → q15 → stage_02_inference → t_split_4
//            ├─→ q17 → stage_02_1_postproc → [tf2] → metamux_2
//            └─→ q18 → stage_02_2_postproc → q19 → stage_03_1_inference → q20 → stage_03_2_inference → q21 → stage_03_postproc → [tf3] → metamux_2
//
//  The pipeline reads camera frames, runs ML inference and postprocessing,
//  and displays the result through Wayland.

void create_and_execute_pipeline() {

  // Converts raw video frames into model input tensor format.
  Element stage_01_preproc("qtimlvconverter", "stage_01_preproc");

  // Executes the ML model and attaches tensor outputs to each frame.
  Element stage_01_inference("qtimltflite", "stage_01_inference");
  stage_01_inference.set("delegate", "gpu");
  stage_01_inference.set("model", home_path + "/Downloads/qimsdk_samples/models/palm_detection_full.tflite");

  // Converts raw video frames into model input tensor format.
  Element stage_02_preproc("qtimlvconverter", "stage_02_preproc");
  stage_02_preproc.set("mode", "roi-batch-cumulative");

  // Executes the ML model and attaches tensor outputs to each frame.
  Element stage_02_inference("qtimltflite", "stage_02_inference");
  stage_02_inference.set("delegate", "xnnpack");
  stage_02_inference.set("model", home_path + "/Downloads/qimsdk_samples/models/hand_landmark_full.tflite");

  // Executes the ML model and attaches tensor outputs to each frame.
  Element stage_03_1_inference("qtimltflite", "stage_03_1_inference");
  stage_03_1_inference.set("delegate", "gpu");
  stage_03_1_inference.set("model", home_path + "/Downloads/qimsdk_samples/models/gesture_embedder.tflite");

  // Executes the ML model and attaches tensor outputs to each frame.
  Element stage_03_2_inference("qtimltflite", "stage_03_2_inference");
  stage_03_2_inference.set("delegate", "gpu");
  stage_03_2_inference.set("model", home_path + "/Downloads/qimsdk_samples/models/canned_gesture_classifier.tflite");

  // Captures frames from the camera source.
  Element source("qtiqmmfsrc", "source");

  // Splits decoded frames into display and ML branches.
  Element t_split_1("tee", "t_split_1");

  // Merges metadata produced by the ML branch with original video frames.
  Element metamux_1("qtimetamux", "metamux_1");

  // Queues data between pipeline stages.
  Element q22("queue", "q22");

  // Transforms metadata for downstream processing stages.
  Element metatransform("qtimetatransform", "metatransform");
  metatransform.set("module", "roi-palmd");

  // Splits decoded frames into display and ML branches.
  Element t_split_2("tee", "t_split_2");

  // Queues data between pipeline stages.
  Element q5("queue", "q5");

  // Queues data between pipeline stages.
  Element q6("queue", "q6");

  // Queues data between pipeline stages.
  Element q7("queue", "q7");

  // Merges metadata produced by the ML branch with original video frames.
  Element metamux_2("qtimetamux", "metamux_2");

  // Queues data between pipeline stages.
  Element q23("queue", "q23");

  // Renders ML metadata over the video frame.
  Element overlay("qtivoverlay", "overlay");

  // Render video stream on display.
  //
  // async=false enforce state transition to ensure the buffers are returned on time.
  // sync=false disables strict rendering synchronization to the pipeline clock.
  // fullscreen=true renders the output fullscreen on the target display.
  Element display("waylandsink", "display");
  display.set("sync", false);
  display.set("fullscreen", true);

  // Queues data between pipeline stages.
  Element q14("queue", "q14");

  // Queues data between pipeline stages.
  Element q15("queue", "q15");

  // Splits decoded frames into display and ML branches.
  Element t_split_4("tee", "t_split_4");

  // Queues data between pipeline stages.
  Element q17("queue", "q17");

  // Queues data between pipeline stages.
  Element q18("queue", "q18");

  // Queues data between pipeline stages.
  Element q19("queue", "q19");

  // Queues data between pipeline stages.
  Element q20("queue", "q20");

  // Queues data between pipeline stages.
  Element q21("queue", "q21");

  static const std::vector<LabelEntry> palmd_labels =
      load_labels(home_path + "/Downloads/qimsdk_samples/labels/palmd_labels.json");
  static const std::vector<LabelEntry> hlandmarks_labels =
      load_labels(home_path + "/Downloads/qimsdk_samples/labels/hlandmarks.json");
  static const std::vector<LabelEntry> gesture_rec_labels =
      load_labels(home_path + "/Downloads/qimsdk_samples/labels/gesture_rec.json");

  // ML postprocessing element.
  MLPostprocess stage_01_postproc("stage_01_postproc");
  // ML postprocessing element.
  MLPostprocess stage_02_1_postproc("stage_02_1_postproc");
  // ML postprocessing element.
  MLPostprocess stage_02_2_postproc("stage_02_2_postproc");
  // ML postprocessing element.
  MLPostprocess stage_03_postproc("stage_03_postproc");

  // ML postprocessing lambda function implementation.
  stage_01_postproc.set_handler(
      [](const MLFrame& frame, const MLParam& params,
         MLDetections& detections) {
        return decode_object_detection(
            frame, params, palmd_labels, detections,
            /*confidence_threshold=*/0.7f);
      });

  // ML postprocessing lambda function implementation.
  stage_02_1_postproc.set_handler(
      [](const MLFrame& frame, const MLParam& params,
         MLPoses& poses) {
        return decode_pose_estimation(
            frame, hlandmarks_labels, params, poses,
            /*confidence_threshold=*/0.70f);
      });

  // ML postprocessing lambda function implementation.
  stage_02_2_postproc.set_handler(
      [](const MLFrame& frame, const MLParam& params, MLFrame& output) {
        return decode_tensor(
            frame, params, output);
      });

  // ML postprocessing lambda function implementation.
  stage_03_postproc.set_handler(
      [](const MLFrame& frame, const MLParam& params,
         MLClassifications& classifications) {
        return decode_image_classification(
            frame, gesture_rec_labels, classifications,
            /*confidence_threshold=*/0.70f);
      });

  stage_01_postproc.set("results", 2);
  stage_02_1_postproc.set("results", 6);
  stage_02_2_postproc.set("results", 6);
  stage_03_postproc.set("results", 8);

  // Stream filters used in branch links.
  // They define specific stream characteristics from the supported options.
  auto vf = VideoFilter().resolution(1920, 1080).framerate(30, 1).format("NV12");
  auto tf1 = TextFilter();
  auto tf2 = TextFilter();
  auto tf3 = TextFilter();

  // Creates the pipeline, adds and links elements, and executes it.
  //
  // Explicit linking is applied
  Pipeline pipeline("ml-external-classification");

  pipeline.add(stage_01_preproc)
          .add(stage_01_inference)
          .add(stage_01_postproc)
          .add(stage_02_preproc)
          .add(stage_02_inference)
          .add(stage_02_1_postproc)
          .add(stage_02_2_postproc)
          .add(stage_03_1_inference)
          .add(stage_03_2_inference)
          .add(stage_03_postproc)
          .add(source)
          .add_stream_filter("vf", vf)
          .add(t_split_1)
          .add(metamux_1)
          .add(q22)
          .add(metatransform)
          .add(t_split_2)
          .add(q5)
          .add(q6)
          .add(q7)
          .add_stream_filter("tf1", tf1)
          .add(metamux_2)
          .add(q23)
          .add(overlay)
          .add(display)
          .add(q14)
          .add(q15)
          .add(t_split_4)
          .add(q17)
          .add_stream_filter("tf2", tf2)
          .add(q18)
          .add(q19)
          .add(q20)
          .add(q21)
          .add_stream_filter("tf3", tf3)

          .link("source", "vf", "t_split_1")
          .link("t_split_1", "metamux_1", "q22", "metatransform", "t_split_2")
          .link("t_split_1", "q5", "stage_01_preproc", "q6", "stage_01_inference", "q7", "stage_01_postproc", "tf1", "metamux_1")
          .link("t_split_2", "metamux_2", "q23", "overlay", "display")
          .link("t_split_2", "q14", "stage_02_preproc", "q15", "stage_02_inference", "t_split_4")
          .link("t_split_4", "q17", "stage_02_1_postproc", "tf2", "metamux_2")
          .link("t_split_4", "q18", "stage_02_2_postproc", "q19", "stage_03_1_inference", "q20", "stage_03_2_inference", "q21", "stage_03_postproc", "tf3", "metamux_2");

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
