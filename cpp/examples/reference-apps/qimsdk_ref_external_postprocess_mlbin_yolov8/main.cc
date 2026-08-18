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
#include <getopt.h>

using namespace qti;

static const std::string home_path =
    std::getenv("HOME") ? std::getenv("HOME") : "";

// Base path for sample assets (media/, models/, labels/ live under it).
// Set from the --model-base-path argument, or the default location.
static std::string model_base_path;

// Input source location, overridden via the --input-config argument.
static std::string input_config;

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
      load_labels(model_base_path + "/labels/yolov8.json");

  MLVideoTFLiteBin mlbin("mlbin");
  mlbin
      .set("inference-delegate", "external",
           "inference-external-delegate-path", "libQnnTFLiteDelegate.so",
           "inference-external-delegate-options",
           "QNNExternalDelegate,backend_type=htp;",
           "inference-model", model_base_path + "/models/yolov8_det_quantized.tflite")
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
      .add("filesrc", "src", "location", input_config)
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

int main(int argc, char **argv) {
  if (home_path.empty()) {
    std::cerr << "Error: HOME environment variable is not set." << std::endl;
    return 1;
  }

  // Base path for sample assets; override via --model-base-path argument.
  model_base_path = home_path + "/Downloads/qimsdk_samples";
  // Path for sample input assets; override via --input-config argument.
  input_config = home_path + "/Downloads/qimsdk_samples/media/ai_demo_sample.mp4";

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
