/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#pragma once

#include "qti/qimsdk-postprocess-base.h"

#include <string>
#include <utility>

namespace qti {

class Pipeline;

class MLPostprocess : public MLPostprocessBase {
 public:
  // Create a generic ML postprocess element with an optional name.
  explicit MLPostprocess(const std::string& name = {});
  // Release owned implementation state.
  ~MLPostprocess();

  MLPostprocess(const MLPostprocess&) = delete;
  MLPostprocess& operator=(const MLPostprocess&) = delete;

  MLPostprocess(MLPostprocess&&) noexcept;
  MLPostprocess& operator=(MLPostprocess&&) noexcept;

  // Set one or more GStreamer properties on this element.
  template <typename Value, typename... Rest>
  MLPostprocess& set(const char* prop, Value&& value, Rest&&... rest) {
    Element::set(prop, std::forward<Value>(value), std::forward<Rest>(rest)...);
    return *this;
  }

  // Set one or more GStreamer properties on this element.
  template <typename Value, typename... Rest>
  MLPostprocess& set(const std::string& prop, Value&& value, Rest&&... rest) {
    Element::set(prop, std::forward<Value>(value), std::forward<Rest>(rest)...);
    return *this;
  }

  // Register external postprocess callback.
  template <typename Callback>
  MLPostprocess& set_handler(Callback&& handler) {
    MLPostprocessBase::set_handler(std::forward<Callback>(handler));
    return *this;
  }

 private:
  explicit MLPostprocess(void* existing_gst_elem);
  friend class qti::Pipeline;
};

}  // namespace qti
