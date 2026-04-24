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

namespace qti {
class Pipeline;

class AppSrc : public Element {
 public:
  // Create an appsrc element with an optional instance name.
  explicit AppSrc(const std::string& name = {});
  // Release signal handlers and owned implementation state.
  ~AppSrc();

  AppSrc(const AppSrc&) = delete;
  AppSrc& operator=(const AppSrc&) = delete;

  AppSrc(AppSrc&&) noexcept;
  AppSrc& operator=(AppSrc&&) noexcept;

  // Appsrc "format" property values.
  enum class Format : uint32_t {
    DEFAULT = 1,
    BYTES = 2,
    TIME = 3,
    BUFFERS = 4,
    PERCENT = 5
   };

  // Set one or more GStreamer properties on this element.
  template <typename Value, typename... Rest>
  AppSrc& set(const char* prop, Value&& value, Rest&&... rest) {
    Element::set(prop, std::forward<Value>(value), std::forward<Rest>(rest)...);
    return *this;
  }

  // Set one or more GStreamer properties on this element.
  template <typename Value, typename... Rest>
  AppSrc& set(const std::string& prop, Value&& value, Rest&&... rest) {
    Element::set(prop, std::forward<Value>(value), std::forward<Rest>(rest)...);
    return *this;
  }

  // Register callback used to produce output buffers when appsrc needs data.
  AppSrc& set_buffer_producer(std::function<bool(qti::Buffer&)> producer);

  // Register callback invoked when appsrc reports enough queued data.
  AppSrc& set_enough_handler(std::function<void()> enough);

  // Push a buffer into appsrc. Returns true when accepted by downstream.
  bool push_buffer(qti::Buffer& buffer);
  // Push a movable buffer into appsrc. Returns true when accepted.
  bool push_buffer(qti::Buffer&& buffer);

  // Signal end-of-stream to appsrc.
  void end_of_stream();

 private:
  explicit AppSrc(void* existing_gst_elem);
  friend class qti::Pipeline;

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace qti
