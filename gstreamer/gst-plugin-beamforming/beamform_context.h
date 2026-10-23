/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
*/

#ifndef __GST_QTI_BEAMFORM_CONTEXT_H__
#define __GST_QTI_BEAMFORM_CONTEXT_H__

#include <gst/gst.h>

G_BEGIN_DECLS

#define GST_BEAMFORM_CONTEXT_CAST(obj) ((GstBeamformContext *)(obj))

/* Opaque context handle. The C element only ever holds a pointer; the real
 * struct is defined inside beamform_context.cc (C++). */
typedef struct _GstBeamformContext GstBeamformContext;

/* Plain-C configuration passed from the element's GObject properties. */
typedef struct _GstBeamformConfig {
  gint     image_width;
  gint     image_height;
  gint     camera_fov;
  gint     mic_orientation;
  gboolean mic_flip;
  gint     mic_resolution;
  gfloat   probability_threshold;

  gint     single_act_threshold;
  gint     single_act_deadline;
  gint     single_deact_delay;
  gint     multi_act_threshold;
  gint     multi_act_deadline;
  gint     multi_deact_delay;
  gint     multi_max_speakers;
  gboolean multi_enabled;

  gint     filter_select;        /* 0 = histogram, 1 = internal */
  gboolean remove_large_rois;
  gint     large_roi_percent;
} GstBeamformConfig;

/*
 * Create a new context for the given configuration.
 * [RETURNS] a handle, or NULL on failure.
 */
GstBeamformContext *
gst_beamform_context_new (const GstBeamformConfig * config);

/*
 * Free a context created with gst_beamform_context_new().
 */
void
gst_beamform_context_free (GstBeamformContext * context);

/*
 * Process one video buffer in place:
 *   - reads the vision ROIs (GstVideoRegionOfInterestMeta) on the buffer,
 *   - uses the supplied DOA data (parsed from the DOA sink pad; may be NULL),
 *   - fuses them to find the speaker, and
 *   - removes the ROI metas that are NOT the selected speaker.
 *
 * `doa_fields` is a GstStructure carrying the DOA fields (probability,
 * directions, polar-activity, fov-start/end-deg, timestamp) as delivered on
 * the element's text/x-raw DOA pad. It may be NULL (no DOA yet) in which case
 * all ROIs are left untouched (pass-through).
 *
 * [RETURNS] TRUE on success (buffer possibly modified), FALSE on error.
 */
gboolean
gst_beamform_context_process (GstBeamformContext * context,
                              GstBuffer * buffer,
                              const GstStructure * doa_fields);

G_END_DECLS

#endif // __GST_QTI_BEAMFORM_CONTEXT_H__
