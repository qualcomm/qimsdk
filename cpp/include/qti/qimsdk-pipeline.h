/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#pragma once

#include <memory>
#include <string>
#include <vector>
#include <utility>

namespace qti {

class Element;
class StreamFilter;

// Owns and controls a GStreamer pipeline graph.
class Pipeline {
 public:
  // Create an empty pipeline with a unique name.
  explicit Pipeline(const std::string& name);

  // Create pipeline from configuration and assign a name.
  Pipeline(const std::string& name, const std::string& config);

  // Release owned implementation state.
  ~Pipeline();

  Pipeline(const Pipeline&) = delete;
  Pipeline& operator=(const Pipeline&) = delete;

  Pipeline(Pipeline&&) noexcept;
  Pipeline& operator=(Pipeline&&) noexcept;

  // Add an element by factory and unique instance name, optionally with properties.
  template <typename... Rest>
  Pipeline& add(const std::string& factory, const std::string& unique_name,
                Rest&&... rest) {
    add_by_factory_no_props(factory, unique_name);
    if constexpr (sizeof...(rest) > 0) {
      add_set_props(unique_name, std::forward<Rest>(rest)...);
    }
    return *this;
  }

  // Add an already constructed wrapper element.
  Pipeline& add(const qti::Element& element);

  // Add an explicit stream filter for an existing element name.
  Pipeline& add_stream_filter(const std::string& unique_name,
                              const qti::StreamFilter& caps);

  // Link a chain of element names in order.
  template <typename... Names>
  Pipeline& link(Names&&... names) {
    std::vector<std::string> v{std::forward<Names>(names)...};
    link_by_names(v);
    return *this;
  }

  // Prepare pipeline resources before activation.
  Pipeline& prepare();
  // Activate pipeline elements.
  Pipeline& activate();
  // Deactivate pipeline elements.
  Pipeline& deactivate();
  // Start streaming.
  Pipeline& start();
  // Block until EOS/error/stop.
  Pipeline& wait();
  // Stop streaming.
  Pipeline& stop();
  // Enable or disable EOS handling policy.
  Pipeline& eos(bool enabled);
  // Return current EOS handling policy.
  bool eos() const;

  // Execute convenience lifecycle for configured pipeline.
  void execute();

  // Generate a draw.io graph of the current pipeline topology.
  void generate_graph(const std::string& filename);

  // Get an element wrapper by unique name.
  Element get(const std::string& unique_name);

  // Get a typed wrapper by unique name.
  template <typename T>
  T get(const std::string& unique_name) {
    void* raw = get_raw_element(unique_name);
    return T(raw);
  }

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;

  void add_by_factory_no_props(const std::string& factory,
                               const std::string& unique_name);

  void add_set_one(const std::string& unique_name, const char* prop,
                   const char* value);
  void add_set_one(const std::string& unique_name, const char* prop,
                   const std::string& value);
  void add_set_one(const std::string& unique_name, const char* prop, bool value);
  void add_set_one(const std::string& unique_name, const char* prop, int value);
  void add_set_one(const std::string& unique_name, const char* prop,
                   unsigned int value);
  void add_set_one(const std::string& unique_name, const char* prop, long value);
  void add_set_one(const std::string& unique_name, const char* prop,
                   unsigned long value);
  void add_set_one(const std::string& unique_name, const char* prop,
                   long long value);
  void add_set_one(const std::string& unique_name, const char* prop,
                   unsigned long long value);
  void add_set_one(const std::string& unique_name, const char* prop,
                   float value);
  void add_set_one(const std::string& unique_name, const char* prop,
                   double value);

  void print();

  void link_by_names(const std::vector<std::string>& names);

  void* get_raw_element(const std::string& unique_name);

  inline void add_set_props(const std::string& /*unique_name*/) {}

  template <typename Value, typename... Rest>
  void add_set_props(const std::string& unique_name, const char* prop,
                     Value&& value, Rest&&... rest) {
    add_set_one(unique_name, prop, std::forward<Value>(value));
    if constexpr (sizeof...(rest) > 0) {
      add_set_props(unique_name, std::forward<Rest>(rest)...);
    }
  }

  template <typename... Rest>
  void add_set_props(const std::string& /*unique_name*/, Rest&&...) {
    static_assert(sizeof...(Rest) == 0,
                  "Pipeline::add(...) property list must be pairs: prop, "
                  "value, prop, value, ...");
  }
};

} // namespace qti
