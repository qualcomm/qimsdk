/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifndef __GST_STREAM_DEMUX_PADS_H__
#define __GST_STREAM_DEMUX_PADS_H__

#include <gst/gst.h>
#include <gst/base/gstdataqueue.h>

G_BEGIN_DECLS

#define GST_TYPE_STREAM_DEMUX_SRCPAD (gst_stream_demux_srcpad_get_type())
#define GST_STREAM_DEMUX_SRCPAD(obj) \
    (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_STREAM_DEMUX_SRCPAD,GstStreamDemuxSrcPad))
#define GST_STREAM_DEMUX_SRCPAD_CLASS(klass) \
    (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_STREAM_DEMUX_SRCPAD,GstStreamDemuxSrcPadClass))
#define GST_IS_STREAM_DEMUX_SRCPAD(obj) \
    (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_STREAM_DEMUX_SRCPAD))
#define GST_IS_STREAM_DEMUX_SRCPAD_CLASS(klass) \
    (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_STREAM_DEMUX_SRCPAD))
#define GST_STREAM_DEMUX_SRCPAD_CAST(obj) ((GstStreamDemuxSrcPad *)(obj))

#define GST_STREAM_DEMUX_SRCPAD_GET_LOCK(obj) \
    (&GST_STREAM_DEMUX_SRCPAD(obj)->lock)
#define GST_STREAM_DEMUX_SRCPAD_LOCK(obj) \
    g_mutex_lock(GST_STREAM_DEMUX_SRCPAD_GET_LOCK(obj))
#define GST_STREAM_DEMUX_SRCPAD_UNLOCK(obj) \
    g_mutex_unlock(GST_STREAM_DEMUX_SRCPAD_GET_LOCK(obj))

#define GST_TYPE_STREAM_DEMUX_SINKPAD (gst_stream_demux_sinkpad_get_type())
#define GST_STREAM_DEMUX_SINKPAD(obj) \
    (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_STREAM_DEMUX_SINKPAD,GstStreamDemuxSinkPad))
#define GST_STREAM_DEMUX_SINKPAD_CLASS(klass) \
    (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_STREAM_DEMUX_SINKPAD,GstStreamDemuxSinkPadClass))
#define GST_IS_STREAM_DEMUX_SINKPAD(obj) \
    (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_STREAM_DEMUX_SINKPAD))
#define GST_IS_STREAM_DEMUX_SINKPAD_CLASS(klass) \
    (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_STREAM_DEMUX_SINKPAD))
#define GST_STREAM_DEMUX_SINKPAD_CAST(obj) ((GstStreamDemuxSinkPad *)(obj))

typedef struct _GstStreamDemuxSrcPad GstStreamDemuxSrcPad;
typedef struct _GstStreamDemuxSrcPadClass GstStreamDemuxSrcPadClass;
typedef struct _GstStreamDemuxSinkPad GstStreamDemuxSinkPad;
typedef struct _GstStreamDemuxSinkPadClass GstStreamDemuxSinkPadClass;

struct _GstStreamDemuxSrcPad {
  /// Inherited parent structure.
  GstPad       parent;

  /// Global mutex lock.
  GMutex       lock;
  /// Muxed stream ID with which this pad was created.
  guint        id;

  /// Segment.
  GstSegment   segment;
  /// Flag indicating whether STREAM_START event need to be sent.
  gboolean     stmstart;

  /// Worker queue.
  GstDataQueue *queue;
};

struct _GstStreamDemuxSrcPadClass {
  /// Inherited parent structure.
  GstPadClass parent;
};

struct _GstStreamDemuxSinkPad {
  /// Inherited parent structure.
  GstPad     parent;

  /// Segment.
  GstSegment segment;
};

struct _GstStreamDemuxSinkPadClass {
  /// Inherited parent structure.
  GstPadClass parent;
};

GType gst_stream_demux_srcpad_get_type (void);
GType gst_stream_demux_sinkpad_get_type (void);

G_END_DECLS

#endif // __GST_STREAM_DEMUX_PADS_H__
