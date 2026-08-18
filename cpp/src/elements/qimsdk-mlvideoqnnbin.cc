/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include "qti/qimsdk-mlvideoqnnbin.h"

namespace qti {

MLVideoQNNBin::MLVideoQNNBin(const std::string& name)
    : Element("qtimlvideoqnnbin", name),
      MLPreprocessBase("qtimlvideoqnnbin",
                       name,
                       "qtimlvideoqnnbin",
                       "MLVideoQNNBin"),
      MLPostprocessBase("qtimlvideoqnnbin",
                        name,
                        "qtimlvideoqnnbin",
                        "MLVideoQNNBin") {
}

MLVideoQNNBin::MLVideoQNNBin(void* existing_gst_elem)
    : Element(existing_gst_elem, true),
      MLPreprocessBase(existing_gst_elem,
                       "qtimlvideoqnnbin",
                       "MLVideoQNNBin"),
      MLPostprocessBase(existing_gst_elem,
                        "qtimlvideoqnnbin",
                        "MLVideoQNNBin") {
}

MLVideoQNNBin::~MLVideoQNNBin() = default;

MLVideoQNNBin::MLVideoQNNBin(MLVideoQNNBin&&) noexcept = default;
MLVideoQNNBin& MLVideoQNNBin::operator=(MLVideoQNNBin&&) noexcept = default;

}  // namespace qti
