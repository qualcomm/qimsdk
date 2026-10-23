/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifndef __DOA_LOG_H__
#define __DOA_LOG_H__

#include <stdio.h>

#ifndef LOG_TAG
#define LOG_TAG "doa_histogram"
#endif

#define LOGV(...) do { } while (0)
#define LOGD(...) do { } while (0)
#define LOGI(...) do { } while (0)
#define LOGW(fmt, ...) \
    fprintf(stderr, "[W][" LOG_TAG "] " fmt "\n", ##__VA_ARGS__)
#define LOGE(fmt, ...) \
    fprintf(stderr, "[E][" LOG_TAG "] " fmt "\n", ##__VA_ARGS__)

#endif /* __DOA_LOG_H__ */
