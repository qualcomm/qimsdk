/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#pragma once

#include "qti/qimsdk-preprocess-base.h"

#include <string>
#include <utility>

namespace qti {

class Pipeline;

// ML video converter with optional external preprocess callback.
class MLVConverter : public MLPreprocessBase {
 public:
  // Create an ML video converter with an optional instance name.
  explicit MLVConverter(const std::string& name = {});
  // Release owned implementation state.
  ~MLVConverter();

  MLVConverter(const MLVConverter&) = delete;
  MLVConverter& operator=(const MLVConverter&) = delete;

  MLVConverter(MLVConverter&&) noexcept;
  MLVConverter& operator=(MLVConverter&&) noexcept;

  // Set one or more GStreamer properties on this element.
  template <typename Value, typename... Rest>
  MLVConverter& set(const char* prop, Value&& value, Rest&&... rest) {
    Element::set(prop, std::forward<Value>(value), std::forward<Rest>(rest)...);
    return *this;
  }

  // Set one or more GStreamer properties on this element.
  template <typename Value, typename... Rest>
  MLVConverter& set(const std::string& prop, Value&& value, Rest&&... rest) {
    Element::set(prop, std::forward<Value>(value), std::forward<Rest>(rest)...);
    return *this;
  }

  // Register external preprocess callback.
  template <typename Callback>
  MLVConverter& set_handler(Callback&& handler) {
    MLPreprocessBase::set_handler(std::forward<Callback>(handler));
    return *this;
  }

 private:
  explicit MLVConverter(void* existing_gst_elem);
  friend class qti::Pipeline;
};

}  // namespace qti
