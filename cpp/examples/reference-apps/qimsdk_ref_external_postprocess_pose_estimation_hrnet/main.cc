/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <algorithm>
#include <cfloat>
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

struct KeypointLinkIds {
  uint32_t s_kp_id;
  uint32_t d_kp_id;

  KeypointLinkIds()
      : s_kp_id(0), d_kp_id(0) {};

  KeypointLinkIds(uint32_t s_kp_id, uint32_t d_kp_id)
      : s_kp_id(s_kp_id), d_kp_id(d_kp_id) {};
};

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

bool decode_pose_estimation(const MLFrame& frame,
                            MLPoses& estimations,
                            const MLParam& mlparams,
                            const std::vector<LabelEntry>& labels,
                            float confidence_threshold)
{
    estimations.clear();

    if (frame.tensors.empty()) {
        std::cerr << "No tensors in frame!" << std::endl;
        return false;
    }

    const auto& tensor = frame.tensors[0];

    if (tensor.dimensions.size() != 4 || tensor.dimensions[0] != 1) {
        std::cerr << "Unexpected tensor shape!" << std::endl;
        return false;
    }

    // Layout: [1, H, W, K]
    uint32_t H = tensor.dimensions[1];
    uint32_t W = tensor.dimensions[2];
    uint32_t K = tensor.dimensions[3];

    const float* heatmap = reinterpret_cast<const float*>(tensor.data);

    Region region;
    mlparams.get("input-region-x", region.x);
    mlparams.get("input-region-y", region.y);
    mlparams.get("input-region-width", region.width);
    mlparams.get("input-region-height", region.height);

    float tensor_w = 0, tensor_h = 0;
    mlparams.get("input-tensor-width", tensor_w);
    mlparams.get("input-tensor-height", tensor_h);

    // Prepare output pose
    MLPose pose;
    pose.keypoints.resize(K);
    pose.confidence = 0.0f;

    // Decode each keypoint
    for (uint32_t k = 0; k < K; ++k) {
        float best_val = -FLT_MAX;
        uint32_t best_x = 0;
        uint32_t best_y = 0;

        for (uint32_t y = 0; y < H; ++y) {
            for (uint32_t x = 0; x < W; ++x) {
                uint32_t idx = (y * W * K) + (x * K) + k; // NHWC layout

                float val = heatmap[idx];

                if (val > best_val) {
                    best_val = val;
                    best_x = x;
                    best_y = y;
                }
            }
        }

        float x = static_cast<float>(best_x);
        float y = static_cast<float>(best_y);

        if (best_x > 0 && best_x < (W - 1) &&
            best_y > 0 && best_y < (H - 1)) {

            uint32_t left  = (best_y * W * K) + ((best_x - 1) * K) + k;
            uint32_t right = (best_y * W * K) + ((best_x + 1) * K) + k;
            uint32_t up    = ((best_y - 1) * W * K) + (best_x * K) + k;
            uint32_t down  = ((best_y + 1) * W * K) + (best_x * K) + k;

            float dx = heatmap[right] - heatmap[left];
            float dy = heatmap[down]  - heatmap[up];

            x += (dx > 0 ? 0.25f : -0.25f);
            y += (dy > 0 ? 0.25f : -0.25f);
        }

        float px = x * (tensor_w / static_cast<float>(W));
        float py = y * (tensor_h / static_cast<float>(H));

        float nx = (px - region.x) / region.width;
        float ny = (py - region.y) / region.height;

        nx = std::clamp(nx, 0.0f, 1.0f);
        ny = std::clamp(ny, 0.0f, 1.0f);

        MLKeypoint& kp = pose.keypoints[k];

        kp.x = nx;
        kp.y = ny;
        kp.confidence = best_val;

        if (k < labels.size()) {
            kp.name = labels[k].name;
            kp.color = labels[k].color;
        }

        pose.confidence += kp.confidence;
    }

    pose.confidence /= static_cast<float>(K);

    if (pose.confidence < confidence_threshold) {
        return true;
    }

    static const std::vector<std::pair<uint32_t, uint32_t>> edges = {
        {0,1}, {0,2}, {0,5}, {0,6},
        {1,3}, {2,4},
        {5,6}, {5,7}, {5,11},
        {6,8}, {6,12},
        {7,9}, {8,10},
        {11,13}, {12,14},
        {13,15}, {14,16}
    };

    for (auto& e : edges) {
        if (e.first >= K || e.second >= K)
            continue;

        const MLKeypoint& a = pose.keypoints[e.first];
        const MLKeypoint& b = pose.keypoints[e.second];

        if (a.confidence >= confidence_threshold &&
            b.confidence >= confidence_threshold) {

            pose.links.push_back(MLKeypointLink{a, b});
        }
    }

    estimations.emplace_back(std::move(pose));

    std::cout << "[external-postprocess][pose] decoded "
              << estimations.size() << " pose(s)" << std::endl;

    return true;
}

static const std::string home_path =
    std::getenv("HOME") ? std::getenv("HOME") : "";

//  Example pipeline:
//
//    src → demux → parse → decoder → [vf] → split
//         ├──→ mlmuxer (passthrough display branch)
//         └──→ q1 → preprocessing → q2 → inferencing (YOLOv5) → det_postprocessing → [mlf] → mlmuxer → split2
//               ├──→ mlmuxer2 (passthrough)
//               └──→ q3 → preprocessing2 (ROI) → q4 → inferencing2 (HRNet) → pose_postprocessing → [mlf2] → mlmuxer2
//    mlmuxer2 → overlay → display
//
//  The pipeline reads an MP4/H.264 file, decodes it using the hardware decoder,
//  performs object detection (YOLOv5) followed by ROI-based pose estimation (HRNet),
//  merges metadata from both inference stages back into the video stream,
//  overlays the results, and renders the output via Wayland.

void create_and_execute_pipeline() {

  // Reads the input media file as raw bytes.
  Element src("filesrc", "src");
  src.set("location", home_path + "/Downloads/qimsdk_samples/media/pose_sample.mp4");

  // Extracts elementary streams from MP4 container.
  Element demux("qtdemux", "demux");

  // Parses H.264 bitstream into a format suitable for decoding.
  Element parse("h264parse", "parse");

  // Decodes the compressed H.264 stream into raw video frames.
  //
  // The I/O mode is configured to enforce DMA buffer usage,
  // avoiding unnecessary buffer copies.
  Element decoder("v4l2h264dec", "decoder");
  decoder.set("output-io-mode", 4);
  decoder.set("capture-io-mode", 4);

  // Splits stream into display and ML branches.
  Element split("tee", "split");

  // Queue buffers for detection branch.
  Element q1("queue", "q1");

  // Converts frames into tensor format for detection model.
  Element preprocessing("qtimlvconverter", "preprocessing");

  // Queue before preprocessing.
  Element q5("queue", "q5");

  // Queue before inference.
  Element q2("queue", "q2");

  // Executes YOLOv5 object detection model.
  Element inferencing("qtimltflite", "inferencing");
  inferencing.set("delegate", "external");
  inferencing.set("external-delegate-path", "libQnnTFLiteDelegate.so");
  inferencing.set("external-delegate-options", "QNNExternalDelegate,backend_type=htp;");
  inferencing.set("model", home_path + "/Downloads/qimsdk_samples/models/yolov5m-320x320-int8.tflite");

  // Metadata muxer (first stage - detection results).
  Element mlmuxer("qtimetamux", "mlmuxer");

  // Queue buffers after metadata muxer.
  Element q7("queue", "q7");

  // Splits stream again for pose estimation.
  Element split2("tee", "split2");

  // Queue buffers for pose branch.
  Element q3("queue", "q3");

  // ROI-based preprocessing based on detection results.
  Element preprocessing2("qtimlvconverter", "preprocessing2");
  preprocessing2.set("mode", "roi-batch-cumulative");
  preprocessing2.set("image_disposition", "centre");

  // Queue before pose preprocessing.
  Element q6("queue", "q6");

  // Queue before pose inference.
  Element q4("queue", "q4");

  // Executes HRNet pose estimation model.
  Element inferencing2("qtimltflite", "inferencing2");
  inferencing2.set("delegate", "external");
  inferencing2.set("external-delegate-path", "libQnnTFLiteDelegate.so");
  inferencing2.set("external-delegate-options", "QNNExternalDelegate,backend_type=htp;");
  inferencing2.set("model", home_path + "/Downloads/qimsdk_samples/models/hrnet_pose_w8a8.tflite");

  // Metadata muxer (second stage - pose results).
  Element mlmuxer2("qtimetamux", "mlmuxer2");

  // Queue buffers after metadata muxer.
  Element q8("queue", "q8");

  // Overlay ML metadata (detections + poses) on frames.
  Element overlay("qtivoverlay", "overlay");

  // Displays frames using Wayland.
  //
  // async=false enforces state transition to ensure buffers are returned on time.
  // sync=false disables strict clock sync for lower latency.
  Element display("waylandsink", "display");
  display.set("fullscreen", true);


  static const std::vector<LabelEntry> labels_yolov5m =
      load_labels(home_path + "/Downloads/qimsdk_samples/labels/yolov5m.json");

  static const std::vector<LabelEntry> labels_hrnet =
      load_labels(home_path + "/Downloads/qimsdk_samples/labels/hrnet.json");

  // Detection postprocessing.
  MLPostprocess det_postprocessing("det_postprocessing");
  det_postprocessing.set_handler(
      [&](const MLFrame& frame, const MLParam& params,
          MLDetections& detections) {

        const bool ok = decode_detection(frame, detections, params,
                                         labels_yolov5m, 0.70f);

        std::cout << "[external-postprocess][detection] tensors="
                  << frame.tensors.size()
                  << ", detections=" << detections.size()
                  << ", ok=" << (ok ? "true" : "false") << std::endl;

        return true;
      });

  // Pose estimation postprocessing.
  MLPostprocess pose_postprocessing("pose_postprocessing");
  pose_postprocessing.set("results", 2);
  pose_postprocessing.set_handler(
      [&](const MLFrame& frame, const MLParam& params,
          MLPoses& poses) {

        const bool ok = decode_pose_estimation(frame, poses, params,
                                               labels_hrnet, 0.70f);

        std::cout << "[external-postprocess][pose] tensors="
                  << frame.tensors.size()
                  << ", poses=" << poses.size()
                  << ", ok=" << (ok ? "true" : "false") << std::endl;

        return true;
      });

  auto vf = VideoFilter().format("NV12");
  auto mlf = TextFilter();
  auto mlf2 = TextFilter();

  Pipeline pipeline("hrnet_pose_estimation");

  pipeline.add(src)
          .add(demux)
          .add(parse)
          .add(decoder)
          .add_stream_filter("vf", vf)
          .add(split)

          // Detection branch
          .add(q1)
          .add(preprocessing)
          .add(q2)
          .add(inferencing)
          .add(q5)
          .add(det_postprocessing)
          .add_stream_filter("mlf", mlf)
          .add(mlmuxer)
          .add(q7)
          .add(split2)

          // Pose branch
          .add(q3)
          .add(preprocessing2)
          .add(q4)
          .add(inferencing2)
          .add(q6)
          .add(pose_postprocessing)
          .add_stream_filter("mlf2", mlf2)
          .add(mlmuxer2)
          .add(q8)

          // Output
          .add(overlay)
          .add(display)

          // Main pipeline
          .link("src", "demux", "parse", "decoder", "vf", "split")

          // Main branch
          .link("split", "mlmuxer")

          // Detection branch
          .link("split", "q1", "preprocessing", "q2",
                "inferencing", "q5", "det_postprocessing",
                "mlf", "mlmuxer", "q7", "split2")

          // Main branch
          .link("split2", "mlmuxer2")

          // Pose branch
          .link("split2", "q3", "preprocessing2", "q4",
                "inferencing2", "q6", "pose_postprocessing",
                "mlf2", "mlmuxer2")

          // Main branch
          .link("mlmuxer2", "q8", "overlay", "display");

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
  } catch (const std::exception &ex) {
    std::cerr << "Exception: " << ex.what() << std::endl;
    return 1;
  }

  return 0;
}
