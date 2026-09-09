/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <stdexcept>

#include <gst/gst.h>

#include <qti/qimsdk-buffer.h>

namespace qti {

struct Buffer::Impl {
  GstBuffer* buf_ = nullptr;
  GstMapInfo map_ = {};

  Impl() = default;

  bool mapped() const { return map_.memory != nullptr; }

  // Map the payload with at least `flags`, upgrading an existing weaker
  // mapping if needed. Returns the mapped data pointer, or nullptr on failure.
  uint8_t* map(GstMapFlags flags) {
    if (mapped()) {
      if ((map_.flags & flags) == flags) return map_.data;  // already enough
      unmap();  // upgrade, e.g. READ -> WRITE
    }
    if (!buf_ || !gst_buffer_map(buf_, &map_, flags)) return nullptr;
    return map_.data;
  }

  void unmap() {
    if (mapped()) {
      gst_buffer_unmap(buf_, &map_);
      map_ = {};
    }
  }

  void reset() {
    unmap();
    if (buf_) {
      gst_buffer_unref(buf_);
      buf_ = nullptr;
    }
  }

  ~Impl() { reset(); }
};

Buffer::Buffer() : impl_(new Impl) {}

Buffer::Buffer(size_t size) : impl_(new Impl) {
  impl_->buf_ = gst_buffer_new_allocate(nullptr, size, nullptr);
  if (!impl_->buf_) throw std::bad_alloc();
}

Buffer::Buffer(void* gst_buffer_opaque) : impl_(new Impl) {
  // Takes ownership of the reference held by the caller. Writability, size and
  // timing are queried from the buffer on demand, never declared here.
  impl_->buf_ = static_cast<GstBuffer*>(gst_buffer_opaque);
}

uint8_t* Buffer::data() {
  // Mutable access requires an exclusively-owned buffer; refuse rather than
  // hand out a pointer into memory shared with the rest of the pipeline.
  if (!is_writable()) return nullptr;
  return impl_->map(GST_MAP_WRITE);
}
const uint8_t* Buffer::data() const { return impl_->map(GST_MAP_READ); }

size_t Buffer::size() const {
  return valid() ? gst_buffer_get_size(impl_->buf_) : 0;
}

void Buffer::resize(size_t n) {
  if (!is_writable()) {
    throw std::runtime_error("Buffer::resize allowed only for writable buffers");
  }

  // Reallocate via a fresh buffer, carrying timestamps/flags across.
  impl_->unmap();
  GstBuffer* grown = gst_buffer_new_allocate(nullptr, n, nullptr);
  if (!grown) throw std::bad_alloc();
  gst_buffer_copy_into(grown, impl_->buf_, GST_BUFFER_COPY_METADATA, 0, static_cast<gsize>(-1));

  gst_buffer_unref(impl_->buf_);
  impl_->buf_ = grown;
}

void Buffer::set_pts(uint64_t ns) {
  if (valid()) GST_BUFFER_PTS(impl_->buf_) = ns;
}
void Buffer::set_dts(uint64_t ns) {
  if (valid()) GST_BUFFER_DTS(impl_->buf_) = ns;
}
void Buffer::set_duration(uint64_t ns) {
  if (valid()) GST_BUFFER_DURATION(impl_->buf_) = ns;
}
uint64_t Buffer::pts() const {
  return valid() ? GST_BUFFER_PTS(impl_->buf_) : GST_CLOCK_TIME_NONE;
}
uint64_t Buffer::dts() const {
  return valid() ? GST_BUFFER_DTS(impl_->buf_) : GST_CLOCK_TIME_NONE;
}
uint64_t Buffer::duration() const {
  return valid() ? GST_BUFFER_DURATION(impl_->buf_) : GST_CLOCK_TIME_NONE;
}

bool Buffer::is_writable() const {
  return valid() && gst_buffer_is_writable(impl_->buf_);
}
bool Buffer::is_readonly() const { return valid() && !is_writable(); }

bool Buffer::make_writable() {
  if (!valid()) return false;
  // make_writable may substitute a fresh copy, so drop any active mapping and
  // let the caller re-fetch data() afterwards.
  impl_->unmap();
  impl_->buf_ = gst_buffer_make_writable(impl_->buf_);
  return valid();
}

bool Buffer::valid() const { return impl_->buf_ != nullptr; }

void* Buffer::get_raw_buffer() {
  impl_->unmap();
  GstBuffer* out = impl_->buf_;
  impl_->buf_ = nullptr;
  return out;  // ownership -> caller
}

Buffer::Buffer(Buffer&& other) noexcept : impl_(std::move(other.impl_)) {}

Buffer& Buffer::operator=(Buffer&& other) noexcept {
  if (this != &other) impl_ = std::move(other.impl_);
  return *this;
}

Buffer::~Buffer() = default;

}  // namespace qti
