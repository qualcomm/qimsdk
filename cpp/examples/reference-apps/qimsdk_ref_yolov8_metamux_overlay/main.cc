/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <iostream>
#include <cstdlib>

#include <qti/qimsdk.h>

using namespace qti;

static const std::string home_path =
    std::getenv("HOME") ? std::getenv("HOME") : "";

void create_and_execute_pipeline() {
  Pipeline pipeline("ml-pipeline");
  pipeline.add("filesrc", "src", "location", home_path + "/Downloads/qimsdk_samples/media/ai_demo_sample.mp4")
          .add("qtdemux", "demux")
          .add("h264parse", "parse")
          .add("v4l2h264dec", "decoder", "output-io-mode", 4, "capture-io-mode", 4)
          .add_stream_filter("vf", VideoFilter().format("NV12"))
          .add("tee", "split")
          .add("queue", "q1")
          .add("qtimlvconverter", "preprocessing")
          .add("queue", "q2")
          .add("qtimltflite", "inferencing",
               "delegate", "external",
               "external-delegate-path", "libQnnTFLiteDelegate.so",
               "external-delegate-options", "QNNExternalDelegate,backend_type=htp;",
               "model", home_path + "/Downloads/qimsdk_samples/models/yolov8_det_quantized.tflite")
          .add("queue", "q4")
          .add("qtimlpostprocess", "postprocessing",
                "module", "yolov8",
                "labels", home_path + "/Downloads/qimsdk_samples/labels/yolov8.json")
          .add_stream_filter("mlf", TextFilter())
          .add("qtimetamux", "mlmuxer")
          .add("queue", "q5")
          .add("qtivoverlay", "overlay")
          .add("waylandsink", "display", "fullscreen", true)
          .link("src", "demux", "parse", "decoder", "vf", "split")
          .link("split", "mlmuxer")
          .link("split", "q1", "preprocessing", "q2", "inferencing", "q4", "postprocessing", "mlf", "mlmuxer", "q5", "overlay", "display")
          .execute();
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
