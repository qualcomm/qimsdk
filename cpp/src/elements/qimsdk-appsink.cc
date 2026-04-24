/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include "qti/qimsdk-appsink.h"
#include "qimsdk-logger-internal.h"

#include <gst/gst.h>

#include <cstring>
#include <stdexcept>

namespace qti {

struct AppSink::Impl {
  GstElement* sink_ = nullptr;

  std::function<void(qti::Buffer)> consumer_;
  std::function<bool(qti::Buffer&&)> preroll_;
  std::function<void()> eos_;

  gulong new_sample_handler_id_ = 0;
  gulong new_preroll_handler_id_ = 0;
  gulong eos_handler_id_ = 0;

  explicit Impl(AppSink& self, const std::string& /*name*/) : sink_(nullptr) {
    auto* raw_elem = static_cast<GstElement*>(self.get_raw_gst_element());
    if (!raw_elem) {
      throw std::runtime_error("AppSink: null GstElement");
    }
    if (!GST_IS_ELEMENT(raw_elem)) {
      throw std::runtime_error("AppSink: underlying object is not a GstElement");
    }

    GstElementFactory* fac = gst_element_get_factory(raw_elem);
    const gchar* fac_name =
      fac ? gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(fac)) : nullptr;
    if (!fac_name || std::strcmp(fac_name, "appsink") != 0) {
      throw std::runtime_error("AppSink: underlying element is not an appsink");
    }

    sink_ = raw_elem;

    g_object_set(G_OBJECT(sink_), "emit-signals", TRUE, nullptr);

    new_sample_handler_id_ =
      g_signal_connect(sink_, "new-sample",
        G_CALLBACK(&Impl::on_new_sample), this);
    new_preroll_handler_id_ =
      g_signal_connect(sink_, "new-preroll",
        G_CALLBACK(&Impl::on_new_preroll), this);
    eos_handler_id_ = g_signal_connect(sink_, "eos",
      G_CALLBACK(&Impl::on_eos), this);
  }

  ~Impl() {
    if (sink_) {
      if (new_sample_handler_id_ != 0) {
        g_signal_handler_disconnect(sink_, new_sample_handler_id_);
        new_sample_handler_id_ = 0;
      }
      if (new_preroll_handler_id_ != 0) {
        g_signal_handler_disconnect(sink_, new_preroll_handler_id_);
        new_preroll_handler_id_ = 0;
      }
      if (eos_handler_id_ != 0) {
        g_signal_handler_disconnect(sink_, eos_handler_id_);
        eos_handler_id_ = 0;
      }
    }
  }

  AppSink& set_buffer_consumer(AppSink& self, std::function<void(qti::Buffer)> fn) {
    consumer_ = std::move(fn);
    return self;
  }

  AppSink& set_preroll_handler(AppSink& self, std::function<bool(qti::Buffer&&)> fn) {
    preroll_ = std::move(fn);
    return self;
  }

  AppSink& set_eos_handler(AppSink& self, std::function<void()> fn) {
    eos_ = std::move(fn);
    return self;
  }

  static GstFlowReturn on_new_sample(GstElement* /*unused*/, gpointer ud) {
    auto* self = static_cast<Impl*>(ud);
    if (!self) return GST_FLOW_ERROR;

    GstSample* sample = nullptr;
    g_signal_emit_by_name(self->sink_, "pull-sample", &sample);
    if (!sample) return GST_FLOW_EOS;

    try {
      qti::Buffer b =
        qti::Buffer::from_readable_sample(static_cast<void*>(sample));
      if (self->consumer_) {
        self->consumer_(std::move(b));
      }
      else {
        gst_sample_unref(sample);
      }
    }
    catch (...) {
      if (sample) gst_sample_unref(sample);
      return GST_FLOW_ERROR;
    }

    return GST_FLOW_OK;
  }

  static GstFlowReturn on_new_preroll(GstElement* /*unused*/, gpointer ud) {
    auto* self = static_cast<Impl*>(ud);
    if (!self) return GST_FLOW_ERROR;

    GstSample* sample = nullptr;
    g_signal_emit_by_name(self->sink_, "pull-preroll", &sample);
    if (!sample) return GST_FLOW_EOS;

    bool ok = true;
    try {
      if (self->preroll_) {
        qti::Buffer b =
          qti::Buffer::from_readable_sample(static_cast<void*>(sample));
        ok = self->preroll_(std::move(b));
      }
      else {
        gst_sample_unref(sample);
      }
    }
    catch (...) {
      if (sample) gst_sample_unref(sample);
      return GST_FLOW_ERROR;
    }

    return ok ? GST_FLOW_OK : GST_FLOW_ERROR;
  }

  static void on_eos(GstElement* /*unused*/, gpointer ud) {
    auto* self = static_cast<Impl*>(ud);
    if (!self) return;
    try {
      if (self->eos_) self->eos_();
    }
    catch (const std::exception& e) {
      QIMSDK_LOG_ERROR << "[APPSINK][EOS] exception in callback: " << e.what();
    }
    catch (...) {
      QIMSDK_LOG_ERROR << "[APPSINK][EOS] unknown exception in callback";
    }
  }
};

AppSink::AppSink(const std::string& name)
  : Element("appsink", name), impl_(std::make_unique<Impl>(*this, name)) {
}

AppSink::AppSink(void* existing_gst_elem)
  : Element(existing_gst_elem, true),
    impl_(std::make_unique<Impl>(*this, std::string{})) {
}

AppSink::~AppSink() = default;
AppSink::AppSink(AppSink&&) noexcept = default;
AppSink& AppSink::operator=(AppSink&&) noexcept = default;

AppSink& AppSink::set_buffer_consumer(std::function<void(qti::Buffer)> fn) {
  return impl_->set_buffer_consumer(*this, std::move(fn));
}
AppSink& AppSink::set_preroll_handler(std::function<bool(qti::Buffer&&)> fn) {
  return impl_->set_preroll_handler(*this, std::move(fn));
}
AppSink& AppSink::set_eos_handler(std::function<void()> fn) {
  return impl_->set_eos_handler(*this, std::move(fn));
}

}  // namespace qti
