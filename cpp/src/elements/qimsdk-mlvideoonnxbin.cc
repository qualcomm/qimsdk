/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include "qti/qimsdk-mlvideoonnxbin.h"

namespace qti {

MLVideoONNXBin::MLVideoONNXBin(const std::string& name)
    : Element("qtimlvideoonnxbin", name),
      MLPreprocessBase("qtimlvideoonnxbin",
                       name,
                       "qtimlvideoonnxbin",
                       "MLVideoONNXBin"),
      MLPostprocessBase("qtimlvideoonnxbin",
                        name,
                        "qtimlvideoonnxbin",
                        "MLVideoONNXBin") {
}

MLVideoONNXBin::MLVideoONNXBin(void* existing_gst_elem)
    : Element(existing_gst_elem, true),
      MLPreprocessBase(existing_gst_elem,
                       "qtimlvideoonnxbin",
                       "MLVideoONNXBin"),
      MLPostprocessBase(existing_gst_elem,
                        "qtimlvideoonnxbin",
                        "MLVideoONNXBin") {
}

MLVideoONNXBin::~MLVideoONNXBin() = default;

MLVideoONNXBin::MLVideoONNXBin(MLVideoONNXBin&&) noexcept = default;
MLVideoONNXBin& MLVideoONNXBin::operator=(MLVideoONNXBin&&) noexcept = default;

}  // namespace qti
