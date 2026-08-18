/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include "qti/qimsdk-stream-filter.h"
#include "qimsdk-runtime.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <gst/gst.h>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace qti {

// ============================================================================
// helpers
// ============================================================================
namespace {

inline std::string to_lower(std::string s) {
  for (auto& c : s)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

inline std::string to_upper(std::string s) {
  for (auto& c : s)
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

inline std::string trim(std::string s) {
  auto not_space = [](unsigned char c) { return !std::isspace(c); };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), [&](unsigned char c) {
    return not_space(c);
    }));
  s.erase(std::find_if(s.rbegin(), s.rend(),
    [&](unsigned char c) { return not_space(c); })
    .base(),
    s.end());
  return s;
}

inline bool is_bayer_fmt(const std::string& fmt_up) {
  return (fmt_up == "BGGR" || fmt_up == "RGGB" || fmt_up == "GBRG" ||
    fmt_up == "GRBG" || fmt_up == "MONO");
}

inline bool is_jpeg_fmt(const std::string& fmt_up) {
  return (fmt_up == "JPEG" || fmt_up == "IMAGE/JPEG");
}

} // namespace

// ============================================================================
// StreamFilter::Impl — lazy builder storage
// ============================================================================
struct StreamFilter::Impl {
  qti::GstRuntime* runtime_ = nullptr;

  // Cached caps + dirty bit
  GstCaps* cached_caps_ = nullptr;
  bool dirty_ = true;

  // Generic additional k=v pairs (applied last)
  std::vector<std::pair<std::string, std::string>> extra_kv_;

  // Upstream SRC pad hint (e.g. "image_%u")
  std::string upstream_pad_hint_;

  // ---- Video/Image unified settings (lazy) ----
  enum class VidKind { None, Raw, Bayer, Jpeg };
  VidKind vkind_ = VidKind::None;
  std::optional<std::string> vfmt_token_; // RAW: UPPER; BAYER: lower; JPEG: n/a
  std::optional<int> width_, height_;
  std::optional<std::pair<int, int>> framerate_;
  std::optional<std::string> colorimetry_;
  std::optional<std::string> range_;
  std::optional<std::string> interlace_;
  std::optional<std::pair<int, int>> par_;

  Impl() { runtime_ = &qti::GstRuntime::get_instance(); }

  explicit Impl(const std::string& caps_or_mt) : Impl() {
    const std::string text = trim(caps_or_mt);
    if (text.empty()) {
      return;
    }

    cached_caps_ = gst_caps_from_string(text.c_str());
    if (!cached_caps_) {
      throw std::runtime_error("Invalid StreamFilter caps string: " + text);
    }

    dirty_ = false;
  }

  ~Impl() {
    if (cached_caps_) {
      gst_caps_unref(cached_caps_);
      cached_caps_ = nullptr;
    }
    if (runtime_) {
      qti::GstRuntime::release_instance();
      runtime_ = nullptr;
    }
  }

  // Builds new caps for Video/Image according to current fields.
  // Returns nullptr if vkind_ == None (i.e., not a video/image lazy instance).
  GstCaps* build_video_like_caps() const {
    if (vkind_ == VidKind::None)
      return nullptr;

    const char* media_type = nullptr;
    switch (vkind_) {
    case VidKind::Raw:
      media_type = "video/x-raw";
      break;
    case VidKind::Bayer:
      media_type = "video/x-bayer";
      break;
    case VidKind::Jpeg:
      media_type = "image/jpeg";
      break;
    default:
      return nullptr;
    }

    GstCaps* caps = gst_caps_new_empty_simple(media_type);

    // format
    if (vkind_ == VidKind::Raw && vfmt_token_) {
      std::string f = to_upper(*vfmt_token_);
      gst_caps_set_simple(caps, "format", G_TYPE_STRING, f.c_str(), nullptr);
    }
    else if (vkind_ == VidKind::Bayer && vfmt_token_) {
      std::string f = to_lower(*vfmt_token_);
      gst_caps_set_simple(caps, "format", G_TYPE_STRING, f.c_str(), nullptr);
    }
    // dimensions
    if (width_ && height_) {
      gst_caps_set_simple(caps, "width", G_TYPE_INT, *width_, "height",
        G_TYPE_INT, *height_, nullptr);
    }
    // framerate
    if (framerate_) {
      gst_caps_set_simple(caps, "framerate", GST_TYPE_FRACTION,
        framerate_->first, framerate_->second, nullptr);
    }
    // colorimetry / range / interlace / par (only when meaningful)
    if (vkind_ == VidKind::Raw) {
      if (colorimetry_)
        gst_caps_set_simple(caps, "colorimetry", G_TYPE_STRING,
          colorimetry_->c_str(), nullptr);
      if (range_)
        gst_caps_set_simple(caps, "range", G_TYPE_STRING, range_->c_str(),
          nullptr);
      if (interlace_)
        gst_caps_set_simple(caps, "interlace-mode", G_TYPE_STRING,
          interlace_->c_str(), nullptr);
      if (par_)
        gst_caps_set_simple(caps, "pixel-aspect-ratio", GST_TYPE_FRACTION,
          par_->first, par_->second, nullptr);
    }
    return caps;
  }

  // Applies extra k=v pairs as string values (last-win).
  static void
    apply_extras(GstCaps* caps,
      const std::vector<std::pair<std::string, std::string>>& kv) {
    if (!caps)
      return;
    for (const auto& [k, v] : kv) {
      gst_caps_set_simple(caps, k.c_str(), G_TYPE_STRING, v.c_str(), nullptr);
    }
  }
};

// ============================================================================
// StreamFilter public API
// ============================================================================
StreamFilter::StreamFilter() : impl_(std::make_shared<Impl>()) {}
StreamFilter::StreamFilter(const std::string& caps_or_mediatype)
  : impl_(std::make_shared<Impl>(caps_or_mediatype)) {
}

StreamFilter::StreamFilter(const StreamFilter& other) : impl_(other.impl_) {}
StreamFilter::~StreamFilter() = default;

StreamFilter& StreamFilter::operator=(const StreamFilter& other) {
  impl_ = other.impl_;
  return *this;
}

StreamFilter::StreamFilter(StreamFilter&& other) noexcept
  : impl_(std::move(other.impl_)) {
}

StreamFilter& StreamFilter::operator=(StreamFilter&& other) noexcept {
  impl_.swap(other.impl_);
  return *this;
}

void* StreamFilter::get_caps_opaque() const {
  // Non-lazy instances keep caps directly.
  if (impl_->vkind_ == Impl::VidKind::None) {
    return static_cast<void*>(impl_->cached_caps_);
  }

  // Lazy video/image instances rebuild caps when missing or dirty.
  if (!impl_->cached_caps_ || impl_->dirty_) {
    if (impl_->cached_caps_) {
      gst_caps_unref(impl_->cached_caps_);
    }

    impl_->cached_caps_ = impl_->build_video_like_caps();
    Impl::apply_extras(impl_->cached_caps_, impl_->extra_kv_);
    impl_->dirty_ = false;
  }

  return static_cast<void*>(impl_->cached_caps_);
}

std::string StreamFilter::to_string() const {
  GstCaps* caps = static_cast<GstCaps*>(get_caps_opaque());
  if (!caps)
    return {};
  gchar* s = gst_caps_to_string(caps);
  std::string out = s ? s : "";
  if (s)
    g_free(s);
  return out;
}

const std::string& StreamFilter::upstream_pad_hint() const {
  return impl_->upstream_pad_hint_;
}

// ============================================================================
// VideoFilter — lazy setters
// ============================================================================
namespace {

const char* fmt_to_str(VideoFilter::Format f) {
  using F = VideoFilter::Format;
  switch (f) {
  case F::NV12:
    return "NV12";
  case F::NV21:
    return "NV21";
  case F::I420:
    return "I420";
  case F::YV12:
    return "YV12";
  case F::YUY2:
    return "YUY2";
  case F::UYVY:
    return "UYVY";
  case F::VYUY:
    return "VYUY";
  case F::YVYU:
    return "YVYU";
  case F::NV16:
    return "NV16";
  case F::NV61:
    return "NV61";
  case F::Y42B:
    return "Y42B";
  case F::I422_10LE:
    return "I422_10LE";
  case F::I422_10BE:
    return "I422_10BE";
  case F::I420_10LE:
    return "I420_10LE";
  case F::I420_10BE:
    return "I420_10BE";
  case F::RGB:
    return "RGB";
  case F::BGR:
    return "BGR";
  case F::RGBx:
    return "RGBx";
  case F::xRGB:
    return "xRGB";
  case F::BGRx:
    return "BGRx";
  case F::xBGR:
    return "xBGR";
  case F::RGBA:
    return "RGBA";
  case F::ARGB:
    return "ARGB";
  case F::BGRA:
    return "BGRA";
  case F::ABGR:
    return "ABGR";
  case F::P010_10LE:
    return "P010_10LE";
  case F::P010_10BE:
    return "P010_10BE";
  case F::P016_LE:
    return "P016_LE";
  case F::P016_BE:
    return "P016_BE";
  case F::Y410:
    return "Y410";
  case F::r210:
    return "r210";
  case F::RGB10A2_LE:
    return "RGB10A2_LE";
  case F::BGR10A2_LE:
    return "BGR10A2_LE";
  case F::GRAY8:
    return "GRAY8";
  case F::GRAY16_LE:
    return "GRAY16_LE";
  case F::GRAY16_BE:
    return "GRAY16_BE";
  case F::GRAY10_LE16:
    return "GRAY10_LE16";
  case F::GRAY10_LE32:
    return "GRAY10_LE32";
  case F::Y444:
    return "Y444";
  case F::Y444_10LE:
    return "Y444_10LE";
  case F::Y444_10BE:
    return "Y444_10BE";
  case F::Y444_12LE:
    return "Y444_12LE";
  case F::Y444_12BE:
    return "Y444_12BE";
  case F::Y444_16LE:
    return "Y444_16LE";
  case F::Y444_16BE:
    return "Y444_16BE";
  case F::I422_12LE:
    return "I422_12LE";
  case F::I422_12BE:
    return "I422_12BE";
  case F::I420_12LE:
    return "I420_12LE";
  case F::I420_12BE:
    return "I420_12BE";
  case F::RGBA64_LE:
    return "RGBA64_LE";
  case F::ARGB64_LE:
    return "ARGB64_LE";
  case F::BGRA64_LE:
    return "BGRA64_LE";
  case F::ABGR64_LE:
    return "ABGR64_LE";
  case F::RGBA64_BE:
    return "RGBA64_BE";
  case F::ARGB64_BE:
    return "ARGB64_BE";
  case F::BGRA64_BE:
    return "BGRA64_BE";
  case F::ABGR64_BE:
    return "ABGR64_BE";
  case F::UYVP:
    return "UYVP";
  case F::v210:
    return "v210";
  case F::v216:
    return "v216";
  case F::v308:
    return "v308";
  case F::NV24:
    return "NV24";
  case F::NV12_10LE32:
    return "NV12_10LE32";
  case F::NV12_10LE40:
    return "NV12_10LE40";
  case F::NV16_10LE32:
    return "NV16_10LE32";
  case F::NV16_10LE40:
    return "NV16_10LE40";
  case F::JPEG:
    return "JPEG";
  case F::BGGR:
    return "BGGR";
  case F::RGGB:
    return "RGGB";
  case F::GBRG:
    return "GBRG";
  case F::GRBG:
    return "GRBG";
  case F::MONO:
    return "MONO";
  }
  return "NV12";
}

} // namespace

VideoFilter::VideoFilter() {
  impl_->vkind_ = Impl::VidKind::Raw;
  impl_->vfmt_token_ = std::string("NV12");
  impl_->dirty_ = true;
}

VideoFilter& VideoFilter::format(const std::string& fmt) {
  const std::string fup = to_upper(fmt);
  if (is_jpeg_fmt(fup)) {
    impl_->vkind_ = Impl::VidKind::Jpeg;
    impl_->vfmt_token_.reset();
  }
  else if (is_bayer_fmt(fup)) {
    impl_->vkind_ = Impl::VidKind::Bayer;
    impl_->vfmt_token_ = fup;
  }
  else {
    impl_->vkind_ = Impl::VidKind::Raw;
    impl_->vfmt_token_ = fup;
  }
  impl_->dirty_ = true;
  return *this;
}

VideoFilter& VideoFilter::format(Format f) { return format(fmt_to_str(f)); }

VideoFilter& VideoFilter::resolution(int w, int h) {
  impl_->width_ = w;
  impl_->height_ = h;
  impl_->dirty_ = true;
  return *this;
}

VideoFilter& VideoFilter::framerate(int num, int den) {
  if (den == 0)
    den = 1;
  impl_->framerate_ = std::make_pair(num, den);
  impl_->dirty_ = true;
  return *this;
}

VideoFilter& VideoFilter::framerate(float num) {
  // 30.0 -> 30/1 (for example)
  int n = static_cast<int>(num * 1000.0f + 0.5f);
  impl_->framerate_ = std::make_pair(n, 1000);
  impl_->dirty_ = true;
  return *this;
}

VideoFilter& VideoFilter::colorimetry(const std::string& c) {
  impl_->colorimetry_ = c;
  impl_->dirty_ = true;
  return *this;
}

VideoFilter& VideoFilter::range(const std::string& r) {
  impl_->range_ = r;
  impl_->dirty_ = true;
  return *this;
}

VideoFilter& VideoFilter::interlace(const std::string& m) {
  impl_->interlace_ = m;
  impl_->dirty_ = true;
  return *this;
}

VideoFilter& VideoFilter::pixel_aspect_ratio(int num, int den) {
  if (den == 0)
    den = 1;
  impl_->par_ = std::make_pair(num, den);
  impl_->dirty_ = true;
  return *this;
}

VideoFilter& VideoFilter::add(const std::string& expr) {
  const auto pos = expr.find('=');
  if (pos == std::string::npos) {
    throw std::runtime_error("VideoFilter::add expects 'key=value'");
  }

  std::string key = trim(expr.substr(0, pos));
  std::string val = trim(expr.substr(pos + 1));
  if (key.empty()) {
    throw std::runtime_error("VideoFilter::add: empty key");
  }

  impl_->extra_kv_.emplace_back(std::move(key), std::move(val));
  impl_->dirty_ = true;
  return *this;
}

// ============================================================================
// ImageFilter
// ============================================================================
ImageFilter::ImageFilter() {
  impl_->vkind_ = Impl::VidKind::Jpeg;
  impl_->vfmt_token_.reset();
  impl_->upstream_pad_hint_ = "image_%u";
  impl_->dirty_ = true;
}

ImageFilter& ImageFilter::format(const std::string& fmt) {
  const std::string fup = to_upper(fmt);
  if (is_jpeg_fmt(fup)) {
    impl_->vkind_ = Impl::VidKind::Jpeg;
    impl_->vfmt_token_.reset();
  }
  else if (is_bayer_fmt(fup)) {
    impl_->vkind_ = Impl::VidKind::Bayer;
    impl_->vfmt_token_ = fup;
  }
  else {
    impl_->vkind_ = Impl::VidKind::Raw;
    impl_->vfmt_token_ = fup;
  }
  impl_->dirty_ = true;
  return *this;
}

ImageFilter& ImageFilter::resolution(int w, int h) {
  impl_->width_ = w;
  impl_->height_ = h;
  impl_->dirty_ = true;
  return *this;
}

ImageFilter& ImageFilter::framerate(int num, int den) {
  if (den == 0)
    den = 1;
  impl_->framerate_ = std::make_pair(num, den);
  impl_->dirty_ = true;
  return *this;
}

ImageFilter& ImageFilter::framerate(float num) {
  int n = static_cast<int>(num * 1000.0f + 0.5f);
  impl_->framerate_ = std::make_pair(n, 1000);
  impl_->dirty_ = true;
  return *this;
}

ImageFilter& ImageFilter::add(const std::string& expr) {
  const auto pos = expr.find('=');
  if (pos == std::string::npos) {
    throw std::runtime_error("ImageFilter::add expects 'key=value'");
  }

  std::string key = trim(expr.substr(0, pos));
  std::string val = trim(expr.substr(pos + 1));
  if (key.empty()) {
    throw std::runtime_error("ImageFilter::add: empty key");
  }

  impl_->extra_kv_.emplace_back(std::move(key), std::move(val));
  impl_->dirty_ = true;
  return *this;
}

// ============================================================================
// H264Filter / TensorFilter / TextFilter / AudioFilter
// (remain "direct"; for simplicity they do not switch to lazy here)
// ============================================================================
H264Filter::H264Filter() {
  if (impl_->cached_caps_) {
    gst_caps_unref(impl_->cached_caps_);
  }
  impl_->cached_caps_ = gst_caps_new_empty_simple("video/x-h264");
  impl_->dirty_ = false;
}

H264Filter& H264Filter::resolution(int w, int h) {
  gst_caps_set_simple(impl_->cached_caps_, "width", G_TYPE_INT, w, "height",
    G_TYPE_INT, h, nullptr);
  return *this;
}

H264Filter& H264Filter::framerate(int num, int den) {
  if (den == 0)
    den = 1;
  gst_caps_set_simple(impl_->cached_caps_, "framerate", GST_TYPE_FRACTION, num,
    den, nullptr);
  return *this;
}

H264Filter& H264Filter::framerate(float num) {
  int n = static_cast<int>(num * 1000.0f + 0.5f);
  return framerate(n, 1000);
}

H264Filter& H264Filter::profile(const std::string& p) {
  gst_caps_set_simple(impl_->cached_caps_, "profile", G_TYPE_STRING, p.c_str(),
    nullptr);
  return *this;
}

H264Filter& H264Filter::level(const std::string& l) {
  gst_caps_set_simple(impl_->cached_caps_, "level", G_TYPE_STRING, l.c_str(),
    nullptr);
  return *this;
}

H264Filter& H264Filter::stream_format(const std::string& f) {
  gst_caps_set_simple(impl_->cached_caps_, "stream-format", G_TYPE_STRING,
    f.c_str(), nullptr);
  return *this;
}

H264Filter& H264Filter::alignment(const std::string& a) {
  gst_caps_set_simple(impl_->cached_caps_, "alignment", G_TYPE_STRING,
    a.c_str(), nullptr);
  return *this;
}

H264Filter& H264Filter::codec_data(const std::string& bytes) {
  if (bytes.empty())
    return *this;
  GstBuffer* b = gst_buffer_new_allocate(nullptr, bytes.size(), nullptr);
  if (!b)
    throw std::bad_alloc();
  GstMapInfo w{};
  if (!gst_buffer_map(b, &w, GST_MAP_WRITE)) {
    gst_buffer_unref(b);
    throw std::runtime_error("H264::codec_data map failed");
  }
  std::memcpy(w.data, bytes.data(), bytes.size());
  gst_buffer_unmap(b, &w);
  gst_caps_set_simple(impl_->cached_caps_, "codec_data", GST_TYPE_BUFFER, b,
    nullptr);
  gst_buffer_unref(b);
  return *this;
}

H264Filter& H264Filter::set(const std::string& k, const std::string& v) {
  gst_caps_set_simple(impl_->cached_caps_, k.c_str(), G_TYPE_STRING, v.c_str(),
    nullptr);
  return *this;
}

H264Filter& H264Filter::add(const std::string& expr) {
  const auto pos = expr.find('=');
  if (pos == std::string::npos) {
    throw std::runtime_error("H264Filter::add expects 'key=value'");
  }

  std::string key = trim(expr.substr(0, pos));
  std::string val = trim(expr.substr(pos + 1));
  if (key.empty()) {
    throw std::runtime_error("H264Filter::add: empty key");
  }

  return set(key, val);
}

TensorFilter::TensorFilter() {
  if (impl_->cached_caps_) {
    gst_caps_unref(impl_->cached_caps_);
  }
  impl_->cached_caps_ = gst_caps_new_empty_simple("neural-network/tensors");
  impl_->dirty_ = false;
}

TensorFilter& TensorFilter::type(const std::string& t) {
  gst_caps_set_simple(impl_->cached_caps_, "type", G_TYPE_STRING, t.c_str(),
    nullptr);
  return *this;
}

TensorFilter& TensorFilter::type(Type t) {
  const char* s = nullptr;
  switch (t) {
  case Type::UINT8:
    s = "UINT8";
    break;
  case Type::UINT16:
    s = "UINT16";
    break;
  case Type::UINT32:
    s = "UINT32";
    break;
  case Type::INT8:
    s = "INT8";
    break;
  case Type::INT16:
    s = "INT16";
    break;
  case Type::INT32:
    s = "INT32";
    break;
  case Type::FLOAT16:
    s = "FLOAT16";
    break;
  case Type::FLOAT32:
    s = "FLOAT32";
    break;
  }
  return type(s ? s : "UINT8");
}

static void
set_caps_array_of_int_arrays(GstCaps* caps, const char* key,
  const std::vector<std::vector<int>>& tensors) {
  if (!caps)
    return;
  GstStructure* st = gst_caps_get_structure(caps, 0);
  if (!st)
    return;
  if (gst_structure_has_field(st, key))
    gst_structure_remove_field(st, key);
  GValue outer = G_VALUE_INIT;
  g_value_init(&outer, GST_TYPE_ARRAY);
  for (const auto& vec : tensors) {
    GValue inner = G_VALUE_INIT;
    g_value_init(&inner, GST_TYPE_ARRAY);
    for (int v : vec) {
      GValue iv = G_VALUE_INIT;
      g_value_init(&iv, G_TYPE_INT);
      g_value_set_int(&iv, v);
      gst_value_array_append_value(&inner, &iv);
      g_value_unset(&iv);
    }
    gst_value_array_append_value(&outer, &inner);
    g_value_unset(&inner);
  }
  gst_structure_set_value(st, key, &outer);
  g_value_unset(&outer);
}

TensorFilter& TensorFilter::dimensions(const std::vector<int>& one) {
  set_caps_array_of_int_arrays(impl_->cached_caps_, "dimensions",
    std::vector<std::vector<int>>{one});
  return *this;
}

TensorFilter&
TensorFilter::dimensions(const std::vector<std::vector<int>>& many) {
  set_caps_array_of_int_arrays(impl_->cached_caps_, "dimensions", many);
  return *this;
}

TensorFilter& TensorFilter::add(const std::string& expr) {
  const auto pos = expr.find('=');
  if (pos == std::string::npos) {
    throw std::runtime_error("TensorFilter::add expects 'key=value'");
  }

  std::string key = trim(expr.substr(0, pos));
  std::string val = trim(expr.substr(pos + 1));
  if (key.empty()) {
    throw std::runtime_error("TensorFilter::add: empty key");
  }

  gst_caps_set_simple(impl_->cached_caps_, key.c_str(), G_TYPE_STRING,
    val.c_str(), nullptr);
  return *this;
}

TextFilter::TextFilter() {
  if (impl_->cached_caps_) {
    gst_caps_unref(impl_->cached_caps_);
  }
  impl_->cached_caps_ = gst_caps_new_empty_simple("text/x-raw");
  impl_->dirty_ = false;
}

TextFilter& TextFilter::add(const std::string& expr) {
  const auto pos = expr.find('=');
  if (pos == std::string::npos) {
    throw std::runtime_error("TextFilter::add expects 'key=value'");
  }

  std::string key = trim(expr.substr(0, pos));
  std::string val = trim(expr.substr(pos + 1));
  if (key.empty()) {
    throw std::runtime_error("TextFilter::add: empty key");
  }

  gst_caps_set_simple(impl_->cached_caps_, key.c_str(), G_TYPE_STRING,
    val.c_str(), nullptr);
  return *this;
}

AudioFilter::AudioFilter() {
  if (impl_->cached_caps_) {
    gst_caps_unref(impl_->cached_caps_);
  }
  impl_->cached_caps_ = gst_caps_new_empty_simple("audio/x-raw");
  impl_->dirty_ = false;
}

static const char* audio_format_to_str(AudioFilter::Format f) {
  using F = AudioFilter::Format;
  switch (f) {
  case F::S8:
    return "S8";
  case F::U8:
    return "U8";
  case F::S16LE:
    return "S16LE";
  case F::S16BE:
    return "S16BE";
  case F::U16LE:
    return "U16LE";
  case F::U16BE:
    return "U16BE";
  case F::S24_32LE:
    return "S24_32LE";
  case F::S24_32BE:
    return "S24_32BE";
  case F::U24_32LE:
    return "U24_32LE";
  case F::U24_32BE:
    return "U24_32BE";
  case F::S32LE:
    return "S32LE";
  case F::S32BE:
    return "S32BE";
  case F::U32LE:
    return "U32LE";
  case F::U32BE:
    return "U32BE";
  case F::S24LE:
    return "S24LE";
  case F::S24BE:
    return "S24BE";
  case F::U24LE:
    return "U24LE";
  case F::U24BE:
    return "U24BE";
  case F::S20LE:
    return "S20LE";
  case F::S20BE:
    return "S20BE";
  case F::U20LE:
    return "U20LE";
  case F::U20BE:
    return "U20BE";
  case F::S18LE:
    return "S18LE";
  case F::S18BE:
    return "S18BE";
  case F::U18LE:
    return "U18LE";
  case F::U18BE:
    return "U18BE";
  case F::F32LE:
    return "F32LE";
  case F::F32BE:
    return "F32BE";
  case F::F64LE:
    return "F64LE";
  case F::F64BE:
    return "F64BE";
  }
  return "S16LE";
}

AudioFilter& AudioFilter::format(const std::string& fmt) {
  gst_caps_set_simple(impl_->cached_caps_, "format", G_TYPE_STRING, fmt.c_str(),
    nullptr);
  return *this;
}

AudioFilter& AudioFilter::format(Format f) {
  return format(audio_format_to_str(f));
}

AudioFilter& AudioFilter::channels(int n) {
  gst_caps_set_simple(impl_->cached_caps_, "channels", G_TYPE_INT, n, nullptr);
  return *this;
}

AudioFilter& AudioFilter::rate(int hz) {
  gst_caps_set_simple(impl_->cached_caps_, "rate", G_TYPE_INT, hz, nullptr);
  return *this;
}

AudioFilter& AudioFilter::layout(const std::string& l) {
  gst_caps_set_simple(impl_->cached_caps_, "layout", G_TYPE_STRING, l.c_str(),
    nullptr);
  return *this;
}

AudioFilter& AudioFilter::layout(Layout l) {
  return layout(l == Layout::INTERLEAVED ? "interleaved" : "non-interleaved");
}

AudioFilter& AudioFilter::add(const std::string& expr) {
  const auto pos = expr.find('=');
  if (pos == std::string::npos) {
    throw std::runtime_error("AudioFilter::add expects 'key=value'");
  }

  std::string key = trim(expr.substr(0, pos));
  std::string val = trim(expr.substr(pos + 1));
  if (key.empty()) {
    throw std::runtime_error("AudioFilter::add: empty key");
  }

  gst_caps_set_simple(impl_->cached_caps_, key.c_str(), G_TYPE_STRING,
    val.c_str(), nullptr);
  return *this;
}

} // namespace qti
