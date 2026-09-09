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
  // Allocate a writable buffer with the requested payload size.
  explicit Buffer(size_t size);

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
  // Ensure the payload is writable, copying the underlying memory if it is
  // currently shared. Returns true when the buffer is writable afterwards.
  // Any pointer previously returned by data() is invalidated; re-fetch it.
  bool make_writable();
  // Returns true when this wrapper currently references valid data.
  bool valid() const;

  Buffer(const Buffer&) = delete;
  Buffer& operator=(const Buffer&) = delete;

  Buffer(Buffer&&) noexcept;
  Buffer& operator=(Buffer&&) noexcept;

  ~Buffer();

 private:
  // Take ownership of a GstBuffer. Used by element wrappers to hand samples
  // to the application.
  explicit Buffer(void* gst_buffer_opaque);
  // Transfer ownership of the underlying GstBuffer to the caller.
  void* get_raw_buffer();

  friend class AppSrc;
  friend class AppSink;

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace qti
