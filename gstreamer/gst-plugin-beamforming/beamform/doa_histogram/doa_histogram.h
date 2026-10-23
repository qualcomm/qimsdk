/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifndef __DOA_HISTOGRAM_H__
#define __DOA_HISTOGRAM_H__

#include <stdlib.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Initializes a new DOA instance
 * [IN] max_samples      - maximum number of samples in the buffer
 * [IN] max_speakers     - maximum number of speakers that will be returned
 * [IN] bin_resolution   - resolution of the histogram bins
 * [IN] filter_threshold - histogram cutoff threshold
 * [IN] csv_logging      - flag to enable CSV logging
 * [IN] log_filename     - file to dump the CSV log to
 * [RETURNS]             - pointer to the new DOA instance, or NULL on failure
 */
void *doa_connect(const int32_t max_samples,
                  const int32_t max_speakers,
                  const int32_t bin_resolution,
                  const int32_t filter_threshold,
                  const int32_t csv_logging,
                  const char * const log_filename);

/*
 * Frees memory allocated for DOA instance
 * [IN] doa_instance - pointer to the DOA instance
 */
void doa_disconnect(void *doa_instance);

/*
 * Writes DOA samples to the buffer
 * [IN] doa_instance - pointer to the DOA instance
 * [IN] samples      - array of samples to be written to the buffer
 * [IN] num_samples  - number of samples in the array
 */
int32_t doa_write(void * const doa_instance,
                  const int32_t * const samples,
                  const int32_t num_samples);

/*
 * Provides a sorted array of the strongest DOAs
 * [IN]  doa_instance - pointer to the DOA instance
 * [OUT] speakers     - array to store the list of speakers
 */
int32_t doa_read(void * const doa_instance, int32_t * const speakers);

#ifdef __cplusplus
};
#endif

#endif /*__DOA_HISTOGRAM_H__*/
