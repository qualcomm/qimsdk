/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#pragma once

#include <memory>
#include <cstddef>
#include <cstdint>

namespace qti {

class Buffer {
 public:
  // Create an empty buffer wrapper.
  Buffer();
  // Allocate a buffer with the requested payload size.
  explicit Buffer(size_t size);

  // Wrap a readable GstSample and expose its payload/metadata as Buffer.
  static Buffer from_readable_sample(void* gst_sample_opaque);

  // Mutable payload access.
  uint8_t* data();
  // Immutable payload access.
  const uint8_t* data() const;

  // Current payload size in bytes.
  size_t size() const;
  // Resize payload storage.
  void resize(size_t n);

  // Set presentation timestamp (nanoseconds).
  void set_pts(uint64_t ns);
  // Set decode timestamp (nanoseconds).
  void set_dts(uint64_t ns);
  // Set buffer duration (nanoseconds).
  void set_duration(uint64_t ns);

  // Read presentation timestamp (nanoseconds).
  uint64_t pts() const;
  // Read decode timestamp (nanoseconds).
  uint64_t dts() const;
  // Read buffer duration (nanoseconds).
  uint64_t duration() const;

  // Returns true when underlying memory can be modified in-place.
  bool is_writable() const;
  // Returns true when underlying memory is read-only.
  bool is_readonly() const;
  // Returns true when this wrapper currently references valid data.
  bool valid() const;

  // Transfer ownership of underlying GstBuffer to caller.
  void* take_gst_buffer();
  // Prepare buffer storage for appsrc producer callbacks.
  void refill_for_appsrc(size_t n);
  // Rebind this wrapper to a sample payload (internal use).
  void wrap_from_sample(void* sample);

  Buffer(const Buffer&) = delete;
  Buffer& operator=(const Buffer&) = delete;

  Buffer(Buffer&&) noexcept;
  Buffer& operator=(Buffer&&) noexcept;

  ~Buffer();

  friend class AppSrc;
  friend class AppSink;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace qti
