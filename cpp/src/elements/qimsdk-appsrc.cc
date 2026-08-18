/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include "qti/qimsdk-appsrc.h"
#include "qimsdk-logger-internal.h"

#include <gst/gst.h>

#include <cstring>
#include <stdexcept>

namespace qti {

struct AppSrc::Impl {
  GstElement* src_ = nullptr;

  std::function<bool(qti::Buffer&)> producer_;
  std::function<void()> enough_;

  gulong need_data_handler_id_ = 0;
  gulong enough_data_handler_id_ = 0;

  explicit Impl(AppSrc& self, const std::string& /*name*/) : src_(nullptr) {
    auto* raw_elem = static_cast<GstElement*>(self.get_raw_gst_element());
    if (!raw_elem) {
      throw std::runtime_error("AppSrc: null GstElement");
    }
    if (!GST_IS_ELEMENT(raw_elem)) {
      throw std::runtime_error("AppSrc: underlying object is not a GstElement");
    }

    GstElementFactory* fac = gst_element_get_factory(raw_elem);
    const gchar* fac_name =
      fac ? gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(fac)) : nullptr;
    if (!fac_name || std::strcmp(fac_name, "appsrc") != 0) {
      throw std::runtime_error("AppSrc: underlying element is not an appsrc");
    }

    src_ = raw_elem;

    g_object_set(G_OBJECT(src_), "emit-signals", TRUE, nullptr);

    need_data_handler_id_ =
      g_signal_connect(src_, "need-data",
        G_CALLBACK(&Impl::on_need_data), this);
    enough_data_handler_id_ =
      g_signal_connect(src_, "enough-data",
        G_CALLBACK(&Impl::on_enough_data), this);
  }

  ~Impl() {
    if (src_) {
      if (need_data_handler_id_ != 0) {
        g_signal_handler_disconnect(src_, need_data_handler_id_);
        need_data_handler_id_ = 0;
      }
      if (enough_data_handler_id_ != 0) {
        g_signal_handler_disconnect(src_, enough_data_handler_id_);
        enough_data_handler_id_ = 0;
      }
    }
  }

  AppSrc& set_buffer_producer(AppSrc& self,
    std::function<bool(qti::Buffer&)> fn) {
    producer_ = std::move(fn);
    return self;
  }

  AppSrc& set_enough_handler(AppSrc& self, std::function<void()> fn) {
    enough_ = std::move(fn);
    return self;
  }

  bool push_buffer(qti::Buffer& buffer) {
    GstBuffer* raw = static_cast<GstBuffer*>(buffer.take_gst_buffer());
    if (!raw) {
      if (buffer.size() == 0 || buffer.data() == nullptr) {
        return false;
      }
      raw = gst_buffer_new_allocate(nullptr, buffer.size(), nullptr);
      if (!raw) return false;

      GstMapInfo w{};
      if (!gst_buffer_map(raw, &w, GST_MAP_WRITE)) {
        gst_buffer_unref(raw);
        return false;
      }
      std::memcpy(w.data, buffer.data(), buffer.size());
      gst_buffer_unmap(raw, &w);
    }

    GstFlowReturn fr = GST_FLOW_OK;
    g_signal_emit_by_name(src_, "push-buffer", raw, &fr);

    gst_buffer_unref(raw);

    return (fr == GST_FLOW_OK);
  }

  bool push_buffer(qti::Buffer&& buffer) { return push_buffer(buffer); }

  void end_of_stream() {
    GstFlowReturn fr = GST_FLOW_OK;
    g_signal_emit_by_name(src_, "end-of-stream", &fr);
    (void)fr;  // intentionally ignore
  }

  static void on_need_data(GstElement* /*src*/, guint length, gpointer ud) {
    auto* self = static_cast<Impl*>(ud);
    if (!self) return;

    try {
      if (!self->producer_) return;

      qti::Buffer buf;
      buf.refill_for_appsrc(length);

      const bool should_push = self->producer_(buf);
      if (should_push) {
        (void)self->push_buffer(buf);
      }
    }
    catch (const std::exception& e) {
      QIMSDK_LOG_ERROR << "[APPSRC][NEED_DATA] exception in callback: "
                      << e.what();
    }
    catch (...) {
      QIMSDK_LOG_ERROR << "[APPSRC][NEED_DATA] unknown exception in callback";
    }
  }

  static void on_enough_data(GstElement* /*src*/, gpointer ud) {
    auto* self = static_cast<Impl*>(ud);
    if (!self) return;
    try {
      if (self->enough_) self->enough_();
    }
    catch (const std::exception& e) {
      QIMSDK_LOG_ERROR << "[APPSRC][ENOUGH_DATA] exception in callback: "
                      << e.what();
    }
    catch (...) {
      QIMSDK_LOG_ERROR << "[APPSRC][ENOUGH_DATA] unknown exception in callback";
    }
  }
};

AppSrc::AppSrc(const std::string& name)
  : Element("appsrc", name), impl_(std::make_unique<Impl>(*this, name)) {
}


AppSrc::AppSrc(void* existing_gst_elem)
  : Element(existing_gst_elem, true),
    impl_(std::make_unique<Impl>(*this, std::string{})) {
}

AppSrc::~AppSrc() = default;
AppSrc::AppSrc(AppSrc&&) noexcept = default;
AppSrc& AppSrc::operator=(AppSrc&&) noexcept = default;

AppSrc& AppSrc::set_buffer_producer(std::function<bool(qti::Buffer&)> fn) {
  return impl_->set_buffer_producer(*this, std::move(fn));
}
AppSrc& AppSrc::set_enough_handler(std::function<void()> fn) {
  return impl_->set_enough_handler(*this, std::move(fn));
}
bool AppSrc::push_buffer(qti::Buffer& b) { return impl_->push_buffer(b); }
bool AppSrc::push_buffer(qti::Buffer&& b) { return impl_->push_buffer(b); }
void AppSrc::end_of_stream() { impl_->end_of_stream(); }

}  // namespace qti
