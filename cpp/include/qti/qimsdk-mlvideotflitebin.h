/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#pragma once

#include "qti/qimsdk-preprocess-base.h"
#include "qti/qimsdk-postprocess-base.h"

#include <string>
#include <utility>

namespace qti {

class Pipeline;

// TFLite ML video bin with optional external preprocess and postprocess callbacks.
class MLVideoTFLiteBin : public MLPreprocessBase, public MLPostprocessBase {
 public:
  // Create a TFLite ML video bin with an optional instance name.
  explicit MLVideoTFLiteBin(const std::string& name = {});
  // Release owned implementation state.
  ~MLVideoTFLiteBin();

  MLVideoTFLiteBin(const MLVideoTFLiteBin&) = delete;
  MLVideoTFLiteBin& operator=(const MLVideoTFLiteBin&) = delete;

  MLVideoTFLiteBin(MLVideoTFLiteBin&&) noexcept;
  MLVideoTFLiteBin& operator=(MLVideoTFLiteBin&&) noexcept;

  // Set one or more GStreamer properties on this element.
  template <typename Value, typename... Rest>
  MLVideoTFLiteBin& set(const char* prop, Value&& value, Rest&&... rest) {
    Element::set(prop, std::forward<Value>(value), std::forward<Rest>(rest)...);
    return *this;
  }

  // Set one or more GStreamer properties on this element.
  template <typename Value, typename... Rest>
  MLVideoTFLiteBin& set(const std::string& prop, Value&& value,
                        Rest&&... rest) {
    Element::set(prop, std::forward<Value>(value), std::forward<Rest>(rest)...);
    return *this;
  }

  // Register external preprocess callback.
  template <typename Callback>
  MLVideoTFLiteBin& set_preprocess_handler(Callback&& handler) {
    MLPreprocessBase::set_handler(std::forward<Callback>(handler));
    return *this;
  }

  // Register external postprocess callback.
  template <typename Callback>
  MLVideoTFLiteBin& set_postprocess_handler(Callback&& handler) {
    MLPostprocessBase::set_handler(std::forward<Callback>(handler));
    return *this;
  }

 private:
  explicit MLVideoTFLiteBin(void* existing_gst_elem);
  friend class qti::Pipeline;
};

}  // namespace qti
