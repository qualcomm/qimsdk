/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include "qti/qimsdk-mlvideotflitebin.h"

namespace qti {

MLVideoTFLiteBin::MLVideoTFLiteBin(const std::string& name)
    : Element("qtimlvideotflitebin", name),
      MLPreprocessBase("qtimlvideotflitebin",
                       name,
                       "qtimlvideotflitebin",
                       "MLVideoTFLiteBin"),
      MLPostprocessBase("qtimlvideotflitebin",
                        name,
                        "qtimlvideotflitebin",
                        "MLVideoTFLiteBin") {
}

MLVideoTFLiteBin::MLVideoTFLiteBin(void* existing_gst_elem)
    : Element(existing_gst_elem, true),
      MLPreprocessBase(existing_gst_elem,
                       "qtimlvideotflitebin",
                       "MLVideoTFLiteBin"),
      MLPostprocessBase(existing_gst_elem,
                        "qtimlvideotflitebin",
                        "MLVideoTFLiteBin") {
}

MLVideoTFLiteBin::~MLVideoTFLiteBin() = default;

MLVideoTFLiteBin::MLVideoTFLiteBin(MLVideoTFLiteBin&&) noexcept = default;
MLVideoTFLiteBin& MLVideoTFLiteBin::operator=(MLVideoTFLiteBin&&) noexcept =
    default;

}  // namespace qti
