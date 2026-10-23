/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifndef __GST_QTI_BEAMFORM_PADS_H__
#define __GST_QTI_BEAMFORM_PADS_H__

#include <gst/gst.h>

G_BEGIN_DECLS

#define GST_TYPE_BEAMFORM_SINK_PAD (gst_beamform_sink_pad_get_type())
#define GST_BEAMFORM_SINK_PAD(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_BEAMFORM_SINK_PAD,\
      GstBeamformSinkPad))
#define GST_IS_BEAMFORM_SINK_PAD(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_BEAMFORM_SINK_PAD))
#define GST_BEAMFORM_SINK_PAD_CAST(obj) ((GstBeamformSinkPad *)(obj))

#define GST_TYPE_BEAMFORM_DATA_PAD (gst_beamform_data_pad_get_type())
#define GST_BEAMFORM_DATA_PAD(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_BEAMFORM_DATA_PAD,\
      GstBeamformDataPad))
#define GST_IS_BEAMFORM_DATA_PAD(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_BEAMFORM_DATA_PAD))
#define GST_BEAMFORM_DATA_PAD_CAST(obj) ((GstBeamformDataPad *)(obj))

#define GST_TYPE_BEAMFORM_SRC_PAD (gst_beamform_src_pad_get_type())
#define GST_BEAMFORM_SRC_PAD(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_BEAMFORM_SRC_PAD,\
      GstBeamformSrcPad))
#define GST_IS_BEAMFORM_SRC_PAD(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_BEAMFORM_SRC_PAD))
#define GST_BEAMFORM_SRC_PAD_CAST(obj) ((GstBeamformSrcPad *)(obj))

typedef struct _GstBeamformSinkPad GstBeamformSinkPad;
typedef struct _GstBeamformSinkPadClass GstBeamformSinkPadClass;
typedef struct _GstBeamformDataPad GstBeamformDataPad;
typedef struct _GstBeamformDataPadClass GstBeamformDataPadClass;
typedef struct _GstBeamformSrcPad GstBeamformSrcPad;
typedef struct _GstBeamformSrcPadClass GstBeamformSrcPadClass;

/* One cached DOA entry: the parsed structure and its timestamp. */
typedef struct _GstBeamformDoaItem {
  GstStructure *fields;      /* owned copy of the DOA GstStructure */
  GstClockTime  timestamp;   /* PTS of the DOA buffer */
} GstBeamformDoaItem;

/* The video sink pad. */
struct _GstBeamformSinkPad {
  GstPad     parent;
  GstSegment segment;
};

struct _GstBeamformSinkPadClass {
  GstPadClass parent;
};

/* The DOA data pad. Caches the latest DOA entry (mutex-protected). */
struct _GstBeamformDataPad {
  GstPad             parent;
  GstSegment         segment;

  /// Protects the cached DOA item below.
  GMutex             lock;
  /// The most recently received DOA entry (or NULL if none yet).
  GstBeamformDoaItem *latest;
};

struct _GstBeamformDataPadClass {
  GstPadClass parent;
};

/* The source pad. */
struct _GstBeamformSrcPad {
  GstPad     parent;
  GstSegment segment;
  gboolean   stmstart;
};

struct _GstBeamformSrcPadClass {
  GstPadClass parent;
};

GType gst_beamform_sink_pad_get_type (void);
GType gst_beamform_data_pad_get_type (void);
GType gst_beamform_src_pad_get_type (void);

/* Store a copy of `fields`/`timestamp` as the latest DOA on the data pad. */
void gst_beamform_data_pad_set_latest (GstBeamformDataPad * pad,
                                       const GstStructure * fields,
                                       GstClockTime timestamp);

/* Return a copy of the latest DOA structure (caller frees), or NULL. */
GstStructure * gst_beamform_data_pad_get_latest (GstBeamformDataPad * pad);

/* Free a DOA item helper. */
void gst_beamform_doa_item_free (GstBeamformDoaItem * item);

G_END_DECLS

#endif // __GST_QTI_BEAMFORM_PADS_H__
