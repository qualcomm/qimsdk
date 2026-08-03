/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <math.h>

#include <glib-unix.h>
#include <stdio.h>

#include <gst_sample_apps_utils.h>
#include <gst/video/video.h>
#include <gst/ml/ml-frame.h>
#include <gst/ml/ml-module-utils.h>
#include <gst/ml/ml-post-process-segmentation.h>

/*
 * Description of the pipeline.
 */
#define DESCRIPTION \
  "This application sets up a GStreamer pipeline for semantic segmentation " \
  "using a quantized DeepLabV3+ model, with a custom (application-provided) " \
  "postprocess callback instead of the qtimlpostprocess built-in module. " \
  "It supports various video input sources and output types."

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
 * Default label/color used for mask pixels whose class id has no matching
 * labels entry (or falls outside the labels array).
 */
#define UNKNOWN_LABEL "unknown"
#define UNKNOWN_COLOR 0x0000000F

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
 *     {"id": 0, "color": "0x00FF00FF", "label": "background"},
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
 * Custom postprocess callback for the "process-segmentation" action signal
 * on qtimlpostprocess.
 *
 * Decodes the raw model output tensor (a single tensor, either pre-argmaxed
 * per-pixel class ids or raw per-class scores requiring an inline argmax),
 * maps the region of interest from frame-space into the tensor's pixel grid
 * using the "input-tensor-width"/"input-tensor-height"/"input-tensor-region"
 * parameters attached to mlparam, and fills a single GstMLSegmentation mask
 * covering that region with per-pixel label/color pairs. Pixels whose class
 * id has no matching labels entry keep the "unknown"/near-transparent
 * default. The library itself performs the actual mask-to-frame drawing once
 * this callback returns, via gst_ml_segmentations_visualize() /
 * gst_cairo_draw_mask() downstream.
 *
 * @postprocess: the qtimlpostprocess GstElement emitting the signal.
 * @mlframe: input tensors for this inference batch (single score/id tensor).
 * @mlparam: additional per-batch parameters, including source dimensions
 *           and region.
 * @predictions: pre-allocated output list to be filled by this callback.
 * @userdata: pointer to the AppContext.
 *
 * Returns: TRUE on success, FALSE on a hard failure (e.g. missing tensor or
 *          parameters, or an empty/invalid region).
 */
static gboolean
on_process_segmentation (GstElement * postprocess,
    GstMLFrame * mlframe, GstStructure * mlparam,
    GstMLSegmentations * predictions, gpointer userdata)
{
  AppContext *appctx = (AppContext *) userdata;
  GstMLTensor tensor = { 0, };
  GstVideoRectangle region = { 0, };
  GstMLSegmentation segmentation = { 0, };
  const gfloat *data = NULL;
  guint source_width = 0, source_height = 0;
  guint mlwidth = 0, mlheight = 0, n_scores = 0;
  gfloat wscale = 0.0f, hscale = 0.0f;
  guint left = 0, top = 0, right = 0, bottom = 0;
  guint row = 0, column = 0;

  (void) postprocess;

  if (GST_ML_FRAME_N_TENSORS (mlframe) == 0) {
    g_printerr ("ERROR: Segmentation callback received no tensors!\n");
    return FALSE;
  }

  tensor = gst_ml_frame_get_tensor (mlframe, 0);
  if (tensor.type != GST_ML_TYPE_FLOAT32 || tensor.data == NULL) {
    g_printerr ("ERROR: Segmentation callback expected a float32 tensor!\n");
    return FALSE;
  }

  data = (const gfloat *) tensor.data;
  mlwidth = tensor.dimensions[2];
  mlheight = tensor.dimensions[1];
  /* The 4th tensor dimension, when present, holds per-class scores; a
   * pre-argmaxed model only has 3 dimensions and each paxel is a class id. */
  n_scores = (tensor.n_dimensions != 4) ? 1 : tensor.dimensions[3];

  gst_ml_structure_get_source_dimensions (mlparam, &source_width,
      &source_height);
  if (source_width == 0 || source_height == 0) {
    g_printerr ("ERROR: Segmentation callback missing source dimensions!\n");
    return FALSE;
  }

  gst_ml_structure_get_source_region (mlparam, &region);

  /* Scale factors for avoiding division and instead using multiplication
   * when mapping the frame-space region into the tensor's pixel grid. */
  wscale = (gfloat) mlwidth / (gfloat) source_width;
  hscale = (gfloat) mlheight / (gfloat) source_height;

  left = (guint) lroundf (region.x * wscale);
  top = (guint) lroundf (region.y * hscale);
  right = (guint) lroundf ((region.x + region.w) * wscale);
  bottom = (guint) lroundf ((region.y + region.h) * hscale);

  right = MIN (right, mlwidth);
  bottom = MIN (bottom, mlheight);

  if (left >= right || top >= bottom) {
    g_printerr ("ERROR: Segmentation callback computed an empty region!\n");
    return FALSE;
  }

  segmentation.n_rows = bottom - top;
  segmentation.n_columns = right - left;

  {
    guint out_size = segmentation.n_rows * segmentation.n_columns;
    GQuark unknown_quark = g_quark_from_string (UNKNOWN_LABEL);
    guint idx;

    segmentation.labels = g_array_sized_new (FALSE, FALSE, sizeof (GQuark),
        out_size);
    segmentation.colors = g_array_sized_new (FALSE, FALSE, sizeof (guint32),
        out_size);

    for (idx = 0; idx < out_size; ++idx) {
      guint32 unknown_color = UNKNOWN_COLOR;
      g_array_append_val (segmentation.labels, unknown_quark);
      g_array_append_val (segmentation.colors, unknown_color);
    }
  }

  for (row = top; row < bottom; ++row) {
    guint out_row = (row - top) * segmentation.n_columns;
    guint row_base = row * mlwidth * n_scores;

    for (column = left; column < right; ++column) {
      guint pix_base = row_base + (column * n_scores);
      guint class_idx = 0;

      if (n_scores == 1) {
        class_idx = (guint) data[pix_base];
      } else {
        guint best = pix_base, num;

        for (num = pix_base + 1; num < pix_base + n_scores; ++num)
          best = (data[num] > data[best]) ? num : best;

        class_idx = best - pix_base;
      }

      if (appctx->labels != NULL && class_idx < appctx->labels->len) {
        ClassLabel *label = &g_array_index (appctx->labels, ClassLabel, class_idx);

        if (label->name != NULL) {
          guint outidx = out_row + (column - left);

          g_array_index (segmentation.labels, GQuark, outidx) =
              g_quark_from_string (label->name);
          g_array_index (segmentation.colors, guint32, outidx) = label->color;
        }
      }
    }
  }

  /* gst_ml_segmentations_append() copies the struct by value (plain
   * g_array_append_vals) without taking a new ref on labels/colors, so
   * ownership transfers to "predictions" here; do not unref them locally. */
  gst_ml_segmentations_append (predictions, &segmentation);

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

  labels_path = g_strdup_printf ("%slabels/dv3-argmax-labels.json", model_label_base);
  appctx.labels = load_labels (labels_path);
  g_free (labels_path);

  if (appctx.labels == NULL) {
    g_printerr ("ERROR: Could not load labels, aborting.\n");
    return -1;
  }

  /*
   * Compose the full pipeline string from the selected source branch. The
   * qtimlpostprocess element has no "module" property set, which leaves it
   * in signal/callback mode; the custom postprocess handler is connected
   * below, before the pipeline is started. Segmentation's postprocess output is
   * forced to video/x-raw,RGBA and fed directly into qtivcomposer, which
   * alpha-blends the drawn mask over the display branch.
   */
  gchar *ml_pipe = g_strdup_printf (
    // Use a tee element to pass the video frame sequentially,
    // first to vcomposer, then to mlvconverter.
    "tee name=t ! queue ! mixer. "

    // Preprocess the video for inference
    "t. ! qtimlvconverter name=preprocess image-disposition=stretch ! queue ! "

    // Run inference using the deeplabv3 model
    "qtimltflite name=inference delegate=external "
    "external-delegate-path=libQnnTFLiteDelegate.so "
    "external-delegate-options=\"QNNExternalDelegate,backend_type=htp;\" "
    "model=%smodels/dv3_argmax_int32.tflite ! queue ! "

    // Postprocess inference results with a custom (application-provided)
    // callback instead of a built-in module.
    "qtimlpostprocess name=postprocess ! "
    "video/x-raw,format=RGBA ! mixer. "

    // Use qtivcomposer to overlay segmentation result over the video frame
    "qtivcomposer name=mixer sink_1::alpha=0.5 ! video/x-raw,format=NV12",
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
  g_signal_connect (postprocess, "process-segmentation",
      G_CALLBACK (on_process_segmentation), &appctx);
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
