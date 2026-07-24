/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#pragma once

#include "qti-ml-post-process.h"
#include "qti-labels-parser.h"

#include <array>
#include <string>
#include <vector>

// Raw per-anchor detection in the model's own pixel space, before any
// region-normalization or label lookup - what NMS operates on directly.
struct ObjectCandidate {
  uint32_t idx;
  float    confidence;

  float    angle;

  float    cx;
  float    cy;
  float    w;
  float    h;
};

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
  const Tensor& FindTensor(const Tensors& tensors, const std::string& name,
                           uint32_t fallback);

  float IntersectionScore(const ObjectCandidate &l_candidate,
                          const ObjectCandidate &r_candidate);

  int32_t NonMaxSuppression(const ObjectCandidate &candidate,
                            const std::vector<ObjectCandidate> &candidates);

  // Logging callback.
  LogCallback  logger_;
  // Confidence threshold value.
  double       threshold_;
  // Labels parser.
  LabelsParser labels_parser_;
};
