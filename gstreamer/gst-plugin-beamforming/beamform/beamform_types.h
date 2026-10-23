/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifndef __GST_QTI_BEAMFORM_TYPES_H__
#define __GST_QTI_BEAMFORM_TYPES_H__

#include <stdint.h>
#include <vector>

#define ADOA_TOTAL_DEGREES (360)
#define BEAMFORM_MAX_ROI   (100)

/* Fluence audio DSP metadata version. */
enum class FluenceVersion {
  V11,
  V13,
};

/* A rectangular region of interest in image-space coordinates. */
struct ai_crop_data {
  int32_t x;
  int32_t y;
  int32_t width;
  int32_t height;
  uint32_t color;
};

/*
 * A single Direction-of-Arrival (DOA) frame, as produced by the DOA source
 * plugin (FR204948) and consumed by this fusion plugin (FR204949).
 *
 * This mirrors the AdoaFrame structure from the ai-director project.
 */
struct AdoaFrame {
  std::vector<int32_t> directions;              /* candidate speaker angles */
  float probability;                            /* speech probability */
  int64_t timestamp;                            /* frame timestamp (usec) */
  int32_t fov_start_deg;                        /* start of camera FOV */
  int32_t fov_end_deg;                          /* end of camera FOV */
  uint8_t polar_activity[ADOA_TOTAL_DEGREES];   /* audio strength per degree */
};

/* A speaker target expressed as a horizontal image-space range. */
struct target_t {
  int32_t min;
  int32_t max;
};

/* Configuration for the "internal" (state-machine) filter. */
struct int_filter_conf_t {
  int32_t box_tracking_outside_percent;
  int32_t default_detection_audio_doa_list_size;
  int32_t history_filter_depth;
  int32_t default_signal_pos_list_size;
  int32_t detection_audio_doa_threshold_high;
  int32_t detection_audio_doa_threshold_low;
  int32_t doa_detected_delay;
  int32_t doa_change_delay;
  bool average_fov_filter_en;
};

/* Position/signal pair used by the internal filter. */
struct RectPos {
  uint32_t center;
  uint32_t signal;
};

#endif /* __GST_QTI_BEAMFORM_TYPES_H__ */
