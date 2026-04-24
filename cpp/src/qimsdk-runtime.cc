/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include "qimsdk-runtime.h"

#include <cassert>
#include <csignal>
#include <iostream>

namespace qti {

// -------- Static members --------
std::mutex GstRuntime::s_inst_mtx_;
GstRuntime* GstRuntime::s_inst_ = nullptr;
std::uint32_t GstRuntime::s_refs_ = 0;

// ================= Singleton API =================

GstRuntime& GstRuntime::get_instance() {
  std::lock_guard<std::mutex> lk(s_inst_mtx_);
  if (!s_inst_) {
    s_inst_ = new GstRuntime();
    s_inst_->Start();
  }
  ++s_refs_;
  return *s_inst_;
}

void GstRuntime::release_instance() noexcept {
  std::lock_guard<std::mutex> lk(s_inst_mtx_);
  if (!s_inst_ || s_refs_ == 0) return;
  if (--s_refs_ == 0) {
    s_inst_->Stop();
    delete s_inst_;
    s_inst_ = nullptr;
  }
}

// ================= Public API =================

std::shared_future<void> GstRuntime::ShutdownFuture() const {
  std::lock_guard<std::mutex> lk(shutdown_mtx_);
  return shutdown_future_;
}

bool GstRuntime::IsShuttingDown() const noexcept {
  return shutting_down_.load(std::memory_order_acquire);
}

GMainContext* GstRuntime::MainContext() const noexcept {
  return context_;
}

GstRuntime::ShutdownListenerId
GstRuntime::AddShutdownListener(std::function<void()> cb) {
  if (!cb) return 0;

  if (shutting_down_.load(std::memory_order_acquire) ||
      shutdown_notified_.load(std::memory_order_acquire)) {
    try {
      cb();
    } catch (...) {
    }
    return 0;
  }

  std::lock_guard<std::mutex> lk(listeners_mtx_);
  const auto id = next_listener_id_++;
  listeners_.emplace_back(id, std::move(cb));
  return id;
}

bool GstRuntime::RemoveShutdownListener(ShutdownListenerId id) {
  if (id == 0) return false;
  std::lock_guard<std::mutex> lk(listeners_mtx_);
  for (auto it = listeners_.begin(); it != listeners_.end(); ++it) {
    if (it->first == id) {
      listeners_.erase(it);
      return true;
    }
  }
  return false;
}

void GstRuntime::SetDefaultLogLevel(GstLogLevel level) {
  gst_debug_set_default_threshold(static_cast<::GstDebugLevel>(level));
}

void GstRuntime::SetCategoryLogLevel(const std::string& category,
                                     GstLogLevel level) {
  if (category.empty()) {
    return;
  }

  gst_debug_set_threshold_for_name(category.c_str(),
                                   static_cast<::GstDebugLevel>(level));
}

void GstRuntime::SetImsdkLogLevel(int level) {
  if (level < static_cast<int>(ImsdkLogLevel::Error)) {
    level = static_cast<int>(ImsdkLogLevel::Error);
  }
  if (level > static_cast<int>(ImsdkLogLevel::Debug)) {
    level = static_cast<int>(ImsdkLogLevel::Debug);
  }
  ::qti::SetImsdkLogLevel(static_cast<ImsdkLogLevel>(level));
}

void GstRuntime::SetGstLogMode(ImsdkGstLogMode mode) {
  // Mode is intended to be configured once at startup. Ignore updates after
  // runtime has started and debug hook is installed.
  if (debug_hook_installed_) {
    return;
  }
  gst_log_mode_.store(mode, std::memory_order_release);
}

// ================= Lifecycle =================

GstRuntime::GstRuntime() = default;

GstRuntime::~GstRuntime() {
  if (loop_thread_running_.load(std::memory_order_acquire)) {
    Stop();
  }
}

void GstRuntime::Start() {
  if (!gst_is_initialized()) {
    gst_init(nullptr, nullptr);
  }

  gst_log_mode_.store(GetImsdkGstLogMode(), std::memory_order_release);
  const auto mode = gst_log_mode_.load(std::memory_order_acquire);

  // In QIMSDK-parsed mode, enforce QIMSDK-oriented thresholds.
  // In passthrough mode, leave GST logging configuration untouched
  // (including GST_DEBUG behavior).
  if (mode == ImsdkGstLogMode::ImsdkLog) {
    SetDefaultLogLevel(GstLogLevel::Error);
    ImsdkLogger::ConfigureGstParserCategoryLogLevels();
  }

  InstallDebugHook();

  InitShutdownSync();
  StartMainLoop();
  StartUnixSignalSources();
}

void GstRuntime::Stop() noexcept {
  StopUnixSignalSources();
  StopMainLoop();
  FinalizeShutdownSync();
  RemoveDebugHook();
}

void GstRuntime::InitShutdownSync() {
  std::lock_guard<std::mutex> lk(shutdown_mtx_);
  shutting_down_.store(false, std::memory_order_release);
  shutdown_notified_.store(false, std::memory_order_release);

  shutdown_promise_ = std::make_shared<std::promise<void>>();
  shutdown_future_ = shutdown_promise_->get_future().share();
}

void GstRuntime::FinalizeShutdownSync() {
  {
    std::lock_guard<std::mutex> lk(shutdown_mtx_);
    if (shutdown_promise_) {
      try {
        shutdown_promise_->set_value();
      } catch (...) {
      }
    }
  }

  NotifyShutdownListenersOnce();

  std::lock_guard<std::mutex> lk2(shutdown_mtx_);
  shutdown_promise_.reset();
}

void GstRuntime::StartMainLoop() {
  assert(!context_ && !loop_);

  context_ = g_main_context_new();
  loop_ = g_main_loop_new(context_, FALSE);

  loop_thread_running_.store(true, std::memory_order_release);

  loop_thread_ = std::thread([this] {
    g_main_context_push_thread_default(context_);
    g_main_loop_run(loop_);
    g_main_context_pop_thread_default(context_);
    loop_thread_running_.store(false, std::memory_order_release);
  });
}

void GstRuntime::StopMainLoop() noexcept {
  if (loop_) {
    g_main_loop_quit(loop_);
  }
  if (loop_thread_.joinable()) {
    loop_thread_.join();
  }

  if (loop_) {
    g_main_loop_unref(loop_);
    loop_ = nullptr;
  }
  if (context_) {
    g_main_context_unref(context_);
    context_ = nullptr;
  }
}

// ================= Signals =================

gboolean GstRuntime::UnixSignalCb(gpointer user_data) {
  auto* self = static_cast<GstRuntime*>(user_data);
  self->HandleShutdownSignal();
  return G_SOURCE_REMOVE;
}

void GstRuntime::HandleShutdownSignal() {
  shutting_down_.store(true, std::memory_order_release);

  std::shared_ptr<std::promise<void>> p;
  {
    std::lock_guard<std::mutex> lk(shutdown_mtx_);
    p = shutdown_promise_;
  }

  if (p) {
    try {
      p->set_value();
    } catch (...) {
    }
  }

  NotifyShutdownListenersOnce();
}

void GstRuntime::StartUnixSignalSources_on_loop_thread() {
  if (sig_src_int_ == 0) {
    GSource* s = g_unix_signal_source_new(SIGINT);
    g_source_set_callback(s, (GSourceFunc)&GstRuntime::UnixSignalCb,
                          this, nullptr);
    sig_src_int_ = g_source_attach(s, context_);
    g_source_unref(s);
  }

  if (sig_src_term_ == 0) {
    GSource* s = g_unix_signal_source_new(SIGTERM);
    g_source_set_callback(s, (GSourceFunc)&GstRuntime::UnixSignalCb,
                          this, nullptr);
    sig_src_term_ = g_source_attach(s, context_);
    g_source_unref(s);
  }
}

void GstRuntime::StopUnixSignalSources_on_loop_thread() {
  if (sig_src_int_ != 0) {
    if (GSource* s =
            g_main_context_find_source_by_id(context_, sig_src_int_)) {
      g_source_destroy(s);
    }
    sig_src_int_ = 0;
  }

  if (sig_src_term_ != 0) {
    if (GSource* s =
            g_main_context_find_source_by_id(context_, sig_src_term_)) {
      g_source_destroy(s);
    }
    sig_src_term_ = 0;
  }
}

void GstRuntime::StartUnixSignalSources() {
  if (!loop_thread_running_.load(std::memory_order_acquire)) return;

  g_main_context_invoke(
      context_,
      +[](gpointer ud) -> gboolean {
        static_cast<GstRuntime*>(ud)
            ->StartUnixSignalSources_on_loop_thread();
        return G_SOURCE_REMOVE;
      },
      this);
}

void GstRuntime::StopUnixSignalSources() {
  if (!context_) return;

  g_main_context_invoke(
      context_,
      +[](gpointer ud) -> gboolean {
        static_cast<GstRuntime*>(ud)
            ->StopUnixSignalSources_on_loop_thread();
        return G_SOURCE_REMOVE;
      },
      this);
}

// ================= Listeners =================

void GstRuntime::InstallDebugHook() {
  if (debug_hook_installed_) {
    return;
  }

  gst_debug_remove_log_function(gst_debug_log_default);
  gst_debug_add_log_function(&GstRuntime::DebugLogFunction, this, nullptr);
  debug_hook_installed_ = true;
}

void GstRuntime::RemoveDebugHook() {
  if (!debug_hook_installed_) {
    return;
  }

  gst_debug_remove_log_function(&GstRuntime::DebugLogFunction);
  gst_debug_add_log_function(gst_debug_log_default, nullptr, nullptr);
  debug_hook_installed_ = false;
}

void GstRuntime::DebugLogFunction(GstDebugCategory* category,
                                  ::GstDebugLevel level,
                                  const gchar* file,
                                  const gchar* function,
                                  gint line,
                                  GObject* object,
                                  GstDebugMessage* message,
                                  gpointer user_data) {
  auto* self = static_cast<GstRuntime*>(user_data);
  if (!self) {
    gst_debug_log_default(category, level, file, function, line,
                          object, message, nullptr);
    return;
  }

  if (self->gst_log_mode_.load(std::memory_order_acquire) ==
      ImsdkGstLogMode::GstLog) {
    gst_debug_log_default(category, level, file, function, line,
                          object, message, nullptr);
    return;
  }

  ImsdkLogger::ParseAndPrintGstLog(level, category, object, message);
}

void GstRuntime::NotifyShutdownListenersOnce() {
  bool expected = false;
  if (!shutdown_notified_.compare_exchange_strong(
          expected, true, std::memory_order_acq_rel)) {
    return;
  }

  std::vector<std::function<void()>> cbs;

  {
    std::lock_guard<std::mutex> lk(listeners_mtx_);
    cbs.reserve(listeners_.size());
    for (auto& p : listeners_) {
      cbs.push_back(p.second);
    }
  }

  for (auto& cb : cbs) {
    try {
      cb();
    } catch (...) {
    }
  }
}

}  // namespace qti
