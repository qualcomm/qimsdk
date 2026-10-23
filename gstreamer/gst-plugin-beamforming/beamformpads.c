/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "beamformpads.h"

/* ---------------- DOA item helpers ---------------- */

void
gst_beamform_doa_item_free (GstBeamformDoaItem * item)
{
  if (item == NULL)
    return;
  if (item->fields != NULL)
    gst_structure_free (item->fields);
  g_slice_free (GstBeamformDoaItem, item);
}

void
gst_beamform_data_pad_set_latest (GstBeamformDataPad * pad,
    const GstStructure * fields, GstClockTime timestamp)
{
  if (pad == NULL || fields == NULL)
    return;

  g_mutex_lock (&pad->lock);

  if (pad->latest != NULL)
    gst_beamform_doa_item_free (pad->latest);

  pad->latest = g_slice_new0 (GstBeamformDoaItem);
  pad->latest->fields = gst_structure_copy (fields);
  pad->latest->timestamp = timestamp;

  g_mutex_unlock (&pad->lock);
}

GstStructure *
gst_beamform_data_pad_get_latest (GstBeamformDataPad * pad)
{
  GstStructure *copy = NULL;

  if (pad == NULL)
    return NULL;

  g_mutex_lock (&pad->lock);
  if (pad->latest != NULL && pad->latest->fields != NULL)
    copy = gst_structure_copy (pad->latest->fields);
  g_mutex_unlock (&pad->lock);

  return copy;
}

/* ---------------- Sink (video) pad ---------------- */

G_DEFINE_TYPE (GstBeamformSinkPad, gst_beamform_sink_pad, GST_TYPE_PAD);

static void
gst_beamform_sink_pad_finalize (GObject * object)
{
  G_OBJECT_CLASS (gst_beamform_sink_pad_parent_class)->finalize (object);
}

static void
gst_beamform_sink_pad_class_init (GstBeamformSinkPadClass * klass)
{
  GObjectClass *gobject = G_OBJECT_CLASS (klass);
  gobject->finalize = gst_beamform_sink_pad_finalize;
}

static void
gst_beamform_sink_pad_init (GstBeamformSinkPad * pad)
{
  gst_segment_init (&pad->segment, GST_FORMAT_UNDEFINED);
}

/* ---------------- DOA data pad ---------------- */

G_DEFINE_TYPE (GstBeamformDataPad, gst_beamform_data_pad, GST_TYPE_PAD);

static void
gst_beamform_data_pad_finalize (GObject * object)
{
  GstBeamformDataPad *pad = GST_BEAMFORM_DATA_PAD (object);

  if (pad->latest != NULL) {
    gst_beamform_doa_item_free (pad->latest);
    pad->latest = NULL;
  }
  g_mutex_clear (&pad->lock);

  G_OBJECT_CLASS (gst_beamform_data_pad_parent_class)->finalize (object);
}

static void
gst_beamform_data_pad_class_init (GstBeamformDataPadClass * klass)
{
  GObjectClass *gobject = G_OBJECT_CLASS (klass);
  gobject->finalize = gst_beamform_data_pad_finalize;
}

static void
gst_beamform_data_pad_init (GstBeamformDataPad * pad)
{
  gst_segment_init (&pad->segment, GST_FORMAT_UNDEFINED);
  g_mutex_init (&pad->lock);
  pad->latest = NULL;
}

/* ---------------- Src pad ---------------- */

G_DEFINE_TYPE (GstBeamformSrcPad, gst_beamform_src_pad, GST_TYPE_PAD);

static void
gst_beamform_src_pad_finalize (GObject * object)
{
  G_OBJECT_CLASS (gst_beamform_src_pad_parent_class)->finalize (object);
}

static void
gst_beamform_src_pad_class_init (GstBeamformSrcPadClass * klass)
{
  GObjectClass *gobject = G_OBJECT_CLASS (klass);
  gobject->finalize = gst_beamform_src_pad_finalize;
}

static void
gst_beamform_src_pad_init (GstBeamformSrcPad * pad)
{
  gst_segment_init (&pad->segment, GST_FORMAT_UNDEFINED);
  pad->stmstart = TRUE;
}
