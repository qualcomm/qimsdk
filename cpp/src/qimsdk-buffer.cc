/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <stdexcept>

#include <gst/gst.h>

#include <qti/qimsdk-buffer.h>

namespace qti {

struct Buffer::Impl {
  enum class Origin { Empty, ForAppSrc, FromAppSink };

  Origin origin_ = Origin::Empty;
  GstBuffer* gst_buf_ = nullptr;     // For AppSrc
  GstSample* gst_sample_ = nullptr;  // From AppSink

  GstMapInfo map_{};
  bool mapped_ = false;
  bool writable_ = false;

  // Timing (ns)
  uint64_t pts_ns_ = GST_CLOCK_TIME_NONE;
  uint64_t dts_ns_ = GST_CLOCK_TIME_NONE;
  uint64_t dur_ns_ = GST_CLOCK_TIME_NONE;

  Impl() = default;

  void clear() {
    if (mapped_) {
      if (gst_buf_) {
        gst_buffer_unmap(gst_buf_, &map_);
      }
      else if (gst_sample_) {
        GstBuffer* b = gst_sample_get_buffer(gst_sample_);
        if (b) gst_buffer_unmap(b, &map_);
      }
      mapped_ = false;
    }
    if (gst_buf_) {
      gst_buffer_unref(gst_buf_);
      gst_buf_ = nullptr;
    }
    if (gst_sample_) {
      gst_sample_unref(gst_sample_);
      gst_sample_ = nullptr;
    }
    origin_ = Origin::Empty;
    writable_ = false;
    pts_ns_ = dts_ns_ = dur_ns_ = GST_CLOCK_TIME_NONE;
  }

  ~Impl() { clear(); }
};

Buffer::Buffer() : impl_(new Impl) {}

Buffer::Buffer(size_t size) : impl_(new Impl) {
  impl_->origin_ = Impl::Origin::ForAppSrc;
  impl_->gst_buf_ = gst_buffer_new_allocate(nullptr, size, nullptr);
  if (!impl_->gst_buf_) throw std::bad_alloc();

  if (!gst_buffer_map(impl_->gst_buf_, &impl_->map_, GST_MAP_WRITE)) {
    gst_buffer_unref(impl_->gst_buf_);
    impl_->gst_buf_ = nullptr;
    throw std::runtime_error("Buffer: gst_buffer_map(GST_MAP_WRITE) failed");
  }
  impl_->mapped_ = true;
  impl_->writable_ = true;
}

Buffer Buffer::from_readable_sample(void* gst_sample_opaque) {
  Buffer out;
  out.wrap_from_sample(gst_sample_opaque);
  return out;
}

uint8_t* Buffer::data() {
  if (!impl_->mapped_ || !impl_->writable_) return nullptr;
  return impl_->map_.data;
}
const uint8_t* Buffer::data() const {
  if (!impl_->mapped_) return nullptr;
  return impl_->map_.data;
}
size_t Buffer::size() const { return impl_->mapped_ ? impl_->map_.size : 0; }

void Buffer::resize(size_t n) {
  if (impl_->origin_ != Impl::Origin::ForAppSrc) {
    throw std::runtime_error("Buffer::resize allowed only for ForAppSrc buffers");
  }

  // Realloc by creating a new GstBuffer
  if (impl_->mapped_) {
    gst_buffer_unmap(impl_->gst_buf_, &impl_->map_);
    impl_->mapped_ = false;
  }

  if (impl_->gst_buf_)
    gst_buffer_unref(impl_->gst_buf_);

  impl_->gst_buf_ = gst_buffer_new_allocate(nullptr, n, nullptr);

  if (!impl_->gst_buf_)
    throw std::bad_alloc();

  if (!gst_buffer_map(impl_->gst_buf_, &impl_->map_, GST_MAP_WRITE)) {
    gst_buffer_unref(impl_->gst_buf_);
    impl_->gst_buf_ = nullptr;
    throw std::runtime_error("Buffer::resize: gst_buffer_map failed");
  }

  impl_->mapped_ = true;
  impl_->writable_ = true;
}

void Buffer::set_pts(uint64_t ns) { impl_->pts_ns_ = ns; }
void Buffer::set_dts(uint64_t ns) { impl_->dts_ns_ = ns; }
void Buffer::set_duration(uint64_t ns) { impl_->dur_ns_ = ns; }
uint64_t Buffer::pts() const { return impl_->pts_ns_; }
uint64_t Buffer::dts() const { return impl_->dts_ns_; }
uint64_t Buffer::duration() const { return impl_->dur_ns_; }

bool Buffer::is_writable() const { return impl_->writable_; }
bool Buffer::is_readonly() const {
  return !impl_->writable_ && impl_->origin_ == Impl::Origin::FromAppSink;
}
bool Buffer::valid() const {
  return (impl_->origin_ == Impl::Origin::ForAppSrc && impl_->gst_buf_) ||
    (impl_->origin_ == Impl::Origin::FromAppSink && impl_->gst_sample_);
}

void* Buffer::take_gst_buffer() {
  // ForAppSrc: keep existing behavior
  if (impl_->origin_ == Impl::Origin::ForAppSrc && impl_->gst_buf_) {
    if (impl_->mapped_) {
      gst_buffer_unmap(impl_->gst_buf_, &impl_->map_);
      impl_->mapped_ = false;
    }
    if (impl_->pts_ns_ != GST_CLOCK_TIME_NONE)
      GST_BUFFER_PTS(impl_->gst_buf_) = impl_->pts_ns_;
    if (impl_->dts_ns_ != GST_CLOCK_TIME_NONE)
      GST_BUFFER_DTS(impl_->gst_buf_) = impl_->dts_ns_;
    if (impl_->dur_ns_ != GST_CLOCK_TIME_NONE)
      GST_BUFFER_DURATION(impl_->gst_buf_) = impl_->dur_ns_;

    GstBuffer* out = impl_->gst_buf_;
    impl_->gst_buf_ = nullptr;
    impl_->origin_ = Impl::Origin::Empty;
    impl_->writable_ = false;
    return out;  // ownership -> caller
  }

  // FromAppSink: zero-copy extract underlying GstBuffer
  if (impl_->origin_ == Impl::Origin::FromAppSink && impl_->gst_sample_) {
    GstBuffer* b = gst_sample_get_buffer(impl_->gst_sample_);
    if (!b)
      return nullptr;

    if (impl_->mapped_) {
      gst_buffer_unmap(b, &impl_->map_);
      impl_->mapped_ = false;
    }

    gst_buffer_ref(b);

    if (impl_->pts_ns_ != GST_CLOCK_TIME_NONE)
      GST_BUFFER_PTS(b) = impl_->pts_ns_;
    if (impl_->dts_ns_ != GST_CLOCK_TIME_NONE)
      GST_BUFFER_DTS(b) = impl_->dts_ns_;
    if (impl_->dur_ns_ != GST_CLOCK_TIME_NONE)
      GST_BUFFER_DURATION(b) = impl_->dur_ns_;

    gst_sample_unref(impl_->gst_sample_);
    impl_->gst_sample_ = nullptr;

    impl_->origin_ = Impl::Origin::Empty;
    impl_->writable_ = false;

    return b;  // ownership -> caller
  }

  return nullptr;
}

void Buffer::refill_for_appsrc(size_t n) {
  if (impl_->origin_ == Impl::Origin::Empty) {
    impl_->origin_ = Impl::Origin::ForAppSrc;
  }

  if (impl_->origin_ != Impl::Origin::ForAppSrc) {
    impl_->clear();
    impl_->origin_ = Impl::Origin::ForAppSrc;
  }

  if (impl_->mapped_) {
    gst_buffer_unmap(impl_->gst_buf_, &impl_->map_);
    impl_->mapped_ = false;
  }

  if (impl_->gst_buf_) {
    gst_buffer_unref(impl_->gst_buf_);
    impl_->gst_buf_ = nullptr;
  }

  impl_->gst_buf_ = gst_buffer_new_allocate(nullptr, n, nullptr);
  if (!impl_->gst_buf_)
    throw std::bad_alloc();

  if (!gst_buffer_map(impl_->gst_buf_, &impl_->map_, GST_MAP_WRITE)) {
    gst_buffer_unref(impl_->gst_buf_);
    impl_->gst_buf_ = nullptr;
    throw std::runtime_error("Buffer::refill_for_appsrc: map failed");
  }

  impl_->mapped_ = true;
  impl_->writable_ = true;
}

void Buffer::wrap_from_sample(void* sample) {
  impl_->clear();
  impl_->origin_ = Impl::Origin::FromAppSink;
  impl_->gst_sample_ = static_cast<GstSample*>(sample);

  GstBuffer* b = gst_sample_get_buffer(impl_->gst_sample_);
  if (!b || !gst_buffer_map(b, &impl_->map_, GST_MAP_READ))
    return;

  impl_->mapped_ = true;
  impl_->writable_ = false;

  // Copy timing values (if present)
  if (GST_BUFFER_PTS_IS_VALID(b))
    impl_->pts_ns_ = GST_BUFFER_PTS(b);
  if (GST_BUFFER_DTS_IS_VALID(b))
    impl_->dts_ns_ = GST_BUFFER_DTS(b);
  if (GST_BUFFER_DURATION_IS_VALID(b))
    impl_->dur_ns_ = GST_BUFFER_DURATION(b);
}

Buffer::Buffer(Buffer&& other) noexcept : impl_(std::move(other.impl_)) {}

Buffer& Buffer::operator=(Buffer&& other) noexcept {
  if (this != &other) impl_ = std::move(other.impl_);
  return *this;
}

Buffer::~Buffer() = default;

}  // namespace qti
