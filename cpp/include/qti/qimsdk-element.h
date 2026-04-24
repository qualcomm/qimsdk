/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#pragma once

#include <initializer_list>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace qti {

class StreamFilter;
class Pipeline;
class AppSink;
class AppSrc;
class Port;
class CamSrc;
class MLPreprocessBase;
class MLVConverter;
class MLPostprocessBase;
class MLPostprocess;
class MLVideoTFLiteBin;
class MLVideoQNNBin;
class MLVideoSNPEBin;
class MLVideoONNXBin;

class Element {
 public:
  // Create an element instance from a GStreamer factory and optional name.
  explicit Element(const std::string& factory, const std::string& name = {});
  // Release element references and owned implementation state.
  virtual ~Element();

  Element(const Element&) = delete;
  Element& operator=(const Element&) = delete;

  Element(Element&&) noexcept;
  Element& operator=(Element&&) noexcept;

  // Set one or more GStreamer properties on this element.
  template <typename Value, typename... Rest>
  Element& set(const char* prop, Value&& value, Rest&&... rest) {
    set_one(prop, std::forward<Value>(value));
    if constexpr (sizeof...(rest) > 0) {
      set(std::forward<Rest>(rest)...);
    }
    return *this;
  }

  // Set one or more GStreamer properties on this element.
  template <typename Value, typename... Rest>
  Element& set(const std::string& prop, Value&& value, Rest&&... rest) {
    return set(prop.c_str(), std::forward<Value>(value),
               std::forward<Rest>(rest)...);
  }

  // Transition element to inactive state when supported.
  Element& deactivate();
  // Stop element processing when supported.
  Element& stop();
  // Synchronize state with parent pipeline.
  Element& sync();

  // Link this element to a downstream element.
  Element& link(Element& downstream, const std::string& src_pad = "src",
                const std::string& sink_pad = "sink");

  // Unlink this element from a downstream element.
  Element& unlink(Element& downstream, const std::string& src_pad = "src",
                  const std::string& sink_pad = "sink");

  // Unlink all links from the selected source pad.
  Element& unlink(const std::string& src_pad = "src");

  // Access a sink pad by index.
  Port input(unsigned int id);
  // Access a sink pad by name/type and index.
  Port input(const std::string& name_or_type, unsigned int id);

  // Access a source pad by index.
  Port output(unsigned int id);
  // Access a source pad by name/type and index.
  Port output(const std::string& name_or_type, unsigned int id);

 protected:
  void* get_raw_gst_element() const;

  friend class qti::Pipeline;
  friend class qti::AppSink;
  friend class qti::AppSrc;
  friend class qti::Port;
  friend class qti::CamSrc;
  friend class qti::MLPreprocessBase;
  friend class qti::MLVConverter;
  friend class qti::MLPostprocessBase;
  friend class qti::MLPostprocess;
  friend class qti::MLVideoTFLiteBin;
  friend class qti::MLVideoQNNBin;
  friend class qti::MLVideoSNPEBin;
  friend class qti::MLVideoONNXBin;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;

  explicit Element(void* existing_gst_elem, bool add_ref);

  void set_one(const char* prop, const char* value);
  void set_one(const char* prop, const std::string& value);
  void set_one(const char* prop, bool value);
  void set_one(const char* prop, int value);
  void set_one(const char* prop, unsigned int value);
  void set_one(const char* prop, long value);
  void set_one(const char* prop, unsigned long value);
  void set_one(const char* prop, long long value);
  void set_one(const char* prop, unsigned long long value);
  void set_one(const char* prop, float value);
  void set_one(const char* prop, double value);

  void set_one(const char* prop, const qti::StreamFilter& caps);

  template <typename Enum,
            typename = std::enable_if_t<std::is_enum_v<std::decay_t<Enum>>>>
  void set_one(const char* prop, Enum value) {
      set_one(prop, static_cast<int>(value));
  }

  void link_internal(Element& downstream, const std::string& src_pad,
                     const std::string& sink_pad);
  void unlink_internal(Element& downstream, const std::string& src_pad,
                       const std::string& sink_pad);
  void unlink_internal(const std::string& src_pad);
};

class Port {
 public:
  Port(const Port&) = delete;
  Port& operator=(const Port&) = delete;
  Port(Port&&) noexcept;
  Port& operator=(Port&&) noexcept;
  ~Port();

  // Set one or more pad properties.
  template <typename Value, typename... Rest>
  Port& set(const char* prop, Value&& value, Rest&&... rest) {
    set_one(prop, std::forward<Value>(value));
    if constexpr (sizeof...(rest) > 0) {
      set(std::forward<Rest>(rest)...);
    }
    return *this;
  }
  // Set one or more pad properties.
  template <typename Value, typename... Rest>
  Port& set(const std::string& prop, Value&& value, Rest&&... rest) {
    return set(prop.c_str(), std::forward<Value>(value),
               std::forward<Rest>(rest)...);
  }

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;

  Port(void* gst_elem, bool is_sink, const std::string& name, unsigned int id);
  Port(void* gst_elem, bool is_sink, unsigned int id);
  friend class qti::Element;

  void set_one(const char* prop, const char* value);
  void set_one(const char* prop, const std::string& value);
  void set_one(const char* prop, bool value);
  void set_one(const char* prop, int value);
  void set_one(const char* prop, unsigned int value);
  void set_one(const char* prop, long value);
  void set_one(const char* prop, unsigned long value);
  void set_one(const char* prop, long long value);
  void set_one(const char* prop, unsigned long long value);
  void set_one(const char* prop, float value);
  void set_one(const char* prop, double value);
  void set_one(const char* prop, const std::initializer_list<int>& vals);
  void set_one(const char* prop, const std::vector<int>& vals);
};

} // namespace qti
