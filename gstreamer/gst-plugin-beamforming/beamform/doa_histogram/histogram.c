/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "doa_log.h"
#include "histogram.h"

#undef LOG_TAG
#define LOG_TAG "doa_histogram"

#define HISTOGRAM_TOTAL_DEGREES   (360)
#define HISTOGRAM_CSV_MAX_BINS    (90)
#define HISTOGRAM_CSV_MAX_COLUMNS (5)

struct hist *hist_connect(const int32_t max_speakers,
                          const int32_t bin_resolution,
                          const int32_t filter_threshold,
                          const int32_t csv_logging,
                          const char * const log_filename) {
  struct hist *instance;
  int32_t idx;

  LOGV("%s: TRACE", __func__);

  if (max_speakers <= 0) {
    LOGE("%s: max_speakers is out of range: %" PRId32, __func__, max_speakers);
    return NULL;
  }
  if (bin_resolution <= 0 || bin_resolution > HISTOGRAM_TOTAL_DEGREES) {
    LOGE("%s: bin_resolution is out of range: %" PRId32, __func__,
          bin_resolution);
    return NULL;
  }
  if (bin_resolution % 2 == 1) {
    LOGE("%s: bin_resolution is not an even number: %" PRId32, __func__,
          bin_resolution);
    return NULL;
  }
  if (filter_threshold < 0) {
    LOGE("%s: filter_threshold is out of range: %" PRId32, __func__,
          filter_threshold);
    return NULL;
  }

  instance = (struct hist*)calloc(1, sizeof(struct hist));
  if (instance == NULL) {
    LOGE("%s: could not allocate memory for instance", __func__);
    goto clean;
  }

  instance->max_speakers = max_speakers;
  instance->bin_resolution = bin_resolution;
  instance->filter_threshold = filter_threshold;
  instance->buffer_length = HISTOGRAM_TOTAL_DEGREES / bin_resolution;
  instance->csv_logging = csv_logging;

  instance->hist_buffer = (int32_t*)calloc(instance->buffer_length,
                                           sizeof(int32_t));
  if (instance->hist_buffer == NULL) {
    LOGE("%s: could not allocate histogram buffer", __func__);
    goto clean;
  }

  instance->sort_buffer = (int32_t*)calloc(instance->buffer_length,
                                           sizeof(int32_t));
  if (instance->sort_buffer == NULL) {
    LOGE("%s: could not allocate sorting buffer", __func__);
    goto clean;
  }

  if (instance->csv_logging == 1 && log_filename != NULL) {
    instance->csv_log = fopen(log_filename, "w");
    if (instance->csv_log == NULL) {
      LOGE("%s: could not open CSV log file: %s", __func__, log_filename);
      instance->csv_logging = 0;
    } else {
      for (idx = 0; idx < HISTOGRAM_CSV_MAX_BINS; ++idx)
        fprintf(instance->csv_log, "b[%" PRId32 "],", idx);
      for (idx = 0; idx < HISTOGRAM_CSV_MAX_BINS; ++idx)
        fprintf(instance->csv_log, "f[%" PRId32 "],", idx);
      for (idx = 0; idx < HISTOGRAM_CSV_MAX_COLUMNS; ++idx)
        fprintf(instance->csv_log, "s[%" PRId32 "],", idx);
      fprintf(instance->csv_log, "thres\n");
      fflush(instance->csv_log);
    }
  }

  return instance;

clean:
  hist_disconnect(instance);

  return NULL;
}

void hist_disconnect(struct hist *instance) {
  LOGV("%s: TRACE", __func__);

  if (instance != NULL) {
    if (instance->csv_log != NULL) {
      fclose(instance->csv_log);
      instance->csv_log = NULL;
    }

    if (instance->sort_buffer != NULL) {
      free(instance->sort_buffer);
      instance->sort_buffer = NULL;
    }

    if (instance->hist_buffer != NULL) {
      free(instance->hist_buffer);
      instance->hist_buffer = NULL;
    }

    free(instance);
    instance = NULL;
  }
}

int32_t hist_run(struct hist * const instance,
                 int32_t * const samples,
                 const int32_t num_samples,
                 int32_t * const speakers) {
  int32_t idx, idx2;
  int32_t hist_bin;
  int32_t temp_bin1, temp_bin2;

  LOGV("%s: TRACE", __func__);

  if (instance == NULL) {
    LOGE("%s: invalid instance given", __func__);
    return -1;
  }
  if (samples == NULL) {
    LOGE("%s: invalid samples array given", __func__);
    return -1;
  }
  if (num_samples <= 0) {
    LOGE("%s: num_samples needs to be a positive number: %" PRId32, __func__,
          num_samples);
    return -1;
  }
  if (speakers == NULL) {
    LOGE("%s: invalid speakers array given", __func__);
    return -1;
  }

  /* initialize the histogram buffer */
  for (idx = 0; idx < instance->buffer_length; ++idx)
      instance->hist_buffer[idx] = 0;

  /* map each sample to a bin, and increment that bin */
  for (idx = 0; idx < num_samples; ++idx) {
    if (samples[idx] > -1) {
      hist_bin = roundf((float)(samples[idx] + instance->bin_resolution / 2) /
                        instance->bin_resolution);
      instance->hist_buffer[hist_bin - 1] += 1;
    }
  }

  if (instance->csv_logging == 1) {
    for (idx = 0; idx < HISTOGRAM_CSV_MAX_BINS; ++idx) {
      if (idx < instance->buffer_length)
        fprintf(instance->csv_log, "%" PRId32 ",", instance->hist_buffer[idx]);
      else
        fprintf(instance->csv_log, ",");
    }
  }

  /* filter the results through the threshold */
  for (idx = 0; idx < instance->buffer_length; ++idx) {
    if (instance->hist_buffer[idx] < instance->filter_threshold)
      instance->hist_buffer[idx] = -1;
  }

  if (instance->csv_logging == 1) {
    for (idx = 0; idx < HISTOGRAM_CSV_MAX_BINS; ++idx) {
      if (idx < instance->buffer_length && instance->hist_buffer[idx] != -1)
        fprintf(instance->csv_log, "%" PRId32 ",", instance->hist_buffer[idx]);
      else
        fprintf(instance->csv_log, ",");
    }
  }

  /* initialize the sorting buffer */
  for (idx = 0; idx < instance->buffer_length; ++idx)
    instance->sort_buffer[idx] = idx;

  /* sort the results in descending order of dominance */
  for (idx = 0; idx < instance->buffer_length; ++idx) {
    for (idx2 = idx + 1; idx2 < instance->buffer_length; ++idx2) {
      if (instance->hist_buffer[idx] < instance->hist_buffer[idx2]) {
        temp_bin1 = instance->sort_buffer[idx];
        temp_bin2 = instance->hist_buffer[idx];
        instance->sort_buffer[idx] = instance->sort_buffer[idx2];
        instance->hist_buffer[idx] = instance->hist_buffer[idx2];
        instance->sort_buffer[idx2] = temp_bin1;
        instance->hist_buffer[idx2] = temp_bin2;
      }
    }
  }

  /* populate the speakers array */
  for (idx = 0; idx < instance->max_speakers; ++idx) {
    if (instance->hist_buffer[idx] == -1) {
      speakers[idx] = -1;
    } else {
      speakers[idx] = instance->sort_buffer[idx] * instance->bin_resolution +
                      instance->bin_resolution / 2;
    }
  }

  if (instance->csv_logging == 1) {
    for (idx = 0; idx < HISTOGRAM_CSV_MAX_COLUMNS; ++idx) {
      if (idx < instance->max_speakers && speakers[idx] != -1)
        fprintf(instance->csv_log, "%" PRId32 ",", speakers[idx]);
      else
        fprintf(instance->csv_log, ",");
    }
    fprintf(instance->csv_log, "%" PRId32 "\n", instance->filter_threshold);
    fflush(instance->csv_log);
  }

  return 0;
}
