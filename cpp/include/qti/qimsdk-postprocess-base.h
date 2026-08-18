/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#pragma once

#include "qti/qimsdk-element.h"
#include "qti/qimsdk-ml-types.h"

#include <functional>
#include <memory>

namespace qti {


using ClassificationPostprocessCallback =
    std::function<bool(const MLFrame&, const MLParam&, MLClassifications&)>;
using ObjectDetectionPostprocessCallback =
    std::function<bool(const MLFrame&, const MLParam&, MLDetections&)>;
using PoseEstimationPostprocessCallback =
    std::function<bool(const MLFrame&, const MLParam&, MLPoses&)>;
using DepthEstimationPostprocessCallback =
    std::function<bool(const MLFrame&, const MLParam&, MLDepthMaps&)>;
using SegmentationPostprocessCallback =
    std::function<bool(const MLFrame&, const MLParam&, MLSegmentations&)>;
using TensorsPostprocessCallback =
    std::function<bool(const MLFrame&, const MLParam&, MLFrame&)>;

// Shared base for ML postprocess elements with typed callback registration.
class MLPostprocessBase : public virtual Element {
 public:
  // Register classification postprocess callback.
  MLPostprocessBase& set_handler(
      ClassificationPostprocessCallback handler);
  // Register object-detection postprocess callback.
  MLPostprocessBase& set_handler(
      ObjectDetectionPostprocessCallback handler);
  // Register pose-estimation postprocess callback.
  MLPostprocessBase& set_handler(
      PoseEstimationPostprocessCallback handler);
  // Register depth-estimation postprocess callback.
  MLPostprocessBase& set_handler(
      DepthEstimationPostprocessCallback handler);
  // Register segmentation postprocess callback.
  MLPostprocessBase& set_handler(
      SegmentationPostprocessCallback handler);
  // Register raw-tensors postprocess callback.
  MLPostprocessBase& set_handler(TensorsPostprocessCallback handler);

  // Release owned implementation state.
  virtual ~MLPostprocessBase();

  MLPostprocessBase(const MLPostprocessBase&) = delete;
  MLPostprocessBase& operator=(
      const MLPostprocessBase&) = delete;

  MLPostprocessBase(MLPostprocessBase&&) noexcept;
  MLPostprocessBase& operator=(
      MLPostprocessBase&&) noexcept;

 protected:
  MLPostprocessBase(const std::string& factory,
                    const std::string& name,
                    const char* expected_factory,
                    const char* owner_name);
  MLPostprocessBase(void* existing_gst_elem,
                    const char* expected_factory,
                    const char* owner_name);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace qti
