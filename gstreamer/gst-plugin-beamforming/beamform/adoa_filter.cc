/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#define _USE_MATH_DEFINES
#include <cstring>
#include <cmath>

#include "adoa_filter.h"
#include "beamform_log.h"
#include "doa_histogram/doa_histogram.h"

#ifndef M_PI
#define M_PI (3.14159265358979323846)
#endif

#define ADOA_FILTER_TOTAL_DEGREES        (360)
#define ADOA_FILTER_TRIANGLE_DEGREES     (180)
#define ADOA_FILTER_RIGHT_ANGLE_DEGREES  (90)

using std::string;
using std::vector;

AdoaFilter::AdoaFilter() :
  image_width_(0),
  camera_fov_(0),
  mic_orientation_(0),
  mic_flip_(false),
  probability_threshold_(0),
  single_deact_delay_(0),
  multi_deact_delay_(0),
  multi_enabled_(false),
  csv_logging_(false),
  histogram_buffer_length_(0),
  histogram_resolution_(0),
  histogram_threshold_(0),
  histogram_max_speakers_(0),
  histogram_(nullptr),
  histogram_buffer_(nullptr),
  vetted_targets_(nullptr),
  angle2position_(nullptr),
  position2angle_(nullptr),
  filter_select_(0),
  doa_detected_(false),
  doa_detected_delay_val_(0),
  doa_change_delay_val_(0) {
  memset(&int_filter_conf_, 0, sizeof(int_filter_conf_));
}

AdoaFilter::~AdoaFilter() {
  Disconnect();
}

int32_t AdoaFilter::Connect(const int32_t image_width,
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
                            const string &filter_log_filename,
                            const string &histogram_log_filename) {
  (void) filter_log_filename;
  (void) histogram_log_filename;

  image_width_ = image_width;
  camera_fov_ = camera_fov;
  mic_orientation_ = mic_orientation;
  mic_flip_ = mic_flip;
  probability_threshold_ = probability_threshold;
  single_deact_delay_ = single_deact_delay;
  multi_deact_delay_ = multi_deact_delay;
  multi_enabled_ = multi_enabled;

  int_filter_conf_ = int_filter_conf;
  filter_select_ = filter_select;
  doa_detected_delay_val_ = int_filter_conf_.doa_detected_delay;
  doa_change_delay_val_ = int_filter_conf_.doa_change_delay;
  doa_detected_ = false;

  csv_logging_ = csv_logging;
  histogram_buffer_length_ =
      multi_enabled_ ? multi_act_deadline : single_act_deadline;
  histogram_resolution_ = mic_resolution;
  histogram_threshold_ =
      multi_enabled_ ? multi_act_threshold : single_act_threshold;
  histogram_max_speakers_ = multi_enabled_ ? multi_max_speakers : 1;

  histogram_buffer_ = new int32_t[histogram_max_speakers_];
  if (histogram_buffer_ == nullptr) {
    AI_LOG_ERROR("%s: unable to allocate DOA histogram buffer", __func__);
    goto error_exit;
  }

  vetted_targets_ = new struct target_t[histogram_max_speakers_];
  if (vetted_targets_ == nullptr) {
    AI_LOG_ERROR("%s: unable to allocate vetted targets buffer", __func__);
    goto error_exit;
  }

  angle2position_ = new int32_t[ADOA_FILTER_TOTAL_DEGREES];
  if (angle2position_ == nullptr) {
    AI_LOG_ERROR("%s: unable to allocate angle2position buffer", __func__);
    goto error_exit;
  }

  position2angle_ = new int32_t[image_width_];
  if (position2angle_ == nullptr) {
    AI_LOG_ERROR("%s: unable to allocate position2angle buffer", __func__);
    goto error_exit;
  }

  histogram_ = doa_connect(histogram_buffer_length_, histogram_max_speakers_,
                           histogram_resolution_, histogram_threshold_,
                           0, nullptr);
  if (histogram_ == nullptr) {
    AI_LOG_ERROR("%s: unable to initialize DOA histogram", __func__);
    goto error_exit;
  }

  CreateAnglePositionMappings();

  return 0;

error_exit:
  Disconnect();
  return -1;
}

void AdoaFilter::Disconnect() {
  if (histogram_ != nullptr) {
    doa_disconnect(histogram_);
    histogram_ = nullptr;
  }

  if (position2angle_ != nullptr) {
    delete[] position2angle_;
    position2angle_ = nullptr;
  }

  if (angle2position_ != nullptr) {
    delete[] angle2position_;
    angle2position_ = nullptr;
  }

  if (vetted_targets_ != nullptr) {
    delete[] vetted_targets_;
    vetted_targets_ = nullptr;
  }

  if (histogram_buffer_ != nullptr) {
    delete[] histogram_buffer_;
    histogram_buffer_ = nullptr;
  }
}

int32_t AdoaFilter::GetCameraDirection(int32_t angle) {
  angle = angle + mic_orientation_;
  angle = (angle + 360) % 360;
  if (angle > 180) {
    angle -= 360;
  }
  return angle;
}

void AdoaFilter::Filter(const vector<AdoaFrame> &frames,
                        const vector<ai_crop_data> &rois,
                        vector<target_t> &targets) {
  switch (filter_select_) {
    case 0:
      Filter_histogram(frames, rois, targets);
      break;
    case 1:
      Filter_internal(frames, rois, targets);
      break;
    default:
      Filter_histogram(frames, rois, targets);
      break;
  }
}

void AdoaFilter::Filter_internal(const vector<AdoaFrame> &frames,
                                 const vector<ai_crop_data> &rois,
                                 vector<target_t> &targets) {
  vector<target_t> potential_targets;
  for (int32_t idx = 0; idx < (int32_t) frames.size(); ++idx) {
    AdoaFrame frame = frames.at(idx);
    uint32_t average_out = 0;
    uint32_t out_cnt = 0;
    uint32_t average_in = 0;
    uint32_t in_cnt = 0;

    if (doa_detected_delay_val_ > 0) {
      doa_detected_delay_val_--;
    }

    if (doa_change_delay_val_ > 0) {
      doa_change_delay_val_--;
    }

    // Get average signal outside the FOV
    if (int_filter_conf_.average_fov_filter_en) {
      for (int32_t i = 0; i < ADOA_TOTAL_DEGREES; i++) {
        int32_t angle = GetCameraDirection(i);
        if (angle > (camera_fov_/2) ||
            angle < -(camera_fov_/2)) {
          average_out += frame.polar_activity[i];
          out_cnt++;
        }
      }
      if (out_cnt > 0)
        average_out = (int32_t)((float)average_out / (float)out_cnt);
    }

    // Populate the history for each degree in all bboxes
    for (int32_t i = 0; i < (int32_t) rois.size(); i++) {
      ai_crop_data ssd = rois.at(i);

      int32_t current_x = ssd.x;
      int32_t current_width = ssd.width;
      int32_t outside_size =
          current_width * (int_filter_conf_.box_tracking_outside_percent / 100.0);
      if (outside_size > current_x) {
        current_width += current_x * 2;
        current_x = 0;
      } else {
        current_x -= outside_size;
        current_width += outside_size * 2;
      }
      if (current_x + current_width > image_width_) {
        current_width -= (current_x + current_width) - image_width_;
      }

      int32_t last_deg = -1;
      for (int32_t x = 0; x < current_width; x++) {
        if (current_x < 0 || current_x >= image_width_) {
          current_x++;
          continue;
        }
        int32_t deg = position2angle_[current_x];

        if (deg != last_deg && deg >= 0 && deg < ADOA_FILTER_TOTAL_DEGREES) {
          if ((int32_t) history_filter_list_[deg].size() ==
              int_filter_conf_.history_filter_depth) {
            history_filter_list_[deg].erase(history_filter_list_[deg].begin());
          }
          history_filter_list_[deg].push_back(frame.polar_activity[deg]);

          if (int_filter_conf_.average_fov_filter_en) {
            average_in += frame.polar_activity[deg];
          }
          in_cnt++;

          last_deg = deg;
        }

        current_x++;
      }
    }

    // Get average signal in all bboxes
    if (int_filter_conf_.average_fov_filter_en && in_cnt > 0) {
      average_in = (int32_t)((float)average_in / (float)in_cnt);
    }

    // Calculate the dominant bbox based on history
    uint32_t signal_total = 0;
    uint32_t pos_total = 0;
    uint32_t signal_idx = 0;
    for (int32_t i = 0; i < (int32_t) rois.size(); i++) {
      ai_crop_data ssd = rois.at(i);

      int32_t current_x = ssd.x;
      int32_t current_width = ssd.width;
      int32_t outside_size =
          current_width * (int_filter_conf_.box_tracking_outside_percent / 100.0);
      if (outside_size > current_x) {
        current_width += current_x * 2;
        current_x = 0;
      } else {
        current_x -= outside_size;
        current_width += outside_size * 2;
      }
      if (current_x + current_width > image_width_) {
        current_width -= (current_x + current_width) - image_width_;
      }

      int32_t last_deg = -1;

      uint32_t average_signal_rect = 0;
      uint32_t average_rect_cnt = 0;
      for (int32_t x = 0; x < current_width; x++) {
        if (current_x < 0 || current_x >= image_width_) {
          current_x++;
          continue;
        }
        int32_t deg = position2angle_[current_x];

        if (deg != last_deg && deg >= 0 && deg < ADOA_FILTER_TOTAL_DEGREES) {
          uint32_t average = 0;
          for (uint32_t k = 0; k < history_filter_list_[deg].size(); k++) {
            average += history_filter_list_[deg].at(k);
          }
          if (!history_filter_list_[deg].empty())
            average_signal_rect +=
                (uint32_t)((float)average /
                (float)history_filter_list_[deg].size());
          average_rect_cnt++;

          last_deg = deg;
        }

        current_x++;
      }
      if (average_rect_cnt > 0)
        average_signal_rect =
            (int32_t)((float)average_signal_rect / (float)average_rect_cnt);

      if (average_signal_rect > signal_total) {
        signal_total = average_signal_rect;
        pos_total = ssd.x + ssd.width / 2;
        signal_idx = i;
      }
    }
    (void) signal_idx;

    // Clear an element from detection queue
    if ((int32_t) input_audio_doa_detection_list_.size() ==
        int_filter_conf_.default_detection_audio_doa_list_size) {
      input_audio_doa_detection_list_.erase(
          input_audio_doa_detection_list_.begin());
    }

    // Check voice flag and populate the detection queue
    if (frame.probability > probability_threshold_) {
      input_audio_doa_detection_list_.push_back(1);
    } else {
      input_audio_doa_detection_list_.push_back(0);
    }

    // Use dominant bbox signal if reach the threshold of average
    // inside and outside of FOV
    if (average_in >= average_out * 1.7 ||
        !int_filter_conf_.average_fov_filter_en) {
      RectPos pos;
      pos.center = pos_total;
      pos.signal = signal_total;
      if ((int32_t) signal_pos_map_.size() ==
          int_filter_conf_.default_signal_pos_list_size) {
        signal_pos_map_.erase(signal_pos_map_.begin());
      }
      signal_pos_map_.push_back(pos);
    }

    // Calculate the coefficient of detection queue and decide if
    // there is speech
    uint32_t sum = 0;
    for (int32_t i = 0; i < (int32_t) input_audio_doa_detection_list_.size(); i++) {
      sum += input_audio_doa_detection_list_.at(i);
    }
    float detection_coef =
        input_audio_doa_detection_list_.empty() ? 0.0f :
        ((float)sum / (float)input_audio_doa_detection_list_.size()) * 100.0;
    if (detection_coef < int_filter_conf_.detection_audio_doa_threshold_low &&
        !doa_detected_delay_val_) {
      if (doa_detected_) {
        doa_detected_delay_val_ = int_filter_conf_.doa_detected_delay / 2;
      }
      doa_detected_ = false;

      signal_pos_map_.clear();
    }
    if ((detection_coef >=
        int_filter_conf_.detection_audio_doa_threshold_high || doa_detected_) &&
        !doa_detected_delay_val_) {
      if (!doa_detected_) {
        doa_detected_delay_val_ = int_filter_conf_.doa_detected_delay;
      }
      doa_detected_ = true;
    }

    if (doa_detected_) {
      uint32_t box_sum_total = 0;
      int32_t box_idx = -1;
      uint32_t box_sum_total_sec = 0;
      if (!doa_change_delay_val_) {
        // Get highest signal bbox
        for (int32_t i = 0; i < (int32_t) rois.size(); i++) {
          ai_crop_data ssd = rois.at(i);
          uint32_t ssd_center = ssd.x + ssd.width / 2;
          uint32_t ssd_half = ssd.width / 2;

          uint32_t signal_sum = 0;
          for (int32_t ps = 0; ps < (int32_t) signal_pos_map_.size(); ps++) {
            RectPos pos = signal_pos_map_.at(ps);

            if (pos.center <= (ssd_center + ssd_half) &&
                pos.center >= (ssd_center - ssd_half)) {
              signal_sum += pos.signal;
            }
          }

          if (signal_sum > box_sum_total) {
            box_sum_total = signal_sum;
            box_idx = i;
          }
        }

        // Get second highest signal bbox
        for (int32_t i = 0; i < (int32_t) rois.size(); i++) {
          if (i == box_idx) {
            continue;
          }

          ai_crop_data ssd = rois.at(i);
          uint32_t ssd_center = ssd.x + ssd.width / 2;
          uint32_t ssd_half = ssd.width / 2;

          uint32_t signal_sum = 0;
          for (int32_t ps = 0; ps < (int32_t) signal_pos_map_.size(); ps++) {
            RectPos pos = signal_pos_map_.at(ps);

            if (pos.center <= (ssd_center + ssd_half) &&
                pos.center >= (ssd_center - ssd_half)) {
              signal_sum += pos.signal;
            }
          }

          if (signal_sum > box_sum_total_sec) {
            box_sum_total_sec = signal_sum;
          }
        }

        // Switch to bbox if there is enough difference between top two highest
        if (box_idx != -1 && box_sum_total >= box_sum_total_sec * 1.10) {
          ai_crop_data ssd = rois.at(box_idx);

          target_t target;
          target.min = ssd.x;
          target.max = ssd.x + ssd.width;
          doa_change_delay_val_ = int_filter_conf_.doa_change_delay;
          potential_targets.push_back(target);
        }
      }
    }
  }

  // remove any targets in the database that have expired
  for (vector<targetdb_elem_t>::iterator target = targetdb_.begin();
       target != targetdb_.end(); ) {
    if (--(target->delay) <= 0)
      target = targetdb_.erase(target);
    else
      ++target;
  }

  for (const target_t &p_target : potential_targets) {
    // search for existing target
    bool found = false;
    for (targetdb_elem_t &db_target : targetdb_) {
      if (p_target.min == db_target.target.min &&
          p_target.max == db_target.target.max) {
        db_target.delay =
            (multi_enabled_ ? multi_deact_delay_ : single_deact_delay_);
        found = true;
      }
    }

    if (!found && (multi_enabled_ || targetdb_.empty())) {
      targetdb_elem_t new_target;
      new_target.target = p_target;
      new_target.delay =
          (multi_enabled_ ? multi_deact_delay_ : single_deact_delay_);
      targetdb_.push_back(new_target);
    }
  }

  // output the active targets in the database
  for (const targetdb_elem_t &db_target : targetdb_)
    targets.push_back(db_target.target);
}

void AdoaFilter::Filter_histogram(const vector<AdoaFrame> &frames,
                        const vector<ai_crop_data> &rois,
                        vector<target_t> &targets) {
  for (int32_t idx = 0; idx < (int32_t) frames.size(); ++idx) {
    AdoaFrame frame = frames.at(idx);
    int32_t num_dirs = (int32_t) frame.directions.size();
    if (num_dirs <= 0)
      continue;
    vector<int32_t> filtered_directions(num_dirs, 0);

    // filter directions through the speech probability
    if (frame.probability > probability_threshold_) {
      for (int32_t idx2 = 0; idx2 < num_dirs; ++idx2)
        filtered_directions[idx2] = frame.directions[idx2];
    } else {
      for (int32_t idx2 = 0; idx2 < num_dirs; ++idx2)
        filtered_directions[idx2] = -1;
    }

    // write list of filtered directions to the DOA histogram
    int32_t ret = doa_write(histogram_, filtered_directions.data(), num_dirs);
    if (ret < 0) {
      AI_LOG_ERROR("%s: error writing directions to DOA: %d", __func__, ret);
      return;
    }
  }

  // read list of unvetted targets from the DOA histogram
  int32_t ret = doa_read(histogram_, histogram_buffer_);
  if (ret < 0) {
    AI_LOG_ERROR("%s: error reading targets from DOA: %d", __func__, ret);
    return;
  }

  // clear the vetted target list
  for (int32_t idx = 0; idx < histogram_max_speakers_; ++idx) {
    vetted_targets_[idx].min = -1;
    vetted_targets_[idx].max = -1;
  }

  for (int32_t idx = 0; idx < (int32_t) rois.size(); ++idx) {
    ai_crop_data roi = rois.at(idx);

    // get the ROI position boundaries, cropping if necessary
    int32_t min_roi_position = roi.x;
    int32_t width = roi.width;
    if (roi.x + roi.width > image_width_)
      width -= (roi.x + roi.width) - image_width_;
    int32_t max_roi_position = roi.x + width;

    if (min_roi_position < 0) min_roi_position = 0;
    if (max_roi_position >= image_width_) max_roi_position = image_width_ - 1;
    if (min_roi_position > max_roi_position) continue;

    // map the ROI boundaries to min and max microphone angles
    int32_t min_mic_angle = position2angle_[min_roi_position];
    int32_t max_mic_angle = position2angle_[max_roi_position];

    // if any histogram bins intersect the ROI, then mark those bins as vetted
    // and map each bin to target min and max positions
    for (int32_t sidx = 0; sidx < histogram_max_speakers_; ++sidx) {
      if (vetted_targets_[sidx].min == -1 && histogram_buffer_[sidx] != -1) {
        int32_t min_bin_angle = histogram_buffer_[sidx] -
                                histogram_resolution_ / 2;
        int32_t max_bin_angle = histogram_buffer_[sidx] +
                                histogram_resolution_ / 2;
        for (int32_t angle = min_bin_angle; angle <= max_bin_angle; ++angle) {
          if (min_mic_angle <= angle && angle <= max_mic_angle) {
            int32_t a_min = (min_bin_angle % ADOA_FILTER_TOTAL_DEGREES +
                             ADOA_FILTER_TOTAL_DEGREES) %
                            ADOA_FILTER_TOTAL_DEGREES;
            int32_t a_max = (max_bin_angle % ADOA_FILTER_TOTAL_DEGREES +
                             ADOA_FILTER_TOTAL_DEGREES) %
                            ADOA_FILTER_TOTAL_DEGREES;
            vetted_targets_[sidx].min = angle2position_[a_min];
            vetted_targets_[sidx].max = angle2position_[a_max];
            break;
          }
        }
      }
    }
  }

  // convert the array of target positions into a vector and remove duplicates
  vector<target_t> potential_targets;
  for (int32_t idx = 0; idx < histogram_max_speakers_; ++idx) {
    if (vetted_targets_[idx].min != -1) {
      bool found = false;
      for (const target_t &p_target : potential_targets) {
        if (p_target.min == vetted_targets_[idx].min &&
            p_target.max == vetted_targets_[idx].max) {
          found = true;
          break;
        }
      }
      if (!found) potential_targets.push_back(vetted_targets_[idx]);
    }
  }

  // remove any targets in the database that have expired
  for (vector<targetdb_elem_t>::iterator target = targetdb_.begin();
       target != targetdb_.end(); ) {
    if (--(target->delay) <= 0)
      target = targetdb_.erase(target);
    else
      ++target;
  }

  for (const target_t &p_target : potential_targets) {
    bool found = false;
    for (targetdb_elem_t &db_target : targetdb_) {
      if (p_target.min == db_target.target.min &&
          p_target.max == db_target.target.max) {
        db_target.delay =
            (multi_enabled_ ? multi_deact_delay_ : single_deact_delay_);
        found = true;
      }
    }

    if (!found && (multi_enabled_ || targetdb_.empty())) {
      targetdb_elem_t new_target;
      new_target.target = p_target;
      new_target.delay =
          (multi_enabled_ ? multi_deact_delay_ : single_deact_delay_);
      targetdb_.push_back(new_target);
    }
  }

  // output the active targets in the database
  for (const targetdb_elem_t &db_target : targetdb_)
    targets.push_back(db_target.target);
}

// The endpoints of the image width and the camera can be used to form a
// triangle. That triangle can then be split into 2 right triangles (back to
// back) by a bisecting line from the camera to the center of the image.
// Using right triangle properties and the law of sines, it is possible to solve
// for the length of the bisecting line. It is then possible to solve for the
// position in the image corresponding to each angle in the camera's FOV.
//
// LIMITATION: image_width and camera_fov need to be even numbers.
void AdoaFilter::CreateAnglePositionMappings() {
  vector<int32_t> angle2position_temp(camera_fov_, 0);

  // calculate the angle opposite the bisecting side
  float opposite_angle = (float)(ADOA_FILTER_TRIANGLE_DEGREES -
                                 ADOA_FILTER_RIGHT_ANGLE_DEGREES -
                                 camera_fov_ / 2);

  // calculate the length of the bisecting side
  float bisecting_side = (float)(image_width_ / 2) *
                         sinf((float)opposite_angle *
                              (M_PI / ADOA_FILTER_TRIANGLE_DEGREES)) /
                         sinf((float)(camera_fov_ / 2) *
                              (M_PI / ADOA_FILTER_TRIANGLE_DEGREES));

  // solve for half of the image positions
  for (int32_t idx = 0; idx < camera_fov_ / 2; ++idx) {
    float raw_position = (float)bisecting_side *
                         sinf((float)(camera_fov_ / 2 - idx) *
                              (M_PI / ADOA_FILTER_TRIANGLE_DEGREES)) /
                         sinf((float)(ADOA_FILTER_TRIANGLE_DEGREES -
                                      ADOA_FILTER_RIGHT_ANGLE_DEGREES -
                                      (camera_fov_ / 2 - idx)) *
                              (M_PI / ADOA_FILTER_TRIANGLE_DEGREES));
    angle2position_temp[idx] = (int32_t)(roundf((float)(image_width_ / 2) -
                                                raw_position));
  }

  // extrapolate the results for the other half of the image by mirroring
  for (int32_t idx = 0, idx2 = camera_fov_ - 1; idx < camera_fov_ / 2;
       ++idx, --idx2)
    angle2position_temp[idx2] = image_width_ - 1 - angle2position_temp[idx];

  // map the results to the angle2position array, adjusting for mic orientation
  // and polarity
  for (int32_t idx = 0; idx < ADOA_FILTER_TOTAL_DEGREES; ++idx)
    angle2position_[idx] = -1;
  for (int32_t idx = 0; idx < camera_fov_; ++idx) {
    int32_t new_idx;
    if (mic_flip_) {
      new_idx = ((camera_fov_ - 1 - idx) + mic_orientation_ - camera_fov_ / 2 +
                 ADOA_FILTER_TOTAL_DEGREES) % ADOA_FILTER_TOTAL_DEGREES;
    } else {
      new_idx = (idx + mic_orientation_ - camera_fov_ / 2 +
                 ADOA_FILTER_TOTAL_DEGREES) % ADOA_FILTER_TOTAL_DEGREES;
    }
    angle2position_[new_idx] = angle2position_temp[idx];
  }

  // populate matching positions in the position2angle array
  for (int32_t idx = 0; idx < image_width_; ++idx)
    position2angle_[idx] = -1;
  for (int32_t idx = 0; idx < camera_fov_; ++idx) {
    int32_t pos = angle2position_temp[idx];
    if (pos < 0 || pos >= image_width_)
      continue;
    if (mic_flip_)
      position2angle_[pos] = (camera_fov_ - 1) - idx;
    else
      position2angle_[pos] = idx;
  }

  // fill out the rest of the position2angle array
  for (int32_t idx = 1; idx < image_width_ / 2; ++idx) {
    if (position2angle_[idx] == -1)
      position2angle_[idx] = position2angle_[idx - 1];
  }
  for (int32_t idx = image_width_ - 2; idx >= image_width_ / 2; --idx) {
    if (position2angle_[idx] == -1)
      position2angle_[idx] = position2angle_[idx + 1];
  }

  // adjust the position2angle array for mic orientation
  for (int32_t idx = 0; idx < image_width_; ++idx) {
    if (position2angle_[idx] < 0)
      position2angle_[idx] = 0;
    position2angle_[idx] = (position2angle_[idx] + mic_orientation_ -
                            camera_fov_ / 2 + ADOA_FILTER_TOTAL_DEGREES) %
                           ADOA_FILTER_TOTAL_DEGREES;
  }
}
