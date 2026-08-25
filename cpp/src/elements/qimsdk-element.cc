/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include "qti/qimsdk-element.h"
#include "qti/qimsdk-stream-filter.h"

#include "qimsdk-runtime.h"

#include <gst/gst.h>

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace qti {

namespace {

void validate_object_property(GObject* object, const char* prop,
                              const char* context) {
  if (!object || !prop || !*prop) {
    throw std::invalid_argument(std::string(context) + ": invalid property name");
  }

  GObjectClass* klass = G_OBJECT_GET_CLASS(object);
  if (!klass || !g_object_class_find_property(klass, prop)) {
    const char* obj_name = G_OBJECT_TYPE_NAME(object);
    throw std::invalid_argument(std::string(context) +
                                ": unknown property '" + prop +
                                "' for object type '" +
                                (obj_name ? obj_name : "unknown") + "'");
  }
}

}  // namespace

struct Element::Impl {
  qti::GstRuntime* runtime_ = nullptr;
  GstElement* elem_ = nullptr;

  Impl(const std::string& factory, const std::string& name) {
    runtime_ = &qti::GstRuntime::get_instance();
    try {
      elem_ = gst_element_factory_make(factory.c_str(),
                                       name.empty() ? nullptr : name.c_str());
      if (!elem_) {
        throw std::runtime_error("Failed to create GstElement: factory='" +
                                 factory + "', name='" + name + "'");
      }
      gst_object_ref_sink(elem_);
    } catch (...) {
      qti::GstRuntime::release_instance();
      runtime_ = nullptr;
      throw;
    }
  }

  Impl(GstElement* existing, bool add_ref) {
    runtime_ = &qti::GstRuntime::get_instance();
    try {
      if (!existing) {
        throw std::runtime_error("Element::Impl: null GstElement");
      }
      elem_ = existing;
      if (add_ref) {
        gst_object_ref(elem_);
      }
    } catch (...) {
      qti::GstRuntime::release_instance();
      runtime_ = nullptr;
      throw;
    }
  }

  ~Impl() {
    if (elem_) {
      gst_object_unref(elem_);
      elem_ = nullptr;
    }
    if (runtime_) {
      qti::GstRuntime::release_instance();
      runtime_ = nullptr;
    }
  }

  void validate_property(const char* prop) {
    validate_object_property(G_OBJECT(elem_), prop, "Element::set");
  }

  void set_str(const char* prop, const char* v) {
    validate_property(prop);
    gst_util_set_object_arg(G_OBJECT(elem_), prop, v);
  }
  void set_str(const char* prop, const std::string& v) {
    validate_property(prop);
    gst_util_set_object_arg(G_OBJECT(elem_), prop, v.c_str());
  }
  void set_bool(const char* prop, bool v) {
    validate_property(prop);
    g_object_set(G_OBJECT(elem_), prop, static_cast<gboolean>(v), nullptr);
  }
  void set_i(const char* prop, int v) {
    validate_property(prop);
    g_object_set(G_OBJECT(elem_), prop, v, nullptr);
  }
  void set_u(const char* prop, unsigned int v) {
    validate_property(prop);
    g_object_set(G_OBJECT(elem_), prop, v, nullptr);
  }
  void set_l(const char* prop, long v) {
    validate_property(prop);
    g_object_set(G_OBJECT(elem_), prop, v, nullptr);
  }
  void set_ul(const char* prop, unsigned long v) {
    validate_property(prop);
    g_object_set(G_OBJECT(elem_), prop, v, nullptr);
  }
  void set_ll(const char* prop, long long v) {
    validate_property(prop);
    g_object_set(G_OBJECT(elem_), prop, static_cast<gint64>(v), nullptr);
  }
  void set_ull(const char* prop, unsigned long long v) {
    validate_property(prop);
    g_object_set(G_OBJECT(elem_), prop, static_cast<guint64>(v), nullptr);
  }
  void set_f(const char* prop, float v) {
    validate_property(prop);
    g_object_set(G_OBJECT(elem_), prop, static_cast<double>(v), nullptr);
  }
  void set_d(const char* prop, double v) {
    validate_property(prop);
    g_object_set(G_OBJECT(elem_), prop, v, nullptr);
  }
  void set_caps(const char* prop, const qti::StreamFilter& caps) {
    validate_property(prop);
    auto* raw = static_cast<GstCaps*>(caps.get_caps_opaque());
    if (!raw) throw std::runtime_error("Invalid StreamFilter caps");
    g_object_set(G_OBJECT(elem_), prop, raw, nullptr);
  }

  void deactivate() {
    if (gst_element_set_state(elem_, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE) {
      throw std::runtime_error("Failed to set element to PAUSED");
    }
  }
  void stop() { gst_element_set_state(elem_, GST_STATE_NULL); }
  void sync() {
    if (!gst_element_sync_state_with_parent(elem_)) {
      throw std::runtime_error("gst_element_sync_state_with_parent() failed");
    }
  }

  void link(Element::Impl& downstream, const std::string& src_pad,
            const std::string& sink_pad) {
    const gboolean ok = gst_element_link_pads(elem_, src_pad.c_str(), downstream.elem_,
                                              sink_pad.c_str());
    if (!ok) {
      throw std::runtime_error("Failed to link pads '" + src_pad + "' -> '" + sink_pad + "'");
    }
  }
  void unlink(Element::Impl& downstream, const std::string& src_pad,
              const std::string& sink_pad) {
    GstPad* sp = gst_element_get_static_pad(elem_, src_pad.c_str());
    GstPad* dp = gst_element_get_static_pad(downstream.elem_, sink_pad.c_str());
    if (!sp || !dp) {
      if (sp) gst_object_unref(sp);
      if (dp) gst_object_unref(dp);
      throw std::runtime_error("unlink(): cannot get pads");
    }
    gst_pad_unlink(sp, dp);
    gst_object_unref(sp);
    gst_object_unref(dp);
  }
  void unlink(const std::string& src_pad) {
    GstPad* sp = gst_element_get_static_pad(elem_, src_pad.c_str());
    if (!sp) {
      throw std::runtime_error("unlink(): cannot get src pad '" + src_pad + "'");
    }
    GstPad* peer = gst_pad_get_peer(sp);
    if (peer) {
      gst_pad_unlink(sp, peer);
      gst_object_unref(peer);
    }
    gst_object_unref(sp);
  }
};

Element::Element(const std::string& factory, const std::string& name)
    : impl_(std::make_unique<Impl>(factory, name)) {}

Element::~Element() = default;

Element::Element(Element&&) noexcept = default;
Element& Element::operator=(Element&&) noexcept = default;

void Element::set_one(const char* prop, const char* v) { impl_->set_str(prop, v); }
void Element::set_one(const char* prop, const std::string& v) { impl_->set_str(prop, v); }
void Element::set_one(const char* prop, bool v) { impl_->set_bool(prop, v); }
void Element::set_one(const char* prop, int v) { impl_->set_i(prop, v); }
void Element::set_one(const char* prop, unsigned int v) { impl_->set_u(prop, v); }
void Element::set_one(const char* prop, long v) { impl_->set_l(prop, v); }
void Element::set_one(const char* prop, unsigned long v) { impl_->set_ul(prop, v); }
void Element::set_one(const char* prop, long long v) { impl_->set_ll(prop, v); }
void Element::set_one(const char* prop, unsigned long long v) { impl_->set_ull(prop, v); }
void Element::set_one(const char* prop, float v) { impl_->set_f(prop, v); }
void Element::set_one(const char* prop, double v) { impl_->set_d(prop, v); }
void Element::set_one(const char* prop, const qti::StreamFilter& caps) {
  impl_->set_caps(prop, caps);
}

Element& Element::deactivate() {
  impl_->deactivate();
  return *this;
}
Element& Element::stop() {
  impl_->stop();
  return *this;
}
Element& Element::sync() {
  impl_->sync();
  return *this;
}

Element& Element::link(Element& downstream, const std::string& src_pad,
                       const std::string& sink_pad) {
  impl_->link(*downstream.impl_, src_pad, sink_pad);
  return *this;
}
Element& Element::unlink(Element& downstream, const std::string& src_pad,
                         const std::string& sink_pad) {
  impl_->unlink(*downstream.impl_, src_pad, sink_pad);
  return *this;
}
Element& Element::unlink(const std::string& src_pad) {
  impl_->unlink(src_pad);
  return *this;
}

Element::SignalHandlerId Element::connect_signal(const std::string& signal_name,
                                                 SignalCallback callback,
                                                 void* user_data) {
  if (signal_name.empty()) {
    throw std::invalid_argument("Element::connect_signal: empty signal name");
  }
  if (!callback) {
    throw std::invalid_argument("Element::connect_signal: null callback");
  }

  auto* obj = G_OBJECT(impl_->elem_);
  if (g_signal_lookup(signal_name.c_str(), G_OBJECT_TYPE(obj)) == 0) {
    const char* obj_name = G_OBJECT_TYPE_NAME(obj);
    throw std::invalid_argument("Element::connect_signal: unknown signal '" +
                                signal_name + "' for object type '" +
                                (obj_name ? std::string(obj_name) : "unknown") + "'");
  }

  const gulong handler_id = g_signal_connect_data(
      obj, signal_name.c_str(), reinterpret_cast<GCallback>(callback), user_data,
      nullptr, static_cast<GConnectFlags>(0));
  if (handler_id == 0) {
    throw std::runtime_error("Element::connect_signal: failed to connect '" +
                             signal_name + "'");
  }
  return static_cast<SignalHandlerId>(handler_id);
}

Element& Element::disconnect_signal(SignalHandlerId handler_id) {
  if (handler_id != 0 &&
      g_signal_handler_is_connected(G_OBJECT(impl_->elem_),
                                    static_cast<gulong>(handler_id))) {
    g_signal_handler_disconnect(G_OBJECT(impl_->elem_),
                                static_cast<gulong>(handler_id));
  }
  return *this;
}

void* Element::get_raw_gst_element() const { return static_cast<void*>(impl_->elem_); }

Element::Element(void* existing_gst_elem, bool add_ref)
    : impl_(std::make_unique<Impl>(static_cast<GstElement*>(existing_gst_elem), add_ref)) {}

class Port::Impl {
 public:
  Impl(GstElement* elem, bool is_sink, std::string name, unsigned int id)
      : elem_(elem), is_sink_(is_sink), name_(std::move(name)), id_(id) {}
  Impl(GstElement* elem, bool is_sink, unsigned int id)
      : elem_(elem), is_sink_(is_sink), id_(id) {}

  ~Impl() {
    if (pad_) gst_object_unref(pad_);
  }

  GstPad* ensure_pad() {
    if (pad_) return pad_;
    if (!elem_) throw std::runtime_error("Port: null element");

    std::vector<GstPad*> candidates;
    if (GstIterator* it = gst_element_iterate_pads(elem_)) {
      GValue item = G_VALUE_INIT;
      while (gst_iterator_next(it, &item) == GST_ITERATOR_OK) {
        GstPad* p = GST_PAD(g_value_get_object(&item));
        if (GST_PAD_DIRECTION(p) == (is_sink_ ? GST_PAD_SINK : GST_PAD_SRC)) {
          candidates.push_back(GST_PAD(gst_object_ref(p)));
        }
        g_value_unset(&item);
      }
      gst_iterator_free(it);
    }

    auto cleanup = [&] {
      for (auto* p : candidates) gst_object_unref(p);
      candidates.clear();
    };

    auto get_name = [](GstPad* p) -> const char* { return GST_OBJECT_NAME(p); };
    auto matches_expected = [&](const char* pad_name) -> bool {
      if (name_.empty() || !pad_name) return false;
      char expected[128] = {};
      if (name_.find("%u") != std::string::npos) {
        std::snprintf(expected, sizeof(expected), name_.c_str(), id_);
      } else {
        std::snprintf(expected, sizeof(expected), "%s_%u", name_.c_str(), id_);
      }
      return std::strcmp(pad_name, expected) == 0;
    };
    auto numeric_suffix = [](const char* s) -> int {
      if (!s) return -1;
      const char* p = s + std::strlen(s);
      while (p > s && std::isdigit(static_cast<unsigned char>(*(p - 1)))) --p;
      if (p == s || !std::isdigit(static_cast<unsigned char>(*p))) return -1;
      return std::atoi(p);
    };

    if (!name_.empty()) {
      for (auto* p : candidates) {
        if (matches_expected(get_name(p))) {
          pad_ = GST_PAD(gst_object_ref(p));
          cleanup();
          return pad_;
        }
      }
    }
    for (auto* p : candidates) {
      int suf = numeric_suffix(get_name(p));
      if (suf == static_cast<int>(id_)) {
        pad_ = GST_PAD(gst_object_ref(p));
        cleanup();
        return pad_;
      }
    }
    if (id_ < candidates.size()) {
      pad_ = GST_PAD(gst_object_ref(candidates[id_]));
      cleanup();
      return pad_;
    }
    if (!name_.empty()) {
      if (GstPad* p = gst_element_get_static_pad(elem_, name_.c_str())) {
        if (GST_PAD_DIRECTION(p) == (is_sink_ ? GST_PAD_SINK : GST_PAD_SRC)) {
          pad_ = p;
          cleanup();
          return pad_;
        }
        gst_object_unref(p);
      }
      char buf[128] = {};
      std::snprintf(buf, sizeof(buf), "%s_%u", name_.c_str(), id_);
      if (GstPad* p = gst_element_get_static_pad(elem_, buf)) {
        if (GST_PAD_DIRECTION(p) == (is_sink_ ? GST_PAD_SINK : GST_PAD_SRC)) {
          pad_ = p;
          cleanup();
          return pad_;
        }
        gst_object_unref(p);
      }
    }

    cleanup();
    throw std::runtime_error("Port: cannot resolve target pad");
  }

  void validate_property(GstPad* p, const char* prop) {
    validate_object_property(G_OBJECT(p), prop, "Port::set");
  }

  void set(const char* prop, const char* v) {
    GstPad* p = ensure_pad();
    validate_property(p, prop);
    gst_util_set_object_arg(G_OBJECT(p), prop, v);
  }
  void set(const char* prop, const std::string& v) { set(prop, v.c_str()); }
  void set(const char* prop, bool v) {
    GstPad* p = ensure_pad();
    validate_property(p, prop);
    g_object_set(G_OBJECT(p), prop, static_cast<gboolean>(v), nullptr);
  }
  void set(const char* prop, int v) {
    GstPad* p = ensure_pad();
    validate_property(p, prop);
    g_object_set(G_OBJECT(p), prop, v, nullptr);
  }
  void set(const char* prop, unsigned int v) {
    GstPad* p = ensure_pad();
    validate_property(p, prop);
    g_object_set(G_OBJECT(p), prop, v, nullptr);
  }
  void set(const char* prop, long v) {
    GstPad* p = ensure_pad();
    validate_property(p, prop);
    g_object_set(G_OBJECT(p), prop, v, nullptr);
  }
  void set(const char* prop, unsigned long v) {
    GstPad* p = ensure_pad();
    validate_property(p, prop);
    g_object_set(G_OBJECT(p), prop, v, nullptr);
  }
  void set(const char* prop, long long v) {
    GstPad* p = ensure_pad();
    validate_property(p, prop);
    g_object_set(G_OBJECT(p), prop, static_cast<gint64>(v), nullptr);
  }
  void set(const char* prop, unsigned long long v) {
    GstPad* p = ensure_pad();
    validate_property(p, prop);
    g_object_set(G_OBJECT(p), prop, static_cast<guint64>(v), nullptr);
  }
  void set(const char* prop, float v) {
    GstPad* p = ensure_pad();
    validate_property(p, prop);
    g_object_set(G_OBJECT(p), prop, static_cast<double>(v), nullptr);
  }
  void set(const char* prop, double v) {
    GstPad* p = ensure_pad();
    validate_property(p, prop);
    g_object_set(G_OBJECT(p), prop, v, nullptr);
  }

  void set_int_array(const char* prop, const std::vector<int>& vals) {
    GstPad* p = ensure_pad();
    validate_property(p, prop);
    GValue arr = G_VALUE_INIT;
    g_value_init(&arr, GST_TYPE_ARRAY);
    for (int v : vals) {
      GValue iv = G_VALUE_INIT;
      g_value_init(&iv, G_TYPE_INT);
      g_value_set_int(&iv, v);
      gst_value_array_append_value(&arr, &iv);
      g_value_unset(&iv);
    }
    g_object_set_property(G_OBJECT(p), prop, &arr);
    g_value_unset(&arr);
  }

 private:
  GstElement* elem_ = nullptr;
  bool is_sink_ = true;
  std::string name_;
  unsigned int id_ = 0;
  GstPad* pad_ = nullptr;
};

Port::Port(void* gst_elem, bool is_sink, const std::string& name, unsigned int id)
    : impl_(std::make_unique<Impl>(static_cast<GstElement*>(gst_elem), is_sink, name, id)) {}
Port::Port(void* gst_elem, bool is_sink, unsigned int id)
    : impl_(std::make_unique<Impl>(static_cast<GstElement*>(gst_elem), is_sink, id)) {}
Port::Port(Port&&) noexcept = default;
Port& Port::operator=(Port&&) noexcept = default;
Port::~Port() = default;

void Port::set_one(const char* prop, const char* value) { impl_->set(prop, value); }
void Port::set_one(const char* prop, const std::string& value) { impl_->set(prop, value); }
void Port::set_one(const char* prop, bool value) { impl_->set(prop, value); }
void Port::set_one(const char* prop, int value) { impl_->set(prop, value); }
void Port::set_one(const char* prop, unsigned int value) { impl_->set(prop, value); }
void Port::set_one(const char* prop, long value) { impl_->set(prop, value); }
void Port::set_one(const char* prop, unsigned long value) { impl_->set(prop, value); }
void Port::set_one(const char* prop, long long value) { impl_->set(prop, value); }
void Port::set_one(const char* prop, unsigned long long value) { impl_->set(prop, value); }
void Port::set_one(const char* prop, float value) { impl_->set(prop, value); }
void Port::set_one(const char* prop, double value) { impl_->set(prop, value); }
void Port::set_one(const char* prop, const std::initializer_list<int>& vals) {
  impl_->set_int_array(prop, std::vector<int>(vals.begin(), vals.end()));
}
void Port::set_one(const char* prop, const std::vector<int>& vals) {
  impl_->set_int_array(prop, vals);
}

Port Element::input(unsigned int id) { return Port(impl_->elem_, /*is_sink=*/true, id); }
Port Element::output(unsigned int id) { return Port(impl_->elem_, /*is_sink=*/false, id); }
Port Element::input(const std::string& name_or_type, unsigned int id) {
  return Port(impl_->elem_, /*is_sink=*/true, name_or_type, id);
}
Port Element::output(const std::string& name_or_type, unsigned int id) {
  return Port(impl_->elem_, /*is_sink=*/false, name_or_type, id);
}

}  // namespace qti
