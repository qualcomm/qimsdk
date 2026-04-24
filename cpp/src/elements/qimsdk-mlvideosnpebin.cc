/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include "qti/qimsdk-mlvideosnpebin.h"

namespace qti {

MLVideoSNPEBin::MLVideoSNPEBin(const std::string& name)
    : Element("qtimlvideosnpebin", name),
      MLPreprocessBase("qtimlvideosnpebin",
                       name,
                       "qtimlvideosnpebin",
                       "MLVideoSNPEBin"),
      MLPostprocessBase("qtimlvideosnpebin",
                        name,
                        "qtimlvideosnpebin",
                        "MLVideoSNPEBin") {
}

MLVideoSNPEBin::MLVideoSNPEBin(void* existing_gst_elem)
    : Element(existing_gst_elem, true),
      MLPreprocessBase(existing_gst_elem,
                       "qtimlvideosnpebin",
                       "MLVideoSNPEBin"),
      MLPostprocessBase(existing_gst_elem,
                        "qtimlvideosnpebin",
                        "MLVideoSNPEBin") {
}

MLVideoSNPEBin::~MLVideoSNPEBin() = default;

MLVideoSNPEBin::MLVideoSNPEBin(MLVideoSNPEBin&&) noexcept = default;
MLVideoSNPEBin& MLVideoSNPEBin::operator=(MLVideoSNPEBin&&) noexcept = default;

}  // namespace qti
