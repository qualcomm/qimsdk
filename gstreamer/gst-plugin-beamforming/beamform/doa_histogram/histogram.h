/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifndef __HISTOGRAM_H__
#define __HISTOGRAM_H__

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct hist {
  int32_t max_speakers;     /* max number of speakers that will be returned */
  int32_t bin_resolution;   /* resolution of the histogram bins */
  int32_t filter_threshold; /* histogram cutoff threshold */
  int32_t buffer_length;    /* length of the histogram and sort buffers */
  int32_t *hist_buffer;     /* histogram buffer */
  int32_t *sort_buffer;     /* buffer used the sort the histogram results */
  int32_t csv_logging;      /* flag to enable CSV logging */
  FILE *csv_log;            /* file to dump the CSV log to */
};

/*
 * Initializes a new histogram instance
 * [IN] max_speakers     - maximum number of speakers that will be returned
 * [IN] bin_resolution   - resolution of the histogram bins
 * [IN] filter_threshold - histogram cutoff threshold
 * [IN] csv_logging      - flag to enable CSV logging
 * [IN] log_filename     - file to dump the CSV log to
 * [RETURNS]             - pointer to new histogram instance, or NULL on failure
 */
struct hist *hist_connect(const int32_t max_speakers,
                          const int32_t bin_resolution,
                          const int32_t filter_threshold,
                          const int32_t csv_logging,
                          const char * const log_filename);

/*
 * Frees memory allocated for histogram instance
 * [IN] instance - pointer to histogram instance
 */
void hist_disconnect(struct hist *instance);

/*
 * Generates a list of dominant speaker directions
 * [IN]  instance    - pointer to histogram instance
 * [IN]  samples     - array of samples used as input to the histogram
 * [IN]  num_samples - number of samples in the array
 * [OUT] speakers    - array of speaker directions derived from the samples,
 *                     sorted in descending order of dominance
 * [RETURNS]         - indicator of success or failure
 */
int32_t hist_run(struct hist * const instance,
                 int32_t * const samples,
                 const int32_t num_samples,
                 int32_t * const speakers);

#ifdef __cplusplus
};
#endif

#endif /*__HISTOGRAM_H__*/
