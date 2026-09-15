/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifndef __GST_QTI_STREAM_DEMUX_H__
#define __GST_QTI_STREAM_DEMUX_H__

#include <gst/gst.h>

G_BEGIN_DECLS

#define GST_TYPE_STREAM_DEMUX (gst_stream_demux_get_type())
#define GST_STREAM_DEMUX(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_STREAM_DEMUX,GstStreamDemux))
#define GST_STREAM_DEMUX_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_STREAM_DEMUX,GstStreamDemuxClass))
#define GST_IS_STREAM_DEMUX(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_STREAM_DEMUX))
#define GST_IS_STREAM_DEMUX_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_STREAM_DEMUX))

#define GST_STREAM_DEMUX_GET_LOCK(obj)   (&GST_STREAM_DEMUX(obj)->lock)
#define GST_STREAM_DEMUX_LOCK(obj)       g_mutex_lock(GST_STREAM_DEMUX_GET_LOCK(obj))
#define GST_STREAM_DEMUX_UNLOCK(obj)     g_mutex_unlock(GST_STREAM_DEMUX_GET_LOCK(obj))

typedef struct _GstStreamDemux GstStreamDemux;
typedef struct _GstStreamDemuxClass GstStreamDemuxClass;

struct _GstStreamDemux {
  /// Inherited parent structure.
  GstElement parent;

  /// Global mutex lock.
  GMutex     lock;

  /// TRUE if input is not a muxed stream; forward it unchanged.
  gboolean   passthrough;

  /// Next available index for the source pads.
  guint      nextidx;

  /// Convenient local reference to sink pad.
  GstPad     *sinkpad;
  /// Convenient local reference to source pads.
  GList      *srcpads;
};

struct _GstStreamDemuxClass {
  /// Inherited parent structure.
  GstElementClass parent;
};

GType gst_stream_demux_get_type (void);

G_END_DECLS

#endif // __GST_QTI_STREAM_DEMUX_H__
