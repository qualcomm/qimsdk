/*
* Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
* SPDX-License-Identifier: BSD-3-Clause-Clear
*/

#include <qti/qimsdk-element.h>
#include <qti/qimsdk-pipeline.h>
#include <qti/qimsdk-stream-filter.h>

#include "qimsdk-element-registry.h"
#include "qimsdk-runtime.h"

#include <gst/gst.h>

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>
#include <sstream>

namespace qti {

namespace {

void validate_element_property(GstElement* element, const char* prop,
                               const std::string& unique_name) {
  if (!element || !prop || !*prop) {
    throw std::invalid_argument("Pipeline::add(): invalid property name");
  }

  GObjectClass* klass = G_OBJECT_GET_CLASS(element);
  if (!klass || !g_object_class_find_property(klass, prop)) {
    const char* type_name = G_OBJECT_TYPE_NAME(element);
    throw std::invalid_argument("Pipeline::add(): unknown property '" +
                                std::string(prop) + "' for element '" +
                                unique_name + "' (type '" +
                                (type_name ? type_name : "unknown") + "')");
  }
}

}  // namespace

struct Pipeline::Impl {
  qti::GstRuntime* runtime_ = nullptr;
  GstElement* pipeline_ = nullptr;

  GstBus* bus_ = nullptr;
  GSource* bus_source_ = nullptr;
  GAsyncQueue* state_msg_queue_ = nullptr;

  qti::GstRuntime::ShutdownListenerId shutdown_listener_id_ = 0;
  bool eos_on_shutdown_ = false;

  bool done_ = false;
  bool eos_received_ = false;
  std::mutex mtx_;
  std::condition_variable cv_;

  struct ElementInfo {
    std::string name;
    GstElement* elem = nullptr;
    std::string prefer_upstream_src_pad_template; // exmp. "image_%u"
  };

  std::vector<ElementInfo> elements_;
  bool linked_ = false;

  struct PendingLink {
    GstElement* upstream = nullptr;
    GstElement* downstream = nullptr;
    std::string sink_pad_template;
    std::string src_pad_template;
    bool uses_request_pad = false;
    GstPad* requested_sink_pad = nullptr;
    bool uses_request_src_pad = false;
    GstPad* requested_src_pad = nullptr;
    bool completed = false;
  };

  struct HandlerTrack {
    GstElement* upstream = nullptr;
    gulong handler_id = 0;
    int pending_count = 0;
  };

  std::vector<HandlerTrack> handler_tracks_;
  std::vector<PendingLink> pending_;
  std::mutex dl_mtx_;

  static constexpr const char* kMsgPipelineState = "PIPELINE_STATE";
  static constexpr const char* kMsgTerminate = "TERMINATE";
  static constexpr GstClockTime kStateChangeTimeout = 10 * GST_SECOND;

  explicit Impl(const std::string& name) {
    runtime_ = &qti::GstRuntime::get_instance();
    try {
      pipeline_ = gst_pipeline_new(name.empty() ? nullptr : name.c_str());
      if (!pipeline_) {
        throw std::runtime_error("Failed to create GstPipeline: " + name);
      }
    }
    catch (...) {
      qti::GstRuntime::release_instance();
      runtime_ = nullptr;
      throw;
    }
    state_msg_queue_ = g_async_queue_new();
  }

  ~Impl() {
    stop_internal();
    if (state_msg_queue_) {
      g_async_queue_unref(state_msg_queue_);
      state_msg_queue_ = nullptr;
    }
    if (pipeline_) {
      gst_object_unref(pipeline_);
      pipeline_ = nullptr;
    }
    if (runtime_) {
      qti::GstRuntime::release_instance();
      runtime_ = nullptr;
    }
  }

  // ---------- Registry / lookup ----------
  auto find_index(const std::string& name) {
    return std::find_if(elements_.begin(), elements_.end(),
      [&](const ElementInfo& ei) { return ei.name == name; });
  }
  auto find_index(const std::string& name) const {
    return std::find_if(elements_.cbegin(), elements_.cend(),
      [&](const ElementInfo& ei) { return ei.name == name; });
  }

  void register_element(const std::string& name, GstElement* e,
    const std::string& up_src_pad_hint = {}) {
    if (find_index(name) != elements_.end()) {
      throw std::logic_error("Internal error: duplicate element: " + name);
    }
    elements_.push_back({ name, e, up_src_pad_hint });
  }

  GstElement* get_element(const std::string& name) {
    auto it = find_index(name);
    if (it == elements_.end()) {
      throw std::runtime_error("Unknown element name: " + name);
    }
    return (*it).elem;
  }

  // ---------- API helpers ----------
  void add_by_factory_no_props(const std::string& factory,
    const std::string& unique_name) {
    if (find_index(unique_name) != elements_.end()) {
      throw std::invalid_argument("Element '" + unique_name +
        "' already exists in this Pipeline!");
    }

    auto wrapper = ElementRegistry::Instance().Create(factory, unique_name);
    if (!wrapper) {
      throw std::runtime_error("Failed to create " + unique_name + " element!");
    }
    auto* e = static_cast<GstElement*>(wrapper->get_raw_gst_element());
    if (!e) {
      throw std::runtime_error(
        "Wrapper returned null GstElement for factory: " + unique_name);
    }

    if (!gst_bin_add(GST_BIN(pipeline_), e)) {
      throw std::runtime_error("Failed to add '" + unique_name +
        "' in the pipeline");
    }

    register_element(unique_name, e, {});
    linked_ = false;
  }

  void add_set_one(const std::string& unique, const char* prop, const char* v) {
    GstElement* e = get_element(unique);
    validate_element_property(e, prop, unique);
    gst_util_set_object_arg(G_OBJECT(e), prop, v);
  }

  void add_set_one(const std::string& unique, const char* prop,
    const std::string& v) {
    GstElement* e = get_element(unique);
    validate_element_property(e, prop, unique);
    gst_util_set_object_arg(G_OBJECT(e), prop, v.c_str());
  }

  void add_set_one(const std::string& unique, const char* prop, bool v) {
    GstElement* e = get_element(unique);
    validate_element_property(e, prop, unique);
    g_object_set(G_OBJECT(e), prop, static_cast<gboolean>(v), nullptr);
  }

  void add_set_one(const std::string& unique, const char* prop, int v) {
    GstElement* e = get_element(unique);
    validate_element_property(e, prop, unique);
    g_object_set(G_OBJECT(e), prop, v, nullptr);
  }

  void add_set_one(const std::string& unique, const char* prop,
    unsigned int v) {
    GstElement* e = get_element(unique);
    validate_element_property(e, prop, unique);
    g_object_set(G_OBJECT(e), prop, v, nullptr);
  }

  void add_set_one(const std::string& unique, const char* prop, long v) {
    GstElement* e = get_element(unique);
    validate_element_property(e, prop, unique);
    g_object_set(G_OBJECT(e), prop, v, nullptr);
  }

  void add_set_one(const std::string& unique, const char* prop,
    unsigned long v) {
    GstElement* e = get_element(unique);
    validate_element_property(e, prop, unique);
    g_object_set(G_OBJECT(e), prop, v, nullptr);
  }

  void add_set_one(const std::string& unique, const char* prop, long long v) {
    GstElement* e = get_element(unique);
    validate_element_property(e, prop, unique);
    g_object_set(G_OBJECT(e), prop, static_cast<gint64>(v), nullptr);
  }

  void add_set_one(const std::string& unique, const char* prop,
    unsigned long long v) {
    GstElement* e = get_element(unique);
    validate_element_property(e, prop, unique);
    g_object_set(G_OBJECT(e), prop, static_cast<guint64>(v), nullptr);
  }

  void add_set_one(const std::string& unique, const char* prop, float v) {
    GstElement* e = get_element(unique);
    validate_element_property(e, prop, unique);
    g_object_set(G_OBJECT(e), prop, static_cast<double>(v), nullptr);
  }

  void add_set_one(const std::string& unique, const char* prop, double v) {
    GstElement* e = get_element(unique);
    validate_element_property(e, prop, unique);
    g_object_set(G_OBJECT(e), prop, v, nullptr);
  }

  void add_element(const qti::Element& element) {
    auto* e = static_cast<GstElement*>(element.get_raw_gst_element());
    if (!e) {
      throw std::runtime_error("Element has null GstElement");
    }

    if (!gst_bin_add(GST_BIN(pipeline_), e)) {
      throw std::runtime_error(
        "Failed to add external element in the pipeline");
    }

    const gchar* name = GST_ELEMENT_NAME(e);
    if (!name || !*name) {
      throw std::runtime_error("External Element has no valid name");
    }

    register_element(name, e, {});
  }

  void add_stream_filter(const std::string& unique_name,
    const qti::StreamFilter& caps) {
    if (find_index(unique_name) != elements_.end()) {
      throw std::invalid_argument("Element '" + unique_name +
        "' already exists in this Pipeline!");
    }

    auto wrapper =
      ElementRegistry::Instance().Create("capsfilter", unique_name);
    if (!wrapper) {
      throw std::runtime_error("Failed to create stream filter " + unique_name);
    }

    auto* cf = static_cast<GstElement*>(wrapper->get_raw_gst_element());
    if (!cf) {
      throw std::runtime_error(
        "Wrapper returned null GstElement for factory: " + unique_name);
    }

    auto* raw_caps = static_cast<GstCaps*>(caps.get_caps_opaque());
    if (raw_caps) {
      g_object_set(G_OBJECT(cf), "caps", raw_caps, nullptr);
    }

    if (!gst_bin_add(GST_BIN(pipeline_), cf)) {
      throw std::runtime_error("Failed to add '" + unique_name +
        "' in the pipeline");
    }

    std::string up_src_hint = caps.upstream_pad_hint();
    register_element(unique_name, cf, up_src_hint);
  }

  void link_by_names(const std::vector<std::string>& names) {
    if (names.size() < 2)
      return;
    for (size_t i = 0; i + 1 < names.size(); ++i) {
      auto itA = find_index(names[i]);
      auto itB = find_index(names[i + 1]);
      if (itA == elements_.end() || itB == elements_.end()) {
        throw std::runtime_error(
          "Unknown element in link(): '" +
          std::string(itA == elements_.end() ? names[i] : names[i + 1]) +
          "'");
      }
      ElementInfo& up = *itA;
      ElementInfo& down = *itB;

      if (!try_immediate_link_or_defer(up, down)) {
        if (!has_dynamic_src(up.elem)) {
          std::string dummy;
          if (!has_request_or_sometimes_sink(down.elem, &dummy)) {
            throw std::runtime_error("Failed to link '" + names[i] + "' -> '" +
              names[i + 1] + "'");
          }
        }
      }
    }
    linked_ = true;
  }

  Pipeline& prepare(Pipeline& self) {
    ensure_bus_watch();
    ensure_shutdown_listener();
    if (!linked_) {
      link_sequential_or_throw();
      linked_ = true;
    }
    if (!update_pipeline_state(GST_STATE_PAUSED, /*blocking=*/true)) {
      throw std::runtime_error("Failed to set pipeline to PAUSED");
    }
    return self;
  }

  Pipeline& activate(Pipeline& self) {
    if (!update_pipeline_state(GST_STATE_PLAYING, /*blocking=*/false)) {
      throw std::runtime_error("Failed to set pipeline to PLAYING");
    }
    return self;
  }

  Pipeline& deactivate(Pipeline& self) {
    if (!update_pipeline_state(GST_STATE_PAUSED, /*blocking=*/true)) {
      throw std::runtime_error("Failed to set pipeline to PAUSED");
    }
    return self;
  }

  Pipeline& start(Pipeline& self) {
    ensure_bus_watch();
    ensure_shutdown_listener();
    if (!linked_) {
      link_sequential_or_throw();
      linked_ = true;
    }
    if (ShouldLog(ImsdkLogLevel::Debug)) {
      print();
    }
    {
      std::lock_guard<std::mutex> lk(mtx_);
      done_ = false;
      eos_received_ = false;
    }
    if (!update_pipeline_state(GST_STATE_PLAYING, /*blocking=*/false)) {
      throw std::runtime_error("Failed to set pipeline to PLAYING");
    }
    return self;
  }

  Pipeline& wait(Pipeline& self) {
    std::unique_lock<std::mutex> lk(mtx_);
    cv_.wait(lk, [this] { return done_; });
    return self;
  }

  Pipeline& stop(Pipeline& self) {
    if (eos_on_shutdown_) {
      bool should_wait = false;
      {
        std::lock_guard<std::mutex> lk(mtx_);
        should_wait = !eos_received_;
      }

      if (should_wait) {
        gst_element_send_event(pipeline_, gst_event_new_eos());
        std::unique_lock<std::mutex> lk(mtx_);
        cv_.wait(lk, [this] { return eos_received_; });
      }
    }
    stop_internal();
    return self;
  }

  Pipeline& eos(Pipeline& self, bool enabled) {
    eos_on_shutdown_ = enabled;
    return self;
  }

  bool eos() const { return eos_on_shutdown_; }

  void execute(Pipeline& self) {
    start(self);
    wait(self);
    stop(self);
  }

  void link_sequential_or_throw() {
    if (elements_.size() < 2)
      return;
    for (size_t i = 0; i + 1 < elements_.size(); ++i) {
      ElementInfo& up = elements_[i];
      ElementInfo& down = elements_[i + 1];

      if (!try_immediate_link_or_defer(up, down)) {
        if (!has_dynamic_src(up.elem)) {
          std::string dummy;
          if (!has_request_or_sometimes_sink(down.elem, &dummy)) {
            throw std::runtime_error(
              "Failed to link static elements at index " + std::to_string(i) +
              " and " + std::to_string(i + 1));
          }
        }
      }
    }
  }

  std::vector<PendingLinkInfo> pending_links_snapshot() {
    std::vector<PendingLinkInfo> snapshot;
    std::lock_guard<std::mutex> lock(dl_mtx_);
    snapshot.reserve(pending_.size());
    for (const auto& pending_link : pending_) {
      PendingLinkInfo info;
      info.upstream_name =
        pending_link.upstream ? gst_element_get_name(pending_link.upstream)
                              : "<null-upstream>";
      info.downstream_name =
        pending_link.downstream ? gst_element_get_name(pending_link.downstream)
                                : "<null-downstream>";
      info.src_pad_template = pending_link.src_pad_template;
      info.sink_pad_template = pending_link.sink_pad_template;
      info.completed = pending_link.completed;
      snapshot.push_back(std::move(info));
    }
    return snapshot;
  }

  void generate_graph(const std::string& filename) {
    if (!linked_) {
      link_sequential_or_throw();
      linked_ = true;
    }
    auto pending_snapshot = pending_links_snapshot();
    GeneratePipelineGraph(pipeline_, filename, &pending_snapshot);
  }

  void print() {
    if (!linked_) {
      link_sequential_or_throw();
      linked_ = true;
    }
    auto pending_snapshot = pending_links_snapshot();
    PrintPipelineTopology(pipeline_, &pending_snapshot);
  }

  static gboolean bus_watch(GstBus* /*bus*/, GstMessage* msg,
    gpointer user_data) {
    auto* self = static_cast<Impl*>(user_data);
    switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_STATE_CHANGED: {
      if (GST_MESSAGE_SRC(msg) == GST_OBJECT(self->pipeline_)) {
        GstState old_s, new_s, pending;
        gst_message_parse_state_changed(msg, &old_s, &new_s, &pending);
        GstStructure* st =
          gst_structure_new(kMsgPipelineState, "new", G_TYPE_UINT,
            static_cast<guint>(new_s), nullptr);
        g_async_queue_push(self->state_msg_queue_, st);
      }
      break;
    }
    case GST_MESSAGE_ERROR: {
      self->log_unlinked_pads();

      GError* err = nullptr;
      gchar* dbg = nullptr;
      gst_message_parse_error(msg, &err, &dbg);
      if (err)
        g_error_free(err);
      if (dbg)
        g_free(dbg);
      {
        std::lock_guard<std::mutex> lk(self->mtx_);
        self->done_ = true;
        self->cv_.notify_all();
      }
      return G_SOURCE_REMOVE;
    }
    case GST_MESSAGE_EOS: {
      std::lock_guard<std::mutex> lk(self->mtx_);
      self->eos_received_ = true;
      self->done_ = true;
      self->cv_.notify_all();
      return G_SOURCE_REMOVE;
    }
    default:
      break;
    }
    return G_SOURCE_CONTINUE;
  }

  void ensure_bus_watch() {
    if (!bus_) {
      bus_ = gst_element_get_bus(pipeline_);
      if (!bus_)
        throw std::runtime_error("Failed to get bus from pipeline");
    }
    if (!bus_source_) {
      bus_source_ = gst_bus_create_watch(bus_);
      g_source_set_callback(
        bus_source_, reinterpret_cast<GSourceFunc>(bus_watch), this, nullptr);
      g_source_attach(bus_source_, runtime_->MainContext());
    }
  }

  void ensure_shutdown_listener() {
    if (shutdown_listener_id_ == 0) {
      shutdown_listener_id_ = runtime_->AddShutdownListener([this] {
        std::lock_guard<std::mutex> lk(mtx_);
        done_ = true;
        cv_.notify_all();
        });
    }
  }

  bool wait_pipeline_state(GstState state,
    GstClockTime timeout = GST_CLOCK_TIME_NONE) {
    if (!state_msg_queue_)
      return false;
    if (state == GST_STATE_NULL)
      return true;

    const bool finite_timeout = (timeout != GST_CLOCK_TIME_NONE);
    gint64 deadline_us = 0;
    if (finite_timeout) {
      deadline_us = g_get_monotonic_time() + static_cast<gint64>(timeout / 1000);
    }

    while (true) {
      GstStructure* msg = nullptr;
      if (finite_timeout) {
        gint64 remaining_us = deadline_us - g_get_monotonic_time();
        if (remaining_us <= 0)
          return false;

        msg = static_cast<GstStructure*>(g_async_queue_timeout_pop(
          state_msg_queue_, static_cast<guint64>(remaining_us)));
        if (!msg)
          return false;
      }
      else {
        msg = static_cast<GstStructure*>(g_async_queue_pop(state_msg_queue_));
        if (!msg)
          continue;
      }

      if (gst_structure_has_name(msg, kMsgTerminate)) {
        gst_structure_free(msg);
        return false;
      }
      if (gst_structure_has_name(msg, kMsgPipelineState)) {
        guint newstate_uint = 0;

        gst_structure_get_uint(msg, "new", &newstate_uint);
        GstState newstate = static_cast<GstState>(newstate_uint);
        gst_structure_free(msg);

        if (newstate == state)
          return true;
        continue;
      }
      gst_structure_free(msg);
    }
  }

  bool update_pipeline_state(GstState target_state, bool blocking) {
    GstState current = GST_STATE_NULL;
    GstState pending = GST_STATE_VOID_PENDING;
    GstClockTime timeout = (target_state == GST_STATE_NULL) ?
      GST_CLOCK_TIME_NONE : kStateChangeTimeout;

    auto ret = gst_element_get_state(pipeline_, &current, &pending, 0);
    if (ret == GST_STATE_CHANGE_FAILURE)
      return false;

    if (current == target_state)
      return true;

    if (pending == target_state) {
      if (!blocking)
        return false;

      bool ok = wait_pipeline_state(target_state, timeout);
      if (!ok && target_state != GST_STATE_NULL) {
        QIMSDK_LOG_ERROR << "Pipeline state transition timeout while waiting for "
                        << gst_element_state_get_name(target_state)
                        << "; continuing to wait";
        log_unlinked_pads();
        return wait_pipeline_state(target_state, GST_CLOCK_TIME_NONE);
      }
      return ok;
    }

    ret = gst_element_set_state(pipeline_, target_state);
    switch (ret) {
    case GST_STATE_CHANGE_FAILURE:
      return false;
    case GST_STATE_CHANGE_ASYNC:
      if (blocking) {
        ret = gst_element_get_state(pipeline_, nullptr, nullptr, timeout);
        if (ret == GST_STATE_CHANGE_FAILURE)
          return false;
        if (ret == GST_STATE_CHANGE_ASYNC) {
          if (target_state != GST_STATE_NULL) {
            QIMSDK_LOG_ERROR << "Pipeline state transition timeout while setting "
                            << gst_element_state_get_name(target_state)
                            << "; continuing to wait";
            log_unlinked_pads();
            ret = gst_element_get_state(pipeline_, nullptr, nullptr,
              GST_CLOCK_TIME_NONE);
            if (ret == GST_STATE_CHANGE_FAILURE)
              return false;
          }
        }
      }
      break;
    case GST_STATE_CHANGE_NO_PREROLL:
    case GST_STATE_CHANGE_SUCCESS:
    default:
      break;
    }
    if (blocking) {
      bool ok = wait_pipeline_state(target_state, timeout);
      if (!ok && target_state != GST_STATE_NULL) {
        QIMSDK_LOG_ERROR << "Pipeline state transition timeout while waiting for "
                        << gst_element_state_get_name(target_state)
                        << "; continuing to wait";
        log_unlinked_pads();
        ok = wait_pipeline_state(target_state, GST_CLOCK_TIME_NONE);
      }

      if (ok && target_state == GST_STATE_PAUSED) {
        log_unlinked_pads();
      }
      return ok || (target_state == GST_STATE_NULL);
    }

    return true;
  }

  static GstPad* get_static_sink_pad(GstElement* e) {
    if (GstPad* p = gst_element_get_static_pad(e, "sink"))
      return p;

    // try any ALWAYS sink pad
    GstIterator* it = gst_element_iterate_pads(e);
    if (!it)
      return nullptr;

    GValue item = G_VALUE_INIT;
    GstPad* found = nullptr;
    while (gst_iterator_next(it, &item) == GST_ITERATOR_OK) {
      GstPad* pad = GST_PAD(g_value_get_object(&item));
      if (GST_PAD_DIRECTION(pad) == GST_PAD_SINK && GST_PAD_TEMPLATE(pad) &&
        GST_PAD_TEMPLATE(pad)->presence == GST_PAD_ALWAYS) {
        found = GST_PAD(gst_object_ref(pad));
        g_value_unset(&item);
        break;
      }
      g_value_unset(&item);
    }
    gst_iterator_free(it);
    return found;
  }

  static GstPad* request_sink_pad(GstElement* e, const std::string& templ) {
    if (templ.empty())
      return nullptr;
#if GST_CHECK_VERSION(1, 20, 0)
    return gst_element_request_pad_simple(e, templ.c_str());
#else
    return gst_element_get_request_pad(e, templ.c_str());
#endif
  }

  static GstPad* request_src_pad(GstElement* e, const std::string& templ) {
    if (templ.empty())
      return nullptr;
#if GST_CHECK_VERSION(1, 20, 0)
    return gst_element_request_pad_simple(e, templ.c_str());
#else
    return gst_element_get_request_pad(e, templ.c_str());
#endif
  }

  void log_unlinked_pads() {
    LogUnlinkedPadsCaps(pipeline_);
  }

  static bool has_dynamic_src(GstElement* e) {
    GstElementClass* klass = GST_ELEMENT_GET_CLASS(e);
    if (!klass)
      return false;
    for (const GList* it = gst_element_class_get_pad_template_list(klass); it;
      it = it->next) {
      auto* templ = static_cast<GstPadTemplate*>(it->data);
      if (templ->direction == GST_PAD_SRC &&
        (templ->presence == GST_PAD_SOMETIMES ||
          templ->presence == GST_PAD_REQUEST)) {
        return true;
      }
    }
    return false;
  }

  static bool has_request_or_sometimes_sink(GstElement* e,
    std::string* sink_templ_out) {
    if (sink_templ_out)
      sink_templ_out->clear();

    if (GstPad* p = get_static_sink_pad(e)) {
      gst_object_unref(p);
      return true;
    }

    GstElementClass* klass = GST_ELEMENT_GET_CLASS(e);
    if (!klass)
      return false;

    const GList* l = gst_element_class_get_pad_template_list(klass);
    for (const GList* it = l; it; it = it->next) {
      auto* templ = static_cast<GstPadTemplate*>(it->data);
      if (templ->direction == GST_PAD_SINK &&
        (templ->presence == GST_PAD_REQUEST ||
          templ->presence == GST_PAD_SOMETIMES)) {
        if (sink_templ_out && templ->name_template) {
          *sink_templ_out = templ->name_template;
        }
        return true;
      }
    }
    return false;
  }

  bool try_immediate_link_or_defer(ElementInfo& up, ElementInfo& down) {
    std::string sink_tpl;
    (void)has_request_or_sometimes_sink(down.elem, &sink_tpl);

    if (!down.prefer_upstream_src_pad_template.empty()) {
      if (GstPad* srcp =
        request_src_pad(up.elem, down.prefer_upstream_src_pad_template)) {
        GstPad* sinkp = get_static_sink_pad(down.elem);
        bool requested_sink = false;
        if (!sinkp) {
          sinkp = request_sink_pad(down.elem, sink_tpl);
          requested_sink = (sinkp != nullptr);
        }

        if (sinkp) {
          if (!gst_pad_is_linked(sinkp)) {
            if (gst_pad_link(srcp, sinkp) == GST_PAD_LINK_OK) {
              PendingLink pl{};
              pl.upstream = up.elem;
              pl.downstream = down.elem;
              pl.sink_pad_template = sink_tpl;
              pl.src_pad_template = down.prefer_upstream_src_pad_template;
              pl.uses_request_src_pad = true;
              pl.requested_src_pad = srcp;
              pl.uses_request_pad = requested_sink;
              pl.requested_sink_pad = requested_sink ? sinkp : nullptr;
              pl.completed = true;
              if (!requested_sink)
                gst_object_unref(sinkp);
              pending_.push_back(std::move(pl));
              return true;
            }
          }

          if (requested_sink) {
            gst_element_release_request_pad(down.elem, sinkp);
          }
          gst_object_unref(sinkp);
        }

        gst_element_release_request_pad(up.elem, srcp);
        gst_object_unref(srcp);
      }
    }

    if (gst_element_link(up.elem, down.elem))
      return true;

    bool up_dyn = has_dynamic_src(up.elem);
    bool down_dyn = has_request_or_sometimes_sink(down.elem, &sink_tpl);
    if (!up_dyn && !down_dyn)
      return false;

    std::lock_guard<std::mutex> lk(dl_mtx_);

    HandlerTrack* track = nullptr;
    for (auto& ht : handler_tracks_) {
      if (ht.upstream == up.elem) {
        track = &ht;
        break;
      }
    }
    if (!track) {
      gulong id = g_signal_connect(
        up.elem, "pad-added", G_CALLBACK(&Impl::on_pad_added_static), this);
      handler_tracks_.push_back({ up.elem, id, 0 });
      track = &handler_tracks_.back();
    }

    PendingLink pl;
    pl.upstream = up.elem;
    pl.downstream = down.elem;
    pl.sink_pad_template = sink_tpl;
    pl.src_pad_template = down.prefer_upstream_src_pad_template;

    if (!pl.src_pad_template.empty()) {
      if (GstPad* srcp = request_src_pad(up.elem, pl.src_pad_template)) {
        pl.uses_request_src_pad = true;
        pl.requested_src_pad = srcp;

        GstPad* sinkp = get_static_sink_pad(down.elem);
        bool requested_sink = false;
        if (!sinkp) {
          sinkp = request_sink_pad(down.elem, pl.sink_pad_template);
          requested_sink = (sinkp != nullptr);
        }

        if (sinkp) {
          if (!gst_pad_is_linked(sinkp)) {
            if (gst_pad_link(srcp, sinkp) == GST_PAD_LINK_OK) {
              pl.completed = true;
              pl.uses_request_pad = requested_sink;
              pl.requested_sink_pad = requested_sink ? sinkp : nullptr;
              if (!requested_sink)
                gst_object_unref(sinkp);
              pending_.push_back(std::move(pl));
              return true;
            }
          }
          if (requested_sink) {
            gst_element_release_request_pad(down.elem, sinkp);
          }
          gst_object_unref(sinkp);
        }
        gst_element_release_request_pad(up.elem, srcp);
        gst_object_unref(srcp);
        pl.uses_request_src_pad = false;
        pl.requested_src_pad = nullptr;
      }
    }

    pending_.push_back(std::move(pl));
    track->pending_count += 1;
    return false;
  }

  static void on_pad_added_static(GstElement* src, GstPad* new_pad,
    gpointer user_data) {
    static_cast<Impl*>(user_data)->on_pad_added(src, new_pad);
  }

  void on_pad_added(GstElement* src, GstPad* new_pad) {
    if (GST_PAD_DIRECTION(new_pad) != GST_PAD_SRC)
      return;

    std::lock_guard<std::mutex> lk(dl_mtx_);

    for (auto& pl : pending_) {
      if (pl.completed || pl.upstream != src)
        continue;

      GstPad* sink_pad = get_static_sink_pad(pl.downstream);
      bool requested = false;
      if (!sink_pad) {
        sink_pad = request_sink_pad(pl.downstream, pl.sink_pad_template);
        requested = sink_pad != nullptr;
      }
      if (!sink_pad)
        continue;

      if (gst_pad_is_linked(sink_pad)) {
        if (requested) {
          gst_element_release_request_pad(pl.downstream, sink_pad);
          gst_object_unref(sink_pad);
        }
        else {
          gst_object_unref(sink_pad);
        }
        continue;
      }

      GstPadLinkReturn ret = gst_pad_link(new_pad, sink_pad);
      if (ret == GST_PAD_LINK_OK) {
        pl.completed = true;
        pl.uses_request_pad = requested;
        pl.requested_sink_pad = requested ? sink_pad : nullptr;
        if (!requested)
          gst_object_unref(sink_pad);

        for (auto it = handler_tracks_.begin(); it != handler_tracks_.end();
          ++it) {
          if (it->upstream == src) {
            it->pending_count -= 1;
            if (it->pending_count <= 0 && it->handler_id != 0) {
              g_signal_handler_disconnect(src, it->handler_id);
              handler_tracks_.erase(it);
            }
            break;
          }
        }
      }
      else {
        if (requested) {
          gst_element_release_request_pad(pl.downstream, sink_pad);
        }
        gst_object_unref(sink_pad);
      }
    }
  }

  void disconnect_all_pad_added() {
    std::lock_guard<std::mutex> lk(dl_mtx_);
    for (auto& ht : handler_tracks_) {
      if (ht.handler_id != 0 && ht.upstream) {
        g_signal_handler_disconnect(ht.upstream, ht.handler_id);
      }
    }
    handler_tracks_.clear();
  }

  void cleanup_pending_handlers() {
    std::lock_guard<std::mutex> lk(dl_mtx_);
    for (auto& pl : pending_) {
      if (pl.completed && pl.uses_request_pad && pl.downstream &&
        pl.requested_sink_pad) {
        gst_element_release_request_pad(pl.downstream, pl.requested_sink_pad);
        gst_object_unref(pl.requested_sink_pad);
        pl.requested_sink_pad = nullptr;
      }
      if (pl.completed && pl.uses_request_src_pad && pl.upstream &&
        pl.requested_src_pad) {
        gst_element_release_request_pad(pl.upstream, pl.requested_src_pad);
        gst_object_unref(pl.requested_src_pad);
        pl.requested_src_pad = nullptr;
      }
    }
    pending_.erase(
      std::remove_if(pending_.begin(), pending_.end(),
        [](const PendingLink& x) { return x.completed; }),
      pending_.end());
  }

  void stop_internal() {
    if (shutdown_listener_id_ != 0 && runtime_) {
      runtime_->RemoveShutdownListener(shutdown_listener_id_);
      shutdown_listener_id_ = 0;
    }
    disconnect_all_pad_added();
    update_pipeline_state(GST_STATE_NULL, /*blocking=*/true);
    {
      std::lock_guard<std::mutex> lk(mtx_);
      done_ = true;
    }
    cv_.notify_all();
    cleanup_pending_handlers();
    if (bus_source_) {
      g_source_destroy(bus_source_);
      g_source_unref(bus_source_);
      bus_source_ = nullptr;
    }
    if (bus_) {
      gst_object_unref(bus_);
      bus_ = nullptr;
    }
  }
};

Pipeline::Pipeline(const std::string& name)
  : impl_(std::make_unique<Impl>(name)) {
}

Pipeline::~Pipeline() = default;

Pipeline& Pipeline::add(const qti::Element& element) {
  impl_->add_element(element);
  return *this;
}

Pipeline& Pipeline::add_stream_filter(const std::string& name,
  const qti::StreamFilter& caps) {
  impl_->add_stream_filter(name, caps);
  return *this;
}

void Pipeline::link_by_names(const std::vector<std::string>& names) {
  impl_->link_by_names(names);
}

void Pipeline::generate_graph(const std::string& filename) {
  impl_->generate_graph(filename);
}

Pipeline& Pipeline::prepare() { return impl_->prepare(*this); }
Pipeline& Pipeline::activate() { return impl_->activate(*this); }
Pipeline& Pipeline::deactivate() { return impl_->deactivate(*this); }
Pipeline& Pipeline::start() { return impl_->start(*this); }
Pipeline& Pipeline::wait() { return impl_->wait(*this); }
Pipeline& Pipeline::stop() { return impl_->stop(*this); }
Pipeline& Pipeline::eos(bool enabled) { return impl_->eos(*this, enabled); }
bool Pipeline::eos() const { return impl_->eos(); }

void Pipeline::print() { return impl_->print(); }
void Pipeline::execute() { impl_->execute(*this); }

Element Pipeline::get(const std::string& unique_name) {
  auto* raw = static_cast<void*>(impl_->get_element(unique_name));
  return Element(raw, /*add_ref=*/true);
}

void* Pipeline::get_raw_element(const std::string& unique_name) {
  return static_cast<void*>(impl_->get_element(unique_name));
}

void Pipeline::add_by_factory_no_props(const std::string& factory,
  const std::string& unique_name) {
  impl_->add_by_factory_no_props(factory, unique_name);
}

void Pipeline::add_set_one(const std::string& unique_name, const char* prop,
  const char* value) {
  impl_->add_set_one(unique_name, prop, value);
}

void Pipeline::add_set_one(const std::string& unique_name, const char* prop,
  const std::string& value) {
  impl_->add_set_one(unique_name, prop, value);
}

void Pipeline::add_set_one(const std::string& unique_name, const char* prop,
  bool value) {
  impl_->add_set_one(unique_name, prop, value);
}

void Pipeline::add_set_one(const std::string& unique_name, const char* prop,
  int value) {
  impl_->add_set_one(unique_name, prop, value);
}

void Pipeline::add_set_one(const std::string& unique_name, const char* prop,
  unsigned int value) {
  impl_->add_set_one(unique_name, prop, value);
}

void Pipeline::add_set_one(const std::string& unique_name, const char* prop,
  long value) {
  impl_->add_set_one(unique_name, prop, value);
}

void Pipeline::add_set_one(const std::string& unique_name, const char* prop,
  unsigned long value) {
  impl_->add_set_one(unique_name, prop, value);
}

void Pipeline::add_set_one(const std::string& unique_name, const char* prop,
  long long value) {
  impl_->add_set_one(unique_name, prop, value);
}

void Pipeline::add_set_one(const std::string& unique_name, const char* prop,
  unsigned long long value) {
  impl_->add_set_one(unique_name, prop, value);
}

void Pipeline::add_set_one(const std::string& unique_name, const char* prop,
  float value) {
  impl_->add_set_one(unique_name, prop, value);
}

void Pipeline::add_set_one(const std::string& unique_name, const char* prop,
  double value) {
  impl_->add_set_one(unique_name, prop, value);
}

} // namespace qti
