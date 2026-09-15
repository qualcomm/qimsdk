/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include "streamdemuxpads.h"

GST_DEBUG_CATEGORY_EXTERN (gst_stream_demux_debug);
#define GST_CAT_DEFAULT gst_stream_demux_debug

G_DEFINE_TYPE (GstStreamDemuxSrcPad, gst_stream_demux_srcpad, GST_TYPE_PAD);
G_DEFINE_TYPE (GstStreamDemuxSinkPad, gst_stream_demux_sinkpad, GST_TYPE_PAD);

static gboolean
queue_is_full_cb (GstDataQueue * queue, guint visible, guint bytes,
    guint64 time, gpointer checkdata)
{
  // There won't be any condition limiting for the buffer queue size.
  return FALSE;
}

static void
gst_stream_demux_srcpad_worker_task (gpointer userdata)
{
  GstStreamDemuxSrcPad *srcpad = GST_STREAM_DEMUX_SRCPAD (userdata);
  GstDataQueueItem *item = NULL;
  gpointer object = NULL;

  if (gst_data_queue_pop (srcpad->queue, &item)) {
    // Take the object from the queue item and null the object pointer.
    object = gst_mini_object_ref (item->object);
    item->destroy (item);

    if (G_LIKELY (GST_IS_BUFFER (object))) {
      GstBuffer *buffer = GST_BUFFER_CAST (object);

      GST_STREAM_DEMUX_SRCPAD_LOCK (srcpad);

      // Initialize and send the source pad segment for synchronization.
      if (GST_FORMAT_UNDEFINED == srcpad->segment.format) {
        GstEvent *event = NULL;

        gst_segment_init (&(srcpad->segment), GST_FORMAT_TIME);
        event = gst_event_new_segment (&(srcpad)->segment);

        GST_DEBUG_OBJECT (srcpad, "Pushing %" GST_PTR_FORMAT, event);
        gst_pad_push_event (GST_PAD (srcpad), event);
      }

      if (GST_CLOCK_TIME_IS_VALID (GST_BUFFER_TIMESTAMP (buffer)))
        srcpad->segment.position = GST_BUFFER_TIMESTAMP (buffer);

      GST_STREAM_DEMUX_SRCPAD_UNLOCK (srcpad);

      GST_TRACE_OBJECT (srcpad, "Pushing %" GST_PTR_FORMAT, buffer);
      gst_pad_push (GST_PAD (srcpad), buffer);
    } else { // GST_IS_EVENT (object)
      GstEvent *event = GST_EVENT_CAST (object);

      GST_DEBUG_OBJECT (srcpad, "Pushing %" GST_PTR_FORMAT, event);
      gst_pad_push_event (GST_PAD (srcpad), event);
    }
  } else {
    GST_INFO_OBJECT (srcpad, "Pause worker task!");
    gst_pad_pause_task (GST_PAD (srcpad));
  }
}

static gboolean
gst_stream_demux_srcpad_query (GstPad * pad, GstObject * parent,
    GstQuery * query)
{
  GstStreamDemuxSrcPad *srcpad = GST_STREAM_DEMUX_SRCPAD (pad);

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_CAPS:
    {
      GstCaps *caps = NULL, *filter = NULL;

      if (!(caps = gst_pad_get_current_caps (pad)))
        caps = gst_pad_get_pad_template_caps (pad);

      gst_query_parse_caps (query, &filter);

      if (filter != NULL) {
        GstCaps *intersect =
            gst_caps_intersect_full (filter, caps, GST_CAPS_INTERSECT_FIRST);
        gst_caps_unref (caps);
        caps = intersect;
      }

      gst_query_set_caps_result (query, caps);
      gst_caps_unref (caps);
      return TRUE;
    }
    case GST_QUERY_POSITION:
    {
      GstSegment *segment = &srcpad->segment;
      GstFormat format = GST_FORMAT_UNDEFINED;

      gst_query_parse_position (query, &format, NULL);

      if (format != GST_FORMAT_TIME)
        return FALSE;

      gst_query_set_position (query, format,
          gst_segment_to_stream_time (segment, format, segment->position));
      return TRUE;
    }
    default:
      break;
  }

  return gst_pad_query_default (pad, parent, query);
}

static gboolean
gst_stream_demux_srcpad_activate_mode (GstPad * pad, GstObject * parent,
    GstPadMode mode, gboolean active)
{
  GstStreamDemuxSrcPad *srcpad = GST_STREAM_DEMUX_SRCPAD (pad);
  gboolean success = TRUE;

  GST_INFO_OBJECT (pad, "%s worker task", active ? "Activating" : "Deactivating");

  switch (mode) {
    case GST_PAD_MODE_PUSH:
      if (active) {
        gst_data_queue_set_flushing (srcpad->queue, FALSE);

        success = gst_pad_start_task (pad,
            gst_stream_demux_srcpad_worker_task, pad, NULL);
      } else {
        gst_data_queue_set_flushing (srcpad->queue, TRUE);
        gst_data_queue_flush (srcpad->queue);

        success = gst_pad_stop_task (pad);
      }

      srcpad->stmstart = active;
      break;
    default:
      break;
  }

  if (!success) {
    GST_ERROR_OBJECT (pad, "Failed to %s worker task!",
        active ? "activate" : "deactivate");
    return FALSE;
  }

  return gst_pad_activate_mode (pad, mode, active);
}

static void
gst_stream_demux_srcpad_finalize (GObject * object)
{
  GstStreamDemuxSrcPad *pad = GST_STREAM_DEMUX_SRCPAD (object);

  gst_data_queue_set_flushing (pad->queue, TRUE);
  gst_data_queue_flush (pad->queue);

  gst_object_unref (pad->queue);
  g_mutex_clear (&pad->lock);

  G_OBJECT_CLASS (gst_stream_demux_srcpad_parent_class)->finalize (object);
}

static void
gst_stream_demux_srcpad_class_init (GstStreamDemuxSrcPadClass * klass)
{
  GObjectClass *gobject = (GObjectClass *) klass;

  gobject->finalize = GST_DEBUG_FUNCPTR (gst_stream_demux_srcpad_finalize);
}

static void
gst_stream_demux_srcpad_init (GstStreamDemuxSrcPad * pad)
{
  g_mutex_init (&pad->lock);
  gst_segment_init (&pad->segment, GST_FORMAT_UNDEFINED);

  pad->stmstart = FALSE;
  pad->id = 0;

  pad->queue = gst_data_queue_new (queue_is_full_cb, NULL, NULL, pad);

  gst_pad_set_query_function (GST_PAD (pad),
      GST_DEBUG_FUNCPTR (gst_stream_demux_srcpad_query));
  gst_pad_set_activatemode_function (GST_PAD (pad),
      GST_DEBUG_FUNCPTR (gst_stream_demux_srcpad_activate_mode));
}

static void
gst_stream_demux_sinkpad_class_init (GstStreamDemuxSinkPadClass * klass)
{
}

static void
gst_stream_demux_sinkpad_init (GstStreamDemuxSinkPad * pad)
{
  gst_segment_init (&pad->segment, GST_FORMAT_UNDEFINED);
}
