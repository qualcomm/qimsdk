/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include "qti/qimsdk-postprocess-base.h"

#include <gst/gst.h>
#include <gst/ml/ml-frame.h>
#include <gst/ml/ml-post-process-classification.h>
#include <gst/ml/ml-post-process-depth-map.h>
#include <gst/ml/ml-post-process-detection.h>
#include <gst/ml/ml-post-process-pose.h>
#include <gst/ml/ml-post-process-segmentation.h>

#include <cctype>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>

namespace qti {

enum class PostprocessHandlerType {
  ImageClassification,
  AudioClassification,
  ObjectDetection,
  PoseEstimation,
  Segmentation,
  DepthEstimation,
  Tensors,
};

struct PostprocessSignalHandler {
  std::string unique_name;
  PostprocessHandlerType type = PostprocessHandlerType::ObjectDetection;

  ClassificationPostprocessCallback classification_callback;
  ObjectDetectionPostprocessCallback detection_callback;
  PoseEstimationPostprocessCallback pose_callback;
  SegmentationPostprocessCallback segmentation_callback;
  DepthEstimationPostprocessCallback depth_callback;
  TensorsPostprocessCallback tensors_callback;

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

gboolean parse_mlparam_field(GQuark field,
                             const GValue* value,
                             gpointer user_data) {
  auto* params = static_cast<MLParam*>(user_data);
  if (!params)
    return FALSE;

  MLParam::Value out;

  if (G_VALUE_HOLDS_INT(value)) {
    out.type = MLParam::ValueType::Int;
    out.i = g_value_get_int(value);
  } else if (G_VALUE_HOLDS_UINT(value)) {
    out.type = MLParam::ValueType::UInt;
    out.u = g_value_get_uint(value);
  } else if (G_VALUE_HOLDS_INT64(value)) {
    out.type = MLParam::ValueType::Int64;
    out.i64 = g_value_get_int64(value);
  } else if (G_VALUE_HOLDS_UINT64(value)) {
    out.type = MLParam::ValueType::UInt64;
    out.u64 = g_value_get_uint64(value);
  } else if (G_VALUE_HOLDS_FLOAT(value)) {
    out.type = MLParam::ValueType::Float;
    out.f = g_value_get_float(value);
  } else if (G_VALUE_HOLDS_DOUBLE(value)) {
    out.type = MLParam::ValueType::Double;
    out.d = g_value_get_double(value);
  } else if (G_VALUE_HOLDS_BOOLEAN(value)) {
    out.type = MLParam::ValueType::Bool;
    out.b = g_value_get_boolean(value);
  } else if (G_VALUE_HOLDS_STRING(value)) {
    out.type = MLParam::ValueType::String;
    const gchar* str = g_value_get_string(value);
    out.s = str ? str : "";
  } else {
    return TRUE;
  }

  params->fields.emplace_back(g_quark_to_string(field), std::move(out));
  return TRUE;
}

std::string sanitize_structure_name(const std::string& input,
                                    const char* fallback) {
  std::string out = input;

  for (char& ch : out) {
    const auto uch = static_cast<unsigned char>(ch);
    if (std::isalnum(uch) || ch == '_' || ch == '-' || ch == '.') {
      continue;
    }
    ch = '_';
  }

  if (out.empty() || !std::isalpha(static_cast<unsigned char>(out[0]))) {
    out = fallback;
  }

  return out;
}

GstStructure* build_gst_structure(const MLExtraParam& extra) {
  if (extra.fields.empty())
    return nullptr;

  GstStructure* structure = gst_structure_new_empty("ExtraParams");

  for (const auto& field : extra.fields) {
    const auto& key = field.first;
    const auto& value = field.second;

    switch (value.type) {
      case MLParam::ValueType::Int:
        gst_structure_set(structure, key.c_str(), G_TYPE_INT, value.i, nullptr);
        break;
      case MLParam::ValueType::UInt:
        gst_structure_set(structure, key.c_str(), G_TYPE_UINT, value.u, nullptr);
        break;
      case MLParam::ValueType::Int64:
        gst_structure_set(structure, key.c_str(), G_TYPE_INT64, value.i64,
                          nullptr);
        break;
      case MLParam::ValueType::UInt64:
        gst_structure_set(structure, key.c_str(), G_TYPE_UINT64, value.u64,
                          nullptr);
        break;
      case MLParam::ValueType::Float:
        gst_structure_set(structure, key.c_str(), G_TYPE_FLOAT, value.f,
                          nullptr);
        break;
      case MLParam::ValueType::Double:
        gst_structure_set(structure, key.c_str(), G_TYPE_DOUBLE, value.d,
                          nullptr);
        break;
      case MLParam::ValueType::Bool:
        gst_structure_set(structure, key.c_str(), G_TYPE_BOOLEAN,
                          static_cast<gboolean>(value.b), nullptr);
        break;
      case MLParam::ValueType::String:
        gst_structure_set(structure, key.c_str(), G_TYPE_STRING,
                          value.s.c_str(), nullptr);
        break;
    }
  }

  return structure;
}

MLFrame build_mlframe(GstMLFrame* mlframe) {
  MLFrame view;
  if (!mlframe)
    return view;

  const guint n_tensors = GST_ML_FRAME_N_TENSORS(mlframe);
  view.tensors.reserve(n_tensors);

  for (guint idx = 0; idx < n_tensors; ++idx) {
    MLTensor tensor;
    tensor.type = to_tensor_type(GST_ML_FRAME_TYPE(mlframe));

    const guint n_dims = GST_ML_FRAME_N_DIMENSIONS(mlframe, idx);
    tensor.dimensions.reserve(n_dims);
    for (guint d = 0; d < n_dims; ++d) {
      tensor.dimensions.push_back(GST_ML_FRAME_DIM(mlframe, idx, d));
    }

    tensor.data = GST_ML_FRAME_BLOCK_DATA(mlframe, idx);
    tensor.size = GST_ML_FRAME_BLOCK_SIZE(mlframe, idx);

    view.tensors.push_back(std::move(tensor));
  }

  return view;
}

MLParam build_mlparam(const GstStructure* mlparam) {
  MLParam view;
  if (!mlparam)
    return view;

  gst_structure_foreach(mlparam, parse_mlparam_field, &view);
  return view;
}

void append_classifications_to_gst(const MLClassifications& output,
                                   GstMLClassifications* classifications) {
  for (const auto& entry : output) {
    GstMLClassification item = {};
    const std::string safe_name =
        sanitize_structure_name(entry.name, "classification");
    item.name = g_quark_from_string(safe_name.c_str());
    item.confidence = entry.confidence;
    item.color = entry.color;
    item.xtraparams = build_gst_structure(entry.extra);
    gst_ml_classifications_append(classifications, &item);
  }
}

void append_detections_to_gst(const MLDetections& output,
                              GstMLDetections* detections) {
  for (const auto& entry : output) {
    GstMLDetection item = {};
    const std::string safe_name = sanitize_structure_name(entry.name, "detection");
    item.name = g_quark_from_string(safe_name.c_str());
    item.confidence = entry.confidence;
    item.color = entry.color;
    item.top = entry.top;
    item.left = entry.left;
    item.bottom = entry.bottom;
    item.right = entry.right;

    if (!entry.landmarks.empty()) {
      item.landmarks = g_array_sized_new(
          FALSE, FALSE, sizeof(GstMLKeypoint), entry.landmarks.size());

      for (const auto& lmk : entry.landmarks) {
        GstMLKeypoint kp = {};
        kp.name = g_quark_from_string(lmk.name.c_str());
        kp.confidence = lmk.confidence;
        kp.color = lmk.color;
        kp.x = lmk.x;
        kp.y = lmk.y;
        g_array_append_val(item.landmarks, kp);
      }
    }

    item.xtraparams = build_gst_structure(entry.extra);
    gst_ml_detections_append(detections, &item);
  }
}

void append_poses_to_gst(const MLPoses& output, GstMLPoses* poses) {
  for (const auto& entry : output) {
    GstMLPose item = {};
    const std::string safe_name = sanitize_structure_name(entry.name, "pose");
    item.name = g_quark_from_string(safe_name.c_str());
    item.confidence = entry.confidence;

    if (!entry.keypoints.empty()) {
      item.keypoints = g_array_sized_new(
          FALSE, FALSE, sizeof(GstMLKeypoint), entry.keypoints.size());

      for (const auto& in_kp : entry.keypoints) {
        GstMLKeypoint kp = {};
        kp.name = g_quark_from_string(in_kp.name.c_str());
        kp.confidence = in_kp.confidence;
        kp.color = in_kp.color;
        kp.x = in_kp.x;
        kp.y = in_kp.y;
        g_array_append_val(item.keypoints, kp);
      }
    }

    if (!entry.links.empty()) {
      item.links = g_array_sized_new(
          FALSE, FALSE, sizeof(GstMLKeypointLink), entry.links.size());

      for (const auto& in_link : entry.links) {
        GstMLKeypointLink link = {};

        link.l_kp.name = g_quark_from_string(in_link.left.name.c_str());
        link.l_kp.confidence = in_link.left.confidence;
        link.l_kp.color = in_link.left.color;
        link.l_kp.x = in_link.left.x;
        link.l_kp.y = in_link.left.y;

        link.r_kp.name = g_quark_from_string(in_link.right.name.c_str());
        link.r_kp.confidence = in_link.right.confidence;
        link.r_kp.color = in_link.right.color;
        link.r_kp.x = in_link.right.x;
        link.r_kp.y = in_link.right.y;

        link.color = in_link.color;
        g_array_append_val(item.links, link);
      }
    }

    item.xtraparams = build_gst_structure(entry.extra);
    gst_ml_poses_append(poses, &item);
  }
}

void append_depth_maps_to_gst(const MLDepthMaps& output,
                              GstMLDepthMaps* depth_maps) {
  for (const auto& entry : output) {
    GstMLDepthMap item = {};
    item.n_rows = entry.n_rows;
    item.n_columns = entry.n_columns;

    if (!entry.values.empty()) {
      item.values = g_array_sized_new(
          FALSE, FALSE, sizeof(gdouble), entry.values.size());

      for (const auto& value : entry.values) {
        gdouble value_copy = value;
        g_array_append_val(item.values, value_copy);
      }
    }

    if (!entry.colors.empty()) {
      item.colors = g_array_sized_new(
          FALSE, FALSE, sizeof(guint32), entry.colors.size());

      for (const auto& color : entry.colors) {
        guint32 color_copy = color;
        g_array_append_val(item.colors, color_copy);
      }
    }

    item.xtraparams = build_gst_structure(entry.extra);
    gst_ml_depth_maps_append(depth_maps, &item);
  }
}

void append_segmentations_to_gst(const MLSegmentations& output,
                                 GstMLSegmentations* segmentations) {
  for (const auto& entry : output) {
    GstMLSegmentation item = {};
    item.n_rows = entry.n_rows;
    item.n_columns = entry.n_columns;

    if (!entry.labels.empty()) {
      item.labels = g_array_sized_new(
          FALSE, FALSE, sizeof(GQuark), entry.labels.size());

      for (const auto& label : entry.labels) {
        GQuark label_quark = g_quark_from_string(label.c_str());
        g_array_append_val(item.labels, label_quark);
      }
    }

    if (!entry.colors.empty()) {
      item.colors = g_array_sized_new(
          FALSE, FALSE, sizeof(guint32), entry.colors.size());

      for (const auto& color : entry.colors) {
        guint32 color_copy = color;
        g_array_append_val(item.colors, color_copy);
      }
    }

    item.xtraparams = build_gst_structure(entry.extra);
    gst_ml_segmentations_append(segmentations, &item);
  }
}

bool to_gst_ml_type(MLTensorType in, GstMLType* out) {
  if (!out)
    return false;

  switch (in) {
    case MLTensorType::Int8:
      *out = GST_ML_TYPE_INT8;
      return true;
    case MLTensorType::UInt8:
      *out = GST_ML_TYPE_UINT8;
      return true;
    case MLTensorType::Int16:
      *out = GST_ML_TYPE_INT16;
      return true;
    case MLTensorType::UInt16:
      *out = GST_ML_TYPE_UINT16;
      return true;
    case MLTensorType::Int32:
      *out = GST_ML_TYPE_INT32;
      return true;
    case MLTensorType::UInt32:
      *out = GST_ML_TYPE_UINT32;
      return true;
    case MLTensorType::Int64:
      *out = GST_ML_TYPE_INT64;
      return true;
    case MLTensorType::UInt64:
      *out = GST_ML_TYPE_UINT64;
      return true;
    case MLTensorType::Float16:
      *out = GST_ML_TYPE_FLOAT16;
      return true;
    case MLTensorType::Float32:
      *out = GST_ML_TYPE_FLOAT32;
      return true;
    case MLTensorType::Unknown:
    default:
      return false;
  }
}

bool validate_inplace_mlframe_output(const MLFrame& output,
                                     GstMLFrame* outmlframe) {
  if (!outmlframe)
    return false;

  const guint n_tensors = GST_ML_FRAME_N_TENSORS(outmlframe);
  if (output.tensors.size() != n_tensors)
    return false;

  for (guint idx = 0; idx < n_tensors; ++idx) {
    const auto& out_tensor = output.tensors[idx];

    GstMLType gst_type = GST_ML_TYPE_UNKNOWN;
    if (!to_gst_ml_type(out_tensor.type, &gst_type))
      return false;

    if (GST_ML_FRAME_TYPE(outmlframe) != gst_type)
      return false;

    const guint n_dims = GST_ML_FRAME_N_DIMENSIONS(outmlframe, idx);
    if (out_tensor.dimensions.size() != n_dims)
      return false;

    for (guint d = 0; d < n_dims; ++d) {
      if (out_tensor.dimensions[d] != GST_ML_FRAME_DIM(outmlframe, idx, d))
        return false;
    }

    const gsize expected = GST_ML_FRAME_BLOCK_SIZE(outmlframe, idx);
    if (out_tensor.size != expected)
      return false;

    if (expected == 0)
      continue;

    void* dst_data = GST_ML_FRAME_BLOCK_DATA(outmlframe, idx);
    if (!dst_data || out_tensor.data != dst_data)
      return false;
  }

  return true;
}

}  // namespace
gboolean on_process_image_classification(
    GstElement* /*postprocess*/, GstMLFrame* mlframe,
    GstStructure* mlparam, GstMLClassifications* classifications,
    gpointer user_data) {
  auto* handler = static_cast<PostprocessSignalHandler*>(user_data);
  if (!handler || !handler->classification_callback)
    return FALSE;

  try {
    MLFrame frame = build_mlframe(mlframe);
    MLParam params = build_mlparam(mlparam);
    MLClassifications output;

    if (!handler->classification_callback(frame, params, output))
      return FALSE;

    append_classifications_to_gst(output, classifications);
    return TRUE;
  } catch (...) {
    return FALSE;
  }
}

gboolean on_process_audio_classification(
    GstElement* /*postprocess*/, GstMLFrame* mlframe,
    GstStructure* mlparam, GstMLClassifications* classifications,
    gpointer user_data) {
  auto* handler = static_cast<PostprocessSignalHandler*>(user_data);
  if (!handler || !handler->classification_callback)
    return FALSE;

  try {
    MLFrame frame = build_mlframe(mlframe);
    MLParam params = build_mlparam(mlparam);
    MLClassifications output;

    if (!handler->classification_callback(frame, params, output))
      return FALSE;

    append_classifications_to_gst(output, classifications);
    return TRUE;
  } catch (...) {
    return FALSE;
  }
}

gboolean on_process_object_detection(
    GstElement* /*postprocess*/, GstMLFrame* mlframe,
    GstStructure* mlparam, GstMLDetections* detections,
    gpointer user_data) {
  auto* handler = static_cast<PostprocessSignalHandler*>(user_data);
  if (!handler || !handler->detection_callback)
    return FALSE;

  try {
    MLFrame frame = build_mlframe(mlframe);
    MLParam params = build_mlparam(mlparam);
    MLDetections output;

    if (!handler->detection_callback(frame, params, output))
      return FALSE;

    append_detections_to_gst(output, detections);
    return TRUE;
  } catch (...) {
    return FALSE;
  }
}

gboolean on_process_pose_estimation(
    GstElement* /*postprocess*/, GstMLFrame* mlframe,
    GstStructure* mlparam, GstMLPoses* poses,
    gpointer user_data) {
  auto* handler = static_cast<PostprocessSignalHandler*>(user_data);
  if (!handler || !handler->pose_callback)
    return FALSE;

  try {
    MLFrame frame = build_mlframe(mlframe);
    MLParam params = build_mlparam(mlparam);
    MLPoses output;

    if (!handler->pose_callback(frame, params, output))
      return FALSE;

    append_poses_to_gst(output, poses);
    return TRUE;
  } catch (...) {
    return FALSE;
  }
}

gboolean on_process_segmentation(
    GstElement* /*postprocess*/, GstMLFrame* mlframe,
    GstStructure* mlparam, GstMLSegmentations* segmentations,
    gpointer user_data) {
  auto* handler = static_cast<PostprocessSignalHandler*>(user_data);
  if (!handler || !handler->segmentation_callback)
    return FALSE;

  try {
    MLFrame frame = build_mlframe(mlframe);
    MLParam params = build_mlparam(mlparam);
    MLSegmentations output;

    if (!handler->segmentation_callback(frame, params, output))
      return FALSE;

    append_segmentations_to_gst(output, segmentations);
    return TRUE;
  } catch (...) {
    return FALSE;
  }
}

gboolean on_process_depth_estimation(
    GstElement* /*postprocess*/, GstMLFrame* mlframe,
    GstStructure* mlparam, GstMLDepthMaps* depth_maps,
    gpointer user_data) {
  auto* handler = static_cast<PostprocessSignalHandler*>(user_data);
  if (!handler || !handler->depth_callback)
    return FALSE;

  try {
    MLFrame frame = build_mlframe(mlframe);
    MLParam params = build_mlparam(mlparam);
    MLDepthMaps output;

    if (!handler->depth_callback(frame, params, output))
      return FALSE;

    append_depth_maps_to_gst(output, depth_maps);
    return TRUE;
  } catch (...) {
    return FALSE;
  }
}

gboolean on_process_tensors(
    GstElement* /*postprocess*/, GstMLFrame* mlframe,
    GstStructure* mlparam, GstMLFrame* outmlframe,
    gpointer user_data) {
  auto* handler = static_cast<PostprocessSignalHandler*>(user_data);
  if (!handler || !handler->tensors_callback)
    return FALSE;

  try {
    MLFrame frame = build_mlframe(mlframe);
    MLParam params = build_mlparam(mlparam);
    MLFrame output = build_mlframe(outmlframe);

    if (!handler->tensors_callback(frame, params, output))
      return FALSE;

    return static_cast<gboolean>(
        validate_inplace_mlframe_output(output, outmlframe));
  } catch (...) {
    return FALSE;
  }
}

void connect_external_postprocess_handler(PostprocessSignalHandler& handler,
                                          GstElement* postprocess) {
  if (!postprocess) {
    throw std::runtime_error("Postprocess element is null");
  }

  const char* signal_name = nullptr;
  GCallback callback = nullptr;

  switch (handler.type) {
    case PostprocessHandlerType::ImageClassification:
      if (!handler.classification_callback) {
        throw std::runtime_error(
            "ImageClassification handler callback is not set for element '" +
            handler.unique_name + "'");
      }
      signal_name = "process-image-classification";
      callback = G_CALLBACK(on_process_image_classification);
      break;
    case PostprocessHandlerType::AudioClassification:
      if (!handler.classification_callback) {
        throw std::runtime_error(
            "AudioClassification handler callback is not set for element '" +
            handler.unique_name + "'");
      }
      signal_name = "process-audio-classification";
      callback = G_CALLBACK(on_process_audio_classification);
      break;
    case PostprocessHandlerType::ObjectDetection:
      if (!handler.detection_callback) {
        throw std::runtime_error(
            "ObjectDetection handler callback is not set for element '" +
            handler.unique_name + "'");
      }
      signal_name = "process-object-detection";
      callback = G_CALLBACK(on_process_object_detection);
      break;
    case PostprocessHandlerType::PoseEstimation:
      if (!handler.pose_callback) {
        throw std::runtime_error(
            "PoseEstimation handler callback is not set for element '" +
            handler.unique_name + "'");
      }
      signal_name = "process-pose-estimation";
      callback = G_CALLBACK(on_process_pose_estimation);
      break;
    case PostprocessHandlerType::Segmentation:
      if (!handler.segmentation_callback) {
        throw std::runtime_error(
            "Segmentation handler callback is not set for element '" +
            handler.unique_name + "'");
      }
      signal_name = "process-segmentation";
      callback = G_CALLBACK(on_process_segmentation);
      break;
    case PostprocessHandlerType::DepthEstimation:
      if (!handler.depth_callback) {
        throw std::runtime_error(
            "DepthEstimation handler callback is not set for element '" +
            handler.unique_name + "'");
      }
      signal_name = "process-depth-estimation";
      callback = G_CALLBACK(on_process_depth_estimation);
      break;
    case PostprocessHandlerType::Tensors:
      if (!handler.tensors_callback) {
        throw std::runtime_error(
            "Tensors handler callback is not set for element '" +
            handler.unique_name + "'");
      }
      signal_name = "process-tensors";
      callback = G_CALLBACK(on_process_tensors);
      break;
  }

  if (!signal_name || !callback) {
    throw std::runtime_error("Unsupported postprocess handler type");
  }

  if (handler.signal_id != 0) {
    g_signal_handler_disconnect(postprocess, handler.signal_id);
    handler.signal_id = 0;
  }

  handler.signal_id =
      g_signal_connect(postprocess, signal_name, callback, &handler);

  if (handler.signal_id == 0) {
    throw std::runtime_error("Failed to connect signal '" +
                             std::string(signal_name) +
                             "' for element '" + handler.unique_name + "'");
  }
}

void disconnect_external_postprocess_handler(PostprocessSignalHandler& handler,
                                             GstElement* postprocess) {
  if (handler.signal_id == 0) {
    return;
  }

  if (postprocess) {
    g_signal_handler_disconnect(postprocess, handler.signal_id);
  }

  handler.signal_id = 0;
}

const MLParam::Value* MLParam::find(const std::string& key) const {
  for (const auto& field : fields) {
    if (field.first == key) {
      return &field.second;
    }
  }
  return nullptr;
}

bool MLParam::to_number(const Value& value, long double& out) {
  switch (value.type) {
    case ValueType::Int:
      out = static_cast<long double>(value.i);
      return true;
    case ValueType::UInt:
      out = static_cast<long double>(value.u);
      return true;
    case ValueType::Int64:
      out = static_cast<long double>(value.i64);
      return true;
    case ValueType::UInt64:
      out = static_cast<long double>(value.u64);
      return true;
    case ValueType::Float:
      out = static_cast<long double>(value.f);
      return true;
    case ValueType::Double:
      out = static_cast<long double>(value.d);
      return true;
    case ValueType::Bool:
      out = value.b ? 1.0L : 0.0L;
      return true;
    case ValueType::String:
      return false;
  }

  return false;
}

struct MLPostprocessBase::Impl {
  GstElement* elem_ = nullptr;
  GstElement* postprocess_elem_ = nullptr;
  std::string owner_name_;
  PostprocessSignalHandler signal_handler_;

  Impl(GstElement* elem,
       const char* expected_factory,
       const char* owner_name)
      : elem_(elem),
        owner_name_(owner_name ? owner_name : "MLPostprocessBase") {
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

    if (std::strcmp(expected_factory, "qtimlpostprocess") == 0) {
      postprocess_elem_ = elem_;
      return;
    }

    if (GST_IS_BIN(elem_)) {
      postprocess_elem_ = gst_bin_get_by_name(GST_BIN(elem_), "mlpostprocess");
      if (postprocess_elem_ && GST_IS_ELEMENT(postprocess_elem_)) {
        return;
      }
      if (postprocess_elem_) {
        gst_object_unref(postprocess_elem_);
        postprocess_elem_ = nullptr;
      }
    }

    throw std::runtime_error(owner_name_ +
                             ": cannot resolve internal 'mlpostprocess' "
                             "element for external postprocess handler");
  }

  ~Impl() {
    disconnect_handler();
    if (postprocess_elem_ && postprocess_elem_ != elem_) {
      gst_object_unref(postprocess_elem_);
      postprocess_elem_ = nullptr;
    }
  }

  template <typename CallbackSetter>
  void set_handler_impl(PostprocessHandlerType type, CallbackSetter&& setter) {
    GstElement* target = postprocess_elem_;
    if (!target) {
      throw std::runtime_error(owner_name_ +
                               ": postprocess target element is null");
    }

    const gchar* name = GST_ELEMENT_NAME(target);
    if (!name || !*name) {
      throw std::runtime_error(
          owner_name_ + " handler registration requires a named element");
    }

    signal_handler_.type = type;
    signal_handler_.classification_callback = {};
    signal_handler_.detection_callback = {};
    signal_handler_.pose_callback = {};
    signal_handler_.segmentation_callback = {};
    signal_handler_.depth_callback = {};
    signal_handler_.tensors_callback = {};

    setter(signal_handler_);

    signal_handler_.unique_name = name;
    connect_external_postprocess_handler(signal_handler_, target);
  }

  void disconnect_handler() {
    disconnect_external_postprocess_handler(signal_handler_, postprocess_elem_);
  }
};

MLPostprocessBase::MLPostprocessBase(
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

MLPostprocessBase::MLPostprocessBase(
    void* existing_gst_elem,
    const char* expected_factory,
    const char* owner_name)
    : Element(existing_gst_elem, true),
      impl_(std::make_unique<Impl>(
          static_cast<GstElement*>(get_raw_gst_element()),
          expected_factory,
          owner_name)) {
}

MLPostprocessBase::~MLPostprocessBase() = default;

MLPostprocessBase::MLPostprocessBase(
    MLPostprocessBase&&) noexcept = default;
MLPostprocessBase& MLPostprocessBase::operator=(
    MLPostprocessBase&& other) noexcept {
  if (this != &other) {
    Element::operator=(std::move(other));
    impl_ = std::move(other.impl_);
  }
  return *this;
}

MLPostprocessBase& MLPostprocessBase::set_handler(
    ClassificationPostprocessCallback handler) {
  if (!handler) {
    throw std::invalid_argument("Postprocess handler callback is empty");
  }

  impl_->set_handler_impl(PostprocessHandlerType::ImageClassification,
                          [&](PostprocessSignalHandler& signal_handler) {
                            signal_handler.classification_callback =
                                std::move(handler);
                          });
  return *this;
}

MLPostprocessBase& MLPostprocessBase::set_handler(
    ObjectDetectionPostprocessCallback handler) {
  if (!handler) {
    throw std::invalid_argument("Postprocess handler callback is empty");
  }

  impl_->set_handler_impl(PostprocessHandlerType::ObjectDetection,
                          [&](PostprocessSignalHandler& signal_handler) {
                            signal_handler.detection_callback =
                                std::move(handler);
                          });
  return *this;
}

MLPostprocessBase& MLPostprocessBase::set_handler(
    PoseEstimationPostprocessCallback handler) {
  if (!handler) {
    throw std::invalid_argument("Postprocess handler callback is empty");
  }

  impl_->set_handler_impl(PostprocessHandlerType::PoseEstimation,
                          [&](PostprocessSignalHandler& signal_handler) {
                            signal_handler.pose_callback = std::move(handler);
                          });
  return *this;
}

MLPostprocessBase& MLPostprocessBase::set_handler(
    DepthEstimationPostprocessCallback handler) {
  if (!handler) {
    throw std::invalid_argument("Postprocess handler callback is empty");
  }

  impl_->set_handler_impl(PostprocessHandlerType::DepthEstimation,
                          [&](PostprocessSignalHandler& signal_handler) {
                            signal_handler.depth_callback = std::move(handler);
                          });
  return *this;
}

MLPostprocessBase& MLPostprocessBase::set_handler(
    SegmentationPostprocessCallback handler) {
  if (!handler) {
    throw std::invalid_argument("Postprocess handler callback is empty");
  }

  impl_->set_handler_impl(PostprocessHandlerType::Segmentation,
                          [&](PostprocessSignalHandler& signal_handler) {
                            signal_handler.segmentation_callback =
                                std::move(handler);
                          });
  return *this;
}

MLPostprocessBase& MLPostprocessBase::set_handler(
    TensorsPostprocessCallback handler) {
  if (!handler) {
    throw std::invalid_argument("Postprocess handler callback is empty");
  }

  impl_->set_handler_impl(PostprocessHandlerType::Tensors,
                          [&](PostprocessSignalHandler& signal_handler) {
                            signal_handler.tensors_callback = std::move(handler);
                          });
  return *this;
}

}  // namespace qti
