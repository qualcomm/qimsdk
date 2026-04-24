/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#pragma once

#include "qti/qimsdk-buffer.h"
#include "qti/qimsdk-element.h"

#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace qti {
class Pipeline;

class AppSink : public Element {
 public:
  // Create an appsink element with an optional instance name.
  explicit AppSink(const std::string& name = {});
  // Release signal handlers and owned implementation state.
  ~AppSink();

  AppSink(const AppSink&) = delete;
  AppSink& operator=(const AppSink&) = delete;

  AppSink(AppSink&&) noexcept;
  AppSink& operator=(AppSink&&) noexcept;

  // Set one or more GStreamer properties on this element.
  template <typename Value, typename... Rest>
  AppSink& set(const char* prop, Value&& value, Rest&&... rest) {
    Element::set(prop, std::forward<Value>(value), std::forward<Rest>(rest)...);
    return *this;
  }

  // Set one or more GStreamer properties on this element.
  template <typename Value, typename... Rest>
  AppSink& set(const std::string& prop, Value&& value, Rest&&... rest) {
    Element::set(prop, std::forward<Value>(value), std::forward<Rest>(rest)...);
    return *this;
  }

  // Register callback invoked for each new sample pulled from appsink.
  AppSink& set_buffer_consumer(std::function<void(qti::Buffer)> consumer);

  // Register callback invoked for each new preroll sample.
  AppSink& set_preroll_handler(std::function<bool(qti::Buffer&&)> preroll);

  // Register callback invoked when EOS is received on appsink.
  AppSink& set_eos_handler(std::function<void()> eos);

 private:
  explicit AppSink(void* existing_gst_elem);
  friend class qti::Pipeline;

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace qti
