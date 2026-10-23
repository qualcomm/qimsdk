/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifndef __CIRCBUF_H__
#define __CIRCBUF_H__

#include <stdlib.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct circbuf {
   int32_t max_samples; /* maximum number of samples in the buffer */
   int32_t *samples;    /* circular sample buffer */
   int32_t *end_pos;    /* end of circular sample buffer */
   int32_t *write_pos;  /* pointer to next sample to write */
};

/*
 * Initializes a new circular buffer instance
 * [IN] max_samples - maximum number of samples the buffer can hold
 * [RETURNS]        - pointer to new circbuf instance, or NULL on failure
 */
struct circbuf *circbuf_connect(const int32_t max_samples);

/*
 * Frees memory allocated for circbuf instance
 * [IN] instance - pointer to circbuf instance
 */
void circbuf_disconnect(struct circbuf *instance);

/* Writes samples to the circular buffer
 * [IN] instance    - pointer to circbuf instance
 * [IN] samples     - array of samples to be written to the buffer
 * [IN] num_samples - number of samples in the array
 * [RETURNS]        - indicator of success or failure
 */
int32_t circbuf_write(struct circbuf * const instance,
                      const int32_t * const samples,
                      const int32_t num_samples);

#ifdef __cplusplus
};
#endif

#endif /*__CIRCBUF_H__*/
