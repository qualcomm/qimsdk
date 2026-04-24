/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <iostream>

#include <qti/qimsdk.h>

using namespace qti;

void create_and_execute_pipeline() {
  Pipeline pipeline("cam-pipeline");
  pipeline.add("qtiqmmfsrc", "source")
          .add_stream_filter("videostream", VideoFilter().format("NV12").resolution(1920, 1080).framerate(30))
          .add("waylandsink", "display", "sync", false, "fullscreen", true)
          .execute();
}

int main() {
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
