/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include "qti/qimsdk-mlpostprocess.h"

namespace qti {

MLPostprocess::MLPostprocess(const std::string& name)
    : Element("qtimlpostprocess", name),
      MLPostprocessBase("qtimlpostprocess",
                        name,
                        "qtimlpostprocess",
                        "MLPostprocess") {
}

MLPostprocess::MLPostprocess(void* existing_gst_elem)
    : Element(existing_gst_elem, true),
      MLPostprocessBase(existing_gst_elem,
                        "qtimlpostprocess",
                        "MLPostprocess") {
}

MLPostprocess::~MLPostprocess() = default;

MLPostprocess::MLPostprocess(MLPostprocess&&) noexcept = default;
MLPostprocess& MLPostprocess::operator=(MLPostprocess&&) noexcept = default;

}  // namespace qti
