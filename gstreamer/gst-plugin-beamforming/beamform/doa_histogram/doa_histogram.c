/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <stdlib.h>

#include "doa_log.h"
#include "doa_histogram.h"
#include "histogram.h"
#include "circbuf.h"

#undef LOG_TAG
#define LOG_TAG "doa_histogram"

struct doa {
  struct circbuf *circbuf_instance; /* handle to the circ buffer instance */
  struct hist *hist_instance;       /* handle to the histogram instance */
};

void *doa_connect(const int32_t max_samples,
                  const int32_t max_speakers,
                  const int32_t bin_resolution,
                  const int32_t filter_threshold,
                  const int32_t csv_logging,
                  const char * const log_filename) {
  struct doa *instance;

  LOGV("%s: TRACE", __func__);

  instance = (struct doa*)calloc(1, sizeof(struct doa));
  if (instance == NULL) {
    LOGE("%s: could not allocate memory for instance", __func__);
    goto clean;
  }

  instance->circbuf_instance = circbuf_connect(max_samples);
  if (instance->circbuf_instance == NULL) {
    LOGE("%s: could not initialize the circbuf instance", __func__);
    goto clean;
  }

  instance->hist_instance = hist_connect(max_speakers, bin_resolution,
                                         filter_threshold, csv_logging,
                                         log_filename);
  if (instance->hist_instance == NULL) {
    LOGE("%s: could not initialize the hist instance", __func__);
    goto clean;
  }

  return (void*)instance;

clean:
  doa_disconnect((void*)instance);

  return NULL;
}

void doa_disconnect(void *doa_instance) {
  struct doa *instance = (struct doa*)doa_instance;

  LOGV("%s: TRACE", __func__);

  if (instance != NULL) {
    if (instance->hist_instance != NULL) {
      hist_disconnect(instance->hist_instance);
      instance->hist_instance = NULL;
    }

    if (instance->circbuf_instance != NULL) {
      circbuf_disconnect(instance->circbuf_instance);
      instance->circbuf_instance = NULL;
    }

    free(instance);
    instance = NULL;
  }
}

int32_t doa_write(void * const doa_instance,
                  const int32_t * const samples,
                  const int32_t num_samples) {
  struct doa * const instance = (struct doa*)doa_instance;

  LOGV("%s: TRACE", __func__);

  if (instance == NULL || instance->hist_instance == NULL ||
      instance->circbuf_instance == NULL) {
    LOGE("%s: invalid instance given", __func__);
    return -1;
  }
  if (samples == NULL) {
    LOGE("%s: invalid samples array given", __func__);
    return -1;
  }

  return circbuf_write(instance->circbuf_instance, samples, num_samples);
}

int32_t doa_read(void * const doa_instance, int32_t * const speakers) {
  struct doa * const instance = (struct doa*)doa_instance;

  LOGV("%s: TRACE", __func__);

  if (instance == NULL || instance->hist_instance == NULL ||
      instance->circbuf_instance == NULL) {
    LOGE("%s: invalid instance given", __func__);
    return -1;
  }
  if (speakers == NULL) {
    LOGE("%s: invalid speakers array given", __func__);
    return -1;
  }

  return hist_run(instance->hist_instance,
                  instance->circbuf_instance->samples,
                  instance->circbuf_instance->max_samples,
                  speakers);
}
