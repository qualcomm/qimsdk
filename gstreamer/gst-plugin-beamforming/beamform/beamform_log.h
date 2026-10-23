/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifndef __GST_QTI_BEAMFORM_LOG_H__
#define __GST_QTI_BEAMFORM_LOG_H__

#include <cstdio>

#define AI_LOG_DEBUG(...) do { } while (0)
#define AI_LOG_INFO(...)  do { } while (0)
#define AI_LOG_WARN(fmt, ...) \
    fprintf(stderr, "[W][beamform] " fmt "\n", ##__VA_ARGS__)
#define AI_LOG_ERROR(fmt, ...) \
    fprintf(stderr, "[E][beamform] " fmt "\n", ##__VA_ARGS__)

#endif /* __GST_QTI_BEAMFORM_LOG_H__ */
