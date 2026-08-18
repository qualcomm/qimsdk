/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#pragma once

#include <gst/gst.h>
#include <glib-unix.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "qimsdk-logger-internal.h"

namespace qti {

class GstRuntime final {
public:
  // -------- Singleton API --------
  static GstRuntime& get_instance();
  static void release_instance() noexcept;

  GstRuntime(const GstRuntime&) = delete;
  GstRuntime& operator=(const GstRuntime&) = delete;
  GstRuntime(GstRuntime&&) = delete;
  GstRuntime& operator=(GstRuntime&&) = delete;

  using ShutdownListenerId = std::uint64_t;

  std::shared_future<void> ShutdownFuture() const;
  bool IsShuttingDown() const noexcept;
  GMainContext* MainContext() const noexcept;

  ShutdownListenerId AddShutdownListener(std::function<void()> cb);
  bool RemoveShutdownListener(ShutdownListenerId id);

  void SetDefaultLogLevel(GstLogLevel level);
  void SetCategoryLogLevel(const std::string& category, GstLogLevel level);
  void SetImsdkLogLevel(int level);
  void SetGstLogMode(ImsdkGstLogMode mode);

private:
  GstRuntime();
  ~GstRuntime();

  void Start();
  void Stop() noexcept;

  void InitShutdownSync();
  void FinalizeShutdownSync();

  void StartMainLoop();
  void StopMainLoop() noexcept;

  void StartUnixSignalSources_on_loop_thread();
  void StopUnixSignalSources_on_loop_thread();

  void StartUnixSignalSources();
  void StopUnixSignalSources();

  static gboolean UnixSignalCb(gpointer user_data);
  void HandleShutdownSignal();

  void NotifyShutdownListenersOnce();

  void InstallDebugHook();
  void RemoveDebugHook();

  static void DebugLogFunction(GstDebugCategory* category,
                               ::GstDebugLevel level,
                               const gchar* file,
                               const gchar* function,
                               gint line,
                               GObject* object,
                               GstDebugMessage* message,
                               gpointer user_data);

private:
  // -------- Static singleton state --------
  static std::mutex s_inst_mtx_;
  static GstRuntime* s_inst_;
  static std::uint32_t s_refs_;

  // -------- Runtime state --------
  std::atomic<bool> shutting_down_{ false };

  GMainContext* context_ = nullptr;
  GMainLoop* loop_ = nullptr;
  std::thread loop_thread_;
  std::atomic<bool> loop_thread_running_{ false };

  guint sig_src_int_ = 0;
  guint sig_src_term_ = 0;

  mutable std::mutex shutdown_mtx_;
  std::shared_ptr<std::promise<void>> shutdown_promise_;
  std::shared_future<void> shutdown_future_;

  std::mutex listeners_mtx_;
  std::vector<std::pair<ShutdownListenerId, std::function<void()>>> listeners_;
  std::atomic<bool> shutdown_notified_{ false };
  ShutdownListenerId next_listener_id_{ 1 };

  bool debug_hook_installed_ = false;
  std::atomic<ImsdkGstLogMode> gst_log_mode_{ ImsdkGstLogMode::ImsdkLog };
};

}  // namespace qti
