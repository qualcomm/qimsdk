/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include "qti/qimsdk-mlvconverter.h"

namespace qti {

MLVConverter::MLVConverter(const std::string& name)
    : Element("qtimlvconverter", name),
      MLPreprocessBase("qtimlvconverter",
                       name,
                       "qtimlvconverter",
                       "MLVConverter") {
}

MLVConverter::MLVConverter(void* existing_gst_elem)
    : Element(existing_gst_elem, true),
      MLPreprocessBase(existing_gst_elem,
                       "qtimlvconverter",
                       "MLVConverter") {
}

MLVConverter::~MLVConverter() = default;

MLVConverter::MLVConverter(MLVConverter&&) noexcept = default;
MLVConverter& MLVConverter::operator=(MLVConverter&&) noexcept = default;

}  // namespace qti