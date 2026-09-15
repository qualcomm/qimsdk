/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "streamdemux.h"

#include <stdio.h>

#include <gst/video/video.h>
#include <gst/video/gstvideolandmarksmeta.h>
#include <gst/video/gstvideosegmentationmeta.h>
#include <gst/video/gstvideodepthmeta.h>
#include <gst/video/gstvideoclassificationmeta.h>
#include <gst/video/video-utils.h>
#include <gst/utils/gsttextmeta.h>
#include <gst/utils/common-utils.h>

#include "streamdemuxpads.h"

#define GST_CAT_DEFAULT gst_stream_demux_debug
GST_DEBUG_CATEGORY (gst_stream_demux_debug);

#define gst_stream_demux_parent_class parent_class
G_DEFINE_TYPE (GstStreamDemux, gst_stream_demux, GST_TYPE_ELEMENT);

#define GST_STREAM_DEMUX_CAPS \
    "video/x-raw(ANY)"

enum
{
  PROP_0,
};

static GstStaticPadTemplate gst_stream_demux_sink_template =
    GST_STATIC_PAD_TEMPLATE ("sink",
        GST_PAD_SINK,
        GST_PAD_ALWAYS,
        GST_STATIC_CAPS (GST_STREAM_DEMUX_CAPS)
    );

static GstStaticPadTemplate gst_stream_demux_src_template =
    GST_STATIC_PAD_TEMPLATE ("src_%u",
        GST_PAD_SRC,
        GST_PAD_REQUEST,
        GST_STATIC_CAPS (GST_STREAM_DEMUX_CAPS)
    );

static gpointer
gst_list_copy_structure (gconstpointer src, G_GNUC_UNUSED gpointer userdata)
{
  return gst_structure_copy (GST_STRUCTURE (src));
}

static void
gst_data_queue_free_item (gpointer userdata)
{
  GstDataQueueItem *item = userdata;

  if (GST_IS_BUFFER (item->object))
    gst_buffer_unref (GST_BUFFER (item->object));
  else if (GST_IS_EVENT (item->object))
    gst_event_unref (GST_EVENT (item->object));

  g_slice_free (GstDataQueueItem, item);
}

static GstCaps *
gst_caps_derive_new_caps (GstCaps * caps, gboolean passthrough)
{
  GstStructure *structure = NULL;
  GstCaps *newcaps = NULL;

  if (passthrough)
    return gst_caps_ref (caps);

  newcaps = gst_caps_copy (caps);
  structure = gst_caps_get_structure (newcaps, 0);

  // Set multiview-mode to mono and drop views, the src pads output a
  // single demuxed stream rather than the sink's separated multiview one.
  gst_structure_set (structure, "multiview-mode", G_TYPE_STRING, "mono", NULL);
  gst_structure_remove_field (structure, "views");

  return newcaps;
}

static void
gst_query_clear_allocation_metas (GstQuery * query)
{
  guint n_metas = gst_query_get_n_allocation_metas (query), idx = 0;

  for (idx = 0; idx < n_metas; idx++)
    gst_query_remove_nth_allocation_meta (query, 0);
}

static void
gst_query_merge_allocation_params (GstQuery * query, GstAllocationParams * params)
{
  guint n_metas = gst_query_get_n_allocation_params (query), idx = 0;

  // Keep the maximum alignment, prefix and padding across all pads;
  // ignore the allocators/flags, which are tied to downstream allocation.
  for (idx = 0; idx < n_metas; idx++) {
    GstAllocationParams peerparams = { 0, };

    gst_query_parse_nth_allocation_param (query, idx, NULL, &peerparams);

    params->align = MAX (params->align, peerparams.align);
    params->prefix = MAX (params->prefix, peerparams.prefix);
    params->padding = MAX (params->padding, peerparams.padding);
  }
}

static void
gst_query_merge_allocation_pool (GstQuery * query, guint * size,
    guint * minbuffers)
{
  guint peersize = 0, peermin = 0;

  // Keep the biggest size and minimum number of buffers seen across all
  // pads' first reported pool.
  if (gst_query_get_n_allocation_pools (query) == 0)
    return;

  gst_query_parse_nth_allocation_pool (query, 0, NULL, &peersize, &peermin, NULL);

  *size = MAX (*size, peersize);
  *minbuffers = MAX (*minbuffers, peermin);
}

static void
gst_query_merge_allocation_metas (GstQuery * query, GstQuery * peerquery,
    gboolean initialized)
{
  const GstStructure *peerparam = NULL, *param = NULL;
  GstStructure *aggregated = NULL;
  guint n_metas = 0, idx = 0, metaidx = 0;

  n_metas = gst_query_get_n_allocation_metas (peerquery);

  // Aggregate common metas' params via GstMetaInfo's aggregate_func,
  // dropping metas not supported by every pad seen so far.
  for (idx = 0; idx < n_metas; idx++) {
    GType api = gst_query_parse_nth_allocation_meta (peerquery, idx, &peerparam);

    // For the first pad, copy all metas verbatim.
    if (!initialized) {
      gst_query_add_allocation_meta (query, api, peerparam);
      continue;
    }

    if (!gst_query_find_allocation_meta (query, api, &metaidx))
      continue;

    gst_query_parse_nth_allocation_meta (query, metaidx, &param);

    // Keep a meta which has no params on either side unchanged.
    if (param == NULL && peerparam == NULL)
      continue;

    if (gst_meta_api_type_aggregate_params (api, &aggregated, param, peerparam)) {
      gst_query_add_allocation_meta (query, api, aggregated);

      if (aggregated != NULL)
        gst_structure_free (aggregated);
    }

    gst_query_remove_nth_allocation_meta (query, metaidx);
  }

  // Drop metas already accumulated that this pad doesn't support, so
  // only metas common to every pad seen so far survive.
  n_metas = gst_query_get_n_allocation_metas (query);

  for (idx = 0; idx < n_metas;) {
    GType api = gst_query_parse_nth_allocation_meta (query, idx, NULL);

    if (!gst_query_find_allocation_meta (peerquery, api, NULL)) {
      gst_query_remove_nth_allocation_meta (query, idx);
      n_metas--;
      continue;
    }

    idx++;
  }
}

static gboolean
gst_stream_demux_srcpad_push_eos (GstElement * element, GstPad * pad,
    gpointer userdata)
{
  GstStreamDemuxSrcPad *srcpad = GST_STREAM_DEMUX_SRCPAD (pad);
  GstDataQueueItem *item = g_slice_new0 (GstDataQueueItem);
  gboolean success = FALSE;

  item->object = GST_MINI_OBJECT (gst_mini_object_ref (userdata));
  item->visible = FALSE;
  item->destroy = gst_data_queue_free_item;

  if (!(success = gst_data_queue_push (srcpad->queue, item)))
    item->destroy (item);

  return success;
}

static gboolean
gst_stream_demux_srcpad_push_flush (GstElement * element, GstPad * pad,
    gpointer userdata)
{
  GstStreamDemuxSrcPad *srcpad = GST_STREAM_DEMUX_SRCPAD (pad);
  GstEvent *event = GST_EVENT (userdata);

  if (GST_EVENT_TYPE (event) == GST_EVENT_FLUSH_STOP) {
    GST_STREAM_DEMUX_SRCPAD_LOCK (srcpad);
    gst_segment_init (&srcpad->segment, GST_FORMAT_TIME);
    GST_STREAM_DEMUX_SRCPAD_UNLOCK (srcpad);

    gst_data_queue_set_flushing (srcpad->queue, FALSE);
  }

  if (GST_EVENT_TYPE (event) == GST_EVENT_FLUSH_START) {
    gst_data_queue_set_flushing (srcpad->queue, TRUE);
    gst_data_queue_flush (srcpad->queue);
  }

  return gst_pad_push_event (pad, gst_event_ref (event));
}

static void
gst_stream_demux_srcpad_push_stream_start (GstStreamDemux * demux, GstPad * pad)
{
  GstStreamDemuxSrcPad *srcpad = GST_STREAM_DEMUX_SRCPAD (pad);
  gchar *stream_id = NULL;

  if (!srcpad->stmstart)
    return;

  // Send stream start event if not sent, before setting the source caps.;
  stream_id = gst_pad_create_stream_id_printf (pad, GST_ELEMENT (demux),
      "%u", srcpad->id);
  GST_INFO_OBJECT (pad, "Pushing stream start event");

  gst_pad_push_event (pad, gst_event_new_stream_start (stream_id));
  g_free (stream_id);

  srcpad->stmstart = FALSE;
}

static GstStreamDemuxSrcPad *
gst_stream_demux_find_srcpad (GstStreamDemux * demux, guint stream_id)
{
  GstStreamDemuxSrcPad *srcpad = NULL;
  GList *list = NULL;

  GST_STREAM_DEMUX_LOCK (demux);

  for (list = demux->srcpads; list != NULL; list = g_list_next (list)) {
    srcpad = GST_STREAM_DEMUX_SRCPAD_CAST (list->data);

    if (srcpad->id == stream_id)
      break;

    srcpad = NULL;
  }

  GST_STREAM_DEMUX_UNLOCK (demux);
  return srcpad;
}

static GList *
gst_stream_demux_ref_srcpads (GstStreamDemux * demux)
{
  GList *srcpads = NULL, *list = NULL;

  GST_STREAM_DEMUX_LOCK (demux);

  for (list = demux->srcpads; list != NULL; list = g_list_next (list))
    srcpads = g_list_prepend (srcpads, gst_object_ref (GST_PAD (list->data)));

  GST_STREAM_DEMUX_UNLOCK (demux);

  return srcpads;
}

static gboolean
gst_stream_demux_sink_query_allocation (GstStreamDemux * demux,
    GstQuery * query)
{
  GstCaps *caps = NULL;
  GList *srcpads = NULL, *list = NULL;
  GstAllocationParams params = { 0, };
  gboolean initialized = FALSE, success = TRUE;
  guint size = 0, minbuffers = 0, n_pads = 0;

  gst_query_parse_allocation (query, &caps, NULL);

  // Snapshot so the per-pad queries below don't run while holding our own
  // lock (risk of deadlock on any re-entrant callback).
  srcpads = gst_stream_demux_ref_srcpads (demux);

  for (list = srcpads; success && list != NULL; list = g_list_next (list)) {
    GstPad *peer = gst_pad_get_peer (GST_PAD (list->data));
    GstQuery *peerquery = NULL;

    // Pad not linked yet, skip this source pad.
    if (peer == NULL) {
      GST_DEBUG_OBJECT (demux, "Pad %s has no peer yet, skipping",
          GST_PAD_NAME (GST_PAD (list->data)));
      continue;
    }

    peerquery = gst_query_new_allocation (caps, FALSE);
    success = gst_pad_query (peer, peerquery);
    gst_object_unref (peer);

    if (!success) {
      GST_DEBUG_OBJECT (demux, "Allocation query failed on pad %s, "
          "ignoring allocation", GST_PAD_NAME (GST_PAD (list->data)));
      gst_query_unref (peerquery);
      break;
    }

    gst_query_merge_allocation_params (peerquery, &params);
    gst_query_merge_allocation_pool (peerquery, &size, &minbuffers);
    gst_query_merge_allocation_metas (query, peerquery, initialized);

    gst_query_unref (peerquery);

    initialized = TRUE;
    n_pads++;
  }

  g_list_free_full (srcpads, gst_object_unref);

  if (!success) {
    gst_query_clear_allocation_metas (query);
    return FALSE;
  }

  GST_DEBUG_OBJECT (demux, "Aggregated AllocationParams to align [%"
      G_GSIZE_FORMAT "], prefix [%" G_GSIZE_FORMAT "], padding [%"
      G_GSIZE_FORMAT "]", params.align, params.prefix, params.padding);
  GST_DEBUG_OBJECT (demux, "Aggregated allocation pool size [%u], "
      "minimum buffers [%u]", size, minbuffers);

  // Allocate one more buffer when demuxing to more than one stream so we
  // don't starve any of the downstream branches.
  if (n_pads > 1)
    minbuffers++;

  if (params.align || params.prefix || params.padding)
    gst_query_add_allocation_param (query, NULL, &params);

  if (size)
    gst_query_add_allocation_pool (query, NULL, size, minbuffers, 0);

  return TRUE;
}

static gboolean
gst_stream_demux_sink_setcaps (GstStreamDemux * demux, GstPad * pad,
    GstCaps * caps)
{
  GstStructure *structure = NULL;
  const gchar *viewmode = NULL;
  GstCaps *srccaps = NULL;
  GList *list = NULL;
  gboolean passthrough = FALSE;

  GST_DEBUG_OBJECT (pad, "Setting caps %" GST_PTR_FORMAT, caps);

  if (gst_caps_is_empty (caps)) {
    GST_ERROR_OBJECT (pad, "Invalid caps %" GST_PTR_FORMAT, caps);
    return FALSE;
  }

  structure = gst_caps_get_structure (caps, 0);

  // multiview-mode=separated signals that the channel bit in
  // GST_BUFFER_OFFSET should be read.
  if (gst_structure_has_name (structure, "video/x-raw") &&
      gst_structure_has_field (structure, "multiview-mode"))
    viewmode = gst_structure_get_string (structure, "multiview-mode");

  if (g_strcmp0 (viewmode, "separated") == 0) {
    GST_INFO_OBJECT (demux, "Operating in demux mode");
    passthrough = FALSE;
  } else {
    GST_INFO_OBJECT (demux, "Operating in passthrough mode");
    passthrough = TRUE;
  }

  srccaps = gst_caps_derive_new_caps (caps, passthrough);

  GST_STREAM_DEMUX_LOCK (demux);

  demux->passthrough = passthrough;

  // Propagate to any source pads created before this (re-)negotiation.
  for (list = demux->srcpads; list != NULL; list = g_list_next (list)) {
    gst_stream_demux_srcpad_push_stream_start (demux, GST_PAD (list->data));
    gst_pad_set_caps (GST_PAD (list->data), srccaps);
  }

  GST_STREAM_DEMUX_UNLOCK (demux);

  gst_caps_unref (srccaps);
  return TRUE;
}


static void
gst_stream_demux_transfer_metas (GstStreamDemux * demux, GstBuffer * inbuffer,
    GHashTable * outbuffers)
{
  GstMeta *meta = NULL;
  gpointer state = NULL;

  // The muxed stream ID is packed into the upper byte of each meta's id.
  while ((meta = gst_buffer_iterate_meta (inbuffer, &state)) != NULL) {
    GstBuffer *outbuffer = NULL;
    guint stream_id = 0;

    GST_TRACE_OBJECT (demux, "Meta %s on buffer %" GST_PTR_FORMAT,
        g_type_name (meta->info->api), inbuffer);

    if (meta->info->api == GST_VIDEO_REGION_OF_INTEREST_META_API_TYPE) {
      GstVideoRegionOfInterestMeta *roimeta = NULL, *newmeta = NULL;

      roimeta = GST_VIDEO_ROI_META_CAST (meta);
      stream_id = GST_META_ID_GET_STREAM (roimeta->id);

      outbuffer = g_hash_table_lookup (outbuffers, GUINT_TO_POINTER (stream_id));
      if (outbuffer == NULL) {
        GST_TRACE_OBJECT (demux, "No output buffer for stream %u, "
            "dropping meta %s", stream_id, g_type_name (meta->info->api));
        continue;
      }

      newmeta = gst_buffer_add_video_region_of_interest_meta_id (outbuffer,
          roimeta->roi_type, roimeta->x, roimeta->y, roimeta->w, roimeta->h);

      newmeta->id = roimeta->id & ~GST_MUX_STREAM_ID_MASK;
      newmeta->parent_id = roimeta->parent_id & ~GST_MUX_STREAM_ID_MASK;

      newmeta->params = g_list_copy_deep (roimeta->params,
          gst_list_copy_structure, NULL);
    } else if (meta->info->api == GST_VIDEO_LANDMARKS_META_API_TYPE) {
      GstVideoLandmarksMeta *lmeta = NULL, *newmeta = NULL;

      lmeta = GST_VIDEO_LANDMARKS_META_CAST (meta);
      stream_id = GST_META_ID_GET_STREAM (lmeta->id);

      outbuffer = g_hash_table_lookup (outbuffers, GUINT_TO_POINTER (stream_id));
      if (outbuffer == NULL) {
        GST_TRACE_OBJECT (demux, "No output buffer for stream %u, "
            "dropping meta %s", stream_id, g_type_name (meta->info->api));
        continue;
      }

      newmeta = gst_buffer_add_video_landmarks_meta (outbuffer,
          lmeta->confidence, g_array_ref (lmeta->keypoints),
          (lmeta->links != NULL) ? g_array_ref (lmeta->links) : NULL);

      newmeta->id = lmeta->id & ~GST_MUX_STREAM_ID_MASK;
      newmeta->parent_id = lmeta->parent_id & ~GST_MUX_STREAM_ID_MASK;

      newmeta->xtraparams = (lmeta->xtraparams != NULL) ?
          gst_structure_copy (lmeta->xtraparams) : NULL;
    } else if (meta->info->api == GST_VIDEO_SEGMENTATION_META_API_TYPE) {
      GstVideoSegmentationMeta *smeta = NULL, *newmeta = NULL;

      smeta = GST_VIDEO_SEGMENTATION_META_CAST (meta);
      stream_id = GST_META_ID_GET_STREAM (smeta->id);

      outbuffer = g_hash_table_lookup (outbuffers, GUINT_TO_POINTER (stream_id));
      if (outbuffer == NULL) {
        GST_TRACE_OBJECT (demux, "No output buffer for stream %u, "
            "dropping meta %s", stream_id, g_type_name (meta->info->api));
        continue;
      }

      newmeta = gst_buffer_add_video_segmentation_meta (outbuffer,
          g_array_ref (smeta->labelmask),
          (smeta->colormask != NULL) ? g_array_ref (smeta->colormask) : NULL,
          smeta->n_columns, smeta->n_rows);

      newmeta->id = smeta->id & ~GST_MUX_STREAM_ID_MASK;
      newmeta->parent_id = smeta->parent_id & ~GST_MUX_STREAM_ID_MASK;

      newmeta->xtraparams = (smeta->xtraparams != NULL) ?
          gst_structure_copy (smeta->xtraparams) : NULL;
    } else if (meta->info->api == GST_VIDEO_DEPTH_META_API_TYPE) {
      GstVideoDepthMeta *dmeta = NULL, *newmeta = NULL;

      dmeta = GST_VIDEO_DEPTH_META_CAST (meta);
      stream_id = GST_META_ID_GET_STREAM (dmeta->id);

      outbuffer = g_hash_table_lookup (outbuffers, GUINT_TO_POINTER (stream_id));
      if (outbuffer == NULL) {
        GST_TRACE_OBJECT (demux, "No output buffer for stream %u, "
            "dropping meta %s", stream_id, g_type_name (meta->info->api));
        continue;
      }

      newmeta = gst_buffer_add_video_depth_meta (outbuffer,
          g_array_ref (dmeta->depthvals),
          (dmeta->colormask != NULL) ? g_array_ref (dmeta->colormask) : NULL,
          dmeta->n_columns, dmeta->n_rows);

      newmeta->id = dmeta->id & ~GST_MUX_STREAM_ID_MASK;
      newmeta->parent_id = dmeta->parent_id & ~GST_MUX_STREAM_ID_MASK;

      newmeta->xtraparams = (dmeta->xtraparams != NULL) ?
          gst_structure_copy (dmeta->xtraparams) : NULL;
    } else if (meta->info->api == GST_VIDEO_CLASSIFICATION_META_API_TYPE) {
      GstVideoClassificationMeta *cmeta = NULL, *newmeta = NULL;

      cmeta = GST_VIDEO_CLASSIFICATION_META_CAST (meta);
      stream_id = GST_META_ID_GET_STREAM (cmeta->id);

      outbuffer = g_hash_table_lookup (outbuffers, GUINT_TO_POINTER (stream_id));
      if (outbuffer == NULL) {
        GST_TRACE_OBJECT (demux, "No output buffer for stream %u, "
            "dropping meta %s", stream_id, g_type_name (meta->info->api));
        continue;
      }

      newmeta = gst_buffer_add_video_classification_meta (outbuffer,
          g_array_ref (cmeta->labels));

      newmeta->id = cmeta->id & ~GST_MUX_STREAM_ID_MASK;
      newmeta->parent_id = cmeta->parent_id & ~GST_MUX_STREAM_ID_MASK;
    } else if (meta->info->api == GST_TEXT_META_API_TYPE) {
      GstTextMeta *tmeta = NULL, *newmeta = NULL;

      tmeta = GST_TEXT_META_CAST (meta);
      stream_id = GST_META_ID_GET_STREAM (tmeta->id);

      outbuffer = g_hash_table_lookup (outbuffers, GUINT_TO_POINTER (stream_id));
      if (outbuffer == NULL) {
        GST_TRACE_OBJECT (demux, "No output buffer for stream %u, "
            "dropping meta %s", stream_id, g_type_name (meta->info->api));
        continue;
      }

      newmeta = gst_buffer_add_text_meta (outbuffer, tmeta->contents,
          tmeta->confidence, tmeta->color);

      newmeta->id = tmeta->id & ~GST_MUX_STREAM_ID_MASK;
      newmeta->parent_id = tmeta->parent_id & ~GST_MUX_STREAM_ID_MASK;

      newmeta->xtraparams = (tmeta->xtraparams != NULL) ?
          gst_structure_copy (tmeta->xtraparams) : NULL;
    } else {
      GST_TRACE_OBJECT (demux, "Skipping unhandled meta %s",
          g_type_name (meta->info->api));
    }
  }
}

static GstFlowReturn
gst_stream_demux_process (GstStreamDemux * demux, GstBuffer * inbuffer)
{
  GstMeta *meta = NULL;
  GHashTable *outbuffers = NULL;
  GHashTableIter iter;
  GstBuffer *outbuffer = NULL;
  gpointer state = NULL, key = NULL, value = NULL;
  GstClockTime time = GST_CLOCK_TIME_NONE;
  guint memidx = 0, stream_id = 0;

  GST_TRACE_OBJECT (demux, "Processing buffer %" GST_PTR_FORMAT, inbuffer);

  time = gst_util_get_timestamp ();

  outbuffers = g_hash_table_new (NULL, NULL);

  while ((meta = gst_buffer_iterate_meta_filtered (inbuffer, &state,
              GST_PROTECTION_META_API_TYPE))) {
    GstProtectionMeta *pmeta = GST_PROTECTION_META_CAST (meta);
    const gchar *name = gst_structure_get_name (pmeta->info);
    GstMemory *memory = NULL;
    GstVideoMeta *vmeta = NULL, *newmeta = NULL;

    // Extract the stream ID from the service meta based on the name.
    if (sscanf (name, "mux-stream-%u", &stream_id) != 1)
      continue;

    GST_TRACE_OBJECT (demux, "Creating output buffer for stream %u", stream_id);

    outbuffer = gst_buffer_new ();
    g_hash_table_insert (outbuffers, GUINT_TO_POINTER (stream_id), outbuffer);

    gst_structure_get_uint64 (pmeta->info, "timestamp",
        &GST_BUFFER_TIMESTAMP (outbuffer));
    gst_structure_get_uint64 (pmeta->info, "duration",
        &GST_BUFFER_DURATION (outbuffer));
    gst_structure_get_uint (pmeta->info, "flags",
        &GST_BUFFER_FLAGS (outbuffer));

    // If there is no corresponding memory block, mark as GAP buffer.
    if ((memory = gst_buffer_get_memory (inbuffer, memidx)) == NULL) {
      GST_BUFFER_FLAG_SET (outbuffer, GST_BUFFER_FLAG_GAP);
      continue;
    }

    gst_buffer_append_memory (outbuffer, memory);
    // Keep the muxed input buffer alive until every split-off buffer is freed.
    gst_buffer_add_parent_buffer_meta (outbuffer, inbuffer);

    if ((vmeta = gst_buffer_get_video_meta_id (inbuffer, memidx)) != NULL) {
      newmeta = gst_buffer_add_video_meta_full (outbuffer,
          vmeta->flags, vmeta->format, vmeta->width, vmeta->height,
          vmeta->n_planes, vmeta->offset, vmeta->stride);

      newmeta->id = 0;
    }

    memidx++;
  }

  GST_TRACE_OBJECT (demux, "Demuxed %u stream(s) from buffer %"
      GST_PTR_FORMAT, g_hash_table_size (outbuffers), inbuffer);

  gst_stream_demux_transfer_metas (demux, inbuffer, outbuffers);
  g_hash_table_iter_init (&iter, outbuffers);

  while (g_hash_table_iter_next (&iter, &key, &value)) {
    GstStreamDemuxSrcPad *srcpad = NULL;
    GstDataQueueItem *item = NULL;

    stream_id = GPOINTER_TO_UINT (key);
    outbuffer = GST_BUFFER (value);

    srcpad = gst_stream_demux_find_srcpad (demux, stream_id);

    if (srcpad == NULL) {
      GST_DEBUG_OBJECT (demux, "No source pad requested for stream %u, "
          "dropping!", stream_id);
      gst_buffer_unref (outbuffer);
      continue;
    }

    if (GST_CLOCK_TIME_IS_VALID (GST_BUFFER_TIMESTAMP (outbuffer)))
      srcpad->segment.position = GST_BUFFER_TIMESTAMP (outbuffer);

    item = g_slice_new0 (GstDataQueueItem);
    item->object = GST_MINI_OBJECT (outbuffer);
    item->size = gst_buffer_get_size (outbuffer);
    item->duration = GST_BUFFER_DURATION (outbuffer);
    item->visible = TRUE;
    item->destroy = gst_data_queue_free_item;

    GST_TRACE_OBJECT (srcpad, "Submitting %" GST_PTR_FORMAT, outbuffer);

    if (!gst_data_queue_push (srcpad->queue, item))
      item->destroy (item);
  }

  g_hash_table_destroy (outbuffers);
  gst_buffer_unref (inbuffer);

  time = GST_CLOCK_DIFF (time, gst_util_get_timestamp ());

  GST_LOG_OBJECT (demux, "Performance time %" G_GINT64_FORMAT ".%03"
      G_GINT64_FORMAT " ms, HW utilization: CPU", GST_TIME_AS_MSECONDS (time),
      (GST_TIME_AS_USECONDS (time) % 1000));

  return GST_FLOW_OK;
}

static gboolean
gst_stream_demux_sinkpad_query (GstPad * pad, GstObject * parent,
    GstQuery * query)
{
  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_CAPS:
    {
      GstCaps *caps = NULL, *intersect = NULL, *filter = NULL;

      gst_query_parse_caps (query, &filter);

      if (!(caps = gst_pad_get_current_caps (pad)))
        caps = gst_pad_get_pad_template_caps (pad);

      if (filter != NULL) {
        intersect = gst_caps_intersect_full (filter, caps, GST_CAPS_INTERSECT_FIRST);

        gst_caps_unref (caps);
        caps = intersect;
      }

      gst_query_set_caps_result (query, caps);
      gst_caps_unref (caps);
      return TRUE;
    }
    case GST_QUERY_ACCEPT_CAPS:
    {
      GstCaps *caps = NULL, *tmplcaps = NULL;
      gboolean success = FALSE;

      gst_query_parse_accept_caps (query, &caps);
      tmplcaps = gst_pad_get_pad_template_caps (pad);

      success = gst_caps_can_intersect (caps, tmplcaps);
      gst_caps_unref (tmplcaps);

      if (!success) {
        GST_WARNING_OBJECT (pad, "Caps %" GST_PTR_FORMAT " can't intersect "
            "with template!", caps);
      }

      gst_query_set_accept_caps_result (query, success);
      return TRUE;
    }
    case GST_QUERY_ALLOCATION:
      return gst_stream_demux_sink_query_allocation (
          GST_STREAM_DEMUX (parent), query);
    default:
      break;
  }

  return gst_pad_query_default (pad, parent, query);
}

static gboolean
gst_stream_demux_sinkpad_event (GstPad * pad, GstObject * parent,
    GstEvent * event)
{
  GstStreamDemux *demux = GST_STREAM_DEMUX (parent);
  gboolean success = FALSE;

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_CAPS:
    {
      GstCaps *caps = NULL;

      gst_event_parse_caps (event, &caps);
      success = gst_stream_demux_sink_setcaps (demux, pad, caps);
      gst_event_unref (event);

      return success;
    }
    case GST_EVENT_SEGMENT:
      GST_DEBUG_OBJECT (demux, "Received %" GST_PTR_FORMAT, event);

      GST_STREAM_DEMUX_LOCK (demux);
      gst_event_copy_segment (event, &GST_STREAM_DEMUX_SINKPAD (pad)->segment);
      GST_STREAM_DEMUX_UNLOCK (demux);

      gst_event_unref (event);
      return TRUE;
    case GST_EVENT_STREAM_START:
      // Consumed here: each dynamically created source pad pushes its own
      // stream-start, derived from this one via gst_pad_create_stream_id().
      gst_event_unref (event);
      return TRUE;
    case GST_EVENT_FLUSH_START:
    case GST_EVENT_FLUSH_STOP:
      GST_DEBUG_OBJECT (demux, "Received %" GST_PTR_FORMAT, event);

      success = gst_element_foreach_src_pad (GST_ELEMENT (demux),
          gst_stream_demux_srcpad_push_flush, event);

      gst_event_unref (event);
      return success;
    case GST_EVENT_EOS:
      GST_DEBUG_OBJECT (demux, "Received %" GST_PTR_FORMAT, event);

      success = gst_element_foreach_src_pad (GST_ELEMENT (demux),
          gst_stream_demux_srcpad_push_eos, event);

      gst_event_unref (event);
      return success;
    default:
      break;
  }

  return gst_pad_event_default (pad, parent, event);
}

static GstFlowReturn
gst_stream_demux_sink_chain (GstPad * pad, GstObject * parent,
    GstBuffer * inbuffer)
{
  GstStreamDemux *demux = GST_STREAM_DEMUX (parent);
  gboolean passthrough = FALSE;

  GST_TRACE_OBJECT (pad, "Received %" GST_PTR_FORMAT, inbuffer);

  GST_STREAM_DEMUX_LOCK (demux);
  passthrough = demux->passthrough;
  GST_STREAM_DEMUX_UNLOCK (demux);

  if (passthrough) {
    GstStreamDemuxSrcPad *srcpad = gst_stream_demux_find_srcpad (demux, 0);

    if (srcpad == NULL) {
      gst_buffer_unref (inbuffer);
      return GST_FLOW_OK;
    }

    return gst_pad_push (GST_PAD (srcpad), inbuffer);
  } else if (GST_BUFFER_OFFSET (inbuffer) == GST_BUFFER_OFFSET_NONE) {
    GST_WARNING_OBJECT (demux, "Buffer %" GST_PTR_FORMAT " has no muxed "
        "stream markers, dropping!", inbuffer);

    gst_buffer_unref (inbuffer);
    return GST_FLOW_OK;
  }

  return gst_stream_demux_process (demux, inbuffer);
}

static GstPad *
gst_stream_demux_request_pad (GstElement * element, GstPadTemplate * templ,
    const gchar * reqname, const GstCaps * caps)
{
  GstStreamDemux *demux = GST_STREAM_DEMUX (element);
  GstPad *pad = NULL;
  GstCaps *sinkcaps = NULL, *srccaps = NULL;
  gchar *name = NULL;
  guint index = 0, nextindex = 0;
  gboolean passthrough = FALSE;

  GST_STREAM_DEMUX_LOCK (demux);

  if (reqname && sscanf (reqname, "src_%u", &index) == 1) {
    // Update the next source pad index set his name.
    nextindex = (index >= demux->nextidx) ? index + 1 : demux->nextidx;
  } else {
    index = demux->nextidx;
    // Update the index for next source pad and set his name.
    nextindex = index + 1;
  }

  GST_STREAM_DEMUX_UNLOCK (demux);

  name = g_strdup_printf ("src_%u", index);
  pad = g_object_new (GST_TYPE_STREAM_DEMUX_SRCPAD, "name", name,
      "direction", templ->direction, "template", templ, NULL);
  g_free (name);

  if (pad == NULL) {
    GST_ERROR_OBJECT (demux, "Failed to create source pad for stream %u!", index);
    return NULL;
  }

  GST_STREAM_DEMUX_SRCPAD_CAST (pad)->id = index;

  if (!gst_element_add_pad (element, pad)) {
    GST_ERROR_OBJECT (demux, "Failed to add pad for stream %u!", index);
    gst_object_unref (pad);
    return NULL;
  }

  sinkcaps = gst_pad_get_current_caps (demux->sinkpad);

  if (sinkcaps != NULL) {
    GST_STREAM_DEMUX_LOCK (demux);
    passthrough = demux->passthrough;
    GST_STREAM_DEMUX_UNLOCK (demux);

    srccaps = gst_caps_derive_new_caps (sinkcaps, passthrough);
    gst_caps_unref (sinkcaps);

    gst_pad_set_caps (pad, srccaps);
    gst_caps_unref (srccaps);
  }

  GST_STREAM_DEMUX_LOCK (demux);
  demux->srcpads = g_list_append (demux->srcpads, pad);
  demux->nextidx = nextindex;
  GST_STREAM_DEMUX_UNLOCK (demux);

  GST_DEBUG_OBJECT (demux, "Created pad: %s", GST_PAD_NAME (pad));

  return pad;
}

static void
gst_stream_demux_release_pad (GstElement * element, GstPad * pad)
{
  GstStreamDemux *demux = GST_STREAM_DEMUX (element);

  GST_DEBUG_OBJECT (demux, "Releasing pad: %s", GST_PAD_NAME (pad));

  GST_STREAM_DEMUX_LOCK (demux);
  demux->srcpads = g_list_remove (demux->srcpads, pad);
  GST_STREAM_DEMUX_UNLOCK (demux);

  gst_element_remove_pad (element, pad);
}

static void
gst_stream_demux_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  switch (prop_id) {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_stream_demux_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  switch (prop_id) {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_stream_demux_finalize (GObject * object)
{
  GstStreamDemux *demux = GST_STREAM_DEMUX (object);

  g_list_free (demux->srcpads);
  g_mutex_clear (&(demux)->lock);

  G_OBJECT_CLASS (parent_class)->finalize (G_OBJECT (demux));
}

static void
gst_stream_demux_class_init (GstStreamDemuxClass * klass)
{
  GObjectClass *object = G_OBJECT_CLASS (klass);
  GstElementClass *element = GST_ELEMENT_CLASS (klass);

  object->set_property = GST_DEBUG_FUNCPTR (gst_stream_demux_set_property);
  object->get_property = GST_DEBUG_FUNCPTR (gst_stream_demux_get_property);
  object->finalize     = GST_DEBUG_FUNCPTR (gst_stream_demux_finalize);

  gst_element_class_add_static_pad_template_with_gtype (element,
      &gst_stream_demux_sink_template, GST_TYPE_STREAM_DEMUX_SINKPAD);
  gst_element_class_add_static_pad_template_with_gtype (element,
      &gst_stream_demux_src_template, GST_TYPE_STREAM_DEMUX_SRCPAD);

  gst_element_class_set_static_metadata (element,
      "Stream Demuxer", "Video/Demuxer",
      "Split a muxed stream buffer back into its per-stream buffers, or "
      "pass it through unchanged if it is not a muxed stream buffer", "QTI"
  );

  element->request_new_pad = GST_DEBUG_FUNCPTR (gst_stream_demux_request_pad);
  element->release_pad = GST_DEBUG_FUNCPTR (gst_stream_demux_release_pad);

  // Initializes a new Stream Demux GstDebugCategory with the given properties.
  GST_DEBUG_CATEGORY_INIT (gst_stream_demux_debug, "qtistreamdemux", 0,
      "QTI Stream Demux");
}

static void
gst_stream_demux_init (GstStreamDemux * demux)
{
  GstPadTemplate *template = NULL;

  g_mutex_init (&(demux)->lock);

  demux->passthrough = FALSE;
  demux->nextidx = 0;
  demux->srcpads = NULL;

  template = gst_static_pad_template_get (&gst_stream_demux_sink_template);
  demux->sinkpad = g_object_new (GST_TYPE_STREAM_DEMUX_SINKPAD, "name", "sink",
      "direction", template->direction, "template", template, NULL);
  gst_object_unref (template);

  gst_pad_set_query_function (demux->sinkpad,
      GST_DEBUG_FUNCPTR (gst_stream_demux_sinkpad_query));
  gst_pad_set_event_function (demux->sinkpad,
      GST_DEBUG_FUNCPTR (gst_stream_demux_sinkpad_event));
  gst_pad_set_chain_function (demux->sinkpad,
      GST_DEBUG_FUNCPTR (gst_stream_demux_sink_chain));

  gst_element_add_pad (GST_ELEMENT (demux), demux->sinkpad);
}

static gboolean
plugin_init (GstPlugin * plugin)
{
  return gst_element_register (plugin, "qtistreamdemux", GST_RANK_NONE,
      GST_TYPE_STREAM_DEMUX);
}

GST_PLUGIN_DEFINE(
    GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    qtistreamdemux,
    "QTI Stream Demux",
    plugin_init,
    PACKAGE_VERSION,
    PACKAGE_LICENSE,
    PACKAGE_SUMMARY,
    PACKAGE_ORIGIN
)
