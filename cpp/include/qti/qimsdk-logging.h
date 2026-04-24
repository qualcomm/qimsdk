/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#pragma once

namespace qti {

// Selects whether logs are emitted through GStreamer or QIMSDK logger.
enum class ImsdkGstLogMode {
  GstLog,
  ImsdkLog,
};

// Log verbosity level for QIMSDK logging.
enum class ImsdkLogLevel {
  Error = 0,
  Warning = 1,
  Info = 2,
  Debug = 3,
};

// Set global QIMSDK log level.
void SetImsdkLogLevel(ImsdkLogLevel level);
// Select global logging backend mode.
void SetImsdkGstLogMode(ImsdkGstLogMode mode);

}  // namespace qti
