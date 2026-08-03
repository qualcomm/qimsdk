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
#include <gst/ml/ml-post-process-detection.h>
#include <gst/ml/ml-post-process-pose.h>
#include <gst/ml/ml-post-process-keypoint.h>

/*
 * Description of the pipeline.
 */
#define DESCRIPTION \
  "This application sets up a GStreamer pipeline for multi-stage machine " \
  "learning inference on video streams, performing object detection " \
  "(quantized YOLOv5) followed by pose estimation (HRNet) on each detected " \
  "person, with custom (application-provided) postprocess callbacks for " \
  "both stages instead of the qtimlpostprocess built-in modules. It " \
  "supports various video input sources and output types."

/*
 * Default input branch used by the sample application.
 */
#define DEFAULT_INPUT \
  "filesrc location=%s/Downloads/qimsdk_samples/media/Draw_720p_180s_30FPS.mp4 ! qtdemux ! h264parse ! "\
  "v4l2h264dec capture-io-mode=4 output-io-mode=4 ! video/x-raw,format=NV12"

/*
 * Default output branch used by the sample application.
 */
#define DEFAULT_OUTPUT \
  "display"

/*
 * Minimum confidence (raw fraction, NOT percent) for a detection box or a
 * pose to be reported by the custom postprocess callbacks. Compared
 * directly against raw model scores before they are converted to
 * percentages for the output structs.
 */
#define DETECTION_CONFIDENCE_THRESHOLD 0.70f
#define POSE_CONFIDENCE_THRESHOLD 0.70f

/*
 * Minimum bounding box area (20 x 20 tensor pixels) for a tripleblock
 * detection to be considered valid.
 */
#define BBOX_SIZE_THRESHOLD 400

/* Layer index at which the object score resides, within a paxel's layers. */
#define SCORE_IDX 4
/* Layer index from which the class scores begin, within a paxel's layers. */
#define CLASSES_IDX 5

/*
 * Bounding box weights for each of the 3 tripleblock tensors, used to match
 * a tensor to its corresponding row in the anchors table below.
 */
static const guint weights[3] = { 8, 16, 32 };

/*
 * Bounding box anchor values for each of the 3 tripleblock tensors, used to
 * scale the decoded width/height of a box.
 */
static const guint anchors[3][3][2] = {
  { {10,  13}, {16,   30}, {33,   23} },
  { {30,  61}, {62,   45}, {59,  119} },
  { {116, 90}, {156, 198}, {373, 326} },
};

/*
 * Fixed COCO 17-keypoint skeleton, expressed as keypoint-index pairs. Used
 * to build the per-pose link list once both endpoint keypoints individually
 * clear the confidence threshold.
 */
static const guint edges[][2] = {
  {0, 1}, {0, 2}, {0, 5}, {0, 6},
  {1, 3}, {2, 4},
  {5, 6}, {5, 7}, {5, 11},
  {6, 8}, {6, 12},
  {7, 9}, {8, 10},
  {11, 13}, {12, 14},
  {13, 15}, {14, 16}
};

/*
 * ClassLabel:
 * @name: Human readable class/keypoint name.
 * @color: ARGB/RGBA color associated with the class/keypoint, as used by
 *         overlay rendering.
 *
 * A single entry parsed from a labels JSON file, indexed by class/keypoint
 * id. Reused for both the detection (80 COCO classes) and pose (17 COCO
 * keypoints) label vocabularies.
 */
typedef struct
{
  gchar  *name;
  guint32 color;
} ClassLabel;

/*
 * Application-wide context, extending GstAppContext with the data needed
 * by the custom postprocess callbacks. Two separate label vocabularies are
 * needed since this app decodes two distinct model outputs.
 */
typedef struct
{
  GstAppContext base;

  GArray *detection_labels;
  GArray *pose_labels;
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
 * Resolve label name/color for a class/keypoint index, no-op if out of
 * range or the entry has no name.
 */
static void
lookup_label (GArray * labels, guint index, GQuark * name, guint32 * color)
{
  if (labels == NULL || index >= labels->len)
    return;

  ClassLabel *label = &g_array_index (labels, ClassLabel, index);

  if (label->name != NULL) {
    *name = g_quark_from_string (label->name);
    *color = label->color;
  }
}

/*
 * Decode a single detection candidate into a GstMLDetection and hand it to
 * the library's greedy IoU-based NMS, which appends it (or replaces a
 * lower-confidence same-label conflicting box) directly into @predictions.
 */
static void
emit_detection (GstMLDetections * predictions, GArray * labels,
    gfloat top, gfloat left, gfloat bottom, gfloat right,
    guint class_idx, gfloat confidence)
{
  GstMLDetection entry = { 0, };

  entry.top = top;
  entry.left = left;
  entry.bottom = bottom;
  entry.right = right;
  entry.confidence = confidence * 100.0f;

  lookup_label (labels, class_idx, &entry.name, &entry.color);

  gst_ml_detections_non_max_suppression (predictions, &entry,
      GST_ML_DETECTION_NMS_THRESHOLD);
}

/*
 * Decode a "tripleblock" detection frame: 3 raw-grid tensors (one per
 * stride/anchor-scale), each holding per-pixel-per-anchor box/score/class
 * layers requiring sigmoid activation and anchor-box math.
 */
static void
parse_tripleblock (GstMLFrame * mlframe, GstStructure * mlparam,
    GArray * labels, GstMLDetections * predictions)
{
  GstVideoRectangle region = { 0, };
  guint source_width = 0, source_height = 0;
  guint idx;

  gst_ml_structure_get_source_dimensions (mlparam, &source_width,
      &source_height);
  gst_ml_structure_get_source_region (mlparam, &region);

  for (idx = 0; idx < GST_ML_FRAME_N_TENSORS (mlframe); idx++) {
    GstMLTensor tensor = gst_ml_frame_get_tensor (mlframe, idx);
    const gfloat *data = (const gfloat *) tensor.data;
    guint n_anchors, height, width, n_layers, w_idx, paxelsize;
    guint n_paxels, pxl_idx;

    if (tensor.n_dimensions == 5) {
      n_anchors = tensor.dimensions[1];
      height = tensor.dimensions[2];
      width = tensor.dimensions[3];
      n_layers = tensor.dimensions[4];
    } else {
      n_anchors = 3;
      height = tensor.dimensions[1];
      width = tensor.dimensions[2];
      n_layers = tensor.dimensions[3] / n_anchors;
    }

    n_paxels = width * height;
    paxelsize = width ? (source_width / width) : 0;

    for (w_idx = 0; w_idx < 3; w_idx++)
      if (weights[w_idx] == paxelsize)
        break;
    if (w_idx == 3)
      w_idx = 0;

    for (pxl_idx = 0; pxl_idx < n_paxels; pxl_idx++) {
      guint anchor;
      guint num = pxl_idx * n_anchors * n_layers;

      for (anchor = 0; anchor < n_anchors; anchor++, num += n_layers) {
        gfloat score = data[num + SCORE_IDX];
        guint id, m, class_idx;
        gfloat confidence, bbox[4];
        gfloat top, left, bottom, right;
        guint size;
        guint x, y;

        if (score < DETECTION_CONFIDENCE_THRESHOLD)
          continue;

        id = num + CLASSES_IDX;
        for (m = num + CLASSES_IDX + 1; m < num + n_layers; m++)
          id = (data[m] > data[id]) ? m : id;

        class_idx = id - (num + CLASSES_IDX);
        confidence = data[id];

        if (confidence < DETECTION_CONFIDENCE_THRESHOLD)
          continue;

        /* Sigmoid-normalize both the winning class score and the object
         * score, then combine them into the final confidence. */
        confidence = 1.0f / (1.0f + expf (-confidence));
        confidence *= 1.0f / (1.0f + expf (-score));

        bbox[0] = 1.0f / (1.0f + expf (-data[num]));
        bbox[1] = 1.0f / (1.0f + expf (-data[num + 1]));
        bbox[2] = 1.0f / (1.0f + expf (-data[num + 2]));
        bbox[3] = 1.0f / (1.0f + expf (-data[num + 3]));

        x = pxl_idx % width;
        y = pxl_idx / width;

        bbox[0] = (bbox[0] * 2.0f - 0.5f + x) * paxelsize;
        bbox[1] = (bbox[1] * 2.0f - 0.5f + y) * paxelsize;
        bbox[2] = powf (bbox[2] * 2.0f, 2) * anchors[w_idx][anchor][0];
        bbox[3] = powf (bbox[3] * 2.0f, 2) * anchors[w_idx][anchor][1];

        top = bbox[1] - (bbox[3] / 2.0f);
        left = bbox[0] - (bbox[2] / 2.0f);
        bottom = bbox[1] + (bbox[3] / 2.0f);
        right = bbox[0] + (bbox[2] / 2.0f);

        /* Keep dimensions within the region, in tensor-pixel space. */
        top = CLAMP (top, (gfloat) region.y, (gfloat) (region.y + region.h));
        left = CLAMP (left, (gfloat) region.x, (gfloat) (region.x + region.w));
        bottom = CLAMP (bottom, (gfloat) region.y, (gfloat) (region.y + region.h));
        right = CLAMP (right, (gfloat) region.x, (gfloat) (region.x + region.w));

        if (left == right || top == bottom)
          continue;

        size = (guint) ((right - left) * (bottom - top));
        if (size < BBOX_SIZE_THRESHOLD)
          continue;

        {
          GstMLDetection entry = { 0, };

          entry.top = top;
          entry.left = left;
          entry.bottom = bottom;
          entry.right = right;

          gst_ml_detection_relative_transform (&entry, &region, FALSE);

          emit_detection (predictions, labels, entry.top, entry.left,
              entry.bottom, entry.right, class_idx, confidence);
        }
      }
    }
  }
}

/*
 * Decode a "monoblock" detection frame: a single tensor holding
 * [1, n_paxels, n_layers] already-fractional box coordinates and class
 * scores, no sigmoid/anchor math needed.
 */
static void
parse_monoblock (GstMLFrame * mlframe, GstStructure * mlparam,
    GArray * labels, GstMLDetections * predictions)
{
  GstMLTensor tensor = gst_ml_frame_get_tensor (mlframe, 0);
  const gfloat *data = (const gfloat *) tensor.data;
  GstVideoRectangle region = { 0, };
  guint source_width = 0, source_height = 0;
  guint n_paxels, n_layers, num, idx;

  n_paxels = tensor.dimensions[1];
  n_layers = tensor.dimensions[2];

  gst_ml_structure_get_source_dimensions (mlparam, &source_width,
      &source_height);
  gst_ml_structure_get_source_region (mlparam, &region);

  for (num = 0, idx = 0; num < n_paxels; num++, idx += n_layers) {
    gfloat score = data[idx + SCORE_IDX];
    guint id, m, class_idx;
    gfloat confidence, bbox[4];
    GstMLDetection entry = { 0, };

    if (score < DETECTION_CONFIDENCE_THRESHOLD)
      continue;

    id = idx + CLASSES_IDX;
    for (m = idx + CLASSES_IDX + 1; m < idx + n_layers; m++)
      id = (data[m] > data[id]) ? m : id;

    class_idx = id - (idx + CLASSES_IDX);
    confidence = data[id] * score;

    if (confidence < DETECTION_CONFIDENCE_THRESHOLD)
      continue;

    bbox[0] = data[idx];
    bbox[1] = data[idx + 1];
    bbox[2] = data[idx + 2];
    bbox[3] = data[idx + 3];

    entry.top = (bbox[1] - (bbox[3] / 2.0f)) * source_height;
    entry.left = (bbox[0] - (bbox[2] / 2.0f)) * source_width;
    entry.bottom = (bbox[1] + (bbox[3] / 2.0f)) * source_height;
    entry.right = (bbox[0] + (bbox[2] / 2.0f)) * source_width;

    /* clamp=TRUE: entry.top/left/bottom/right are normalized to [0,1] by
     * this call, so clamping must happen in that same space, not against
     * region bounds which are still in tensor-pixel space. */
    gst_ml_detection_relative_transform (&entry, &region, TRUE);

    emit_detection (predictions, labels, entry.top, entry.left, entry.bottom,
        entry.right, class_idx, confidence);
  }
}

/*
 * Custom postprocess callback for the "process-object-detection" action
 * signal on the detection-stage qtimlpostprocess.
 *
 * Dispatches to a tripleblock or monoblock decode path based on the number
 * of tensors in the frame, since the on-device YOLOv5 model's output layout
 * depends on export configuration.
 *
 * @postprocess: the qtimlpostprocess GstElement emitting the signal.
 * @mlframe: input tensors for this inference batch.
 * @mlparam: additional per-batch parameters, including the source region.
 * @predictions: pre-allocated output list to be filled by this callback.
 * @userdata: pointer to the AppContext.
 *
 * Returns: TRUE on success, FALSE on a hard failure (e.g. unsupported
 *          tensor layout).
 */
static gboolean
on_process_object_detection (GstElement * postprocess,
    GstMLFrame * mlframe, GstStructure * mlparam,
    GstMLDetections * predictions, gpointer userdata)
{
  AppContext *appctx = (AppContext *) userdata;
  guint n_tensors = GST_ML_FRAME_N_TENSORS (mlframe);

  (void) postprocess;

  if (n_tensors == 3) {
    parse_tripleblock (mlframe, mlparam, appctx->detection_labels, predictions);
  } else if (n_tensors == 1) {
    parse_monoblock (mlframe, mlparam, appctx->detection_labels, predictions);
  } else {
    g_printerr ("ERROR: Detection callback got unsupported tensor count: %u\n",
        n_tensors);
    return FALSE;
  }

  return TRUE;
}

/*
 * Custom postprocess callback for the "process-pose-estimation" action
 * signal on the pose-stage qtimlpostprocess.
 *
 * Decodes a single-person HRNet heatmap tensor (NHWC [1,H,W,K]) via
 * per-keypoint global argmax plus a +/-0.25 sub-pixel offset based on the
 * sign of the finite-difference gradient between neighboring pixels,
 * remaps keypoint coordinates into region-relative [0,1] space via the
 * library's gst_ml_keypoint_relative_transform(), and emits at most one
 * GstMLPose whose mean keypoint confidence clears the threshold.
 *
 * @postprocess: the qtimlpostprocess GstElement emitting the signal.
 * @mlframe: input tensors for this inference batch (single heatmap tensor).
 * @mlparam: additional per-batch parameters, including source dimensions
 *           and region.
 * @predictions: pre-allocated output list to be filled by this callback.
 * @userdata: pointer to the AppContext.
 *
 * Returns: TRUE on success (including the below-threshold no-op case),
 *          FALSE on a hard failure.
 */
static gboolean
on_process_pose_estimation (GstElement * postprocess,
    GstMLFrame * mlframe, GstStructure * mlparam,
    GstMLPoses * predictions, gpointer userdata)
{
  AppContext *appctx = (AppContext *) userdata;
  GstMLTensor tensor = { 0, };
  GstVideoRectangle region = { 0, };
  GstMLPose pose = { 0, };
  const gfloat *heatmap = NULL;
  guint tensor_width = 0, tensor_height = 0;
  guint H, W, K, k;
  gfloat total_confidence = 0.0f;
  guint e;

  (void) postprocess;

  if (GST_ML_FRAME_N_TENSORS (mlframe) == 0) {
    g_printerr ("ERROR: Pose callback received no tensors!\n");
    return FALSE;
  }

  tensor = gst_ml_frame_get_tensor (mlframe, 0);
  if (tensor.type != GST_ML_TYPE_FLOAT32 || tensor.data == NULL ||
      tensor.n_dimensions != 4 || tensor.dimensions[0] != 1) {
    g_printerr ("ERROR: Pose callback expected a [1,H,W,K] float32 tensor!\n");
    return FALSE;
  }

  H = tensor.dimensions[1];
  W = tensor.dimensions[2];
  K = tensor.dimensions[3];
  heatmap = (const gfloat *) tensor.data;

  gst_ml_structure_get_source_region (mlparam, &region);
  gst_ml_structure_get_source_dimensions (mlparam, &tensor_width,
      &tensor_height);

  /* gst_ml_pose_to_structure() requires a non-empty structure name; an unset
   * (quark 0) name makes it fail and return NULL, corrupting the shared
   * GValue used for the "connections" array and triggering GStreamer
   * criticals for every subsequent keypoint/link append. */
  pose.name = g_quark_from_string ("pose");
  pose.keypoints = g_array_sized_new (FALSE, TRUE, sizeof (GstMLKeypoint), K);
  pose.links = g_array_new (FALSE, TRUE, sizeof (GstMLKeypointLink));

  for (k = 0; k < K; k++) {
    gfloat best_val = -G_MAXFLOAT;
    guint best_x = 0, best_y = 0;
    gfloat x, y;
    guint row, col;
    GstMLKeypoint kp = { 0, };

    for (row = 0; row < H; row++) {
      for (col = 0; col < W; col++) {
        gfloat val = heatmap[(row * W * K) + (col * K) + k];

        if (val > best_val) {
          best_val = val;
          best_x = col;
          best_y = row;
        }
      }
    }

    x = (gfloat) best_x;
    y = (gfloat) best_y;

    if (best_x > 0 && best_x < (W - 1) && best_y > 0 && best_y < (H - 1)) {
      gfloat dx = heatmap[(best_y * W * K) + ((best_x + 1) * K) + k] -
          heatmap[(best_y * W * K) + ((best_x - 1) * K) + k];
      gfloat dy = heatmap[((best_y + 1) * W * K) + (best_x * K) + k] -
          heatmap[((best_y - 1) * W * K) + (best_x * K) + k];

      x += (dx > 0.0f) ? 0.25f : -0.25f;
      y += (dy > 0.0f) ? 0.25f : -0.25f;
    }

    kp.x = x * ((gfloat) tensor_width / (gfloat) W);
    kp.y = y * ((gfloat) tensor_height / (gfloat) H);
    kp.confidence = best_val;

    lookup_label (appctx->pose_labels, k, &kp.name, &kp.color);

    gst_ml_keypoint_relative_transform (&kp, &region, TRUE);

    g_array_append_val (pose.keypoints, kp);
    total_confidence += kp.confidence;
  }

  pose.confidence = total_confidence / (gfloat) K;

  if (pose.confidence < POSE_CONFIDENCE_THRESHOLD) {
    /* Below-confidence pose: silent no-op, not a hard failure. */
    g_array_unref (pose.keypoints);
    g_array_unref (pose.links);
    return TRUE;
  }

  for (e = 0; e < G_N_ELEMENTS (edges); e++) {
    if (edges[e][0] >= K || edges[e][1] >= K)
      continue;

    GstMLKeypoint *a = &g_array_index (pose.keypoints, GstMLKeypoint, edges[e][0]);
    GstMLKeypoint *b = &g_array_index (pose.keypoints, GstMLKeypoint, edges[e][1]);

    if (a->confidence >= POSE_CONFIDENCE_THRESHOLD &&
        b->confidence >= POSE_CONFIDENCE_THRESHOLD) {
      GstMLKeypointLink link = { *a, *b, 0 };
      g_array_append_val (pose.links, link);
    }
  }

  /* gst_ml_poses_append() copies the struct by value (plain
   * g_array_append_vals) without taking a new ref on keypoints/links, so
   * ownership transfers to "predictions" here; do not unref them locally. */
  gst_ml_poses_append (predictions, &pose);

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
  GstElement *detection_postprocess = NULL, *pose_postprocess = NULL;
  GError *error = NULL;
  const gchar *home = g_getenv ("HOME");
  gchar *model_base_path = g_strdup_printf ("%s/Downloads/qimsdk_samples/", home);
  gchar *source = g_strdup_printf (DEFAULT_INPUT, home);
  gchar *model_label_base = NULL;
  gchar *detection_labels_path = NULL, *pose_labels_path = NULL;
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

  detection_labels_path = g_strdup_printf ("%slabels/yolov5m.json", model_label_base);
  appctx.detection_labels = load_labels (detection_labels_path);
  g_free (detection_labels_path);

  pose_labels_path = g_strdup_printf ("%slabels/hrnet.json", model_label_base);
  appctx.pose_labels = load_labels (pose_labels_path);
  g_free (pose_labels_path);

  if (appctx.detection_labels == NULL || appctx.pose_labels == NULL) {
    g_printerr ("ERROR: Could not load labels, aborting.\n");
    return -1;
  }

  /*
   * Compose the full pipeline string from the selected source branch. Both
   * qtimlpostprocess elements have no "module" property set, which leaves
   * them in signal/callback mode; the custom postprocess handlers are
   * connected below, before the pipeline is started. The second
   * qtimlvconverter uses mode=roi-batch-cumulative to crop/batch one tensor
   * per detected person from stage 1's metadata, feeding stage 2 (HRNet).
   */
  gchar *ml_pipe = g_strdup_printf (
    // Use a tee element to pass the video frame sequentially,
    // first to preprocessing, then to metamux_1.
    "tee name=t1 "

    // Stage 1: Preprocess the video for inference
    "t1. ! qtimlvconverter name=detection-preprocess ! queue ! "

    // Run inference using the yolov5 model to detect people in the video
    "qtimltflite name=detection-inference delegate=external "
    "external-delegate-path=libQnnTFLiteDelegate.so "
    "external-delegate-options=\"QNNExternalDelegate,backend_type=htp;\" "
    "model=%smodels/yolov5m-320x320-int8.tflite ! queue ! "

    // Postprocess inference results with a custom (application-provided)
    // callback instead of a built-in module.
    "qtimlpostprocess name=detection-postprocess results=8 ! "
    "text/x-raw ! metamux_1. "

    // Attach ML result to video frame
    "t1. ! qtimetamux name=metamux_1 ! queue ! "

    // Use a tee element to pass the video frame sequentially,
    // first to preprocessing, then to metamux_2.
    "tee name=t2 "

    // Stage 2: Preprocessing generates a cropped/batched tensor for each
    // detected ROI from Stage 1
    "t2. ! qtimlvconverter name=pose-preprocess mode=roi-batch-cumulative "
    "image-disposition=centre ! queue ! "

    // Run inference using the hrnet model
    "qtimltflite name=pose-inference delegate=external "
    "external-delegate-path=libQnnTFLiteDelegate.so "
    "external-delegate-options=\"QNNExternalDelegate,backend_type=htp,htp_performance_mode=(string)2;\" "
    "model=%smodels/hrnet_pose_w8a8.tflite ! queue ! "

    // Postprocess inference results with a custom (application-provided)
    // callback instead of a built-in module.
    "qtimlpostprocess name=pose-postprocess results=2 ! "
    "text/x-raw ! metamux_2. "

    // Attach ML result to video frame
    "t2. ! qtimetamux name=metamux_2 ! "

    // Overlay ML result on top of video frame
    "qtivoverlay",
    model_label_base,
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
   * Find the two named postprocess elements and connect the custom
   * postprocess callbacks. Leaving the "module" property unset on both
   * means each qtimlpostprocess will use the signal/callback dispatch path
   * once a handler is connected.
   */
  detection_postprocess =
      gst_bin_get_by_name (GST_BIN (appctx.base.pipeline), "detection-postprocess");
  if (detection_postprocess == NULL) {
    g_printerr ("ERROR: Failed to find detection-postprocess element.\n");
    return -1;
  }
  g_signal_connect (detection_postprocess, "process-object-detection",
      G_CALLBACK (on_process_object_detection), &appctx);
  gst_object_unref (detection_postprocess);

  pose_postprocess =
      gst_bin_get_by_name (GST_BIN (appctx.base.pipeline), "pose-postprocess");
  if (pose_postprocess == NULL) {
    g_printerr ("ERROR: Failed to find pose-postprocess element.\n");
    return -1;
  }
  g_signal_connect (pose_postprocess, "process-pose-estimation",
      G_CALLBACK (on_process_pose_estimation), &appctx);
  gst_object_unref (pose_postprocess);

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

  if (appctx.detection_labels != NULL)
    g_array_free (appctx.detection_labels, TRUE);
  if (appctx.pose_labels != NULL)
    g_array_free (appctx.pose_labels, TRUE);

  g_free (source);
  g_free (model_base_path);
  g_free (model_label_base);

  /* Deinitialize GStreamer after all GStreamer objects have been released. */
  gst_deinit ();

  return 0;
}
