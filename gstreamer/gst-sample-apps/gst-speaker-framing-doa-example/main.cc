/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

/*
 * GStreamer Application: gst-speaker-framing-doa-example
 *
 * Description:
 * Complete speaker framing application that integrates DOA (Direction-of-Arrival)
 * from PipeWire with person detection and beamforming for automatic speaker tracking.
 *
 * This app:
 *   1. Builds a full GStreamer pipeline with person detection, beamforming, and AFR.
 *   2. Connects to the PipeWire "fnn-doa" metadata object for audio DOA data.
 *   3. Receives DOA updates (degrees, speech_prob, timestamp) via PipeWire callbacks.
 *   4. Pushes DOA data to the beamforming plugin via appsrc every 20ms.
 *   5. Fuses vision (person detection) with audio (DOA) to identify the active speaker.
 *   6. Applies Active Frame Region (AFR) and crops to frame the speaker.
 *
 * Usage:
 *   gst-speaker-framing-doa-example -w 1280 -h 720
 *
 * Expected output:
 *   Camera feed with automatic speaker framing on Wayland display.
 *
 * Prerequisites:
 *   - Audio team's fnn-doa PipeWire plugin must be loaded.
 *   - A PipeWire recording session must be active (pw-record or equivalent).
 *   - ML model files in /data/misc/camera/mle_snpe/
 *   - AFR config in /data/misc/camera/afr_config.json
 *
 * Pipeline:
 *   qtiqmmfsrc -> capsfilter -> tee
 *       tee -> queue_media -> qtimetamux.sink (video)
 *       tee -> queue_ml1 -> qtimlvconverter -> queue_ml2 -> qtimltflite
 *           -> queue_ml3 -> qtimlpostprocess -> capsfilter(text/x-raw)
 *           -> qtimetamux.data_0 (metadata)
 *   qtimetamux.src -> qtibeamforming.sink
 *   appsrc (DOA)   -> qtibeamforming.doa
 *   qtibeamforming.src
 *       -> qtimetatransform(meta-transform-roi-person-merge)
 *       -> qtimetatransform(meta-transform-roi-auto-framing)
 *       -> qtivsplit(src_0::mode=single-roi-meta)
 *       -> waylandsink
 */

#include <errno.h>
#include <glib-unix.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <gst/gst.h>
#include <pipewire/pipewire.h>
#include <pipewire/extensions/metadata.h>
#include <spa/utils/dict.h>

#define DEFAULT_WIDTH  1280
#define DEFAULT_HEIGHT 720

typedef struct _AppContext {
  GstElement *pipeline;
  GMainLoop  *mloop;
  GList      *plugins;

  gint width;
  gint height;
  gboolean debug_mode;

  struct pw_main_loop *pw_loop;
  struct pw_context   *pw_context;
  struct pw_core      *pw_core;
  struct pw_registry  *pw_registry;
  struct pw_metadata  *pw_metadata;
  struct spa_hook      registry_listener;
  struct spa_hook      metadata_listener;
  GThread             *pw_thread;

  gint    latest_degrees;
  gint    latest_speech_prob;
  gint64  latest_timestamp_us;
  gboolean doa_valid;
  guint8  polar_activity[360];
  gint64  last_polar_time_us;

  guint       doa_timer_id;
  GstElement *appsrc;
  GMutex      doa_mutex;

  gchar *model_path;
  gchar *labels_path;

  gint     camera_fov;
  gint     mic_orientation;
  gboolean mic_flip;
  gint     mic_resolution;

  GPid pw_record_pid;
} AppContext;

static gboolean handle_interrupt_signal (gpointer userdata);

static void
start_pw_record (AppContext *ctx)
{
  gchar *argv[] = {
    "pw-record", "-v",
    "--rate",     "48000",
    "--channels", "2",
    "--format",   "s16",
    "/data/audio_capture.wav",
    NULL
  };
  GError *error = NULL;

  if (!g_spawn_async (NULL, argv, NULL,
                      (GSpawnFlags)(G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD),
                      NULL, NULL, &ctx->pw_record_pid, &error)) {
    g_printerr ("[AUDIO] Failed to start pw-record: %s\n",
                error ? error->message : "unknown");
    g_clear_error (&error);
    ctx->pw_record_pid = 0;
  } else {
    g_print ("[AUDIO] pw-record started (pid=%d) → /data/audio_capture.wav\n",
             (int) ctx->pw_record_pid);
  }
}

static void
stop_pw_record (AppContext *ctx)
{
  if (ctx->pw_record_pid <= 0)
    return;

  kill (ctx->pw_record_pid, SIGTERM);

  for (gint i = 0; i < 50; i++) {
    pid_t r = waitpid (ctx->pw_record_pid, NULL, WNOHANG);
    if (r != 0)
      goto done;
    usleep (100000);
  }

  g_printerr ("[AUDIO] pw-record did not exit, sending SIGKILL\n");
  kill (ctx->pw_record_pid, SIGKILL);
  waitpid (ctx->pw_record_pid, NULL, 0);

done:
  g_spawn_close_pid (ctx->pw_record_pid);
  ctx->pw_record_pid = 0;
  g_print ("[AUDIO] pw-record stopped → /data/audio_capture.wav\n");
}

static gint
get_enum_value (const gchar * enum_type_name, const gchar * enum_nick)
{
  GEnumClass *enum_class;
  GEnumValue *enum_value;
  gint value = 0;

  enum_class = (GEnumClass *) g_type_class_ref (g_type_from_name (enum_type_name));
  if (enum_class) {
    enum_value = g_enum_get_value_by_nick (enum_class, enum_nick);
    if (enum_value)
      value = enum_value->value;
    g_type_class_unref (enum_class);
  }
  return value;
}

static int
on_metadata_property (void *data, uint32_t subject,
                      const char *key, const char *type, const char *value)
{
  AppContext *ctx = (AppContext *) data;

  if (key == NULL || value == NULL)
    return 0;

  g_mutex_lock (&ctx->doa_mutex);

  if (strcmp (key, "fnn.doa.degrees") == 0) {
    ctx->latest_degrees = atoi (value);
    ctx->doa_valid = TRUE;
  } else if (strcmp (key, "fnn.doa.speech_prob") == 0) {
    ctx->latest_speech_prob = atoi (value);
  } else if (strcmp (key, "fnn.doa.timestamp_us") == 0) {
    ctx->latest_timestamp_us = (gint64) atoll (value);
  } else if (strcmp (key, "fnn.doa.polar_axis0") == 0) {
    gchar **tokens = g_strsplit (value, " ", 360);
    guint count = g_strv_length (tokens);
    gint64 now = g_get_monotonic_time ();
    gint64 delta_ms = ctx->last_polar_time_us > 0
        ? (now - ctx->last_polar_time_us) / 1000 : 0;
    ctx->last_polar_time_us = now;
    memset (ctx->polar_activity, 0, sizeof (ctx->polar_activity));

    for (gint i = 0; tokens[i] != NULL && i < 360; i++) {
      gint raw = atoi (tokens[i]);
      gint clamped = raw < 0 ? 0 : (raw > 32767 ? 32767 : raw);
      ctx->polar_activity[i] = (guint8) ((clamped * 255) / 32767);
    }

    {
      gdouble prob = (gdouble) ctx->latest_speech_prob / 32767.0;
      gint top_idx[5] = {-1, -1, -1, -1, -1};
      guint8 top_val[5] = {0, 0, 0, 0, 0};
      for (gint i = 0; i < 360; i++) {
        guint8 v = ctx->polar_activity[i];
        for (gint k = 0; k < 5; k++) {
          if (v > top_val[k]) {
            for (gint m = 4; m > k; m--) {
              top_idx[m] = top_idx[m-1];
              top_val[m] = top_val[m-1];
            }
            top_idx[k] = i;
            top_val[k] = v;
            break;
          }
        }
      }
      g_print ("[PW] polar: %u vals delta=%" G_GINT64_FORMAT "ms prob=%.3f deg=%d | top5:",
               count, delta_ms, prob, ctx->latest_degrees);
      for (gint k = 0; k < 5 && top_idx[k] >= 0; k++) {
        gint raw = atoi (tokens[top_idx[k]] ? tokens[top_idx[k]] : "0");
        g_print (" [%d]=%d->%u", top_idx[k], raw, (guint) top_val[k]);
      }
      g_print ("\n");
    }
    g_strfreev (tokens);
  }

  g_mutex_unlock (&ctx->doa_mutex);
  return 0;
}

static const struct pw_metadata_events metadata_events = {
  PW_VERSION_METADATA_EVENTS,
  .property = on_metadata_property,
};

static void
on_registry_global (void *data, uint32_t id, uint32_t permissions,
                    const char *type, uint32_t version,
                    const struct spa_dict *props)
{
  AppContext *ctx = (AppContext *) data;

  if (strcmp (type, PW_TYPE_INTERFACE_Metadata) != 0)
    return;

  const char *name = spa_dict_lookup (props, PW_KEY_METADATA_NAME);
  if (name == NULL || strcmp (name, "fnn-doa") != 0)
    return;

  g_print ("[PW] Found fnn-doa metadata (id=%u)\n", id);

  if (ctx->pw_metadata != NULL) {
    g_print ("[PW] Already bound to fnn-doa, skipping duplicate (id=%u)\n", id);
    return;
  }

  ctx->pw_metadata = (struct pw_metadata *) pw_registry_bind (
      ctx->pw_registry, id, PW_TYPE_INTERFACE_Metadata, PW_VERSION_METADATA, 0);

  if (ctx->pw_metadata == NULL) {
    g_printerr ("[PW] Failed to bind to fnn-doa metadata\n");
    return;
  }
  pw_metadata_set_property (ctx->pw_metadata, 0,
    "fnn.doa.enable", "Spa:Bool", "1");
  pw_metadata_set_property (ctx->pw_metadata, 0,
    "fnn.doa.interval_ms", "Spa:Int", "20");

  pw_metadata_add_listener (ctx->pw_metadata,
      &ctx->metadata_listener,
      &metadata_events,
      ctx);


  g_print ("[PW] DOA enabled. Waiting for updates...\n");
}

static const struct pw_registry_events registry_events = {
  PW_VERSION_REGISTRY_EVENTS,
  .global = on_registry_global,
};

static gpointer
pipewire_thread_func (gpointer userdata)
{
  AppContext *ctx = (AppContext *) userdata;
  g_print ("[PW] PipeWire thread started\n");
  pw_main_loop_run (ctx->pw_loop);
  g_print ("[PW] PipeWire thread exiting\n");
  return NULL;
}

static gboolean
pipewire_init (AppContext *ctx)
{
  pw_init (NULL, NULL);

  ctx->pw_loop = pw_main_loop_new (NULL);
  if (!ctx->pw_loop) {
    g_printerr ("[PW] Failed to create PipeWire main loop\n");
    return FALSE;
  }

  ctx->pw_context = pw_context_new (
      pw_main_loop_get_loop (ctx->pw_loop), NULL, 0);
  if (!ctx->pw_context) {
    g_printerr ("[PW] Failed to create PipeWire context\n");
    return FALSE;
  }

  ctx->pw_core = pw_context_connect (ctx->pw_context, NULL, 0);
  if (!ctx->pw_core) {
    g_printerr ("[PW] Failed to connect to PipeWire\n");
    return FALSE;
  }

  ctx->pw_registry = pw_core_get_registry (ctx->pw_core,
      PW_VERSION_REGISTRY, 0);
  if (!ctx->pw_registry) {
    g_printerr ("[PW] Failed to get PipeWire registry\n");
    return FALSE;
  }

  pw_registry_add_listener (ctx->pw_registry,
      &ctx->registry_listener,
      &registry_events,
      ctx);

  ctx->pw_thread = g_thread_new ("pipewire", pipewire_thread_func, ctx);

  g_print ("[PW] PipeWire initialized\n");
  return TRUE;
}

static void
pipewire_cleanup (AppContext *ctx)
{
  if (ctx->pw_loop)
    pw_main_loop_quit (ctx->pw_loop);

  if (ctx->pw_thread) {
    g_thread_join (ctx->pw_thread);
    ctx->pw_thread = NULL;
  }

  if (ctx->pw_metadata) {
    pw_proxy_destroy ((struct pw_proxy *) ctx->pw_metadata);
    ctx->pw_metadata = NULL;
  }

  if (ctx->pw_registry) {
    pw_proxy_destroy ((struct pw_proxy *) ctx->pw_registry);
    ctx->pw_registry = NULL;
  }

  if (ctx->pw_core) {
    pw_core_disconnect (ctx->pw_core);
    ctx->pw_core = NULL;
  }

  if (ctx->pw_context) {
    pw_context_destroy (ctx->pw_context);
    ctx->pw_context = NULL;
  }

  if (ctx->pw_loop) {
    pw_main_loop_destroy (ctx->pw_loop);
    ctx->pw_loop = NULL;
  }

  pw_deinit ();
}

static gboolean
doa_timer_callback (gpointer userdata)
{
  AppContext *ctx = (AppContext *) userdata;

  g_mutex_lock (&ctx->doa_mutex);
  if (!ctx->doa_valid) {
    g_mutex_unlock (&ctx->doa_mutex);
    return G_SOURCE_CONTINUE;
  }
  if (ctx->appsrc == NULL) {
    g_mutex_unlock (&ctx->doa_mutex);
    return G_SOURCE_CONTINUE;
  }

  gint    degrees     = ctx->latest_degrees;
  gdouble probability = (gdouble) ctx->latest_speech_prob / 32767.0;

  GstStructure *s = gst_structure_new ("doa-data",
      "probability",   G_TYPE_DOUBLE, probability,
      "timestamp",     G_TYPE_INT64,  ctx->latest_timestamp_us,
      "fov-start-deg", G_TYPE_INT,    0,
      "fov-end-deg",   G_TYPE_INT,    ctx->camera_fov,
      NULL);

  GValue arr = G_VALUE_INIT;
  g_value_init (&arr, GST_TYPE_ARRAY);
  GValue v = G_VALUE_INIT;
  g_value_init (&v, G_TYPE_INT);
  g_value_set_int (&v, degrees);
  gst_value_array_append_value (&arr, &v);
  g_value_unset (&v);
  gst_structure_set_value (s, "directions", &arr);
  g_value_unset (&arr);

  {
    gint peak_idx = 0;
    guint8 peak_val = 0;
    for (gint i = 0; i < 360; i++) {
      if (ctx->polar_activity[i] > peak_val) {
        peak_val = ctx->polar_activity[i];
        peak_idx = i;
      }
    }
    g_print ("[DOA] deg=%d prob=%.3f peak=[%d]=%u\n",
             degrees, probability, peak_idx, (guint) peak_val);
  }

  GBytes *bytes = g_bytes_new (ctx->polar_activity, 360);
  g_mutex_unlock (&ctx->doa_mutex);
  GValue bv = G_VALUE_INIT;
  g_value_init (&bv, G_TYPE_BYTES);
  g_value_set_boxed (&bv, bytes);
  gst_structure_set_value (s, "polar-activity", &bv);
  g_bytes_unref (bytes);
  g_value_unset (&bv);

  gchar *text = gst_structure_to_string (s);
  gst_structure_free (s);

  GstBuffer *buf = gst_buffer_new_wrapped (text, strlen (text));
  GstFlowReturn ret;
  g_signal_emit_by_name (ctx->appsrc, "push-buffer", buf, &ret);
  gst_buffer_unref (buf);

  if (ret != GST_FLOW_OK)
    g_printerr ("[DOA] Failed to push buffer to appsrc: %s\n", gst_flow_get_name (ret));

  return G_SOURCE_CONTINUE;
}

static gboolean
create_pipe (AppContext *ctx)
{
  GstElement *camsrc, *capsfilter, *tee;
  GstElement *queue_media;
  GstElement *queue_ml1, *mlvconv, *queue_ml2, *qairt, *queue_ml3;
  GstElement *mlpostproc, *capsfilter_meta, *metamux;
  GstElement *beamforming, *overlay, *afr_frame, *vsplit, *display;
  GstCaps *caps, *doa_caps, *meta_caps;
  GstPad *tee_media_pad, *tee_ml_pad;
  GstPad *queue_media_sink, *queue_ml1_sink;
  GstPad *metamux_data_sink;
  GstPad *vsplit_src;
  gboolean added_to_bin = FALSE;

  camsrc          = gst_element_factory_make ("qticamsrc",        "camsrc");
  capsfilter      = gst_element_factory_make ("capsfilter",       "capsfilter");
  tee             = gst_element_factory_make ("tee",              "tee");
  queue_media     = gst_element_factory_make ("queue",            "queue_media");
  queue_ml1       = gst_element_factory_make ("queue",            "queue_ml1");
  mlvconv         = gst_element_factory_make ("qtimlvconverter",  "mlvconv");
  queue_ml2       = gst_element_factory_make ("queue",            "queue_ml2");
  qairt           = gst_element_factory_make ("qtimlqairt",       "qairt");
  queue_ml3       = gst_element_factory_make ("queue",            "queue_ml3");
  mlpostproc      = gst_element_factory_make ("qtimlpostprocess", "mlpostproc");
  capsfilter_meta = gst_element_factory_make ("capsfilter",       "capsfilter_meta");
  metamux         = gst_element_factory_make ("qtimetamux",       "metamux");
  beamforming     = gst_element_factory_make ("qtibeamforming",   "beamforming");
  overlay         = ctx->debug_mode ? gst_element_factory_make ("qtivoverlay", "overlay") : NULL;
  afr_frame       = gst_element_factory_make ("qtimetatransform", "afr_frame");
  vsplit          = gst_element_factory_make ("qtivsplit",        "vsplit");
  display         = gst_element_factory_make ("waylandsink",      "waylandsink");
  ctx->appsrc     = gst_element_factory_make ("appsrc",           "appsrc");

  if (!camsrc || !capsfilter || !tee || !queue_media ||
      !queue_ml1 || !mlvconv || !queue_ml2 || !qairt ||
      !queue_ml3 || !mlpostproc || !capsfilter_meta || !metamux ||
      !beamforming || !afr_frame || !vsplit ||
      !display || !ctx->appsrc ||
      (ctx->debug_mode && !overlay)) {
    g_printerr ("[GST] Failed to create pipeline elements\n");
    goto cleanup;
  }

  caps = gst_caps_new_simple ("video/x-raw",
      "format",         G_TYPE_STRING,     "NV12",
      "width",          G_TYPE_INT,        ctx->width,
      "height",         G_TYPE_INT,        ctx->height,
      "framerate",      GST_TYPE_FRACTION, 30, 1,
      "interlace-mode", G_TYPE_STRING,     "progressive",
      "colorimetry",    G_TYPE_STRING,     "bt601",
      NULL);
  g_object_set (G_OBJECT (capsfilter), "caps", caps, NULL);
  gst_caps_unref (caps);

  /* appsrc caps: text/x-raw for DOA metadata */
  doa_caps = gst_caps_new_simple ("text/x-raw",
      "format", G_TYPE_STRING, "utf8",
      NULL);
  g_object_set (G_OBJECT (ctx->appsrc),
      "caps",         doa_caps,
      "format",       GST_FORMAT_TIME,
      "is-live",      TRUE,
      "do-timestamp", TRUE,
      NULL);
  gst_caps_unref (doa_caps);

  meta_caps = gst_caps_new_simple ("text/x-raw", NULL, NULL);
  g_object_set (G_OBJECT (capsfilter_meta), "caps", meta_caps, NULL);
  gst_caps_unref (meta_caps);

  {
    const gchar *model = ctx->model_path ? ctx->model_path
                                         : "/data/yolov8_det_w8a8.dlc";
    GValue tensors = G_VALUE_INIT;
    GValue tensor  = G_VALUE_INIT;
    g_value_init (&tensors, GST_TYPE_ARRAY);
    g_value_init (&tensor,  G_TYPE_STRING);
    g_value_set_string (&tensor, "boxes");
    gst_value_array_append_value (&tensors, &tensor);
    g_value_reset (&tensor);
    g_value_set_string (&tensor, "scores");
    gst_value_array_append_value (&tensors, &tensor);
    g_value_reset (&tensor);
    g_value_set_string (&tensor, "class_idx");
    gst_value_array_append_value (&tensors, &tensor);
    g_value_unset (&tensor);
    g_object_set (G_OBJECT (qairt),
        "backend", get_enum_value ("GstMLQairtBackend", "htp"),
        "model",   model,
        NULL);
    g_object_set_property (G_OBJECT (qairt), "tensors", &tensors);
    g_value_unset (&tensors);
    g_print ("[GST] Model: %s\n", model);
  }

  {
    const gchar *labels = ctx->labels_path ? ctx->labels_path
                                           : "/data/yolov8.json";
    g_object_set (G_OBJECT (mlpostproc),
        "module",   get_enum_value ("GstMLPostProcessModules", "yolov8"),
        "labels",   labels,
        "settings", "{\"confidence\": 45.0}",
        NULL);
    g_print ("[GST] Labels: %s\n", labels);
  }

  g_object_set (G_OBJECT (beamforming),
      "image-width",     ctx->width,
      "image-height",    ctx->height,
      "camera-fov",      ctx->camera_fov,
      "mic-orientation", ctx->mic_orientation,
      "mic-flip",        ctx->mic_flip,
      "mic-resolution",  ctx->mic_resolution,
      "filter-select",   1,
      NULL);
  g_print ("[GST] Beamforming: fov=%d mic-orientation=%d mic-flip=%s\n",
      ctx->camera_fov, ctx->mic_orientation, ctx->mic_flip ? "true" : "false");

  g_object_set (G_OBJECT (afr_frame),
      "module", get_enum_value ("GstMetaTranformModules", "roi-auto-framing"),
      NULL);

  /* Display */
  g_object_set (G_OBJECT (display),
      "sync",       FALSE,
      "fullscreen", TRUE,
      NULL);

  gst_bin_add_many (GST_BIN (ctx->pipeline),
      camsrc, capsfilter, tee,
      queue_media,
      queue_ml1, mlvconv, queue_ml2, qairt, queue_ml3,
      mlpostproc, capsfilter_meta, metamux,
      ctx->appsrc, beamforming,
      afr_frame, vsplit, display,
      NULL);

  if (ctx->debug_mode)
    gst_bin_add (GST_BIN (ctx->pipeline), overlay);

  added_to_bin = TRUE;

  if (!gst_element_link_many (camsrc, capsfilter, tee, NULL)) {
    g_printerr ("[GST] Failed to link camsrc -> capsfilter -> tee\n");
    goto cleanup;
  }

  tee_media_pad = gst_element_request_pad_simple (tee, "src_%u");
  queue_media_sink = gst_element_get_static_pad (queue_media, "sink");
  if (gst_pad_link (tee_media_pad, queue_media_sink) != GST_PAD_LINK_OK) {
    g_printerr ("[GST] Failed to link tee -> queue_media\n");
    gst_object_unref (tee_media_pad);
    gst_object_unref (queue_media_sink);
    goto cleanup;
  }
  gst_object_unref (tee_media_pad);
  gst_object_unref (queue_media_sink);

  if (!gst_element_link_pads (queue_media, "src", metamux, "sink")) {
    g_printerr ("[GST] Failed to link queue_media -> metamux.sink\n");
    goto cleanup;
  }

  tee_ml_pad = gst_element_request_pad_simple (tee, "src_%u");
  queue_ml1_sink = gst_element_get_static_pad (queue_ml1, "sink");
  if (gst_pad_link (tee_ml_pad, queue_ml1_sink) != GST_PAD_LINK_OK) {
    g_printerr ("[GST] Failed to link tee -> queue_ml1\n");
    gst_object_unref (tee_ml_pad);
    gst_object_unref (queue_ml1_sink);
    goto cleanup;
  }
  gst_object_unref (tee_ml_pad);
  gst_object_unref (queue_ml1_sink);

  if (!gst_element_link_many (queue_ml1, mlvconv, queue_ml2, qairt,
          queue_ml3, mlpostproc, capsfilter_meta, NULL)) {
    g_printerr ("[GST] Failed to link ML inference path\n");
    goto cleanup;
  }

  metamux_data_sink = gst_element_request_pad_simple (metamux, "data_%u");
  {
    GstPad *meta_src = gst_element_get_static_pad (capsfilter_meta, "src");
    if (!metamux_data_sink || !meta_src ||
        gst_pad_link (meta_src, metamux_data_sink) != GST_PAD_LINK_OK) {
      g_printerr ("[GST] Failed to link capsfilter_meta -> metamux data pad\n");
      if (meta_src)
        gst_object_unref (meta_src);
      if (metamux_data_sink) {
        gst_element_release_request_pad (metamux, metamux_data_sink);
        gst_object_unref (metamux_data_sink);
      }
      goto cleanup;
    }
    gst_object_unref (meta_src);
    gst_object_unref (metamux_data_sink);
  }

  if (!gst_element_link_pads (metamux, "src", beamforming, "sink")) {
    g_printerr ("[GST] Failed to link metamux -> beamforming.sink\n");
    goto cleanup;
  }

  if (!gst_element_link_pads (ctx->appsrc, "src", beamforming, "doa")) {
    g_printerr ("[GST] Failed to link appsrc -> beamforming.doa\n");
    goto cleanup;
  }

  if (ctx->debug_mode) {
    if (!gst_element_link_many (beamforming, overlay, afr_frame, vsplit, NULL)) {
      g_printerr ("[GST] Failed to link beamforming -> overlay -> afr_frame -> vsplit\n");
      goto cleanup;
    }
  } else {
    if (!gst_element_link_many (beamforming, afr_frame, vsplit, NULL)) {
      g_printerr ("[GST] Failed to link beamforming -> afr_frame -> vsplit\n");
      goto cleanup;
    }
  }

  vsplit_src = gst_element_request_pad_simple (vsplit, "src_%u");
  if (vsplit_src) {
    g_object_set (G_OBJECT (vsplit_src), "mode", 2, NULL);
    gst_object_unref (vsplit_src);
  }

  if (!gst_element_link_pads (vsplit, "src_0", display, "sink")) {
    g_printerr ("[GST] Failed to link vsplit.src_0 -> waylandsink\n");
    goto cleanup;
  }

  g_print ("[GST] Full pipeline created successfully (debug=%s)\n",
      ctx->debug_mode ? "on" : "off");
  return TRUE;

cleanup:
  if (!added_to_bin) {
    if (camsrc)          gst_object_unref (camsrc);
    if (capsfilter)      gst_object_unref (capsfilter);
    if (tee)             gst_object_unref (tee);
    if (queue_media)     gst_object_unref (queue_media);
    if (queue_ml1)       gst_object_unref (queue_ml1);
    if (mlvconv)         gst_object_unref (mlvconv);
    if (queue_ml2)       gst_object_unref (queue_ml2);
    if (qairt)           gst_object_unref (qairt);
    if (queue_ml3)       gst_object_unref (queue_ml3);
    if (mlpostproc)      gst_object_unref (mlpostproc);
    if (capsfilter_meta) gst_object_unref (capsfilter_meta);
    if (metamux)         gst_object_unref (metamux);
    if (beamforming)     gst_object_unref (beamforming);
    if (overlay)         gst_object_unref (overlay);
    if (afr_frame)       gst_object_unref (afr_frame);
    if (vsplit)          gst_object_unref (vsplit);
    if (display)         gst_object_unref (display);
    if (ctx->appsrc) {
      gst_object_unref (ctx->appsrc);
      ctx->appsrc = NULL;
    }
  }
  return FALSE;
}

static void
state_changed_cb (GstBus *bus, GstMessage *message, gpointer userdata)
{
  GstElement *pipeline = GST_ELEMENT (userdata);
  GstState old_st, new_st, pending;

  if (GST_MESSAGE_SRC (message) != GST_OBJECT_CAST (pipeline))
    return;

  gst_message_parse_state_changed (message, &old_st, &new_st, &pending);
  g_print ("[GST] State: %s -> %s (pending: %s)\n",
      gst_element_state_get_name (old_st),
      gst_element_state_get_name (new_st),
      gst_element_state_get_name (pending));

  if (new_st == GST_STATE_PAUSED && old_st == GST_STATE_READY) {
    g_print ("[GST] Setting pipeline to PLAYING state...\n");
    gst_element_set_state (pipeline, GST_STATE_PLAYING);
  }
}

static void
warning_cb (GstBus *bus, GstMessage *message, gpointer userdata)
{
  GError *error = NULL;
  gchar  *debug = NULL;
  gst_message_parse_warning (message, &error, &debug);
  g_printerr ("[GST] Warning: %s (%s)\n", error->message, debug ? debug : "none");
  g_free (debug);
  g_error_free (error);
}

static void
error_cb (GstBus *bus, GstMessage *message, gpointer userdata)
{
  GMainLoop *mloop = (GMainLoop *) userdata;
  GError    *error = NULL;
  gchar     *debug = NULL;
  gst_message_parse_error (message, &error, &debug);
  g_printerr ("[GST] Error: %s (%s)\n", error->message, debug ? debug : "none");
  g_free (debug);
  g_error_free (error);
  g_main_loop_quit (mloop);
}

static void
eos_cb (GstBus *bus, GstMessage *message, gpointer userdata)
{
  GMainLoop *mloop = (GMainLoop *) userdata;
  g_print ("[GST] End-of-stream received\n");
  g_main_loop_quit (mloop);
}

static gboolean
handle_interrupt_signal (gpointer userdata)
{
  AppContext *ctx = (AppContext *) userdata;
  g_print ("\n[INFO] Interrupt received, shutting down...\n");
  if (ctx->pipeline)
    gst_element_set_state (ctx->pipeline, GST_STATE_NULL);
  if (ctx->mloop && g_main_loop_is_running (ctx->mloop))
    g_main_loop_quit (ctx->mloop);
  return TRUE;
}

static AppContext *
app_context_new (void)
{
  AppContext *ctx = (AppContext *) g_new0 (AppContext, 1);
  if (!ctx) return NULL;

  ctx->width           = DEFAULT_WIDTH;
  ctx->height          = DEFAULT_HEIGHT;
  ctx->debug_mode      = FALSE;
  ctx->latest_degrees  = 0;
  ctx->latest_timestamp_us = 0;
  ctx->doa_valid       = FALSE;
  ctx->camera_fov      = 110;
  ctx->mic_orientation = 0;
  ctx->mic_flip        = FALSE;
  ctx->mic_resolution  = 2;

  return ctx;
}

static void
app_context_free (AppContext *ctx)
{
  if (!ctx)
    return;

  if (ctx->plugins) {
    GList *list = ctx->plugins;
    GstElement *curr = (GstElement *) list->data;
    GstElement *next = NULL;
    list = list->next;
    for (; list != NULL; list = list->next) {
      next = (GstElement *) list->data;
      gst_element_unlink (curr, next);
      gst_bin_remove (GST_BIN (ctx->pipeline), curr);
      curr = next;
    }
    gst_bin_remove (GST_BIN (ctx->pipeline), curr);
    g_list_free (ctx->plugins);
    ctx->plugins = NULL;
  }

  if (ctx->mloop) {
    g_main_loop_unref (ctx->mloop);
    ctx->mloop = NULL;
  }

  if (ctx->pipeline) {
    gst_object_unref (ctx->pipeline);
    ctx->pipeline = NULL;
  }

  g_free (ctx);
}

gint
main (gint argc, gchar *argv[])
{
  AppContext  *ctx = NULL;
  GstBus      *bus = NULL;
  GOptionContext *opt_ctx = NULL;
  gboolean     ret = FALSE;
  guint        intrpt_watch_id = 0;

  ctx = app_context_new ();
  if (!ctx) {
    g_printerr ("[ERROR] Failed to create app context\n");
    return -1;
  }

  GOptionEntry entries[] = {
    { "width",  'w', 0, G_OPTION_ARG_INT,    &ctx->width,
      "Camera width",  "pixels" },
    { "height", 'h', 0, G_OPTION_ARG_INT,    &ctx->height,
      "Camera height", "pixels" },
    { "model-path",  'm', 0, G_OPTION_ARG_STRING, &ctx->model_path,
      "Path to YOLOv8 model file", "PATH" },
    { "labels-path", 'l', 0, G_OPTION_ARG_STRING, &ctx->labels_path,
      "Path to YOLOv8 labels JSON file", "PATH" },
    { "camera-fov",      0, 0, G_OPTION_ARG_INT,  &ctx->camera_fov,
      "Camera horizontal FOV in degrees (default: 110)", "degrees" },
    { "mic-orientation", 0, 0, G_OPTION_ARG_INT,  &ctx->mic_orientation,
      "Mic orientation offset in degrees (default: 0)", "degrees" },
    { "mic-flip",        0, 0, G_OPTION_ARG_NONE, &ctx->mic_flip,
      "Flip mic angle direction (default: false)", NULL },
    { "mic-resolution",  0, 0, G_OPTION_ARG_INT,  &ctx->mic_resolution,
      "Mic histogram bin resolution in degrees, even number 2-360 (default: 2)", "degrees" },
    { "debug",           0, 0, G_OPTION_ARG_NONE, &ctx->debug_mode,
      "Enable debug overlay (bounding boxes)", NULL },
    { NULL, 0, 0, (GOptionArg) 0, NULL, NULL, NULL }
  };

  opt_ctx = g_option_context_new (
      "- Speaker framing DOA validation app\n"
      "  Displays camera feed and prints DOA degrees from PipeWire fnn-doa metadata.");
  g_option_context_add_main_entries (opt_ctx, entries, NULL);
  g_option_context_add_group (opt_ctx, gst_init_get_option_group ());

  GError *error = NULL;
  if (!g_option_context_parse (opt_ctx, &argc, &argv, &error)) {
    g_printerr ("[ERROR] Option parsing failed: %s\n",
        error ? error->message : "unknown");
    g_clear_error (&error);
    g_option_context_free (opt_ctx);
    app_context_free (ctx);
    return -1;
  }
  g_option_context_free (opt_ctx);

  gst_init (&argc, &argv);
  g_set_prgname ("gst-speaker-framing-doa-example");
  g_print ("[INFO] Starting gst-speaker-framing-doa-example\n");
  g_print ("[INFO] Camera: %dx%d @ 30fps\n", ctx->width, ctx->height);

  ctx->pipeline = gst_pipeline_new ("speaker-framing-doa");
  if (!ctx->pipeline) {
    g_printerr ("[ERROR] Failed to create pipeline\n");
    app_context_free (ctx);
    return -1;
  }

  ret = create_pipe (ctx);
  if (!ret) {
    g_printerr ("[ERROR] Failed to build pipeline\n");
    app_context_free (ctx);
    return -1;
  }

  ctx->mloop = g_main_loop_new (NULL, FALSE);
  if (!ctx->mloop) {
    g_printerr ("[ERROR] Failed to create main loop\n");
    app_context_free (ctx);
    return -1;
  }

  bus = gst_pipeline_get_bus (GST_PIPELINE (ctx->pipeline));
  if (!bus) {
    g_printerr ("[ERROR] Failed to get pipeline bus\n");
    app_context_free (ctx);
    return -1;
  }
  gst_bus_add_signal_watch (bus);
  g_signal_connect (bus, "message::state-changed",
      G_CALLBACK (state_changed_cb), ctx->pipeline);
  g_signal_connect (bus, "message::warning",
      G_CALLBACK (warning_cb), NULL);
  g_signal_connect (bus, "message::error",
      G_CALLBACK (error_cb), ctx->mloop);
  g_signal_connect (bus, "message::eos",
      G_CALLBACK (eos_cb), ctx->mloop);
  gst_object_unref (bus);

  intrpt_watch_id = g_unix_signal_add (SIGINT, handle_interrupt_signal, ctx);

  if (!pipewire_init (ctx)) {
    g_printerr ("[ERROR] Failed to initialize PipeWire\n");
    if (intrpt_watch_id)
      g_source_remove (intrpt_watch_id);
    pipewire_cleanup (ctx);
    app_context_free (ctx);
    return -1;
  }

  start_pw_record (ctx);

  ctx->doa_timer_id = g_timeout_add (20, doa_timer_callback, ctx);
  g_print ("[DOA] Started 20ms timer for DOA data push\n");

  g_print ("[GST] Setting pipeline to PAUSED state...\n");
  switch (gst_element_set_state (ctx->pipeline, GST_STATE_PAUSED)) {
    case GST_STATE_CHANGE_FAILURE:
      g_printerr ("[ERROR] Failed to transition to PAUSED state\n");
      if (intrpt_watch_id)
        g_source_remove (intrpt_watch_id);
      stop_pw_record (ctx);
      pipewire_cleanup (ctx);
      app_context_free (ctx);
      return -1;
    case GST_STATE_CHANGE_NO_PREROLL:
      g_print ("[GST] Pipeline is live, no preroll needed\n");
      gst_element_set_state (ctx->pipeline, GST_STATE_PLAYING);
      break;
    case GST_STATE_CHANGE_ASYNC:
      g_print ("[GST] Pipeline is prerolling...\n");
      break;
    case GST_STATE_CHANGE_SUCCESS:
      g_print ("[GST] State change successful\n");
      break;
  }

  g_print ("[INFO] Application running. Press Ctrl+C to stop.\n");
  g_main_loop_run (ctx->mloop);

  /* Cleanup */
  if (intrpt_watch_id)
    g_source_remove (intrpt_watch_id);

  if (ctx->doa_timer_id) {
    g_source_remove (ctx->doa_timer_id);
    ctx->doa_timer_id = 0;
  }

  g_print ("[INFO] Stopping pipeline...\n");
  gst_element_set_state (ctx->pipeline, GST_STATE_NULL);

  stop_pw_record (ctx);

  g_print ("[INFO] Cleaning up PipeWire...\n");
  pipewire_cleanup (ctx);

  g_print ("[INFO] Freeing app context...\n");
  app_context_free (ctx);

  gst_deinit ();
  g_print ("[INFO] Done.\n");

  return 0;
}
