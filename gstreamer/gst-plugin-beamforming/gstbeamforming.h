/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifndef __GST_QTI_BEAMFORMING_H__
#define __GST_QTI_BEAMFORMING_H__

#include <gst/gst.h>

#include "beamform_context.h"
#include "beamformpads.h"

G_BEGIN_DECLS

#define GST_TYPE_BEAMFORMING  (gst_beamforming_get_type())
#define GST_BEAMFORMING(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_BEAMFORMING,GstBeamforming))
#define GST_BEAMFORMING_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_BEAMFORMING,GstBeamformingClass))
#define GST_IS_BEAMFORMING(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_BEAMFORMING))
#define GST_IS_BEAMFORMING_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_BEAMFORMING))
#define GST_BEAMFORMING_CAST(obj)       ((GstBeamforming *)(obj))

#define GST_BEAMFORMING_GET_LOCK(obj)   (&GST_BEAMFORMING(obj)->lock)
#define GST_BEAMFORMING_LOCK(obj)       g_mutex_lock(GST_BEAMFORMING_GET_LOCK(obj))
#define GST_BEAMFORMING_UNLOCK(obj)     g_mutex_unlock(GST_BEAMFORMING_GET_LOCK(obj))

typedef struct _GstBeamforming GstBeamforming;
typedef struct _GstBeamformingClass GstBeamformingClass;

struct _GstBeamforming {
  /// Inherited parent structure (this is a GstElement, like metamux).
  GstElement           parent;

  /// Global mutex lock.
  GMutex               lock;

  /// The video sink pad (buffers + person ROI metas, from AIMetaMux).
  GstBeamformSinkPad  *sinkpad;
  /// The DOA sink pad (text/x-raw DOA metadata, from DOA Src).
  GstBeamformDataPad  *doapad;
  /// The source pad (video out, only speaker ROI kept).
  GstBeamformSrcPad   *srcpad;

  /// Opaque fusion context (implemented in C++ in beamform_context.cc).
  GstBeamformContext  *context;

  /// Whether the context needs to be (re)created due to a property change.
  gboolean             needs_reconfigure;

  /// Properties.
  gint                 image_width;
  gint                 image_height;
  gint                 camera_fov;
  gint                 mic_orientation;
  gboolean             mic_flip;
  gint                 mic_resolution;
  gfloat               probability_threshold;

  gint                 single_act_threshold;
  gint                 single_act_deadline;
  gint                 single_deact_delay;
  gint                 multi_act_threshold;
  gint                 multi_act_deadline;
  gint                 multi_deact_delay;
  gint                 multi_max_speakers;
  gboolean             multi_enabled;

  gint                 filter_select;
  gboolean             remove_large_rois;
  gint                 large_roi_percent;
};

struct _GstBeamformingClass {
  GstElementClass parent;
};

GType gst_beamforming_get_type (void);

G_END_DECLS

#endif // __GST_QTI_BEAMFORMING_H__
