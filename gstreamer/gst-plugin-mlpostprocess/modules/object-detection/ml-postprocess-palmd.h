/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#pragma once

#include "qti-ml-post-process.h"
#include "qti-labels-parser.h"

#include <array>
#include <string>

typedef std::map<uint32_t, std::map<uint32_t, std::string>> LandmarksMap;

class Module : public IModule {
 public:
  Module(LogCallback cb);
  ~Module() {};

  std::string Caps() override;

  bool Configure(const std::string& labels_file,
                 const std::string& json_settings) override;

  bool Process(const Tensors& tensors, Dictionary& mlparams,
               std::any& output) override;

 private:
  float IntersectionScore(const ObjectDetection &l_entry,
                          const ObjectDetection &r_entry);

  int32_t NonMaxSuppression(const ObjectDetection &l_entry,
                            const ObjectDetections &entries);

  void ComputeTransformMatrix(ObjectDetection &entry, const Region& region);

  // Logging callback.
  LogCallback  logger_;
  // Confidence threshold value.
  double       threshold_;
  // Landmarks map.
  LandmarksMap landmarks_;
  // Labels parser.
  LabelsParser labels_parser_;
  // Anchors vector.
  std::vector<std::array<float, 2>> anchors_;
};
