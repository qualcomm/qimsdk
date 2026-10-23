/****************************************************************************
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.       *
 * SPDX-License-Identifier: BSD-3-Clause-Clear                              *
 ****************************************************************************/

#ifndef __GST_QTI_ADOA_FILTER_H__
#define __GST_QTI_ADOA_FILTER_H__

#include <fstream>
#include <string>
#include <vector>

#include "beamform_types.h"

class AdoaFilter {
 public:
  AdoaFilter();
  ~AdoaFilter();

  int32_t Connect(const int32_t image_width,
                  const int32_t camera_fov,
                  const int32_t mic_orientation,
                  const bool mic_flip,
                  const int32_t mic_resolution,
                  const float probability_threshold,
                  const int32_t single_act_threshold,
                  const int32_t single_act_deadline,
                  const int32_t single_deact_delay,
                  const int32_t multi_act_threshold,
                  const int32_t multi_act_deadline,
                  const int32_t multi_deact_delay,
                  const int32_t multi_max_speakers,
                  const bool multi_enabled,
                  const int_filter_conf_t int_filter_conf,
                  const int32_t filter_select,
                  const bool csv_logging,
                  const std::string &filter_log_filename,
                  const std::string &histogram_log_filename);
  void Disconnect();
  void Filter(const std::vector<AdoaFrame> &frames,
              const std::vector<ai_crop_data> &rois,
              std::vector<target_t> &targets);

 private:
  struct targetdb_elem_t {
    struct target_t target;
    int32_t delay;
  };

  void CreateAnglePositionMappings();

  void Filter_histogram(const std::vector<AdoaFrame> &frames,
                        const std::vector<ai_crop_data> &rois,
                        std::vector<target_t> &targets);
  void Filter_internal(const std::vector<AdoaFrame> &frames,
                       const std::vector<ai_crop_data> &rois,
                       std::vector<target_t> &targets);
  int32_t GetCameraDirection(int32_t angle);

  int32_t image_width_;
  int32_t camera_fov_;
  int32_t mic_orientation_;
  bool mic_flip_;
  float probability_threshold_;
  int32_t single_deact_delay_;
  int32_t multi_deact_delay_;
  bool multi_enabled_;
  bool csv_logging_;
  int32_t histogram_buffer_length_;
  int32_t histogram_resolution_;
  int32_t histogram_threshold_;
  int32_t histogram_max_speakers_;

  std::vector<targetdb_elem_t> targetdb_;
  std::ofstream csv_log_;

  void *histogram_;
  int32_t *histogram_buffer_;
  target_t *vetted_targets_;
  int32_t *angle2position_;
  int32_t *position2angle_;

  int32_t filter_select_;
  int_filter_conf_t int_filter_conf_;
  bool doa_detected_;
  int32_t doa_detected_delay_val_;
  int32_t doa_change_delay_val_;
  std::vector<uint8_t> history_filter_list_[360];
  std::vector<uint32_t> input_audio_doa_detection_list_;
  std::vector<RectPos> signal_pos_map_;
};

#endif /* __GST_QTI_ADOA_FILTER_H__ */
