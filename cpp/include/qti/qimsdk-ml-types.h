/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace qti {

// Tensor element type descriptor used by ML tensor payloads.
enum class MLTensorType {
  Unknown,
  Int8,
  UInt8,
  Int16,
  UInt16,
  Int32,
  UInt32,
  Int64,
  UInt64,
  Float16,
  Float32,
};

struct MLTensor {
  MLTensorType type = MLTensorType::Unknown;
  std::vector<uint32_t> dimensions;
  void* data = nullptr;
  size_t size = 0;
};

struct MLFrame {
  std::vector<MLTensor> tensors;
};



struct MLVideoPoint {
  double x = 0.0;
  double y = 0.0;
};

struct MLVideoQuadrilateral {
  MLVideoPoint a;
  MLVideoPoint b;
  MLVideoPoint c;
  MLVideoPoint d;
};

struct MLVideoRectangle {
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
};

struct MLVideoPlane {
  const void* data = nullptr;
  int stride = 0;
  uint32_t height = 0;
};

struct MLVideoImage {
  uint32_t width = 0;
  uint32_t height = 0;
  std::string format;
  std::vector<MLVideoPlane> planes;
};

struct MLVideoBlit {
  uint32_t mask = 0;
  MLVideoQuadrilateral source;
  MLVideoRectangle destination;
  uint8_t alpha = 0;
  int rotate = 0;
  MLVideoImage image;
};

struct MLVideoBlits {
  std::vector<MLVideoBlit> entries;
};

struct Region {
  float x = 0.0f;
  float y = 0.0f;
  float width = 1.0f;
  float height = 1.0f;
};

struct MLParam {
  enum class ValueType {
    Int,
    UInt,
    Int64,
    UInt64,
    Float,
    Double,
    Bool,
    String,
  };

  struct Value {
    ValueType type = ValueType::Int;
    int32_t i = 0;
    uint32_t u = 0;
    int64_t i64 = 0;
    uint64_t u64 = 0;
    float f = 0.0f;
    double d = 0.0;
    bool b = false;
    std::string s;

    Value() = default;
    Value(int32_t v) : type(ValueType::Int), i(v) {}
    Value(uint32_t v) : type(ValueType::UInt), u(v) {}
    Value(int64_t v) : type(ValueType::Int64), i64(v) {}
    Value(uint64_t v) : type(ValueType::UInt64), u64(v) {}
    Value(float v) : type(ValueType::Float), f(v) {}
    Value(double v) : type(ValueType::Double), d(v) {}
    Value(bool v) : type(ValueType::Bool), b(v) {}
    Value(const std::string& v) : type(ValueType::String), s(v) {}
    Value(const char* v) : type(ValueType::String), s(v) {}
  };

  std::vector<std::pair<std::string, Value>> fields;

  // Fetch a typed parameter by key. Returns false if key is absent or conversion fails.
  template <typename T>
  bool get(const std::string& key, T& out) const {
    if constexpr (std::is_same_v<T, Region>) {
      const std::string region_key_prefix =
          (key == "input-tensor-region") ? "input-region" : key;

      const bool has_x = get(region_key_prefix + "-x", out.x);
      const bool has_y = get(region_key_prefix + "-y", out.y);
      const bool has_width = get(region_key_prefix + "-width", out.width);
      const bool has_height = get(region_key_prefix + "-height", out.height);
      return has_x || has_y || has_width || has_height;
    }

    const Value* value = find(key);
    if (!value) {
      return false;
    }
    return convert(*value, out);
  }

 private:
  const Value* find(const std::string& key) const;

  static bool to_number(const Value& value, long double& out);

  template <typename T>
  static bool convert(const Value& value, T& out) {
    if constexpr (std::is_same_v<T, Value>) {
      out = value;
      return true;
    } else if constexpr (std::is_same_v<T, std::string>) {
      if (value.type != ValueType::String) {
        return false;
      }
      out = value.s;
      return true;
    } else if constexpr (std::is_same_v<T, const char*>) {
      if (value.type != ValueType::String) {
        return false;
      }
      out = value.s.c_str();
      return true;
    } else if constexpr (std::is_same_v<T, bool>) {
      long double number = 0.0L;
      if (!to_number(value, number)) {
        return false;
      }
      out = (number != 0.0L);
      return true;
    } else if constexpr (std::is_floating_point_v<T>) {
      long double number = 0.0L;
      if (!to_number(value, number)) {
        return false;
      }
      out = static_cast<T>(number);
      return true;
    } else if constexpr (std::is_integral_v<T>) {
      long double number = 0.0L;
      if (!to_number(value, number)) {
        return false;
      }

      if constexpr (std::is_signed_v<T>) {
        if (number < static_cast<long double>(std::numeric_limits<T>::min()) ||
            number > static_cast<long double>(std::numeric_limits<T>::max())) {
          return false;
        }
      } else {
        if (number < 0.0L ||
            number > static_cast<long double>(std::numeric_limits<T>::max())) {
          return false;
        }
      }

      out = static_cast<T>(number);
      return true;
    } else {
      return false;
    }
  }

 public:
};

struct MLExtraParam : public MLParam {
};

struct MLKeypoint {
  std::string name;
  float confidence = 0.0f;
  uint32_t color = 0;
  float x = 0.0f;
  float y = 0.0f;
};

struct MLKeypointLink {
  MLKeypoint left;
  MLKeypoint right;
  uint32_t color = 0;
};

struct MLClassification {
  std::string name;
  float confidence = 0.0f;
  uint32_t color = 0;
  MLExtraParam extra;
};

struct MLDetection {
  std::string name;
  float confidence = 0.0f;
  uint32_t color = 0;
  float top = 0.0f;
  float left = 0.0f;
  float bottom = 0.0f;
  float right = 0.0f;
  std::vector<MLKeypoint> landmarks;
  MLExtraParam extra;
};

struct MLPose {
  std::string name;
  float confidence = 0.0f;
  std::vector<MLKeypoint> keypoints;
  std::vector<MLKeypointLink> links;
  MLExtraParam extra;
};

struct MLDepthMap {
  std::vector<double> values;
  std::vector<uint32_t> colors;

  uint32_t n_rows = 0;
  uint32_t n_columns = 0;

  MLExtraParam extra;
};

struct MLSegmentation {
  std::vector<std::string> labels;
  std::vector<uint32_t> colors;

  uint32_t n_rows = 0;
  uint32_t n_columns = 0;

  MLExtraParam extra;
};

using MLClassifications = std::vector<MLClassification>;
using MLDetections = std::vector<MLDetection>;
using MLPoses = std::vector<MLPose>;
using MLDepthMaps = std::vector<MLDepthMap>;
using MLSegmentations = std::vector<MLSegmentation>;

}  // namespace qti
