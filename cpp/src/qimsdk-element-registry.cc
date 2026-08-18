/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <qti/qimsdk-appsrc.h>
#include <qti/qimsdk-appsink.h>
#include <qti/qimsdk-camsrc.h>
#include <qti/qimsdk-mlvconverter.h>
#include <qti/qimsdk-mlpostprocess.h>
#include <qti/qimsdk-mlvideotflitebin.h>
#include <qti/qimsdk-mlvideoqnnbin.h>
#include <qti/qimsdk-mlvideosnpebin.h>
#include <qti/qimsdk-mlvideoonnxbin.h>
#include "qimsdk-element-registry.h"

namespace qti {

ElementRegistry& ElementRegistry::Instance() {
  static ElementRegistry inst;
  return inst;
}

ElementRegistry::ElementRegistry() {
  creators_.emplace(
    "appsrc",
    [](const std::string& /*factory*/, const std::string& name) {
      return std::make_unique<AppSrc>(name);
    });

  creators_.emplace(
    "appsink",
    [](const std::string& /*factory*/, const std::string& name) {
      return std::make_unique<AppSink>(name);
    });

  creators_.emplace(
    "camsrc",
    [](const std::string& /*factory*/, const std::string& name) {
      return std::make_unique<CamSrc>(name);
    });

  creators_.emplace(
    "qtimlvconverter",
    [](const std::string& /*factory*/, const std::string& name) {
      return std::make_unique<MLVConverter>(name);
    });

  creators_.emplace(
    "qtimlpostprocess",
    [](const std::string& /*factory*/, const std::string& name) {
      return std::make_unique<MLPostprocess>(name);
    });

  creators_.emplace(
    "qtimlvideotflitebin",
    [](const std::string& /*factory*/, const std::string& name) {
      return std::make_unique<MLVideoTFLiteBin>(name);
    });

  creators_.emplace(
    "qtimlvideoqnnbin",
    [](const std::string& /*factory*/, const std::string& name) {
      return std::make_unique<MLVideoQNNBin>(name);
    });

  creators_.emplace(
    "qtimlvideosnpebin",
    [](const std::string& /*factory*/, const std::string& name) {
      return std::make_unique<MLVideoSNPEBin>(name);
    });

  creators_.emplace(
    "qtimlvideoonnxbin",
    [](const std::string& /*factory*/, const std::string& name) {
      return std::make_unique<MLVideoONNXBin>(name);
    });
}

void ElementRegistry::Register(const std::string& factory, Creator creator) {
  std::lock_guard<std::mutex> lk(mtx_);
  creators_[factory] = std::move(creator);
}

std::unique_ptr<Element> ElementRegistry::Create(const std::string& factory,
  const std::string& name) const {
  std::lock_guard<std::mutex> lk(mtx_);
  auto it = creators_.find(factory);
  if (it != creators_.end()) {
    return (it->second)(factory, name);
  }
  // Fallback to generic Element
  return std::make_unique<Element>(factory, name);
}

}  // namespace qti
