/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifndef __QTI_ML_QNN_ENGINE_H__
#define __QTI_ML_QNN_ENGINE_H__

#include <gst/gst.h>
#include <gst/ml/ml-info.h>
#include <gst/ml/ml-frame.h>

G_BEGIN_DECLS

/**
 * GST_ML_QNN_ENGINE_OPT_MODEL:
 *
 * #G_TYPE_STRING, neural network model file path and name
 * Default: NULL
 */
#define GST_ML_QNN_ENGINE_OPT_MODEL "GstMLQNNEngine.model"

/**
 * GST_ML_QNN_ENGINE_OPT_BACKEND:
 *
 * #G_TYPE_STRING, set the delegate
 * Default: /usr/lib/libQnnCpu.so
 */
#define GST_ML_QNN_ENGINE_OPT_BACKEND "GstMLQNNEngine.backend"

/**
 * GST_ML_QNN_ENGINE_OPT_SYSLIB:
 *
 * #G_TYPE_STRING, QNN system library file path and name
 * Default: /usr/lib/libQnnSystem.so
 */
#define GST_ML_QNN_ENGINE_OPT_SYSLIB "GstMLQNNEngine.sysLib"

/**
 * GST_ML_QNN_ENGINE_OPT_BACKEND_DEVICE_ID:
 *
 * #G_TYPE_UINT, QNN backend device id
 * Default: 0
 */
#define GST_ML_QNN_ENGINE_OPT_BACKEND_DEVICE_ID "GstMLQNNEngine.backend_device_id"

/**
 * GST_ML_QNN_ENGINE_OPT_OUTPUTS:
 *
 * #G_TYPE_POINTER, list of output configurations
 * Default: NULL
 */
#define GST_ML_QNN_ENGINE_OPT_OUTPUTS "GstMLQNNEngine.outputs"

/**
 * GST_ML_QNN_ENGINE_OPT_BACKEND_OPTIONS:
 *
 * #GST_TYPE_STRUCTURE, backend specific options. Currently supports a "htp"
 * structure with a "perf-mode" field, either as a string name (e.g. "burst",
 * "power-saver") or as its underlying integer value. Sets the HTP DCVS
 * performance mode, which configures a distinct combination of core/bus
 * voltage corners, sleep latency and RPC polling behavior, as defined in the
 * QNN HTP backend documentation. Recognised names, in increasing performance
 * order: extreme-power-saver (1), low-power-saver (2), power-saver (3),
 * high-power-saver (4), low-balanced (5), balanced (6), high-performance (7),
 * sustained-high-performance (8), burst (9). Leaving "perf-mode" unset
 * leaves the HTP DCVS settings untouched.
 * Default: NULL
 */
#define GST_ML_QNN_ENGINE_OPT_BACKEND_OPTIONS \
    "GstMLQNNEngine.backend_options"

#define GET_OPT_MODEL(s) \
  gst_structure_get_string (s, GST_ML_QNN_ENGINE_OPT_MODEL)
#define GET_OPT_BACKEND(s) \
  gst_structure_get_string (s, GST_ML_QNN_ENGINE_OPT_BACKEND)
#define GET_OPT_SYSLIB(s) \
  gst_structure_get_string (s, GST_ML_QNN_ENGINE_OPT_SYSLIB)

typedef struct _GstMLQnnEngine GstMLQnnEngine;

GST_API GstMLQnnEngine *
gst_ml_qnn_engine_new                 (GstStructure * settings);

GST_API void
gst_ml_qnn_engine_free                (GstMLQnnEngine * engine);

GST_API const GstMLInfo *
gst_ml_qnn_engine_get_input_info      (GstMLQnnEngine * engine);

GST_API const GstMLInfo *
gst_ml_qnn_engine_get_output_info     (GstMLQnnEngine * engine);

GST_API gboolean
gst_ml_qnn_engine_execute             (GstMLQnnEngine * engine,
                                       GstMLFrame * inframe,
                                       GstMLFrame * outframe);

G_END_DECLS

#endif // __QTI_ML_QNN_ENGINE_H__
