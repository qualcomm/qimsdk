/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include "qti/qimsdk-preprocess-base.h"

#include <gst/gst.h>
#include <gst/ml/gstmlbundle.h>
#include <gst/ml/ml-frame.h>
#include <gst/video/video-converter-engine-param.h>
#include <gst/video/video-frame.h>

#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace qti {

struct PreprocessSignalHandler {
  std::string unique_name;
  TensorsPreprocessCallback tensors_callback;
  gulong signal_id = 0;
};

namespace {

MLTensorType to_tensor_type(GstMLType type) {
  switch (type) {
    case GST_ML_TYPE_INT8:
      return MLTensorType::Int8;
    case GST_ML_TYPE_UINT8:
      return MLTensorType::UInt8;
    case GST_ML_TYPE_INT16:
      return MLTensorType::Int16;
    case GST_ML_TYPE_UINT16:
      return MLTensorType::UInt16;
    case GST_ML_TYPE_INT32:
      return MLTensorType::Int32;
    case GST_ML_TYPE_UINT32:
      return MLTensorType::UInt32;
    case GST_ML_TYPE_INT64:
      return MLTensorType::Int64;
    case GST_ML_TYPE_UINT64:
      return MLTensorType::UInt64;
    case GST_ML_TYPE_FLOAT16:
      return MLTensorType::Float16;
    case GST_ML_TYPE_FLOAT32:
      return MLTensorType::Float32;
    case GST_ML_TYPE_UNKNOWN:
    default:
      return MLTensorType::Unknown;
  }
}

MLFrame build_mlframe(GstMLFrame* mlframe) {
  MLFrame frame;
  if (!mlframe)
    return frame;

  const guint n_tensors = GST_ML_FRAME_N_TENSORS(mlframe);
  frame.tensors.reserve(n_tensors);

  for (guint idx = 0; idx < n_tensors; ++idx) {
    MLTensor tensor;
    tensor.type = to_tensor_type(GST_ML_FRAME_TYPE(mlframe));

    const guint n_dims = GST_ML_FRAME_N_DIMENSIONS(mlframe, idx);
    tensor.dimensions.reserve(n_dims);

    for (guint dim = 0; dim < n_dims; ++dim) {
      tensor.dimensions.push_back(GST_ML_FRAME_DIM(mlframe, idx, dim));
    }

    tensor.data = GST_ML_FRAME_BLOCK_DATA(mlframe, idx);
    tensor.size = GST_ML_FRAME_BLOCK_SIZE(mlframe, idx);

    frame.tensors.push_back(std::move(tensor));
  }

  return frame;
}

struct MappedVideoFrame {
  GstVideoFrame frame;
  bool mapped = false;

  MappedVideoFrame() {
    std::memset(&frame, 0, sizeof(frame));
  }

  ~MappedVideoFrame() {
    if (mapped) {
      gst_video_frame_unmap(&frame);
      mapped = false;
    }
  }

  MappedVideoFrame(const MappedVideoFrame&) = delete;
  MappedVideoFrame& operator=(const MappedVideoFrame&) = delete;
  MappedVideoFrame(MappedVideoFrame&&) = delete;
  MappedVideoFrame& operator=(MappedVideoFrame&&) = delete;
};

MLVideoBlits build_mlvideo_blits(
    const GstVideoBlits* blits,
    std::vector<std::unique_ptr<MappedVideoFrame>>& mapped_frames) {
  MLVideoBlits out;
  if (!blits) {
    return out;
  }

  const guint n_blits = gst_video_blits_size(const_cast<GstVideoBlits*>(blits));
  out.entries.reserve(n_blits);

  for (guint idx = 0; idx < n_blits; ++idx) {
    const GstVideoBlit* in =
        gst_video_blits_entry(const_cast<GstVideoBlits*>(blits), idx);
    if (!in) {
      continue;
    }

    MLVideoBlit blit;
    blit.mask = in->mask;
    blit.source.a.x = in->source.a.x;
    blit.source.a.y = in->source.a.y;
    blit.source.b.x = in->source.b.x;
    blit.source.b.y = in->source.b.y;
    blit.source.c.x = in->source.c.x;
    blit.source.c.y = in->source.c.y;
    blit.source.d.x = in->source.d.x;
    blit.source.d.y = in->source.d.y;
    blit.destination.x = in->destination.x;
    blit.destination.y = in->destination.y;
    blit.destination.w = in->destination.w;
    blit.destination.h = in->destination.h;
    blit.alpha = in->alpha;
    blit.rotate = static_cast<int>(in->rotate);

    if (in->buffer != nullptr && in->info != nullptr) {
      auto mapped = std::make_unique<MappedVideoFrame>();
      mapped->mapped = gst_video_frame_map(
          &mapped->frame,
          in->info,
          in->buffer,
          static_cast<GstMapFlags>(GST_MAP_READ));

      if (mapped->mapped) {
        blit.image.width = GST_VIDEO_FRAME_WIDTH(&mapped->frame);
        blit.image.height = GST_VIDEO_FRAME_HEIGHT(&mapped->frame);

        const GstVideoFormat format = GST_VIDEO_FRAME_FORMAT(&mapped->frame);
        const gchar* format_name = gst_video_format_to_string(format);
        blit.image.format = format_name ? format_name : "";

        const guint n_planes = GST_VIDEO_FRAME_N_PLANES(&mapped->frame);
        blit.image.planes.reserve(n_planes);

        for (guint p = 0; p < n_planes; ++p) {
          MLVideoPlane plane;
          plane.data = GST_VIDEO_FRAME_PLANE_DATA(&mapped->frame, p);
          plane.stride = GST_VIDEO_FRAME_PLANE_STRIDE(&mapped->frame, p);
          plane.height = GST_VIDEO_FRAME_COMP_HEIGHT(&mapped->frame, p);
          blit.image.planes.push_back(std::move(plane));
        }

        mapped_frames.push_back(std::move(mapped));
      }
    }

    out.entries.push_back(std::move(blit));
  }

  return out;
}

bool validate_inplace_mlframe_output(const MLFrame& output,
                                     GstMLFrame* outmlframe) {
  if (!outmlframe) {
    return false;
  }

  const guint n_tensors = GST_ML_FRAME_N_TENSORS(outmlframe);
  if (output.tensors.size() != n_tensors) {
    return false;
  }

  for (guint idx = 0; idx < n_tensors; ++idx) {
    const auto& out_tensor = output.tensors[idx];

    const size_t dst_size = GST_ML_FRAME_BLOCK_SIZE(outmlframe, idx);
    if (out_tensor.size != dst_size) {
      return false;
    }

    if (dst_size == 0) {
      continue;
    }

    void* dst = GST_ML_FRAME_BLOCK_DATA(outmlframe, idx);
    if (!dst || out_tensor.data != dst) {
      return false;
    }
  }

  return true;
}

}  // namespace

gboolean on_process_tensors_preprocess(
    GstElement* /*preprocess*/, GstVideoBlits* blits,
    GstMLBundle* outmlbundle, gpointer user_data) {
  auto* handler = static_cast<PreprocessSignalHandler*>(user_data);
  if (!handler || !handler->tensors_callback || !outmlbundle) {
    return FALSE;
  }

  try {
    GstMLFrame* outmlframe = gst_ml_bundle_get_frame(outmlbundle, GST_MAP_WRITE);
    if (!outmlframe) {
      return FALSE;
    }

    std::vector<std::unique_ptr<MappedVideoFrame>> mapped_frames;
    MLVideoBlits wrapped_blits = build_mlvideo_blits(blits, mapped_frames);

    MLFrame output = build_mlframe(outmlframe);
    if (!handler->tensors_callback(wrapped_blits, output)) {
      return FALSE;
    }

    if (output.tensors.empty()) {
      return FALSE;
    }

    return static_cast<gboolean>(
        validate_inplace_mlframe_output(output, outmlframe));
  } catch (...) {
    return FALSE;
  }
}

void connect_external_preprocess_handler(PreprocessSignalHandler& handler,
                                         GstElement* preprocess) {
  if (!preprocess) {
    throw std::runtime_error("Preprocess element is null");
  }

  if (!handler.tensors_callback) {
    throw std::runtime_error(
        "Tensors preprocess handler callback is not set for element '" +
        handler.unique_name + "'");
  }

  constexpr const char* kSignalName = "process";

  if (handler.signal_id != 0) {
    g_signal_handler_disconnect(preprocess, handler.signal_id);
    handler.signal_id = 0;
  }

  handler.signal_id =
      g_signal_connect(preprocess, kSignalName,
                       G_CALLBACK(on_process_tensors_preprocess),
                       &handler);

  if (handler.signal_id == 0) {
    throw std::runtime_error("Failed to connect signal '" +
                             std::string(kSignalName) +
                             "' for element '" + handler.unique_name + "'");
  }
}

void disconnect_external_preprocess_handler(PreprocessSignalHandler& handler,
                                            GstElement* preprocess) {
  if (handler.signal_id == 0) {
    return;
  }

  if (preprocess) {
    g_signal_handler_disconnect(preprocess, handler.signal_id);
  }

  handler.signal_id = 0;
}

struct MLPreprocessBase::Impl {
  GstElement* elem_ = nullptr;
  GstElement* preprocess_elem_ = nullptr;
  std::string owner_name_;
  PreprocessSignalHandler signal_handler_;

  Impl(GstElement* elem,
       const char* expected_factory,
       const char* owner_name)
      : elem_(elem),
        owner_name_(owner_name ? owner_name : "MLPreprocessBase") {
    if (!elem_ || !GST_IS_ELEMENT(elem_)) {
      throw std::runtime_error(owner_name_ + ": invalid GstElement");
    }

    GstElementFactory* fac = gst_element_get_factory(elem_);
    const gchar* fac_name =
        fac ? gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(fac)) : nullptr;
    if (!fac_name || !expected_factory ||
        std::strcmp(fac_name, expected_factory) != 0) {
      throw std::runtime_error(owner_name_ + ": underlying element must be '" +
                               (expected_factory ? expected_factory : "") +
                               "'");
    }

    if (std::strcmp(expected_factory, "qtimlvconverter") == 0) {
      preprocess_elem_ = elem_;
      return;
    }

    if (GST_IS_BIN(elem_)) {
      preprocess_elem_ = gst_bin_get_by_name(GST_BIN(elem_), "mlpreprocess");
      if (preprocess_elem_ && GST_IS_ELEMENT(preprocess_elem_)) {
        return;
      }
      if (preprocess_elem_) {
        gst_object_unref(preprocess_elem_);
        preprocess_elem_ = nullptr;
      }
    }

    throw std::runtime_error(owner_name_ +
                             ": cannot resolve internal 'mlpreprocess' "
                             "element for external preprocess handler");
  }

  ~Impl() {
    disconnect_handler();
    if (preprocess_elem_ && preprocess_elem_ != elem_) {
      gst_object_unref(preprocess_elem_);
      preprocess_elem_ = nullptr;
    }
  }

  void set_handler_impl(TensorsPreprocessCallback callback) {
    if (!callback) {
      throw std::invalid_argument("Preprocess handler callback is empty");
    }

    GstElement* target = preprocess_elem_;
    if (!target) {
      throw std::runtime_error(owner_name_ +
                               ": preprocess target element is null");
    }

    const gchar* name = GST_ELEMENT_NAME(target);
    if (!name || !*name) {
      throw std::runtime_error(
          owner_name_ + " handler registration requires a named element");
    }

    signal_handler_.tensors_callback = std::move(callback);
    signal_handler_.unique_name = name;
    connect_external_preprocess_handler(signal_handler_, target);
  }

  void disconnect_handler() {
    disconnect_external_preprocess_handler(signal_handler_, preprocess_elem_);
  }
};

MLPreprocessBase::MLPreprocessBase(
    const std::string& factory,
    const std::string& name,
    const char* expected_factory,
    const char* owner_name)
    : Element(factory, name),
      impl_(std::make_unique<Impl>(
          static_cast<GstElement*>(get_raw_gst_element()),
          expected_factory,
          owner_name)) {
}

MLPreprocessBase::MLPreprocessBase(
    void* existing_gst_elem,
    const char* expected_factory,
    const char* owner_name)
    : Element(existing_gst_elem, true),
      impl_(std::make_unique<Impl>(
          static_cast<GstElement*>(get_raw_gst_element()),
          expected_factory,
          owner_name)) {
}

MLPreprocessBase::~MLPreprocessBase() = default;

MLPreprocessBase::MLPreprocessBase(
    MLPreprocessBase&&) noexcept = default;

MLPreprocessBase& MLPreprocessBase::operator=(
    MLPreprocessBase&& other) noexcept {
  if (this != &other) {
    Element::operator=(std::move(other));
    impl_ = std::move(other.impl_);
  }
  return *this;
}

MLPreprocessBase& MLPreprocessBase::set_handler(
    TensorsPreprocessCallback handler) {
  impl_->set_handler_impl(std::move(handler));
  return *this;
}

}  // namespace qti
