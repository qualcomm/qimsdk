/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
*/

#include <algorithm>
#include <vector>

#include <gst/video/video.h>

#include "beamform_context.h"
#include "adoa_filter.h"
#include "beamform_types.h"

#define GST_CAT_DEFAULT gst_beamform_context_debug
GST_DEBUG_CATEGORY_STATIC (gst_beamform_context_debug);

#define GST_BEAMFORM_MAX_ROI 100
#define GST_BEAMFORM_DOA_META_NAME "doa-meta"

using std::max;
using std::min;
using std::vector;

/* The real context struct (opaque to the C element). */
struct _GstBeamformContext {
  AdoaFilter        filter;
  GstBeamformConfig config;
};

/* ----- ROI selection helpers (ported from ai_beamforming.cc) ----- */

/* Length of the 1-D horizontal overlap between [x_start,x_end] and
 * [y_start,y_end]; 0 if they don't overlap. */
static int32_t
CalculateIntersection (int32_t x_start, int32_t x_end,
    int32_t y_start, int32_t y_end)
{
  if (x_start > y_end || x_end < y_start)
    return 0;
  return min (x_end, y_end) - max (x_start, y_start);
}

/* Remove duplicate ROIs (same x & width) and optionally drop ROIs that are
 * wider than large_roi_percent of the frame width. */
static vector<ai_crop_data>
RoiUnique (const vector<ai_crop_data> &rois, int32_t image_width,
    bool remove_large_rois, int32_t large_roi_percent)
{
  vector<ai_crop_data> unique_rois;

  for (size_t idx = 0; idx < rois.size (); ++idx) {
    ai_crop_data roi = rois.at (idx);
    bool found = false;
    for (size_t idx2 = 0; idx2 < unique_rois.size (); ++idx2) {
      ai_crop_data uroi = unique_rois.at (idx2);
      if (uroi.x == roi.x && uroi.width == roi.width) {
        found = true;
        break;
      }
    }
    if (!found) {
      bool large_roi = false;
      if (remove_large_rois &&
          roi.width >
              (int32_t) ((float) image_width * large_roi_percent / 100)) {
        large_roi = true;
      }
      if (!large_roi)
        unique_rois.push_back ({roi.x, roi.y, roi.width, roi.height, 255});
    }
  }

  return unique_rois;
}

/* For each speaker target (an image-X range), pick the ROI with the largest
 * horizontal overlap. If nothing overlaps, return the ROIs unchanged. */
static vector<ai_crop_data>
SelectOutput (const vector<ai_crop_data> &rois,
    const vector<target_t> &targets)
{
  vector<ai_crop_data> selected_rois;

  for (const target_t &target : targets) {
    int32_t intersect = 0;
    ai_crop_data potential_roi{};
    for (size_t idx = 0; idx < rois.size (); ++idx) {
      ai_crop_data roi = rois.at (idx);
      int32_t new_intersect = CalculateIntersection (target.min, target.max,
          roi.x, roi.x + roi.width);
      if (intersect < new_intersect) {
        intersect = new_intersect;
        potential_roi = roi;
      }
    }
    if (potential_roi.width > 0)
      selected_rois.push_back (potential_roi);
  }

  if (selected_rois.empty ())
    return rois;

  return selected_rois;
}

/* ----- DOA meta parsing ----- */

/*
 * Parse a DOA GstStructure (delivered on the element's text/x-raw DOA pad)
 * into a single AdoaFrame. Returns TRUE and fills `frame` on success.
 */
static gboolean
parse_doa_fields (const GstStructure * s, AdoaFrame * frame)
{
  gdouble prob = 0.0;
  gint64 ts = 0;
  gint fov_start = 0, fov_end = 0;

  if (s == NULL)
    return FALSE;

  frame->probability = 0.0f;
  frame->timestamp = 0;
  frame->fov_start_deg = 0;
  frame->fov_end_deg = 0;
  frame->directions.clear ();
  memset (frame->polar_activity, 0, sizeof (frame->polar_activity));

  if (gst_structure_get_double (s, "probability", &prob))
    frame->probability = (float) prob;
  if (gst_structure_get_int64 (s, "timestamp", &ts))
    frame->timestamp = ts;
  if (gst_structure_get_int (s, "fov-start-deg", &fov_start))
    frame->fov_start_deg = fov_start;
  if (gst_structure_get_int (s, "fov-end-deg", &fov_end))
    frame->fov_end_deg = fov_end;

  /* directions: GST_TYPE_ARRAY of ints */
  const GValue *dv = gst_structure_get_value (s, "directions");
  if (dv != NULL && GST_VALUE_HOLDS_ARRAY (dv)) {
    guint n = gst_value_array_get_size (dv);
    frame->directions.reserve (n);
    for (guint i = 0; i < n; ++i) {
      const GValue *e = gst_value_array_get_value (dv, i);
      frame->directions.push_back ((int32_t) g_value_get_int (e));
    }
  }

  /* polar-activity: 360 bytes as GBytes */
  const GValue *pv = gst_structure_get_value (s, "polar-activity");
  if (pv != NULL && G_VALUE_HOLDS (pv, G_TYPE_BYTES)) {
    GBytes *bytes = (GBytes *) g_value_get_boxed (pv);
    gsize sz = 0;
    const guint8 *data = (const guint8 *) g_bytes_get_data (bytes, &sz);
    if (data != NULL && sz >= ADOA_TOTAL_DEGREES)
      memcpy (frame->polar_activity, data, ADOA_TOTAL_DEGREES);
  }

  return TRUE;
}

/* ----- Public C API ----- */

GstBeamformContext *
gst_beamform_context_new (const GstBeamformConfig * config)
{
  GST_DEBUG_CATEGORY_INIT (gst_beamform_context_debug, "qtibeamforming", 0,
      "QTI beamforming fusion plugin");

  if (config == NULL)
    return NULL;

  GstBeamformContext *ctx = new (std::nothrow) _GstBeamformContext ();
  if (ctx == NULL)
    return NULL;

  ctx->config = *config;

  /* Reasonable defaults for the internal-filter tuning. */
  int_filter_conf_t int_conf;
  int_conf.box_tracking_outside_percent = 10;
  int_conf.default_detection_audio_doa_list_size = 30;
  int_conf.history_filter_depth = 10;
  int_conf.default_signal_pos_list_size = 30;
  int_conf.detection_audio_doa_threshold_high = 60;
  int_conf.detection_audio_doa_threshold_low = 30;
  int_conf.doa_detected_delay = 30;
  int_conf.doa_change_delay = 15;
  int_conf.average_fov_filter_en = false;

  int32_t ret = ctx->filter.Connect (
      config->image_width,
      config->camera_fov,
      config->mic_orientation,
      config->mic_flip,
      config->mic_resolution,
      config->probability_threshold,
      config->single_act_threshold,
      config->single_act_deadline,
      config->single_deact_delay,
      config->multi_act_threshold,
      config->multi_act_deadline,
      config->multi_deact_delay,
      config->multi_max_speakers,
      config->multi_enabled,
      int_conf,
      config->filter_select,
      false,
      std::string (),
      std::string ());

  if (ret != 0) {
    GST_WARNING ("AdoaFilter::Connect failed");
    delete ctx;
    return NULL;
  }

  GST_INFO ("Created beamform context (w=%d h=%d fov=%d)",
      config->image_width, config->image_height, config->camera_fov);
  return ctx;
}

void
gst_beamform_context_free (GstBeamformContext * context)
{
  if (context == NULL)
    return;
  context->filter.Disconnect ();
  delete context;
}

gboolean
gst_beamform_context_process (GstBeamformContext * context, GstBuffer * buffer,
    const GstStructure * doa_fields)
{
  if (context == NULL || buffer == NULL)
    return FALSE;

  GstVideoRegionOfInterestMeta *roimeta = NULL;
  gpointer state = NULL;
  vector<ai_crop_data> rois;
  vector<GstVideoRegionOfInterestMeta *> roi_ptrs;

  /* Collect the vision ROIs. */
  while ((roimeta = (GstVideoRegionOfInterestMeta *)
          gst_buffer_iterate_meta_filtered (buffer, &state,
              GST_VIDEO_REGION_OF_INTEREST_META_API_TYPE)) != NULL) {
    if (rois.size () >= GST_BEAMFORM_MAX_ROI)
      break;
    ai_crop_data roi;
    roi.x = (int32_t) roimeta->x;
    roi.y = (int32_t) roimeta->y;
    roi.width = (int32_t) roimeta->w;
    roi.height = (int32_t) roimeta->h;
    roi.color = 255;
    rois.push_back (roi);
    roi_ptrs.push_back (roimeta);
  }

  /* No input ROIs -> nothing to select; leave the buffer as-is. */
  if (rois.empty ())
    return TRUE;

  /* Parse the DOA fields (supplied by the element from the DOA pad). */
  vector<AdoaFrame> frames;
  AdoaFrame frame;
  if (parse_doa_fields (doa_fields, &frame))
    frames.push_back (frame);

  vector<ai_crop_data> selected;

  if (frames.empty ()) {
    /* No DOA data -> keep all ROIs (pass through). */
    selected = rois;
  } else {
    /* Deduplicate ROIs, run the filter, then select by overlap. */
    vector<ai_crop_data> urois = RoiUnique (rois, context->config.image_width,
        context->config.remove_large_rois, context->config.large_roi_percent);

    vector<target_t> targets;
    context->filter.Filter (frames, urois, targets);

    if (targets.empty ())
      selected = rois;          /* heard audio but no target -> keep all */
    else
      selected = SelectOutput (urois, targets);
  }

  GST_LOG ("beamforming: %zu input ROIs, %zu DOA, %zu selected",
      rois.size (), frames.size (), selected.size ());

  /*
   * Output policy: keep only the selected speaker ROI meta(s), drop the rest.
   * On fallback (no DOA / no target) `selected` == all ROIs, so nothing is
   * dropped.
   */
  for (size_t i = 0; i < rois.size (); ++i) {
    bool keep = false;
    for (size_t j = 0; j < selected.size (); ++j) {
      if (rois[i].x == selected[j].x &&
          rois[i].y == selected[j].y &&
          rois[i].width == selected[j].width &&
          rois[i].height == selected[j].height) {
        keep = true;
        break;
      }
    }
    if (!keep)
      gst_buffer_remove_meta (buffer, (GstMeta *) roi_ptrs[i]);
  }

  return TRUE;
}
