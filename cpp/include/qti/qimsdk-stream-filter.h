/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#pragma once

#include <memory>
#include <string>
#include <type_traits>
#include <vector>

namespace qti {

class Element;
class Pipeline;

// ---------------- StreamFilter Base ----------------
class StreamFilter {
public:
  // Create an empty stream filter expression.
  StreamFilter();
  // Create a filter from caps/media-type string.
  explicit StreamFilter(const std::string& caps_or_mediatype);

  StreamFilter(const StreamFilter&);
  StreamFilter& operator=(const StreamFilter&);

  StreamFilter(StreamFilter&&) noexcept;
  StreamFilter& operator=(StreamFilter&&) noexcept;

  virtual ~StreamFilter();

  // Serialize current filter expression to caps string.
  std::string to_string() const;

protected:
  struct Impl;
  std::shared_ptr<Impl> impl_;

  void* get_caps_opaque() const;

  // Hint for preferred upstream SRC pad (e.g. "image_%u").
  // Not public; visible only to friend classes.
  const std::string& upstream_pad_hint() const;

  friend class qti::Element;
  friend class qti::Pipeline;
};

// ---------------- Video Stream Filter ----------------
class VideoFilter : public StreamFilter {
public:
  // Create a video stream filter.
  VideoFilter();

  enum class Format {

    // video/x-raw
    NV12, NV21, I420, YV12,
    YUY2, UYVY, VYUY, YVYU,
    NV16, NV61, Y42B, I422_10LE, I422_10BE,
    I420_10LE, I420_10BE,
    RGB, BGR, RGBx, xRGB, BGRx, xBGR, RGBA, ARGB, BGRA, ABGR,
    P010_10LE, P010_10BE, P016_LE, P016_BE,
    Y410, r210, RGB10A2_LE, BGR10A2_LE,
    GRAY8, GRAY16_LE, GRAY16_BE, GRAY10_LE16, GRAY10_LE32,
    Y444, Y444_10LE, Y444_10BE, Y444_12LE, Y444_12BE,
    Y444_16LE, Y444_16BE,
    I422_12LE, I422_12BE, I420_12LE, I420_12BE,
    RGBA64_LE, ARGB64_LE, BGRA64_LE, ABGR64_LE,
    RGBA64_BE, ARGB64_BE, BGRA64_BE, ABGR64_BE,
    UYVP, v210, v216, v308, NV24,
    NV12_10LE32, NV12_10LE40, NV16_10LE32, NV16_10LE40,

    // image/jpeg
    JPEG,

    // video/x-bayer
    BGGR, RGGB, GBRG, GRBG, MONO
  };

  // Set pixel format by string or enum.
  VideoFilter& format(const std::string& fmt);
  VideoFilter& format(Format fmt);

  // Set output resolution in pixels.
  VideoFilter& resolution(int width, int height);

  // Set output frame rate.
  VideoFilter& framerate(int num, int den = 1);
  VideoFilter& framerate(float num);

  VideoFilter& colorimetry(const std::string& colo);

  VideoFilter& range(const std::string& range);

  VideoFilter& interlace(const std::string& mode);

  VideoFilter& pixel_aspect_ratio(int num, int den = 1);
  VideoFilter& add(const std::string& expr);
};

// ---------------- Image Capture Filter ----------------
class ImageFilter : public StreamFilter {
public:
  // Create an image capture filter.
  ImageFilter();

  ImageFilter& format(const std::string& fmt);

  ImageFilter& resolution(int width, int height);

  ImageFilter& framerate(int num, int den = 1);
  ImageFilter& framerate(float num);
  ImageFilter& add(const std::string& expr);
};

// ---------------- video/x-h264 ----------------
class H264Filter : public StreamFilter {
public:
  // Create an H264 stream filter.
  H264Filter();

  H264Filter& resolution(int width, int height);

  H264Filter& framerate(int num, int den = 1);
  H264Filter& framerate(float num);

  H264Filter& profile(const std::string& p);
  H264Filter& level(const std::string& lvl);
  H264Filter& stream_format(const std::string& f);
  H264Filter& alignment(const std::string& a);

  H264Filter& codec_data(const std::string& bytes);

  H264Filter& set(const std::string& key, const std::string& val);
  H264Filter& add(const std::string& expr);
};

// ---------------- neural-network/tensors ----------------
class TensorFilter : public StreamFilter {
public:
  // Create a neural-network tensors filter.
  TensorFilter();

  enum class Type {
    UINT8, UINT16, UINT32,
    INT8, INT16, INT32,
    FLOAT16, FLOAT32
  };

  TensorFilter& type(const std::string& t);
  TensorFilter& type(Type t);

  template <typename... UInts>
  TensorFilter& dimensions(UInts... dims) {
    static_assert(sizeof...(dims) > 0, "dimensions(...) needs at least one value");
    static_assert((std::conjunction_v<std::is_integral<UInts>...>),
      "dimensions(...) only accepts integral types");

    std::vector<int> v{ static_cast<int>(dims)... };
    return dimensions(v);
  }

  TensorFilter& dimensions(const std::vector<int>& one_tensor_dims);

  TensorFilter& dimensions(const std::vector<std::vector<int>>& many_tensors_dims);
  TensorFilter& add(const std::string& expr);
};

// ---------------- text/x-raw ----------------
class TextFilter : public StreamFilter {
public:
  // Create a text stream filter.
  TextFilter();
  TextFilter& add(const std::string& expr);
};

// ---------------- audio/x-raw ----------------
class AudioFilter : public StreamFilter {
public:
  // Create an audio stream filter.
  AudioFilter();

  enum class Format {
    S8, U8, S16LE, S16BE, U16LE, U16BE,
    S24_32LE, S24_32BE, U24_32LE, U24_32BE,
    S32LE, S32BE, U32LE, U32BE,
    S24LE, S24BE, U24LE, U24BE,
    S20LE, S20BE, U20LE, U20BE,
    S18LE, S18BE, U18LE, U18BE,
    F32LE, F32BE, F64LE, F64BE
  };

  enum class Layout { INTERLEAVED, NON_INTERLEAVED };

  AudioFilter& format(const std::string& fmt);
  AudioFilter& format(Format f);

  AudioFilter& channels(int n);

  AudioFilter& rate(int hz);

  AudioFilter& layout(const std::string& l);
  AudioFilter& layout(Layout l);
  AudioFilter& add(const std::string& expr);
};

} // namespace qti
