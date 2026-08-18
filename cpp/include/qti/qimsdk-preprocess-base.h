/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#pragma once

#include "qti/qimsdk-element.h"
#include "qti/qimsdk-ml-types.h"

#include <functional>
#include <memory>
#include <string>

namespace qti {

// Register raw-tensors preprocess callback.
using TensorsPreprocessCallback =
    std::function<bool(const MLVideoBlits& blits, MLFrame& output)>;

// Shared base for ML preprocess elements with callback registration.
class MLPreprocessBase : public virtual Element {
 public:
  // Register raw-tensors preprocess callback.
  MLPreprocessBase& set_handler(TensorsPreprocessCallback handler);

  // Release owned implementation state.
  virtual ~MLPreprocessBase();

  MLPreprocessBase(const MLPreprocessBase&) = delete;
  MLPreprocessBase& operator=(
      const MLPreprocessBase&) = delete;

  MLPreprocessBase(MLPreprocessBase&&) noexcept;
  MLPreprocessBase& operator=(
      MLPreprocessBase&&) noexcept;

 protected:
  MLPreprocessBase(const std::string& factory,
                   const std::string& name,
                   const char* expected_factory,
                   const char* owner_name);
  MLPreprocessBase(void* existing_gst_elem,
                   const char* expected_factory,
                   const char* owner_name);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace qti
