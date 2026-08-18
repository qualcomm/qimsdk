/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include "qti/qimsdk-camsrc.h"

#include <gst/gst.h>
#include <glib.h>

#include <cstring>
#include <stdexcept>

namespace qti {

struct CamSrc::Impl {
  GstElement* elem_ = nullptr;

  explicit Impl(CamSrc& self) {
    elem_ = static_cast<GstElement*>(self.get_raw_gst_element());
    if (!elem_ || !GST_IS_ELEMENT(elem_)) {
      throw std::runtime_error("CamSrc: invalid GstElement");
    }

    GstElementFactory* fac = gst_element_get_factory(elem_);
    const gchar* fac_name =
      fac ? gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(fac)) : nullptr;
    if (!fac_name || std::strcmp(fac_name, "qtiqmmfsrc") != 0) {
      throw std::runtime_error("CamSrc: underlying element must be 'qtiqmmfsrc'");
    }
  }

  bool emit_capture(CaptureMode mode, unsigned int count, GPtrArray* meta) {
    gboolean ok = FALSE;
    // "capture-image" (GstImageCaptureMode, guint, GPtrArray*, gboolean*)
    g_signal_emit_by_name(elem_, "capture-image",
      static_cast<guint>(mode),
      static_cast<guint>(count),
      meta,
      &ok);
    return ok;
  }

  bool cancel() {
    gboolean ok = FALSE;
    // "cancel-capture" (gboolean*)
    g_signal_emit_by_name(elem_, "cancel-capture", &ok);
    return ok;
  }
};

CamSrc::CamSrc(const std::string& name)
  : Element("qtiqmmfsrc", name),
  impl_(std::make_unique<Impl>(*this)) {
}

CamSrc::CamSrc(void* existing_gst_elem)
  : Element(existing_gst_elem, true),
  impl_(std::make_unique<Impl>(*this)) {
}

CamSrc::~CamSrc() = default;
CamSrc::CamSrc(CamSrc&&) noexcept = default;
CamSrc& CamSrc::operator=(CamSrc&&) noexcept = default;

bool CamSrc::image_capture(unsigned int count) {
  return impl_->emit_capture(CaptureMode::kStill, count, nullptr);
}

bool CamSrc::image_capture(CaptureMode mode, unsigned int count) {
  return impl_->emit_capture(mode, count, nullptr);
}

bool CamSrc::image_capture(CaptureMode mode,
  unsigned int count,
  const std::vector<void*>& metadata_ptrs) {
  GPtrArray* arr = nullptr;
  if (!metadata_ptrs.empty()) {
    arr = g_ptr_array_new_full(metadata_ptrs.size(), nullptr);
    for (void* p : metadata_ptrs) g_ptr_array_add(arr, p);
  }
  const bool ok = impl_->emit_capture(mode, count, arr);
  if (arr) g_ptr_array_unref(arr);
  return ok;
}

bool CamSrc::cancel_capture() {
  return impl_->cancel();
}

} // namespace qti
