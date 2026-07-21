/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include "ml-post-process-utils.h"

#include <cairo/cairo-ft.h>
#include <fontconfig/fontconfig.h>

GST_DEBUG_CATEGORY_EXTERN (gst_ml_post_process_debug);
#define GST_CAT_DEFAULT gst_ml_post_process_debug

// Monospaced font, its glyph advance matches the (length * fontsize * 3 / 5)
// label width estimation used for the label background rectangles.
#define DEFAULT_FONT_FAMILY    "Liberation Mono"

static cairo_font_face_t *
gst_cairo_get_font_face (void)
{
  static cairo_font_face_t *fontface = NULL;
  static gsize inited = 0;
  FcPattern *pattern = NULL, *match = NULL;
  FcChar8 *filename = NULL;
  FcResult result = FcResultNoMatch;

  if (!g_once_init_enter (&inited))
    return fontface;

  pattern = FcPatternBuild (NULL, FC_FAMILY, FcTypeString, DEFAULT_FONT_FAMILY,
      FC_WEIGHT, FcTypeInteger, FC_WEIGHT_BOLD, NULL);

  if (pattern == NULL) {
    GST_ERROR ("Failed to create font pattern for '%s'!", DEFAULT_FONT_FAMILY);
    goto cleanup;
  }

  FcConfigSubstitute (NULL, pattern, FcMatchPattern);
  FcDefaultSubstitute (pattern);

  match = FcFontMatch (NULL, pattern, &result);
  FcPatternDestroy (pattern);

  if (match == NULL) {
    GST_ERROR ("No font found for '%s'!", DEFAULT_FONT_FAMILY);
    goto cleanup;
  }

  FcPatternGetString (match, FC_FILE, 0, &filename);
  GST_INFO ("Using font '%s' for family '%s'", filename, DEFAULT_FONT_FAMILY);

  // Explicitly resolved FreeType font face, avoids the fallback to Cairo's
  // built-in user font which is not thread safe on scaled font creation.
  fontface = cairo_ft_font_face_create_for_pattern (match);
  FcPatternDestroy (match);

  if (cairo_font_face_status (fontface) != CAIRO_STATUS_SUCCESS) {
    GST_ERROR ("Failed to create font face for '%s'!", DEFAULT_FONT_FAMILY);
    cairo_font_face_destroy (fontface);
    fontface = NULL;
  }

cleanup:
  g_once_init_leave (&inited, 1);
  return fontface;
}

void
gst_value_array_append_and_take_ml_structure (GValue * array, const guint id,
    GstStructure * structure)
{
  GValue value = G_VALUE_INIT;

  gst_structure_set (structure, "id", G_TYPE_UINT, id, NULL);

  g_value_init (&value, GST_TYPE_STRUCTURE);
  g_value_take_boxed (&value, structure);

  gst_value_array_append_and_take_value (array, &value);
}

void
gst_ml_predictions_list_append (GValue * list, const GQuark mltype,
    GValue * results, const GstStructure * mlparam)
{
  GstStructure *structure = NULL;
  GValue entry = G_VALUE_INIT;

  if (GST_IS_AUDIO_CLASSIFICATION (mltype)) {
    structure = gst_structure_new_empty ("AudioClassification");
    gst_structure_take_value (structure, "labels", results);
  } else if (GST_IS_IMAGE_CLASSIFICATION (mltype)) {
    structure = gst_structure_new_empty ("ImageClassification");
    gst_structure_take_value (structure, "labels", results);
  } else if (GST_IS_DETECTION (mltype)) {
    structure = gst_structure_new_empty ("ObjectDetection");
    gst_structure_take_value (structure, "bounding-boxes", results);
  } else if (GST_IS_POSE (mltype)) {
    structure = gst_structure_new_empty ("PoseEstimation");
    gst_structure_take_value (structure, "poses", results);
  } else if (GST_IS_SEGMENTATION (mltype)) {
    structure = gst_structure_new_empty ("Segmentation");
    gst_structure_take_value (structure, "masks", results);
  } else if (GST_IS_DEPTH_MAP (mltype)) {
    structure = gst_structure_new_empty ("Depth");
    gst_structure_take_value (structure, "maps", results);
  } else if (GST_IS_EMBEDDINGS (mltype)) {
    structure = gst_structure_new_empty ("Embeddings");
    gst_structure_take_value (structure, "vectors", results);
  }

  gst_ml_params_copy_service_info (structure, mlparam);

  g_value_init (&entry, GST_TYPE_STRUCTURE);
  g_value_take_boxed (&entry, structure);

  gst_value_list_append_and_take_value (list, &entry);
}

void
gst_ml_params_copy_service_info (GstStructure * structure,
    const GstStructure * mlparam)
{
  const GValue *value = NULL;

  if ((value = gst_structure_get_value (mlparam, "timestamp")))
    gst_structure_set_value (structure, "timestamp", value);

  if ((value = gst_structure_get_value (mlparam, "sequence-index")))
    gst_structure_set_value (structure, "sequence-index", value);

  if ((value = gst_structure_get_value (mlparam, "sequence-num-entries")))
    gst_structure_set_value (structure, "sequence-num-entries", value);

  if ((value = gst_structure_get_value (mlparam, "stream-id")))
    gst_structure_set_value (structure, "stream-id", value);

  if ((value = gst_structure_get_value (mlparam, "stream-timestamp")))
    gst_structure_set_value (structure, "stream-timestamp", value);

  if ((value = gst_structure_get_value (mlparam, "parent-id")))
    gst_structure_set_value (structure, "parent-id", value);

  if ((value = gst_structure_get_value (mlparam, "parent-label")))
    gst_structure_set_value (structure, "parent-label", value);
}

gboolean
gst_buffer_serialize_and_take_value (GstBuffer * buffer, GValue * value)
{
  GstMemory *memory = NULL;
  gchar *string = NULL;
  guint length = 0;

  // Serialize the predictions list into string format.
  string = gst_value_serialize (value);
  g_value_unset (value);

  if (string == NULL) {
    GST_ERROR ("Failed serialize predictions structure!");
    return FALSE;
  }

  // Increase the length by 1 byte for the '\0' character.
  length = strlen (string) + 1;

  memory = gst_memory_new_wrapped (0, string, length, 0, length, string, g_free);
  gst_buffer_append_memory (buffer, memory);

  return TRUE;
}

gboolean
gst_cairo_draw_setup (GstVideoFrame * vframe, cairo_surface_t ** surface,
    cairo_t ** context)
{
  cairo_font_face_t *fontface = NULL;
  cairo_font_options_t *options = NULL;
  cairo_format_t format;
  gdouble fontsize = DEFAULT_FONT_SIZE, linewidth = 2.0F;

  // The default font face may resolve to Cairo's built-in user font which is
  // not thread safe on scaled font creation, never draw without own font.
  if ((fontface = gst_cairo_get_font_face ()) == NULL) {
    GST_ERROR ("No font face available!");
    return FALSE;
  }

  switch (GST_VIDEO_FRAME_FORMAT (vframe)) {
    case GST_VIDEO_FORMAT_BGRA:
    case GST_VIDEO_FORMAT_RGBA:
      format = CAIRO_FORMAT_ARGB32;
      break;
    case GST_VIDEO_FORMAT_BGRx:
    case GST_VIDEO_FORMAT_RGBx:
      format = CAIRO_FORMAT_RGB24;
      break;
    case GST_VIDEO_FORMAT_BGR16:
    case GST_VIDEO_FORMAT_RGB16:
      format = CAIRO_FORMAT_RGB16_565;
      break;
    default:
      GST_ERROR ("Unsupported format: %s!",
          gst_video_format_to_string (GST_VIDEO_FRAME_FORMAT (vframe)));
      return FALSE;
  }

  *surface = cairo_image_surface_create_for_data (
      GST_VIDEO_FRAME_PLANE_DATA (vframe, 0), format,
      GST_VIDEO_FRAME_WIDTH (vframe), GST_VIDEO_FRAME_HEIGHT (vframe),
      GST_VIDEO_FRAME_PLANE_STRIDE (vframe, 0));
  g_return_val_if_fail (*surface, FALSE);

  *context = cairo_create (*surface);
  g_return_val_if_fail (*context, FALSE);

  // Set font.
  cairo_set_font_face (*context, fontface);
  cairo_set_antialias (*context, CAIRO_ANTIALIAS_BEST);

  // Set font options, BEST maps to LCD subpixel glyphs for FreeType fonts.
  options = cairo_font_options_create ();
  cairo_font_options_set_antialias (options, CAIRO_ANTIALIAS_GRAY);

  cairo_set_font_options (*context, options);
  cairo_font_options_destroy (options);

  cairo_set_line_width (*context, linewidth);
  cairo_set_font_size (*context, fontsize);

  // Clear any leftovers from previous operations.
  cairo_set_operator (*context, CAIRO_OPERATOR_CLEAR);
  cairo_paint (*context);
  // Flush to ensure all writing to the surface has been done.
  cairo_surface_flush (*surface);

  // Set operator to draw over the source.
  cairo_set_operator (*context, CAIRO_OPERATOR_OVER);
  // Mark the surface dirty so Cairo clears its caches.
  cairo_surface_mark_dirty (*surface);

  return TRUE;
}

void
gst_cairo_draw_cleanup (cairo_surface_t * surface, cairo_t * context)
{
  // Flush to ensure all writing to the surface has been done.
  cairo_surface_flush (surface);

  cairo_destroy (context);
  cairo_surface_destroy (surface);
}

gboolean
gst_cairo_draw_label (cairo_t * context, const guint index,
    const gchar * label, const guint32 color)
{
  cairo_matrix_t matrix;
  guint length = 0, textcolor = 0;
  gdouble fontsize = 0;

  // Set text background color.
  cairo_set_source_rgba (context, GST_FLOAT_COLOR_BLUE (color),
      GST_FLOAT_COLOR_GREEN (color), GST_FLOAT_COLOR_RED (color),
      GST_FLOAT_COLOR_ALPHA (color));

  cairo_get_font_matrix (context, &matrix);
  fontsize = matrix.yy;

  length = strlen (label) * fontsize * 3.0F / 5.0F;

  cairo_rectangle (context, 0, (index * fontsize), length, fontsize);
  cairo_fill (context);

  // Choose the best contrasting text color to the background.
  textcolor = GST_COLOR_ALPHA (color);
  textcolor += ((GST_COLOR_BLUE (color) > 0x7F) ? 0x00 : 0xFF) << 8;
  textcolor += ((GST_COLOR_GREEN (color) > 0x7F) ? 0x00 : 0xFF) << 16;
  textcolor += ((GST_COLOR_RED (color) > 0x7F) ? 0x00 : 0xFF) << 24;

  cairo_set_source_rgba (context, GST_FLOAT_COLOR_BLUE (textcolor),
      GST_FLOAT_COLOR_GREEN (textcolor), GST_FLOAT_COLOR_RED (textcolor),
      GST_FLOAT_COLOR_ALPHA (textcolor));

  // (0,0) is at top left corner of the buffer and draw the string.
  cairo_move_to (context, 0, ((index + 1) * fontsize * 4.0F / 5.0F));
  cairo_show_text (context, label);

  return (cairo_status (context) == CAIRO_STATUS_SUCCESS) ? TRUE : FALSE;
}

gboolean
gst_cairo_draw_detection (cairo_t * context, GstMLDetection * detection,
    GstVideoRegionOfInterestMeta * roimeta, gdouble matrix[3][3])
{
  cairo_matrix_t cairomatrix;
  const gchar *label = g_quark_to_string (detection->name);
  GstVideoQuadrilateral corners = {};
  gdouble fontsize = 0, x = 0, y = 0, width = 0, height = 0;
  guint32 textcolor = 0;

  corners.a.x = corners.b.x = detection->left;
  corners.c.x = corners.d.x = detection->right;
  corners.a.y = corners.c.y = detection->top;
  corners.b.y = corners.d.y = detection->bottom;

  if (matrix != NULL) {
    gst_video_point_affine_transform (&(corners.a), matrix);
    gst_video_point_affine_transform (&(corners.b), matrix);
    gst_video_point_affine_transform (&(corners.c), matrix);
    gst_video_point_affine_transform (&(corners.d), matrix);
  }

  corners.a.x = roimeta->x + (corners.a.x * roimeta->w);
  corners.a.y = roimeta->y + (corners.a.y * roimeta->h);
  corners.b.x = roimeta->x + (corners.b.x * roimeta->w);
  corners.b.y = roimeta->y + (corners.b.y * roimeta->h);
  corners.c.x = roimeta->x + (corners.c.x * roimeta->w);
  corners.c.y = roimeta->y + (corners.c.y * roimeta->h);
  corners.d.x = roimeta->x + (corners.d.x * roimeta->w);
  corners.d.y = roimeta->y + (corners.d.y * roimeta->h);

  cairo_set_source_rgba (context, GST_FLOAT_COLOR_BLUE (detection->color),
      GST_FLOAT_COLOR_GREEN (detection->color),
      GST_FLOAT_COLOR_RED (detection->color),
      GST_FLOAT_COLOR_ALPHA (detection->color));

  // Draw the (possibly transformed) box as its 4 edges: a-c, c-d, d-b, b-a.
  cairo_move_to (context, corners.a.x, corners.a.y);

  cairo_line_to (context, corners.c.x, corners.c.y);
  cairo_line_to (context, corners.d.x, corners.d.y);
  cairo_line_to (context, corners.b.x, corners.b.y);
  cairo_line_to (context, corners.a.x, corners.a.y);

  cairo_stroke (context);
  g_return_val_if_fail (CAIRO_STATUS_SUCCESS == cairo_status (context), FALSE);

  GST_TRACE ("Object: '%s' Confidence %.2f A(%f, %f) B(%f, %f) C(%f, %f) "
      "D(%f, %f)", label, detection->confidence, corners.a.x, corners.a.y,
      corners.b.x, corners.b.y, corners.c.x, corners.c.y, corners.d.x, corners.d.y);

  cairo_get_font_matrix (context, &cairomatrix);
  fontsize = cairomatrix.yy;

  // Set the width and height of the label background rectangle.
  width = strlen (label) * fontsize * 3.0F / 5.0F;
  height = fontsize;

  // Calculate the X and Y position of the label.
  x = roimeta->x + (CLAMP (detection->left, 0.0f, 1.0f) * roimeta->w);
  y = roimeta->y + (CLAMP (detection->top, 0.0f, 1.0f) * roimeta->h);

  if ((y -= height) < 0.0)
    y = roimeta->y + roimeta->h;

  if ((x + width - 1) > (gdouble) roimeta->w)
    x = roimeta->x + roimeta->w - width;

  cairo_rectangle (context, (x - 1), y, width, height);
  cairo_fill (context);

  // Choose the best contrasting text color to the background.
  textcolor = GST_COLOR_ALPHA (detection->color);
  textcolor += ((GST_COLOR_BLUE (detection->color) > 0x7F) ? 0x00 : 0xFF) << 8;
  textcolor += ((GST_COLOR_GREEN (detection->color) > 0x7F) ? 0x00 : 0xFF) << 16;
  textcolor += ((GST_COLOR_RED (detection->color) > 0x7F) ? 0x00 : 0xFF) << 24;

  cairo_set_source_rgba (context, GST_FLOAT_COLOR_BLUE (textcolor),
      GST_FLOAT_COLOR_GREEN (textcolor), GST_FLOAT_COLOR_RED (textcolor),
      GST_FLOAT_COLOR_ALPHA (textcolor));

  // Set the starting position of the label text and draw the string.
  cairo_move_to (context, x, (y + (fontsize * 4.0F / 5.0F)));
  cairo_show_text (context, label);

  return (cairo_status (context) == CAIRO_STATUS_SUCCESS) ? TRUE : FALSE;
}

gboolean
gst_cairo_draw_keypoint (cairo_t * context, GstMLKeypoint * keypoint,
    GstVideoRegionOfInterestMeta * roimeta, gdouble matrix[3][3])
{
  GstVideoPoint point = {keypoint->x, keypoint->y};
  gdouble linewidth = cairo_get_line_width (context);

  if (matrix != NULL)
    gst_video_point_affine_transform (&point, matrix);

  // Adjust coordinates based on the output buffer dimensions.
  point.x = roimeta->x + (point.x * roimeta->w);
  point.y = roimeta->y + (point.y * roimeta->h);

  cairo_set_source_rgba (context, GST_FLOAT_COLOR_BLUE (keypoint->color),
      GST_FLOAT_COLOR_GREEN (keypoint->color),
      GST_FLOAT_COLOR_RED (keypoint->color),
      GST_FLOAT_COLOR_ALPHA (keypoint->color));

  GST_TRACE ("Keypoint: '%s' [%.1f %.1f] Confidence %.2f",
      g_quark_to_string (keypoint->name), point.x, point.y, keypoint->confidence);

  // Set circle position and dimensions.
  cairo_arc (context, point.x, point.y, linewidth, 0, 2 * G_PI);
  cairo_fill (context);

  return (cairo_status (context) == CAIRO_STATUS_SUCCESS) ? TRUE : FALSE;
}

gboolean
gst_cairo_draw_link (cairo_t * context, GstMLKeypointLink * link,
    GstVideoRegionOfInterestMeta * roimeta, gdouble matrix[3][3])
{
  GstVideoPoint l_point = {link->l_kp.x, link->l_kp.y};
  GstVideoPoint r_point = {link->r_kp.x, link->r_kp.y};

  if (matrix != NULL) {
    gst_video_point_affine_transform (&l_point, matrix);
    gst_video_point_affine_transform (&r_point, matrix);
  }

  // Adjust coordinates based on the output buffer dimensions.
  l_point.x = roimeta->x + (l_point.x * roimeta->w);
  l_point.y = roimeta->y + (l_point.y * roimeta->h);

  r_point.x = roimeta->x + (r_point.x * roimeta->w);
  r_point.y = roimeta->y + (r_point.y * roimeta->h);

  cairo_set_source_rgba (context, GST_FLOAT_COLOR_BLUE (link->color),
      GST_FLOAT_COLOR_GREEN (link->color), GST_FLOAT_COLOR_RED (link->color),
      GST_FLOAT_COLOR_ALPHA (link->color));

  GST_TRACE ("Link: '%s' [%.1f x %.2f] <--> '%s' [%.1f x %.1f]",
      g_quark_to_string (link->l_kp.name), l_point.x, l_point.y,
      g_quark_to_string (link->r_kp.name), r_point.x, r_point.y);

  cairo_move_to (context, l_point.x, l_point.y);
  cairo_line_to (context, r_point.x, r_point.y);
  cairo_stroke (context);

  return (cairo_status (context) == CAIRO_STATUS_SUCCESS) ? TRUE : FALSE;
}

gboolean
gst_cairo_draw_mask (cairo_t * context, guint32 * colormask,
    guint n_rows, guint n_columns, GstVideoRegionOfInterestMeta * roimeta,
    gdouble matrix[3][3])
{
  cairo_surface_t *surface = NULL;
  guint idx = 0, n_pixels = n_rows * n_columns;

  GST_TRACE ("Mask [%u, %u] Region [%u %u %u %u]", n_columns, n_rows,
      roimeta->x, roimeta->y, roimeta->w, roimeta->h);

  // The colormask is stored as RGBA, while cairo's ARGB32 surface format.
  for (idx = 0; idx < n_pixels; idx++)
    colormask[idx] = GST_COLOR_TO_ABGR (colormask[idx]);

  surface = cairo_image_surface_create_for_data ((guchar*) colormask,
      CAIRO_FORMAT_ARGB32, n_columns, n_rows, (n_columns * 4));

  g_return_val_if_fail (
      CAIRO_STATUS_SUCCESS == cairo_surface_status (surface), FALSE);

  cairo_save (context);

  if (matrix != NULL) {
    cairo_matrix_t cairomatrix;
    gdouble scale[3][3] = {0}, normalize[3][3] = {0};
    gdouble intermediary[3][3] = {0}, combined[3][3] = {0};

    // Normalize native mask pixel indices into the mask's own [0,1] box space.
    normalize[0][0] = 1.0 / n_columns;
    normalize[1][1] = 1.0 / n_rows;
    normalize[2][2] = 1;

    // Scale/translate the [0,1] box space into absolute roimeta pixel space.
    scale[0][0] = roimeta->w;
    scale[1][1] = roimeta->h;
    scale[0][2] = roimeta->x;
    scale[1][2] = roimeta->y;
    scale[2][2] = 1;

    gst_matrix_multiply (&matrix[0][0], &normalize[0][0], 9, &intermediary[0][0]);
    gst_matrix_multiply (&scale[0][0], &intermediary[0][0], 9, &combined[0][0]);

    // Map the 3x3 affine array to a cairo_matrix_t and apply it.
    cairo_matrix_init (&cairomatrix,
        combined[0][0], combined[1][0], combined[0][1],
        combined[1][1], combined[0][2], combined[1][2]
    );

    cairo_transform (context, &cairomatrix);
  } else {
    // Place the mask directly at the ROI, scaling its native resolution to fit.
    cairo_translate (context, roimeta->x, roimeta->y);
    cairo_scale (context, (gdouble) roimeta->w / n_columns,
        (gdouble) roimeta->h / n_rows);
  }

  cairo_set_source_surface (context, surface, 0, 0);
  cairo_pattern_set_filter (cairo_get_source (context), CAIRO_FILTER_BILINEAR);
  cairo_pattern_set_extend (cairo_get_source (context), CAIRO_EXTEND_NONE);
  cairo_paint (context);

  cairo_restore (context);
  cairo_surface_destroy (surface);

  return (cairo_status (context) == CAIRO_STATUS_SUCCESS) ? TRUE : FALSE;
}

GstVideoRegionOfInterestMeta *
gst_buffer_setup_image_region (GstBuffer * buffer, const GstStructure * mlparam)
{
  GstVideoMeta *vmeta = gst_buffer_get_video_meta (buffer);
  GstVideoRectangle region = { 0, };

  // Extract the source tensor region with actual data.
  gst_ml_structure_get_source_region (mlparam, &region);

  // Update the image region then add it to the buffer.
  if ((region.w * vmeta->height) > (region.h * vmeta->width)) {
    region.h = gst_util_uint64_scale_int (vmeta->width, region.h, region.w);
    region.w = vmeta->width;
  } else if ((region.w * vmeta->height) < (region.h * vmeta->width)) {
    region.w = gst_util_uint64_scale_int (vmeta->height, region.w, region.h);
    region.h = vmeta->height;
  } else {
    region.w = vmeta->width;
    region.h = vmeta->height;
  }

  GST_TRACE ("Region [%d %d %d %d]", region.x, region.y, region.w, region.h);

  // Add ROI meta with the actual part of the buffer filled with image data.
  return gst_buffer_add_video_region_of_interest_meta (buffer, "ImageRegion",
      region.x, region.y, region.w, region.h);
}
