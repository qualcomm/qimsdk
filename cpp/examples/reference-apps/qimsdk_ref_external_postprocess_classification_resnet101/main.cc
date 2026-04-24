/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include <qti/qimsdk.h>

#include "qimsdk-test-utils.h"

using namespace qti;

bool decode_top1_classification(const MLFrame& frame,
                                const MLParam& params,
                                const std::vector<LabelEntry>& labels,
                                MLClassifications& classifications,
                                float confidence_threshold,
                                const char* log_tag) {
  classifications.clear();

  if (frame.tensors.empty()) {
    std::cout << "[external-postprocess][classification]";
    if (log_tag && *log_tag) {
      std::cout << "[" << log_tag << "]";
    }
    std::cout << " called: "
              << "tensors=" << frame.tensors.size()
              << ", params=" << params.fields.size()
              << ", classifications(out)=" << classifications.size()
              << ", decode_ok=false"
              << std::endl;
    return false;
  }

  const MLTensor& tensor = frame.tensors[0];
  uint32_t n_inferences = tensor.dimensions[1];
  const float *data = static_cast<const float*>(tensor.data);

  // Calculate the sum of the exponents for softmax function.
  double sum = 0.0;
  for (uint32_t idx = 0; idx < n_inferences; ++idx) {
    sum += std::exp(data[idx]);
  }

  // Fill the prediction table.
  for (uint32_t idx = 0; idx < n_inferences; ++idx) {
    double confidence = (std::exp(data[idx]) / sum) * 100.0;

     // Discard results with confidence below the set threshold.
    if (confidence < confidence_threshold) {
      continue;
    }

    MLClassification cls;
    cls.confidence = confidence;

    if (idx < labels.size() && !labels[idx].name.empty()) {
      cls.name = labels[idx].name;
      cls.color = labels[idx].color;
    }

    classifications.push_back(std::move(cls));
  }

  const bool ok = true;

  std::cout << "[external-postprocess][classification]";
  if (log_tag && *log_tag) {
    std::cout << "[" << log_tag << "]";
  }
  std::cout << " called: "
            << "tensors=" << frame.tensors.size()
            << ", params=" << params.fields.size()
            << ", classifications(out)=" << classifications.size()
            << ", decode_ok=" << (ok ? "true" : "false");
  if (!classifications.empty()) {
    std::cout << ", top1='" << classifications.front().name
              << "'@" << classifications.front().confidence;
  }
  std::cout << std::endl;

  return ok;
}

static const std::string home_path =
    std::getenv("HOME") ? std::getenv("HOME") : "";

void create_and_execute_pipeline() {
  Pipeline pipeline("ml-external-classification");

  // Read Labels used by the external postprocess callback for class decoding.
  static const std::vector<LabelEntry> labels =
      load_labels(home_path + "/Downloads/qimsdk_samples/labels/resnet101.json");

  MLPostprocess postprocessing("postprocessing");
  postprocessing
      .set("results", 1)
      // ML postprocessing lambda function implementation.
      .set_handler([](const MLFrame& frame, const MLParam& params,
                      MLClassifications& classifications) {
        return decode_top1_classification(
            frame, params, labels, classifications,
            /*confidence_threshold=*/51.0f,
            "");
      });

  pipeline.add("filesrc", "src", "location", home_path + "/Downloads/qimsdk_samples/media/classification_sample.mp4")
          .add("qtdemux", "demux")
          .add("h264parse", "parse")
          .add("v4l2h264dec", "decoder", "capture-io-mode", 4, "output-io-mode", 4)
          .add_stream_filter("vf", VideoFilter().format("NV12"))
          .add("tee", "split")
          .add("queue", "q1")
          .add("qtimlvconverter", "preprocessing")
          .add("queue", "q2")
          .add("qtimltflite", "inferencing",
               "delegate", "external",
               "external-delegate-path", "libQnnTFLiteDelegate.so",
               "external-delegate-options", "QNNExternalDelegate,backend_type=htp;",
               "model", home_path + "/Downloads/qimsdk_samples/models/Resnet101_Quantized.tflite")
          .add("queue", "q3")
          .add(postprocessing)
          .add_stream_filter("mlf", TextFilter())
          .add("qtimetamux", "mlmuxer")
          .add("queue", "q4")
          .add("qtivoverlay", "overlay")
          .add("waylandsink", "display", "fullscreen", true)
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
