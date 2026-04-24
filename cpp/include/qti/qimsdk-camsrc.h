/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#pragma once

#include "qti/qimsdk-element.h"

#include <memory>
#include <string>
#include <vector>

namespace qti {

class CamSrc : public Element {
 public:
  // Create a camera source element with an optional instance name.
  explicit CamSrc(const std::string& name = {});
  // Release owned implementation state.
  ~CamSrc();

  CamSrc(const CamSrc&) = delete;
  CamSrc& operator=(const CamSrc&) = delete;

  CamSrc(CamSrc&&) noexcept;
  CamSrc& operator=(CamSrc&&) noexcept;

  // Camera capture mode used by image_capture.
  enum class CaptureMode : unsigned int {
    kStill = 0,
    kBurst = 1,
  };

  // Capture one or more images using default capture mode.
  bool image_capture(unsigned int count = 1u);

  // Capture one or more images using the selected mode.
  bool image_capture(CaptureMode mode, unsigned int count = 1u);

  // Capture images with optional per-request metadata payloads.
  bool image_capture(CaptureMode mode,
                     unsigned int count,
                     const std::vector<void*>& metadata_ptrs);

  // Cancel pending capture requests.
  bool cancel_capture();

private:
  explicit CamSrc(void* existing_gst_elem);
  friend class qti::Pipeline;

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace qti
