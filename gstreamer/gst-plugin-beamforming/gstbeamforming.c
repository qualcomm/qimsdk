/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstbeamforming.h"

#define GST_CAT_DEFAULT gst_beamforming_debug
GST_DEBUG_CATEGORY_STATIC (gst_beamforming_debug);

#define gst_beamforming_parent_class parent_class
G_DEFINE_TYPE (GstBeamforming, gst_beamforming, GST_TYPE_ELEMENT);

/* Default property values. */
#define DEFAULT_IMAGE_WIDTH            640
#define DEFAULT_IMAGE_HEIGHT           480
#define DEFAULT_CAMERA_FOV            110
#define DEFAULT_MIC_ORIENTATION        0
#define DEFAULT_MIC_FLIP               FALSE
#define DEFAULT_MIC_RESOLUTION        10
#define DEFAULT_PROBABILITY_THRESHOLD 0.5f
#define DEFAULT_SINGLE_ACT_THRESHOLD  10
#define DEFAULT_SINGLE_ACT_DEADLINE   30
#define DEFAULT_SINGLE_DEACT_DELAY    120
#define DEFAULT_MULTI_ACT_THRESHOLD   10
#define DEFAULT_MULTI_ACT_DEADLINE    30
#define DEFAULT_MULTI_DEACT_DELAY     30
#define DEFAULT_MULTI_MAX_SPEAKERS     5
#define DEFAULT_MULTI_ENABLED         FALSE
#define DEFAULT_FILTER_SELECT          0
#define DEFAULT_REMOVE_LARGE_ROIS     TRUE
#define DEFAULT_LARGE_ROI_PERCENT     90

enum
{
  PROP_0,
  PROP_IMAGE_WIDTH,
  PROP_IMAGE_HEIGHT,
  PROP_CAMERA_FOV,
  PROP_MIC_ORIENTATION,
  PROP_MIC_FLIP,
  PROP_MIC_RESOLUTION,
  PROP_PROBABILITY_THRESHOLD,
  PROP_SINGLE_ACT_THRESHOLD,
  PROP_SINGLE_ACT_DEADLINE,
  PROP_SINGLE_DEACT_DELAY,
  PROP_MULTI_ACT_THRESHOLD,
  PROP_MULTI_ACT_DEADLINE,
  PROP_MULTI_DEACT_DELAY,
  PROP_MULTI_MAX_SPEAKERS,
  PROP_MULTI_ENABLED,
  PROP_FILTER_SELECT,
  PROP_REMOVE_LARGE_ROIS,
  PROP_LARGE_ROI_PERCENT,
};

#define GST_BEAMFORMING_VIDEO_CAPS "video/x-raw(ANY)"
#define GST_BEAMFORMING_DOA_CAPS   "text/x-raw"

static GstStaticPadTemplate gst_beamforming_sink_template =
    GST_STATIC_PAD_TEMPLATE ("sink",
        GST_PAD_SINK,
        GST_PAD_ALWAYS,
        GST_STATIC_CAPS (GST_BEAMFORMING_VIDEO_CAPS));

static GstStaticPadTemplate gst_beamforming_doa_template =
    GST_STATIC_PAD_TEMPLATE ("doa",
        GST_PAD_SINK,
        GST_PAD_ALWAYS,
        GST_STATIC_CAPS (GST_BEAMFORMING_DOA_CAPS));

static GstStaticPadTemplate gst_beamforming_src_template =
    GST_STATIC_PAD_TEMPLATE ("src",
        GST_PAD_SRC,
        GST_PAD_ALWAYS,
        GST_STATIC_CAPS (GST_BEAMFORMING_VIDEO_CAPS));

/* ----- config + context lifecycle ----- */

static void
gst_beamforming_fill_config (GstBeamforming * self, GstBeamformConfig * config)
{
  config->image_width = self->image_width;
  config->image_height = self->image_height;
  config->camera_fov = self->camera_fov;
  config->mic_orientation = self->mic_orientation;
  config->mic_flip = self->mic_flip;
  config->mic_resolution = self->mic_resolution;
  config->probability_threshold = self->probability_threshold;
  config->single_act_threshold = self->single_act_threshold;
  config->single_act_deadline = self->single_act_deadline;
  config->single_deact_delay = self->single_deact_delay;
  config->multi_act_threshold = self->multi_act_threshold;
  config->multi_act_deadline = self->multi_act_deadline;
  config->multi_deact_delay = self->multi_deact_delay;
  config->multi_max_speakers = self->multi_max_speakers;
  config->multi_enabled = self->multi_enabled;
  config->filter_select = self->filter_select;
  config->remove_large_rois = self->remove_large_rois;
  config->large_roi_percent = self->large_roi_percent;
}

static gboolean
gst_beamforming_ensure_context (GstBeamforming * self)
{
  GstBeamformConfig config;

  if (self->context != NULL && !self->needs_reconfigure)
    return TRUE;

  if (self->context != NULL) {
    gst_beamform_context_free (self->context);
    self->context = NULL;
  }

  gst_beamforming_fill_config (self, &config);
  self->context = gst_beamform_context_new (&config);
  self->needs_reconfigure = FALSE;

  if (self->context == NULL) {
    GST_WARNING_OBJECT (self, "Failed to create beamform context");
    return FALSE;
  }

  return TRUE;
}

/* ----- DOA pad: parse text/x-raw buffer into a GstStructure and cache it ----- */

static GstFlowReturn
gst_beamforming_doa_chain (GstPad * pad, GstObject * parent, GstBuffer * buffer)
{
  GstBeamforming *self = GST_BEAMFORMING (parent);
  GstBeamformDataPad *doapad = GST_BEAMFORM_DATA_PAD (pad);
  GstMapInfo map;
  GstStructure *s = NULL;

  if (gst_buffer_map (buffer, &map, GST_MAP_READ)) {
    /* The DOA source serializes the DOA GstStructure to text on the
     * text/x-raw pad; parse it back. */
    if (map.data != NULL && map.size > 0) {
      gchar *text = g_strndup ((const gchar *) map.data, map.size);
      s = gst_structure_from_string (text, NULL);
      g_free (text);
    }
    gst_buffer_unmap (buffer, &map);
  }

  if (s != NULL) {
    gst_beamform_data_pad_set_latest (doapad, s,
        GST_BUFFER_PTS (buffer));
    gst_structure_free (s);
  } else {
    GST_WARNING_OBJECT (self, "Failed to parse DOA text/x-raw buffer");
  }

  /* DOA buffers are consumed here (not forwarded). */
  gst_buffer_unref (buffer);
  return GST_FLOW_OK;
}

/* ----- Video sink pad: run the fusion and push out src ----- */

static GstFlowReturn
gst_beamforming_sink_chain (GstPad * pad, GstObject * parent,
    GstBuffer * buffer)
{
  GstBeamforming *self = GST_BEAMFORMING (parent);
  GstStructure *doa = NULL;

  /* GAP buffer: forward unchanged. */
  if (gst_buffer_get_size (buffer) == 0 &&
      GST_BUFFER_FLAG_IS_SET (buffer, GST_BUFFER_FLAG_GAP))
    return gst_pad_push (GST_PAD (self->srcpad), buffer);

  GST_BEAMFORMING_LOCK (self);
  if (!gst_beamforming_ensure_context (self)) {
    GST_BEAMFORMING_UNLOCK (self);
    /* Pass through on failure. */
    return gst_pad_push (GST_PAD (self->srcpad), buffer);
  }

  /* Make the buffer writable so we can drop ROI metas. */
  buffer = gst_buffer_make_writable (buffer);

  /* Grab the latest cached DOA (ASYNC "use latest"). May be NULL. */
  doa = gst_beamform_data_pad_get_latest (self->doapad);

  gst_beamform_context_process (self->context, buffer, doa);

  if (doa != NULL)
    gst_structure_free (doa);

  GST_BEAMFORMING_UNLOCK (self);

  return gst_pad_push (GST_PAD (self->srcpad), buffer);
}

/* ----- Sink pad event: forward the important ones to src ----- */

static gboolean
gst_beamforming_sink_event (GstPad * pad, GstObject * parent, GstEvent * event)
{
  GstBeamforming *self = GST_BEAMFORMING (parent);
  return gst_pad_push_event (GST_PAD (self->srcpad), event);
}

/* ----- DOA pad event: consume caps/segment/eos locally ----- */

static gboolean
gst_beamforming_doa_event (GstPad * pad, GstObject * parent, GstEvent * event)
{
  /* DOA stream events are internal to this element; don't forward. */
  gst_event_unref (event);
  return TRUE;
}

/* ----- GObject property plumbing ----- */

static void
gst_beamforming_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstBeamforming *self = GST_BEAMFORMING (object);

  GST_BEAMFORMING_LOCK (self);
  switch (prop_id) {
    case PROP_IMAGE_WIDTH:
      self->image_width = g_value_get_int (value);
      break;
    case PROP_IMAGE_HEIGHT:
      self->image_height = g_value_get_int (value);
      break;
    case PROP_CAMERA_FOV:
      self->camera_fov = g_value_get_int (value);
      break;
    case PROP_MIC_ORIENTATION:
      self->mic_orientation = g_value_get_int (value);
      break;
    case PROP_MIC_FLIP:
      self->mic_flip = g_value_get_boolean (value);
      break;
    case PROP_MIC_RESOLUTION:
      self->mic_resolution = g_value_get_int (value);
      break;
    case PROP_PROBABILITY_THRESHOLD:
      self->probability_threshold = g_value_get_float (value);
      break;
    case PROP_SINGLE_ACT_THRESHOLD:
      self->single_act_threshold = g_value_get_int (value);
      break;
    case PROP_SINGLE_ACT_DEADLINE:
      self->single_act_deadline = g_value_get_int (value);
      break;
    case PROP_SINGLE_DEACT_DELAY:
      self->single_deact_delay = g_value_get_int (value);
      break;
    case PROP_MULTI_ACT_THRESHOLD:
      self->multi_act_threshold = g_value_get_int (value);
      break;
    case PROP_MULTI_ACT_DEADLINE:
      self->multi_act_deadline = g_value_get_int (value);
      break;
    case PROP_MULTI_DEACT_DELAY:
      self->multi_deact_delay = g_value_get_int (value);
      break;
    case PROP_MULTI_MAX_SPEAKERS:
      self->multi_max_speakers = g_value_get_int (value);
      break;
    case PROP_MULTI_ENABLED:
      self->multi_enabled = g_value_get_boolean (value);
      break;
    case PROP_FILTER_SELECT:
      self->filter_select = g_value_get_int (value);
      break;
    case PROP_REMOVE_LARGE_ROIS:
      self->remove_large_rois = g_value_get_boolean (value);
      break;
    case PROP_LARGE_ROI_PERCENT:
      self->large_roi_percent = g_value_get_int (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      GST_BEAMFORMING_UNLOCK (self);
      return;
  }
  self->needs_reconfigure = TRUE;
  GST_BEAMFORMING_UNLOCK (self);
}

static void
gst_beamforming_get_property (GObject * object, guint prop_id, GValue * value,
    GParamSpec * pspec)
{
  GstBeamforming *self = GST_BEAMFORMING (object);

  switch (prop_id) {
    case PROP_IMAGE_WIDTH:
      g_value_set_int (value, self->image_width);
      break;
    case PROP_IMAGE_HEIGHT:
      g_value_set_int (value, self->image_height);
      break;
    case PROP_CAMERA_FOV:
      g_value_set_int (value, self->camera_fov);
      break;
    case PROP_MIC_ORIENTATION:
      g_value_set_int (value, self->mic_orientation);
      break;
    case PROP_MIC_FLIP:
      g_value_set_boolean (value, self->mic_flip);
      break;
    case PROP_MIC_RESOLUTION:
      g_value_set_int (value, self->mic_resolution);
      break;
    case PROP_PROBABILITY_THRESHOLD:
      g_value_set_float (value, self->probability_threshold);
      break;
    case PROP_SINGLE_ACT_THRESHOLD:
      g_value_set_int (value, self->single_act_threshold);
      break;
    case PROP_SINGLE_ACT_DEADLINE:
      g_value_set_int (value, self->single_act_deadline);
      break;
    case PROP_SINGLE_DEACT_DELAY:
      g_value_set_int (value, self->single_deact_delay);
      break;
    case PROP_MULTI_ACT_THRESHOLD:
      g_value_set_int (value, self->multi_act_threshold);
      break;
    case PROP_MULTI_ACT_DEADLINE:
      g_value_set_int (value, self->multi_act_deadline);
      break;
    case PROP_MULTI_DEACT_DELAY:
      g_value_set_int (value, self->multi_deact_delay);
      break;
    case PROP_MULTI_MAX_SPEAKERS:
      g_value_set_int (value, self->multi_max_speakers);
      break;
    case PROP_MULTI_ENABLED:
      g_value_set_boolean (value, self->multi_enabled);
      break;
    case PROP_FILTER_SELECT:
      g_value_set_int (value, self->filter_select);
      break;
    case PROP_REMOVE_LARGE_ROIS:
      g_value_set_boolean (value, self->remove_large_rois);
      break;
    case PROP_LARGE_ROI_PERCENT:
      g_value_set_int (value, self->large_roi_percent);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_beamforming_finalize (GObject * object)
{
  GstBeamforming *self = GST_BEAMFORMING (object);

  if (self->context != NULL) {
    gst_beamform_context_free (self->context);
    self->context = NULL;
  }

  g_mutex_clear (&self->lock);

  G_OBJECT_CLASS (parent_class)->finalize (G_OBJECT (self));
}

/* ----- class / instance init ----- */

static void
gst_beamforming_class_init (GstBeamformingClass * klass)
{
  GObjectClass *gobject = G_OBJECT_CLASS (klass);
  GstElementClass *element = GST_ELEMENT_CLASS (klass);

  gobject->set_property = GST_DEBUG_FUNCPTR (gst_beamforming_set_property);
  gobject->get_property = GST_DEBUG_FUNCPTR (gst_beamforming_get_property);
  gobject->finalize     = GST_DEBUG_FUNCPTR (gst_beamforming_finalize);

  g_object_class_install_property (gobject, PROP_IMAGE_WIDTH,
      g_param_spec_int ("image-width", "Image Width",
          "Width of the video frame in pixels", 2, G_MAXINT,
          DEFAULT_IMAGE_WIDTH, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject, PROP_IMAGE_HEIGHT,
      g_param_spec_int ("image-height", "Image Height",
          "Height of the video frame in pixels", 2, G_MAXINT,
          DEFAULT_IMAGE_HEIGHT, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject, PROP_CAMERA_FOV,
      g_param_spec_int ("camera-fov", "Camera FOV",
          "Camera field of view in degrees (even number)", 2, 360,
          DEFAULT_CAMERA_FOV, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject, PROP_MIC_ORIENTATION,
      g_param_spec_int ("mic-orientation", "Mic Orientation",
          "Microphone array orientation in degrees", 0, 359,
          DEFAULT_MIC_ORIENTATION, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject, PROP_MIC_FLIP,
      g_param_spec_boolean ("mic-flip", "Mic Flip",
          "Flip the microphone angle polarity", DEFAULT_MIC_FLIP,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject, PROP_MIC_RESOLUTION,
      g_param_spec_int ("mic-resolution", "Mic Resolution",
          "Histogram bin resolution in degrees (even number)", 2, 360,
          DEFAULT_MIC_RESOLUTION, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject, PROP_PROBABILITY_THRESHOLD,
      g_param_spec_float ("probability-threshold", "Probability Threshold",
          "Minimum speech probability to trust a DOA frame", 0.0f, 1.0f,
          DEFAULT_PROBABILITY_THRESHOLD,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject, PROP_SINGLE_ACT_THRESHOLD,
      g_param_spec_int ("single-act-threshold", "Single Act Threshold",
          "Single-speaker histogram activation threshold", 0, G_MAXINT,
          DEFAULT_SINGLE_ACT_THRESHOLD,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject, PROP_SINGLE_ACT_DEADLINE,
      g_param_spec_int ("single-act-deadline", "Single Act Deadline",
          "Single-speaker histogram buffer length (samples)", 1, G_MAXINT,
          DEFAULT_SINGLE_ACT_DEADLINE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject, PROP_SINGLE_DEACT_DELAY,
      g_param_spec_int ("single-deact-delay", "Single Deact Delay",
          "Single-speaker target deactivation delay (frames)", 0, G_MAXINT,
          DEFAULT_SINGLE_DEACT_DELAY,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject, PROP_MULTI_ACT_THRESHOLD,
      g_param_spec_int ("multi-act-threshold", "Multi Act Threshold",
          "Multi-speaker histogram activation threshold", 0, G_MAXINT,
          DEFAULT_MULTI_ACT_THRESHOLD,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject, PROP_MULTI_ACT_DEADLINE,
      g_param_spec_int ("multi-act-deadline", "Multi Act Deadline",
          "Multi-speaker histogram buffer length (samples)", 1, G_MAXINT,
          DEFAULT_MULTI_ACT_DEADLINE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject, PROP_MULTI_DEACT_DELAY,
      g_param_spec_int ("multi-deact-delay", "Multi Deact Delay",
          "Multi-speaker target deactivation delay (frames)", 0, G_MAXINT,
          DEFAULT_MULTI_DEACT_DELAY,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject, PROP_MULTI_MAX_SPEAKERS,
      g_param_spec_int ("multi-max-speakers", "Multi Max Speakers",
          "Maximum number of simultaneous speakers", 1, 100,
          DEFAULT_MULTI_MAX_SPEAKERS,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject, PROP_MULTI_ENABLED,
      g_param_spec_boolean ("multi-enabled", "Multi Enabled",
          "Enable multi-speaker tracking", DEFAULT_MULTI_ENABLED,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject, PROP_FILTER_SELECT,
      g_param_spec_int ("filter-select", "Filter Select",
          "DOA filter: 0=histogram, 1=internal", 0, 1,
          DEFAULT_FILTER_SELECT, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject, PROP_REMOVE_LARGE_ROIS,
      g_param_spec_boolean ("remove-large-rois", "Remove Large ROIs",
          "Discard ROIs wider than large-roi-percent of the frame",
          DEFAULT_REMOVE_LARGE_ROIS,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject, PROP_LARGE_ROI_PERCENT,
      g_param_spec_int ("large-roi-percent", "Large ROI Percent",
          "Width threshold (percent of frame) for large-ROI removal", 1, 100,
          DEFAULT_LARGE_ROI_PERCENT,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gst_element_class_set_static_metadata (element, "Beamforming",
      "Filter/Effect/Video",
      "Fuses DOA audio metadata (doa pad) with vision-AI ROIs (sink pad) to "
      "select the ROI of the person currently speaking", "QTI");

  gst_element_class_add_pad_template (element,
      gst_static_pad_template_get (&gst_beamforming_sink_template));
  gst_element_class_add_pad_template (element,
      gst_static_pad_template_get (&gst_beamforming_doa_template));
  gst_element_class_add_pad_template (element,
      gst_static_pad_template_get (&gst_beamforming_src_template));

  GST_DEBUG_CATEGORY_INIT (gst_beamforming_debug, "qtibeamforming", 0,
      "QTI beamforming fusion plugin");
}

static void
gst_beamforming_init (GstBeamforming * self)
{
  GstElementClass *klass = GST_ELEMENT_GET_CLASS (self);
  GstPadTemplate *tmpl;

  g_mutex_init (&self->lock);

  self->context = NULL;
  self->needs_reconfigure = TRUE;

  self->image_width = DEFAULT_IMAGE_WIDTH;
  self->image_height = DEFAULT_IMAGE_HEIGHT;
  self->camera_fov = DEFAULT_CAMERA_FOV;
  self->mic_orientation = DEFAULT_MIC_ORIENTATION;
  self->mic_flip = DEFAULT_MIC_FLIP;
  self->mic_resolution = DEFAULT_MIC_RESOLUTION;
  self->probability_threshold = DEFAULT_PROBABILITY_THRESHOLD;
  self->single_act_threshold = DEFAULT_SINGLE_ACT_THRESHOLD;
  self->single_act_deadline = DEFAULT_SINGLE_ACT_DEADLINE;
  self->single_deact_delay = DEFAULT_SINGLE_DEACT_DELAY;
  self->multi_act_threshold = DEFAULT_MULTI_ACT_THRESHOLD;
  self->multi_act_deadline = DEFAULT_MULTI_ACT_DEADLINE;
  self->multi_deact_delay = DEFAULT_MULTI_DEACT_DELAY;
  self->multi_max_speakers = DEFAULT_MULTI_MAX_SPEAKERS;
  self->multi_enabled = DEFAULT_MULTI_ENABLED;
  self->filter_select = DEFAULT_FILTER_SELECT;
  self->remove_large_rois = DEFAULT_REMOVE_LARGE_ROIS;
  self->large_roi_percent = DEFAULT_LARGE_ROI_PERCENT;

  /* Create the video sink pad. */
  tmpl = gst_element_class_get_pad_template (klass, "sink");
  self->sinkpad = g_object_new (GST_TYPE_BEAMFORM_SINK_PAD,
      "name", "sink", "direction", GST_PAD_SINK, "template", tmpl, NULL);
  gst_pad_set_chain_function (GST_PAD (self->sinkpad),
      GST_DEBUG_FUNCPTR (gst_beamforming_sink_chain));
  gst_pad_set_event_function (GST_PAD (self->sinkpad),
      GST_DEBUG_FUNCPTR (gst_beamforming_sink_event));
  gst_element_add_pad (GST_ELEMENT (self), GST_PAD (self->sinkpad));

  /* Create the DOA sink pad. */
  tmpl = gst_element_class_get_pad_template (klass, "doa");
  self->doapad = g_object_new (GST_TYPE_BEAMFORM_DATA_PAD,
      "name", "doa", "direction", GST_PAD_SINK, "template", tmpl, NULL);
  gst_pad_set_chain_function (GST_PAD (self->doapad),
      GST_DEBUG_FUNCPTR (gst_beamforming_doa_chain));
  gst_pad_set_event_function (GST_PAD (self->doapad),
      GST_DEBUG_FUNCPTR (gst_beamforming_doa_event));
  gst_element_add_pad (GST_ELEMENT (self), GST_PAD (self->doapad));

  /* Create the source pad. */
  tmpl = gst_element_class_get_pad_template (klass, "src");
  self->srcpad = g_object_new (GST_TYPE_BEAMFORM_SRC_PAD,
      "name", "src", "direction", GST_PAD_SRC, "template", tmpl, NULL);
  gst_element_add_pad (GST_ELEMENT (self), GST_PAD (self->srcpad));
}

static gboolean
plugin_init (GstPlugin * plugin)
{
  return gst_element_register (plugin, "qtibeamforming", GST_RANK_NONE,
      GST_TYPE_BEAMFORMING);
}

GST_PLUGIN_DEFINE (
    GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    qtibeamforming,
    "QTI beamforming fusion plugin",
    plugin_init,
    PACKAGE_VERSION,
    PACKAGE_LICENSE,
    PACKAGE_SUMMARY,
    PACKAGE_ORIGIN
)
