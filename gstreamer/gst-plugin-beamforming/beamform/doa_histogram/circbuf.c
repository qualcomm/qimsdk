/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <inttypes.h>
#include <stdlib.h>

#include "doa_log.h"
#include "circbuf.h"

#undef LOG_TAG
#define LOG_TAG "doa_histogram"

struct circbuf *circbuf_connect(const int32_t max_samples) {
  struct circbuf *instance;
  int32_t idx;

  LOGV("%s: TRACE", __func__);

  if (max_samples <= 0) {
    LOGE("%s: max_samples is out of range: %" PRId32, __func__, max_samples);
    return NULL;
  }

  instance = (struct circbuf*)calloc(1, sizeof(struct circbuf));
  if (instance == NULL) {
    LOGE("%s: could not allocate memory for instance", __func__);
    goto clean;
  }

  instance->max_samples = max_samples;

  instance->samples = (int32_t*)calloc(instance->max_samples, sizeof(int32_t));
  if (instance->samples == NULL) {
    LOGE("%s: could not allocate buffer for samples", __func__);
    goto clean;
  }

  for (idx = 0; idx < instance->max_samples; ++idx)
    instance->samples[idx] = -1;

  instance->end_pos = instance->samples + instance->max_samples;
  instance->write_pos = instance->samples;

  return instance;

clean:
  circbuf_disconnect(instance);

  return NULL;
}

void circbuf_disconnect(struct circbuf *instance) {
  LOGV("%s: TRACE", __func__);

  if (instance != NULL) {
    if (instance->samples != NULL) {
      free(instance->samples);
      instance->samples = NULL;
    }

    free(instance);
    instance = NULL;
  }
}

int32_t circbuf_write(struct circbuf * const instance,
                      const int32_t * const samples,
                      const int32_t num_samples) {
  int32_t idx;

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
  if (num_samples > instance->max_samples) {
    LOGE("%s: num_samples[%" PRId32 "] greater than buffer capacity[%" PRId32 \
          "]", __func__, num_samples, instance->max_samples);
    return -1;
  }

  // write the contents of the sample array into the sample buffer,
  // wrapping to the beginning if necessary
  for (idx = 0; idx < num_samples; ++idx) {
    *(instance->write_pos) = samples[idx];
    ++(instance->write_pos);
    if (instance->write_pos == instance->end_pos)
      instance->write_pos = instance->samples;
  }

  return 0;
}
