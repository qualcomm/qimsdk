/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <glib-unix.h>
#include <stdio.h>

#include <gst_sample_apps_utils.h>
#include <gst/video/video.h>
#include <gst/ml/ml-frame.h>
#include <gst/ml/ml-module-utils.h>
#include <gst/ml/ml-post-process-detection.h>

/*
 * Description of the pipeline.
 */
#define DESCRIPTION \
  "This application sets up a GStreamer pipeline for object detection using a " \
  "quantized YOLOv8 model, with a custom (application-provided) postprocess " \
  "callback instead of the qtimlpostprocess built-in module. It supports " \
  "various video input sources and output types."

/*
 * Default input branch used by the sample application.
 */
#define DEFAULT_INPUT \
  "filesrc location=%s/Downloads/qimsdk_samples/media/video.mp4 ! qtdemux ! h264parse ! "\
  "v4l2h264dec capture-io-mode=4 output-io-mode=4 ! video/x-raw,format=NV12"

/*
 * Default output branch used by the sample application.
 */
#define DEFAULT_OUTPUT \
  "display"

/*
 * Minimum confidence (raw fraction, NOT percent) for a box to be reported by
 * the custom postprocess callback. Compared directly against the raw model
 * score before it is converted to a percentage for the output struct.
 */
#define CONFIDENCE_THRESHOLD 0.70f

/*
 * ClassLabel:
 * @name: Human readable class name.
 * @color: ARGB/RGBA color associated with the class, as used by overlay
 *         rendering.
 *
 * A single entry parsed from the labels JSON file, indexed by class id.
 */
typedef struct
{
  gchar  *name;
  guint32 color;
} ClassLabel;

/*
 * Application-wide context, extending GstAppContext with the data needed
 * by the custom postprocess callback.
 */
typedef struct
{
  GstAppContext base;

  GArray *labels;
} AppContext;

/*
 * Get GST output type.
 */
gchar*
gst_get_sink (const gchar *type, const gchar *home)
{
    if (g_strcmp0(type, "display") == 0) {
        // Display using Wayland
        return g_strdup ("waylandsink fullscreen=true");
    }
    else if (g_strcmp0(type, "video") == 0) {
        // Encode and save video
        return g_strdup_printf (
          "v4l2h264enc capture-io-mode=4 output-io-mode=4 ! queue ! "
          "h264parse ! mp4mux ! "
          "filesink location=%s/Downloads/qimsdk_samples/media/output.mp4", home
        );
    }
    else if (g_str_has_prefix(type, "appsink")) {
        // Appsink with dynamic name
        return g_strdup ("queue ! appsink name=appsink sync=false emit-signals=true");
    }
    else {
        g_error("Unknown sink type: %s", type);
        return NULL;
    }
}

/*
 * Free the contents of a ClassLabel entry (used as a GArray clear func).
 */
static void
class_label_clear (gpointer data)
{
  ClassLabel *label = (ClassLabel *) data;
  g_free (label->name);
  label->name = NULL;
}

/*
 * Load the labels JSON file into a GArray of ClassLabel indexed by class id.
 *
 * The file format is a list of objects, e.g.:
 *   [
 *     {"id": 0, "color": "0x00FF00FF", "label": "person"},
 *     ...
 *   ]
 *
 * @path: path to the labels file.
 * @result: (transfer full): a new GArray of ClassLabel, or NULL on failure.
 */
static GArray *
load_labels (const gchar * path)
{
  GArray *labels = NULL;
  GRegex *object_re = NULL, *id_re = NULL, *label_re = NULL, *color_re = NULL;
  GMatchInfo *match_info = NULL;
  gchar *content = NULL;
  GError *error = NULL;

  if (!g_file_get_contents (path, &content, NULL, &error)) {
    g_printerr ("ERROR: Failed to read labels file '%s': %s\n", path,
        error ? error->message : "unknown error");
    g_clear_error (&error);
    return NULL;
  }

  labels = g_array_new (FALSE, TRUE, sizeof (ClassLabel));
  g_array_set_clear_func (labels, class_label_clear);

  object_re = g_regex_new ("\\{[^\\}]*\\}", 0, 0, NULL);
  id_re = g_regex_new ("\"id\"\\s*:\\s*(\\d+)", 0, 0, NULL);
  label_re = g_regex_new ("\"label\"\\s*:\\s*\"([^\"]+)\"", 0, 0, NULL);
  color_re = g_regex_new ("\"color\"\\s*:\\s*\"([0-9a-fA-FxX]+)\"", 0, 0, NULL);

  g_regex_match (object_re, content, 0, &match_info);

  while (g_match_info_matches (match_info)) {
    gchar *object = g_match_info_fetch (match_info, 0);
    GMatchInfo *id_match = NULL, *label_match = NULL, *color_match = NULL;

    if (g_regex_match (id_re, object, 0, &id_match) &&
        g_regex_match (label_re, object, 0, &label_match) &&
        g_regex_match (color_re, object, 0, &color_match)) {
      gchar *id_str = g_match_info_fetch (id_match, 1);
      gchar *label_str = g_match_info_fetch (label_match, 1);
      gchar *color_str = g_match_info_fetch (color_match, 1);

      guint id = (guint) g_ascii_strtoull (id_str, NULL, 10);
      guint32 color = (guint32) g_ascii_strtoull (color_str, NULL, 16);

      if (id >= labels->len) {
        g_array_set_size (labels, id + 1);
        /* g_array_set_size zero-fills new elements, so freshly grown
         * entries already have name = NULL, color = 0. */
      }

      ClassLabel *entry = &g_array_index (labels, ClassLabel, id);
      g_free (entry->name);
      entry->name = label_str;
      entry->color = color;

      g_free (id_str);
      g_free (color_str);
    }

    g_match_info_free (id_match);
    g_match_info_free (label_match);
    g_match_info_free (color_match);

    g_free (object);
    g_match_info_next (match_info, NULL);
  }

  g_match_info_free (match_info);

  g_regex_unref (object_re);
  g_regex_unref (id_re);
  g_regex_unref (label_re);
  g_regex_unref (color_re);

  g_free (content);

  g_print ("Loaded %u labels from '%s'\n", labels->len, path);

  return labels;
}

/*
 * Custom postprocess callback for the "process-object-detection" action
 * signal on qtimlpostprocess.
 *
 * Decodes the raw model output tensors (boxes/scores/classes, YOLOv8 with
 * NMS/argmax already baked into the exported tflite) by threshold-filtering
 * on raw score, remapping box coordinates from tensor-space into
 * frame-normalized [0,1] space via the source region attached to mlparam,
 * discarding out-of-bounds boxes, and resolving per-class label/color. Greedy
 * IoU-based NMS across boxes of the same class is delegated to the library's
 * own gst_ml_detections_non_max_suppression(), which both merges/rejects
 * overlapping boxes and appends surviving ones in a single call. The
 * element's own "results" property subsequently sorts and truncates the
 * output to the top-N results.
 *
 * @postprocess: the qtimlpostprocess GstElement emitting the signal.
 * @mlframe: input tensors for this inference batch (boxes, scores, classes).
 * @mlparam: additional per-batch parameters, including the source region.
 * @predictions: pre-allocated output list to be filled by this callback.
 * @userdata: pointer to the AppContext.
 *
 * Returns: TRUE on success, FALSE on a hard failure (e.g. missing tensors).
 */
static gboolean
on_process_object_detection (GstElement * postprocess,
    GstMLFrame * mlframe, GstStructure * mlparam,
    GstMLDetections * predictions, gpointer userdata)
{
  AppContext *appctx = (AppContext *) userdata;
  GstMLTensor boxes = { 0, }, scores = { 0, }, classes = { 0, };
  GstVideoRectangle region = { 0, };
  const gfloat *bbox_data = NULL, *score_data = NULL, *class_data = NULL;
  guint n_paxels = 0, idx = 0;

  (void) postprocess;

  if (GST_ML_FRAME_N_TENSORS (mlframe) < 3) {
    g_printerr ("ERROR: Detection callback expected 3 tensors, got %u!\n",
        GST_ML_FRAME_N_TENSORS (mlframe));
    return FALSE;
  }

  boxes = gst_ml_frame_get_tensor (mlframe, 0);
  scores = gst_ml_frame_get_tensor (mlframe, 1);
  classes = gst_ml_frame_get_tensor (mlframe, 2);

  gst_ml_structure_get_source_region (mlparam, &region);

  n_paxels = boxes.dimensions[1];
  bbox_data = (const gfloat *) boxes.data;
  score_data = (const gfloat *) scores.data;
  class_data = (const gfloat *) classes.data;

  for (idx = 0; idx < n_paxels; ++idx) {
    gfloat confidence = score_data[idx];
    guint class_idx = (guint) class_data[idx];
    GstMLDetection entry = { 0, };

    if (confidence < CONFIDENCE_THRESHOLD)
      continue;

    entry.left = bbox_data[idx * 4];
    entry.top = bbox_data[idx * 4 + 1];
    entry.right = bbox_data[idx * 4 + 2];
    entry.bottom = bbox_data[idx * 4 + 3];

    /* Remap from tensor-space into frame-normalized [0,1] space without
     * clamping, so out-of-bounds boxes can be discarded below. */
    gst_ml_detection_relative_transform (&entry, &region, FALSE);

    if (entry.top < 0.0f || entry.left < 0.0f ||
        entry.right < 0.0f || entry.bottom < 0.0f ||
        entry.top > 1.0f || entry.left > 1.0f ||
        entry.right > 1.0f || entry.bottom > 1.0f)
      continue;

    entry.confidence = confidence * 100.0f;

    if (appctx->labels != NULL && class_idx < appctx->labels->len) {
      ClassLabel *label = &g_array_index (appctx->labels, ClassLabel, class_idx);

      if (label->name != NULL) {
        entry.name = g_quark_from_string (label->name);
        entry.color = label->color;
      }
    }

    gst_ml_detections_non_max_suppression (predictions, &entry,
        GST_ML_DETECTION_NMS_THRESHOLD);
  }

  return TRUE;
}

/*
 * Application entry point.
 */
int main(int argc, char * argv[])
{
  AppContext appctx = { 0 };
  guint interrupt_watch_id = 0;
  GstBus *bus = NULL;
  GstElement *postprocess = NULL;
  GError *error = NULL;
  const gchar *home = g_getenv ("HOME");
  gchar *model_base_path = g_strdup_printf ("%s/Downloads/qimsdk_samples/", home);
  gchar *source = g_strdup_printf (DEFAULT_INPUT, home);
  gchar *model_label_base = NULL;
  gchar *labels_path = NULL;
  gchar *output = g_strdup (DEFAULT_OUTPUT);

  /* Initialize GStreamer before using any GstElement, GstBus or caps API. */
  gst_init (&argc, &argv);

  /* Command line options allow the default input branch to be replaced. */
  GOptionEntry entries[] = {
    { "source", 's', 0, G_OPTION_ARG_STRING, &source,
      "GStreamer source pipeline", NULL },
    { "model-base-path", 0, 0, G_OPTION_ARG_STRING, &model_base_path,
      "Directory containing models/ and labels/", NULL },
    { "output", 'o', 0, G_OPTION_ARG_STRING, &output,
      "GStreamer output pipeline", NULL },
    { NULL }
  };

  GOptionContext *context =
      g_option_context_new (DESCRIPTION);
  g_option_context_add_main_entries (context, entries, NULL);
  if (!g_option_context_parse (context, &argc, &argv, NULL)) {
    g_printerr ("ERROR: Failed to parse arguments\n");
    return -1;
  }

  model_label_base = g_str_has_suffix (model_base_path, "/") ?
      g_strdup (model_base_path) : g_strdup_printf ("%s/", model_base_path);

  labels_path = g_strdup_printf ("%slabels/yolov8.json", model_label_base);
  appctx.labels = load_labels (labels_path);
  g_free (labels_path);

  if (appctx.labels == NULL) {
    g_printerr ("ERROR: Could not load labels, aborting.\n");
    return -1;
  }

  /*
   * Compose the full pipeline string from the selected source branch and a
   * named appsink. The qtimlpostprocess element has no "module" property
   * set, which leaves it in signal/callback mode; the custom postprocess
   * handler is connected below, before the pipeline is started.
   */
  gchar *ml_pipe = g_strdup_printf (
    // Use a tee element to pass the video frame sequentially,
    // first to mlvconverter, then to metamux.
    "tee name=t "

    // Preprocess the video for inference
    "t. ! qtimlvconverter name=preprocess ! queue ! "

    // Run inference using the yolov8 model
    "qtimltflite name=inference delegate=external "
    "external-delegate-path=libQnnTFLiteDelegate.so "
    "external-delegate-options=\"QNNExternalDelegate,backend_type=htp;\" "
    "model=%smodels/yolov8_det_quantized.tflite ! queue ! "

    // Postprocess inference results with a custom (application-provided)
    // callback instead of a built-in module.
    "qtimlpostprocess name=postprocess results=8 ! "
    "text/x-raw ! metamux. "

    // Attch ML result to video frame
    "t. ! qtimetamux name=metamux ! "

    // Overlay ML result on top of video frame
    "qtivoverlay",
    model_label_base
  );

  gchar *sink = gst_get_sink (output, home);
  gchar *pipeline_str = g_strdup_printf ("%s ! %s ! %s",
    // Video source input
    source,

    // ML processing branch
    ml_pipe,

    // Output (e.g. display, appsink, encoded video)
    sink
  );

  g_free (ml_pipe);
  g_free (sink);

  g_print ("Pipeline:\n%s\n\n", pipeline_str);

  /* Create the GStreamer pipeline instance from the final pipeline string. */
  appctx.base.pipeline = gst_parse_launch (pipeline_str, &error);
  g_free (pipeline_str);
  if (!appctx.base.pipeline) {
    g_printerr ("ERROR: Pipeline creation failed: %s\n",
               error ? error->message : "unknown error");
    if (error) g_error_free (error);
    return -1;
  }

  /* The main loop keeps the application alive while the pipeline is running. */
  appctx.base.mloop = g_main_loop_new (NULL, FALSE);

  /*
   * Watch the pipeline bus for EOS. The EOS callback stops the main loop, which
   * then lets the application continue into the shutdown path below.
   */
  bus = gst_pipeline_get_bus (GST_PIPELINE (appctx.base.pipeline));
  if (bus == NULL) {
    g_printerr ("ERROR: Failed to get pipeline bus.\n");
    return FALSE;
  }
  gst_bus_add_signal_watch (bus);
  g_signal_connect (bus, "message::eos", G_CALLBACK (eos_cb), appctx.base.mloop);
  gst_object_unref (bus);

  /*
   * Find the named postprocess element and connect the custom postprocess
   * callback. Leaving the "module" property unset means qtimlpostprocess
   * will use the signal/callback dispatch path once a handler is connected.
   */
  postprocess =
      gst_bin_get_by_name (GST_BIN (appctx.base.pipeline), "postprocess");
  if (postprocess == NULL) {
    g_printerr ("ERROR: Failed to find postprocess element.\n");
    return -1;
  }
  g_signal_connect (postprocess, "process-object-detection",
      G_CALLBACK (on_process_object_detection), &appctx);
  gst_object_unref (postprocess);

  /* Install a Ctrl+C handler so the pipeline receives EOS before shutdown. */
  interrupt_watch_id = g_unix_signal_add (SIGINT,
      handle_interrupt_signal, &appctx.base);

  g_print ("Starting pipeline...\n");

  /* Move the pipeline to PLAYING; streaming starts after this state change. */
  gst_element_set_state (appctx.base.pipeline, GST_STATE_PLAYING);

  /* Block here until EOS, Ctrl+C or another callback quits the main loop. */
  g_main_loop_run (appctx.base.mloop);

  g_print ("Main loop stopped.\n");

  /* Remove the Unix signal source before destroying the application context. */
  g_source_remove (interrupt_watch_id);

  /* Stop the pipeline and release the pipeline object. */
  gst_element_set_state (appctx.base.pipeline, GST_STATE_NULL);
  gst_object_unref (appctx.base.pipeline);

  /* Release the GLib main loop and command line string allocation. */
  g_main_loop_unref (appctx.base.mloop);

  if (appctx.labels != NULL)
    g_array_free (appctx.labels, TRUE);

  g_free (source);
  g_free (model_base_path);
  g_free (model_label_base);

  /* Deinitialize GStreamer after all GStreamer objects have been released. */
  gst_deinit ();

  return 0;
}
