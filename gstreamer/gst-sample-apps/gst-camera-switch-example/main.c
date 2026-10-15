/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

/*
 * GStreamer dual-camera switch example.
 *
 * Both qtiqmmfsrc instances remain active. Camera switching is performed
 * by changing the alpha property of the qtivcomposer input pads.
 *
 * Output modes:
 *   -d 1 : display only (default)
 *   -d 0 : preview and encode to MP4
 */

#include <glib-unix.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <sys/stat.h>

#include <gst/gst.h>

#include <gst_sample_apps_utils.h>

#define GST_APP_SUMMARY \
  "Switch two cameras using qtivcomposer.\n" \
  "-d 1: display only (default)\n" \
  "-d 0: preview and encode\n"

#define DEFAULT_WIDTH 1280
#define DEFAULT_HEIGHT 720
#define DEFAULT_FPS_NUM 30
#define DEFAULT_FPS_DEN 1
#define SWITCH_INTERVAL_SECONDS 5
#define OUTPUT_FILE_NAME "mux.mp4"

typedef struct {
  GstElement *pipeline;
  GstElement *camsrc0;
  GstElement *camsrc1;
  GstElement *capsfilter0;
  GstElement *capsfilter1;
  GstElement *queue0;
  GstElement *queue1;
  GstElement *composer;
  GstElement *waylandsink;
  GstElement *tee;
  GstElement *output_capsfilter;
  GstElement *preview_queue;
  GstElement *record_queue;
  GstElement *encoder;
  GstElement *h264parse;
  GstElement *mp4mux;
  GstElement *filesink;
  GstPad *composer_sink0;
  GstPad *composer_sink1;
  GMainLoop *main_loop;
  guint camera0_id;
  guint camera1_id;
  guint use_display;
  guint camera0_width;
  guint camera0_height;
  guint camera1_width;
  guint camera1_height;
  guint output_width;
  guint output_height;
  gboolean camera0_active;
  guint switch_source_id;
  guint signal_source_id;
  gboolean shutting_down;
} GstCameraSwitchCtx;

static void
build_pad_property (GValue *property, gint values[], gint count)
{
  GValue value = G_VALUE_INIT;
  g_value_init (&value, G_TYPE_INT);

  for (gint index = 0; index < count; index++) {
    g_value_set_int (&value, values[index]);
    gst_value_array_append_value (property, &value);
  }

  g_value_unset (&value);
}

static gboolean
configure_composer_pad (GstPad *pad, gint x, gint y, gint width, gint height,
    gdouble alpha)
{
  GValue position = G_VALUE_INIT;
  GValue dimensions = G_VALUE_INIT;
  gint position_values[] = { x, y };
  gint dimension_values[] = { width, height };

  if (pad == NULL) {
    return FALSE;
  }

  g_value_init (&position, GST_TYPE_ARRAY);
  g_value_init (&dimensions, GST_TYPE_ARRAY);
  build_pad_property (&position, position_values, 2);
  build_pad_property (&dimensions, dimension_values, 2);

  g_object_set_property (G_OBJECT (pad), "position", &position);
  g_object_set_property (G_OBJECT (pad), "dimensions", &dimensions);
  g_object_set (G_OBJECT (pad), "alpha", alpha, NULL);

  g_value_unset (&position);
  g_value_unset (&dimensions);
  return TRUE;
}

static GstElement *
create_capsfilter (const gchar *name, guint width, guint height)
{
  GstElement *capsfilter;
  GstCaps *caps;

  capsfilter = gst_element_factory_make ("capsfilter", name);
  if (capsfilter == NULL) {
    return NULL;
  }

  caps = gst_caps_new_simple ("video/x-raw",
      "format", G_TYPE_STRING, "NV12",
      "width", G_TYPE_INT, width,
      "height", G_TYPE_INT, height,
      "framerate", GST_TYPE_FRACTION, DEFAULT_FPS_NUM, DEFAULT_FPS_DEN,
      NULL);
  g_object_set (capsfilter, "caps", caps, NULL);
  gst_caps_unref (caps);
  return capsfilter;
}

static gboolean
request_composer_pads (GstCameraSwitchCtx *ctx)
{
  ctx->composer_sink0 = gst_element_request_pad_simple (ctx->composer,
      "sink_%u");
  ctx->composer_sink1 = gst_element_request_pad_simple (ctx->composer,
      "sink_%u");

  if (ctx->composer_sink0 == NULL || ctx->composer_sink1 == NULL) {
    g_printerr ("Failed to request qtivcomposer input pads.\n");
    return FALSE;
  }

  g_print ("Requested composer pads: %s and %s\n",
      GST_PAD_NAME (ctx->composer_sink0), GST_PAD_NAME (ctx->composer_sink1));
  return TRUE;
}

static gboolean
link_camera_to_composer (GstElement *queue, GstPad *composer_pad)
{
  GstPad *queue_src;
  GstPadLinkReturn result;

  queue_src = gst_element_get_static_pad (queue, "src");
  if (queue_src == NULL) {
    return FALSE;
  }

  result = gst_pad_link (queue_src, composer_pad);
  gst_object_unref (queue_src);

  if (result != GST_PAD_LINK_OK) {
    g_printerr ("Failed to link camera queue to composer: %s\n",
        gst_pad_link_get_name (result));
    return FALSE;
  }
  return TRUE;
}

static gboolean
create_pipe (GstCameraSwitchCtx *ctx)
{
  GstElement *elements[16];
  guint element_count = 0;
  gchar *media_dir = NULL;
  gchar *output_path = NULL;
  const gchar *home_dir;

  ctx->camsrc0 = gst_element_factory_make ("qtiqmmfsrc", "camsrc_0");
  ctx->camsrc1 = gst_element_factory_make ("qtiqmmfsrc", "camsrc_1");
  ctx->capsfilter0 = create_capsfilter ("capsfilter_0", ctx->camera0_width, ctx->camera0_height);
  ctx->capsfilter1 = create_capsfilter ("capsfilter_1", ctx->camera1_width, ctx->camera1_height);
  ctx->queue0 = gst_element_factory_make ("queue", "camera_queue_0");
  ctx->queue1 = gst_element_factory_make ("queue", "camera_queue_1");
  ctx->composer = gst_element_factory_make ("qtivcomposer", "qtivcomposer");
  ctx->waylandsink = gst_element_factory_make ("waylandsink", "waylandsink");

  if (ctx->use_display == 0) {
    ctx->tee = gst_element_factory_make ("tee", "output_tee");
    ctx->output_capsfilter = create_capsfilter ("output_capsfilter", ctx->output_width, ctx->output_height);
    ctx->preview_queue = gst_element_factory_make ("queue", "preview_queue");
    ctx->record_queue = gst_element_factory_make ("queue", "record_queue");
    ctx->encoder = gst_element_factory_make ("v4l2h264enc", "v4l2h264enc");
    ctx->h264parse = gst_element_factory_make ("h264parse", "h264parse");
    ctx->mp4mux = gst_element_factory_make ("mp4mux", "mp4mux");
    ctx->filesink = gst_element_factory_make ("filesink", "filesink");
  }

  elements[element_count++] = ctx->camsrc0;
  elements[element_count++] = ctx->camsrc1;
  elements[element_count++] = ctx->capsfilter0;
  elements[element_count++] = ctx->capsfilter1;
  elements[element_count++] = ctx->queue0;
  elements[element_count++] = ctx->queue1;
  elements[element_count++] = ctx->composer;
  elements[element_count++] = ctx->waylandsink;

  if (ctx->use_display == 0) {
    elements[element_count++] = ctx->tee;
    elements[element_count++] = ctx->output_capsfilter;
    elements[element_count++] = ctx->preview_queue;
    elements[element_count++] = ctx->record_queue;
    elements[element_count++] = ctx->encoder;
    elements[element_count++] = ctx->h264parse;
    elements[element_count++] = ctx->mp4mux;
    elements[element_count++] = ctx->filesink;
  }

  for (guint index = 0; index < element_count; index++) {
    if (elements[index] == NULL) {
      g_printerr ("Failed to create required GStreamer element.\n");
      return FALSE;
    }
  }

  g_object_set (ctx->camsrc0, "camera", ctx->camera0_id, NULL);
  g_object_set (ctx->camsrc1, "camera", ctx->camera1_id, NULL);
  g_object_set (ctx->waylandsink, "enable-last-sample", FALSE, NULL);

  gst_bin_add_many (GST_BIN (ctx->pipeline),
      ctx->camsrc0, ctx->capsfilter0, ctx->queue0,
      ctx->camsrc1, ctx->capsfilter1, ctx->queue1,
      ctx->composer, ctx->waylandsink, NULL);

  if (!gst_element_link_many (ctx->camsrc0, ctx->capsfilter0, ctx->queue0,
          NULL) ||
      !gst_element_link_many (ctx->camsrc1, ctx->capsfilter1, ctx->queue1,
          NULL)) {
    g_printerr ("Failed to link camera input branches.\n");
    return FALSE;
  }

  if (!request_composer_pads (ctx) ||
      !link_camera_to_composer (ctx->queue0, ctx->composer_sink0) ||
      !link_camera_to_composer (ctx->queue1, ctx->composer_sink1)) {
    return FALSE;
  }

  configure_composer_pad (ctx->composer_sink0, 0, 0,
      ctx->output_width, ctx->output_height, 1.0);
  configure_composer_pad (ctx->composer_sink1, 0, 0,
      ctx->output_width, ctx->output_height, 0.0);
  ctx->camera0_active = TRUE;

  if (ctx->use_display != 0) {
    if (!gst_element_link (ctx->composer, ctx->waylandsink)) {
      g_printerr ("Failed to link display branch.\n");
      return FALSE;
    }
  } else {
    home_dir = g_get_home_dir ();
    if (home_dir == NULL || home_dir[0] == '\0') {
      g_printerr ("Failed to resolve HOME directory.\n");
      return FALSE;
    }

    media_dir = g_build_filename (home_dir, "Downloads", "qimsdk_samples",
        "media", NULL);
    if (g_mkdir_with_parents (media_dir, 0755) != 0) {
      g_printerr ("Failed to create media directory '%s': %s\n", media_dir,
          g_strerror (errno));
      g_free (media_dir);
      return FALSE;
    }

    output_path = g_build_filename (media_dir, OUTPUT_FILE_NAME, NULL);
    g_free (media_dir);
    g_object_set (ctx->filesink, "location", output_path,
        "enable-last-sample", FALSE, NULL);
    g_free (output_path);

    gst_bin_add_many (GST_BIN (ctx->pipeline), ctx->tee, ctx->output_capsfilter, ctx->preview_queue,
        ctx->record_queue, ctx->encoder, ctx->h264parse, ctx->mp4mux,
        ctx->filesink, NULL);

    gst_element_set_enum_property (ctx->encoder, "capture-io-mode", "dmabuf");
    gst_element_set_enum_property (ctx->encoder, "output-io-mode",
        "dmabuf-import");

    if (!gst_element_link_many (ctx->composer, ctx->output_capsfilter, ctx->tee, NULL) ||
        !gst_element_link (ctx->preview_queue, ctx->waylandsink) ||
        !gst_element_link_many (ctx->record_queue, ctx->encoder,
            ctx->h264parse, ctx->mp4mux, ctx->filesink, NULL)) {
      g_printerr ("Failed to link preview and recording branches.\n");
      return FALSE;
    }

    {
      GstPad *tee_preview_src;
      GstPad *tee_record_src;
      GstPad *preview_sink;
      GstPad *record_sink;

      tee_preview_src = gst_element_request_pad_simple (ctx->tee, "src_%u");
      tee_record_src = gst_element_request_pad_simple (ctx->tee, "src_%u");
      preview_sink = gst_element_get_static_pad (ctx->preview_queue, "sink");
      record_sink = gst_element_get_static_pad (ctx->record_queue, "sink");

      if (tee_preview_src == NULL || tee_record_src == NULL ||
          preview_sink == NULL || record_sink == NULL ||
          gst_pad_link (tee_preview_src, preview_sink) != GST_PAD_LINK_OK ||
          gst_pad_link (tee_record_src, record_sink) != GST_PAD_LINK_OK) {
        g_printerr ("Failed to link tee branches.\n");
        if (tee_preview_src != NULL) gst_object_unref (tee_preview_src);
        if (tee_record_src != NULL) gst_object_unref (tee_record_src);
        if (preview_sink != NULL) gst_object_unref (preview_sink);
        if (record_sink != NULL) gst_object_unref (record_sink);
        return FALSE;
      }

      gst_object_unref (tee_preview_src);
      gst_object_unref (tee_record_src);
      gst_object_unref (preview_sink);
      gst_object_unref (record_sink);
    }
  }

  return TRUE;
}

static gboolean
switch_camera_cb (gpointer user_data)
{
  GstCameraSwitchCtx *ctx = user_data;
  gdouble alpha0 = ctx->camera0_active ? 0.0 : 1.0;
  gdouble alpha1 = ctx->camera0_active ? 1.0 : 0.0;
  guint camera_id = ctx->camera0_active ? ctx->camera1_id : ctx->camera0_id;

  g_object_set (ctx->composer_sink0, "alpha", alpha0, NULL);
  g_object_set (ctx->composer_sink1, "alpha", alpha1, NULL);
  ctx->camera0_active = !ctx->camera0_active;

  g_print ("Switched camera %u\n", camera_id);
  return G_SOURCE_CONTINUE;
}

static gboolean
signal_cb (gpointer user_data)
{
  GstCameraSwitchCtx *ctx = user_data;
  if (!ctx->shutting_down) {
    ctx->shutting_down = TRUE;
    g_main_loop_quit (ctx->main_loop);
  }
  return G_SOURCE_REMOVE;
}

static gboolean
bus_cb (GstBus *bus, GstMessage *message, gpointer user_data)
{
  GstCameraSwitchCtx *ctx = user_data;
  GError *error = NULL;
  gchar *debug = NULL;
  (void) bus;

  switch (GST_MESSAGE_TYPE (message)) {
    case GST_MESSAGE_ERROR:
      gst_message_parse_error (message, &error, &debug);
      g_printerr ("GStreamer error: %s\n", error->message);
      if (debug != NULL) g_printerr ("Debug: %s\n", debug);
      g_clear_error (&error);
      g_free (debug);
      g_main_loop_quit (ctx->main_loop);
      break;
    case GST_MESSAGE_EOS:
      g_main_loop_quit (ctx->main_loop);
      break;
    default:
      break;
  }
  return G_SOURCE_CONTINUE;
}

static void
release_request_pad (GstCameraSwitchCtx *ctx, GstPad **pad)
{
  if (*pad != NULL) {
    gst_element_release_request_pad (ctx->composer, *pad);
    gst_object_unref (*pad);
    *pad = NULL;
  }
}

int
main (int argc, char *argv[])
{
  GOptionContext *option_context;
  GOptionEntry options[] = {
    { "display", 'd', 0, G_OPTION_ARG_INT, NULL,
      "1=display only, 0=preview and encode", "0 or 1" },
    { "camera0_id", 'm', 0, G_OPTION_ARG_INT, NULL,
      "Camera 0 ID", "ID" },
    { "camera1_id", 's', 0, G_OPTION_ARG_INT, NULL,
      "Camera 1 ID", "ID" },
    { "camera0-width", 0, 0, G_OPTION_ARG_INT, NULL,
      "Camera 0 width", "WIDTH" },
    { "camera0-height", 0, 0, G_OPTION_ARG_INT, NULL,
      "Camera 0 height", "HEIGHT" },
    { "camera1-width", 0, 0, G_OPTION_ARG_INT, NULL,
      "Camera 1 width", "WIDTH" },
    { "camera1-height", 0, 0, G_OPTION_ARG_INT, NULL,
      "Camera 1 height", "HEIGHT" },
    { "output-width", 0, 0, G_OPTION_ARG_INT, NULL,
      "Composed output width", "WIDTH" },
    { "output-height", 0, 0, G_OPTION_ARG_INT, NULL,
      "Composed output height", "HEIGHT" },
    { NULL }
  };
  GstCameraSwitchCtx ctx = { 0 };
  GstBus *bus;
  GError *error = NULL;
  GstStateChangeReturn state_result;

  options[0].arg_data = &ctx.use_display;
  options[1].arg_data = &ctx.camera0_id;
  options[2].arg_data = &ctx.camera1_id;
  options[3].arg_data = &ctx.camera0_width;
  options[4].arg_data = &ctx.camera0_height;
  options[5].arg_data = &ctx.camera1_width;
  options[6].arg_data = &ctx.camera1_height;
  options[7].arg_data = &ctx.output_width;
  options[8].arg_data = &ctx.output_height;
  ctx.use_display = 1;
  ctx.camera0_id = 0;
  ctx.camera1_id = 1;
  ctx.camera0_width = DEFAULT_WIDTH;
  ctx.camera0_height = DEFAULT_HEIGHT;
  ctx.camera1_width = DEFAULT_WIDTH;
  ctx.camera1_height = DEFAULT_HEIGHT;
  ctx.output_width = DEFAULT_WIDTH;
  ctx.output_height = DEFAULT_HEIGHT;

  gst_init (&argc, &argv);
  option_context = g_option_context_new (GST_APP_SUMMARY);
  g_option_context_add_main_entries (option_context, options, NULL);
  g_option_context_add_group (option_context, gst_init_get_option_group ());

  if (!g_option_context_parse (option_context, &argc, &argv, &error)) {
    g_printerr ("Option parsing failed: %s\n", error->message);
    g_clear_error (&error);
    g_option_context_free (option_context);
    return 1;
  }
  g_option_context_free (option_context);

  ctx.pipeline = gst_pipeline_new ("gst-camera-switch");
  ctx.main_loop = g_main_loop_new (NULL, FALSE);
  if (ctx.pipeline == NULL || ctx.main_loop == NULL || !create_pipe (&ctx)) {
    g_printerr ("Failed to create pipeline.\n");
    if (ctx.main_loop != NULL) g_main_loop_unref (ctx.main_loop);
    if (ctx.pipeline != NULL) gst_object_unref (ctx.pipeline);
    return 1;
  }

  bus = gst_element_get_bus (ctx.pipeline);
  gst_bus_add_watch (bus, bus_cb, &ctx);
  gst_object_unref (bus);

  ctx.switch_source_id = g_timeout_add_seconds (SWITCH_INTERVAL_SECONDS,
      switch_camera_cb, &ctx);
  ctx.signal_source_id = g_unix_signal_add (SIGINT, signal_cb, &ctx);

  state_result = gst_element_set_state (ctx.pipeline, GST_STATE_PLAYING);
  if (state_result == GST_STATE_CHANGE_FAILURE) {
    g_printerr ("Failed to set pipeline to PLAYING.\n");
    g_main_loop_quit (ctx.main_loop);
  } else {
    g_print ("Running in %s mode. Camera 0=%u, camera 1=%u\n",
        ctx.use_display ? "display" : "preview and encode",
        ctx.camera0_id, ctx.camera1_id);
    g_main_loop_run (ctx.main_loop);
  }

  if (ctx.switch_source_id != 0) g_source_remove (ctx.switch_source_id);
  if (ctx.signal_source_id != 0) g_source_remove (ctx.signal_source_id);

  if (ctx.use_display == 0) {
    gst_element_send_event (ctx.pipeline, gst_event_new_eos ());
    gst_element_get_state (ctx.pipeline, NULL, NULL, 5 * GST_SECOND);
  }

  gst_element_set_state (ctx.pipeline, GST_STATE_NULL);
  release_request_pad (&ctx, &ctx.composer_sink0);
  release_request_pad (&ctx, &ctx.composer_sink1);
  gst_object_unref (ctx.pipeline);
  g_main_loop_unref (ctx.main_loop);
  gst_deinit ();
  return 0;
}
