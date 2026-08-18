/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#pragma once

#include "qti/qimsdk-logging.h"

#include <gst/gst.h>

#include <ostream>
#include <sstream>
#include <string>
#include <vector>

namespace qti {

enum class GstLogLevel {
  None = 0,
  Error = 1,
  Warning = 2,
  Fixme = 3,
  Info = 4,
  Debug = 5,
  Log = 6,
  Trace = 7,
  Memdump = 9,
};

ImsdkLogLevel GetImsdkLogLevel();
void SetImsdkLogLevel(ImsdkLogLevel level);

ImsdkGstLogMode GetImsdkGstLogMode();
void SetImsdkGstLogMode(ImsdkGstLogMode mode);

bool ShouldLog(ImsdkLogLevel level);
void EmitImsdkLog(ImsdkLogLevel level, const std::string& message);
struct PendingLinkInfo {
  std::string upstream_name;
  std::string downstream_name;
  std::string src_pad_template;
  std::string sink_pad_template;
  bool completed = false;
};

void LogUnlinkedPadsCaps(GstElement* pipeline);
void PrintPipelineTopology(
    GstElement* pipeline,
    const std::vector<PendingLinkInfo>* pending_links = nullptr);
void GeneratePipelineGraph(
    GstElement* pipeline,
    const std::string& filename,
    const std::vector<PendingLinkInfo>* pending_links = nullptr);

class ImsdkLogger {
 public:
  static void ConfigureGstParserCategoryLogLevels();

  static bool ParseAndPrintGstLog(::GstDebugLevel level,
                                  GstDebugCategory* category,
                                  GObject* object,
                                  GstDebugMessage* message);
};

class ImsdkLogMessage {
 public:
  explicit ImsdkLogMessage(ImsdkLogLevel level) : level_(level) {}

  ~ImsdkLogMessage();

  std::ostream& stream();

 private:
  ImsdkLogLevel level_;
  std::ostringstream stream_;
};

}  // namespace qti

#define QIMSDK_LOG_STREAM(level)                                                \
  for (bool _qimsdk_log_enabled = ::qti::ShouldLog(level);                     \
       _qimsdk_log_enabled;                                                     \
       _qimsdk_log_enabled = false)                                             \
    ::qti::ImsdkLogMessage(level).stream()

#define QIMSDK_LOG_ERROR QIMSDK_LOG_STREAM(::qti::ImsdkLogLevel::Error)
#define QIMSDK_LOG_WARNING QIMSDK_LOG_STREAM(::qti::ImsdkLogLevel::Warning)
#define QIMSDK_LOG_INFO QIMSDK_LOG_STREAM(::qti::ImsdkLogLevel::Info)
#define QIMSDK_LOG_DEBUG QIMSDK_LOG_STREAM(::qti::ImsdkLogLevel::Debug)
