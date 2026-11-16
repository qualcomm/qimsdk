/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstgeniex.h"

#include <string.h>

#include <geniex.h>
#include <geniex_model.h>
#include <glib/gstdio.h>

#define GST_CAT_DEFAULT gst_geniex_debug
#define GST_GENIEX_IMAGE_CAPS "image/jpeg"
#define GST_GENIEX_TEXT_CAPS "text/x-raw, format=(string) utf8"

GST_DEBUG_CATEGORY_STATIC (gst_geniex_debug);

struct _GstGenieX {
  GstElement parent;
  GstPad *sinkpad;
  GstPad *srcpad;
  GMutex lock;
  geniex_VLM *vlm;
  gboolean runtime_acquired;
  gchar *model_id;
  gchar *prompt;
  gchar *device;
};

G_DEFINE_TYPE (GstGenieX, gst_geniex, GST_TYPE_ELEMENT);

typedef enum {
  PROP_0,
  PROP_MODEL,
  PROP_PROMPT,
  PROP_DEVICE,
  N_PROPERTIES
} GstGenieXProperty;

static GParamSpec *properties[N_PROPERTIES];

G_LOCK_DEFINE_STATIC (geniex_runtime);
static guint geniex_runtime_users;
static gboolean geniex_runtime_owned;

G_LOCK_DEFINE_STATIC (geniex_model_manager);
static gboolean geniex_model_manager_ready;

static GstStaticPadTemplate sink_template =
GST_STATIC_PAD_TEMPLATE ("sink", GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_GENIEX_IMAGE_CAPS));

static GstStaticPadTemplate src_template =
GST_STATIC_PAD_TEMPLATE ("src", GST_PAD_SRC, GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_GENIEX_TEXT_CAPS));

/* Helpers: GenieX logging, error formatting, model IDs, and temporary images. */
static const gchar *
gst_geniex_error_message (gint32 code)
{
  const gchar *message = geniex_get_error_message ((geniex_ErrorCode) code);
  return message != NULL ? message : "Unknown GenieX error";
}

static void
gst_geniex_log (geniex_LogLevel level, const gchar *message)
{
  if (message == NULL)
    return;

  switch (level) {
    case GENIEX_LOG_LEVEL_TRACE:
      GST_CAT_TRACE (gst_geniex_debug, "%s", message);
      break;
    case GENIEX_LOG_LEVEL_DEBUG:
      GST_CAT_DEBUG (gst_geniex_debug, "%s", message);
      break;
    case GENIEX_LOG_LEVEL_INFO:
      GST_CAT_INFO (gst_geniex_debug, "%s", message);
      break;
    case GENIEX_LOG_LEVEL_WARN:
      GST_CAT_WARNING (gst_geniex_debug, "%s", message);
      break;
    case GENIEX_LOG_LEVEL_ERROR:
      GST_CAT_ERROR (gst_geniex_debug, "%s", message);
      break;
    default:
      GST_CAT_WARNING (gst_geniex_debug,
          "Unknown GenieX log level %d: %s", level, message);
      break;
  }
}

static geniex_LogLevel
gst_geniex_min_log_level (void)
{
  GstDebugLevel threshold = gst_debug_category_get_threshold (gst_geniex_debug);

  if (threshold >= GST_LEVEL_TRACE)
    return GENIEX_LOG_LEVEL_TRACE;
  if (threshold >= GST_LEVEL_DEBUG)
    return GENIEX_LOG_LEVEL_DEBUG;
  if (threshold >= GST_LEVEL_INFO)
    return GENIEX_LOG_LEVEL_INFO;
  if (threshold >= GST_LEVEL_WARNING)
    return GENIEX_LOG_LEVEL_WARN;
  return GENIEX_LOG_LEVEL_ERROR;
}

static gboolean
gst_geniex_hub_uses_explicit_tag (const gchar *model_id)
{
  return g_str_has_prefix (model_id, "qualcomm/") ||
      g_str_has_prefix (model_id, "qai-hub-models/") ||
      g_str_has_prefix (model_id, "docker.io/") ||
      g_str_has_prefix (model_id, "index.docker.io/") ||
      g_str_has_prefix (model_id, "https://hub.docker.com/r/");
}

static gchar *
gst_geniex_mm_error (const gchar *operation, gint32 rc)
{
  const gchar *detail = geniex_model_last_error_message ();
  return g_strdup_printf ("%s: %s (%d)%s%s", operation,
      gst_geniex_error_message (rc), rc,
      detail != NULL ? ": " : "", detail != NULL ? detail : "");
}

static gboolean
gst_geniex_write_image (const guint8 *data, gsize size,
    gchar **directory, gchar **path, gchar **error)
{
  GError *io_error = NULL;

  *directory = g_dir_make_tmp ("gst-geniex-XXXXXX", &io_error);
  if (*directory == NULL) {
    *error = g_strdup (io_error->message);
    g_clear_error (&io_error);
    return FALSE;
  }

  *path = g_build_filename (*directory, "image.jpg", NULL);
  if (!g_file_set_contents (*path, (const gchar *) data, (gssize) size,
          &io_error)) {
    *error = g_strdup (io_error->message);
    g_clear_error (&io_error);
    return FALSE;
  }
  return TRUE;
}

static void
gst_geniex_remove_image (gchar *directory, gchar *path)
{
  if (path != NULL)
    g_remove (path);
  if (directory != NULL)
    g_rmdir (directory);
  g_free (path);
  g_free (directory);
}

/* GenieX runtime and model management. */
static gboolean
gst_geniex_runtime_acquire (void)
{
  gint32 rc = GENIEX_SUCCESS;

  G_LOCK (geniex_runtime);
  if (geniex_runtime_users == 0) {
    rc = geniex_set_log (gst_geniex_log);
    if (rc != GENIEX_SUCCESS) {
      G_UNLOCK (geniex_runtime);
      GST_ERROR ("Failed to install GenieX log callback: %s (%d)",
          gst_geniex_error_message (rc), rc);
      return FALSE;
    }
    rc = geniex_set_log_level (gst_geniex_min_log_level ());
    if (rc != GENIEX_SUCCESS) {
      G_UNLOCK (geniex_runtime);
      GST_ERROR ("Failed to set GenieX log level: %s (%d)",
          gst_geniex_error_message (rc), rc);
      return FALSE;
    }
    rc = geniex_init ();
    if (rc != GENIEX_SUCCESS &&
        rc != GENIEX_ERROR_COMMON_ALREADY_INITIALIZED) {
      G_UNLOCK (geniex_runtime);
      GST_ERROR ("GenieX initialization failed: %s (%d)",
          gst_geniex_error_message (rc), rc);
      return FALSE;
    }
    geniex_runtime_owned = rc == GENIEX_SUCCESS;
  }
  geniex_runtime_users++;
  G_UNLOCK (geniex_runtime);
  return TRUE;
}

static void
gst_geniex_runtime_release (void)
{
  G_LOCK (geniex_runtime);
  if (geniex_runtime_users > 0)
    geniex_runtime_users--;
  if (geniex_runtime_users == 0 && geniex_runtime_owned) {
    gint32 rc = geniex_deinit ();
    if (rc != GENIEX_SUCCESS)
      GST_WARNING ("GenieX deinitialization failed: %s (%d)",
          gst_geniex_error_message (rc), rc);
    geniex_runtime_owned = FALSE;
  }
  G_UNLOCK (geniex_runtime);
}

static gboolean
gst_geniex_model_manager_init (gchar **error)
{
  gint32 rc = GENIEX_SUCCESS;

  G_LOCK (geniex_model_manager);
  if (!geniex_model_manager_ready) {
    rc = geniex_model_init (NULL);
    if (rc != GENIEX_SUCCESS &&
        rc != GENIEX_ERROR_COMMON_ALREADY_INITIALIZED) {
      const gchar *detail = geniex_model_last_error_message ();
      *error = g_strdup_printf ("GenieX model manager initialization failed: %s (%d)%s%s",
          gst_geniex_error_message (rc), rc,
          detail != NULL ? ": " : "", detail != NULL ? detail : "");
      G_UNLOCK (geniex_model_manager);
      return FALSE;
    }
    geniex_model_manager_ready = TRUE;
  }
  G_UNLOCK (geniex_model_manager);
  return TRUE;
}

static gboolean
gst_geniex_load_model (GstGenieX *self, gchar **error)
{
  geniex_ModelPaths paths = { 0 };
  geniex_VlmCreateInput input = { 0 };
  geniex_ResolveDeviceInput device_input = { 0 };
  geniex_ResolveDeviceOutput device_output = { 0 };
  geniex_VLM *vlm = NULL;
  gchar *resolved_id = NULL;
  gchar *pull_name = NULL;
  const gchar *model_id = self->model_id;
  const gchar *quant = NULL;
  gchar *separator;
  gint32 rc;

  if (!gst_geniex_model_manager_init (error))
    return FALSE;

  if (geniex_model_resolve_alias (self->model_id, &resolved_id) == GENIEX_SUCCESS &&
      resolved_id != NULL)
    model_id = resolved_id;

  rc = geniex_model_get_paths (model_id, &paths);
  if (rc == GENIEX_ERROR_COMMON_FILE_NOT_FOUND) {
    pull_name = g_strdup (model_id);
    separator = strrchr (pull_name, ':');
    if (separator != NULL) {
      *separator = '\0';
      quant = separator + 1;
    } else if (!gst_geniex_hub_uses_explicit_tag (model_id)) {
      quant = "Q4_0";
    }

    geniex_ModelPullInput pull = { 0 };
    pull.struct_size = sizeof (pull);
    pull.model_name = pull_name;
    pull.quant = quant;
    pull.hub = GENIEX_HUB_AUTO;
    pull.model_type = GENIEX_MODEL_TYPE_VLM;

    rc = geniex_model_pull (&pull);
    if (rc != GENIEX_SUCCESS) {
      *error = gst_geniex_mm_error ("Model download failed", rc);
      goto done;
    }

    rc = geniex_model_get_paths (model_id, &paths);
  }
  if (rc != GENIEX_SUCCESS) {
    *error = gst_geniex_mm_error ("Model lookup failed", rc);
    goto done;
  }
  if (paths.model_type != GENIEX_MODEL_TYPE_VLM ||
      (g_strcmp0 (paths.plugin_id, "llama_cpp") == 0 &&
       paths.mmproj_path == NULL)) {
    *error = g_strdup_printf ("Model '%s' is not a GenieX VLM model", model_id);
    goto done;
  }

  device_input.plugin_id = paths.plugin_id;
  device_input.model_name = paths.model_name != NULL ? paths.model_name : model_id;
  device_input.mode = self->device;
  device_input.ngl_default = -1;
  rc = geniex_resolve_device (&device_input, &device_output);
  if (rc != GENIEX_SUCCESS) {
    *error = g_strdup_printf ("Device resolution failed for '%s': %s (%d)",
        self->device, gst_geniex_error_message (rc), rc);
    goto done;
  }
  if (device_output.warning != NULL)
    GST_WARNING_OBJECT (self, "GenieX device resolver: %s", device_output.warning);

  input.model_path = paths.model_path;
  input.mmproj_path = paths.mmproj_path;
  input.tokenizer_path = paths.tokenizer_path;
  input.plugin_id = paths.plugin_id;
  input.device_id = device_output.device_id;
  input.config.n_gpu_layers = device_output.ngl;
  input.config.power_mode = GENIEX_POWER_MODE_BURST;

  rc = geniex_vlm_create (&input, &vlm);
  if (rc != GENIEX_SUCCESS || vlm == NULL) {
    *error = g_strdup_printf ("VLM creation failed: %s (%d)",
        gst_geniex_error_message (rc), rc);
    goto done;
  }

  self->vlm = vlm;
  GST_INFO_OBJECT (self, "Loaded GenieX VLM '%s'", model_id);

done:
  geniex_free (device_output.device_id);
  geniex_free (device_output.warning);
  geniex_model_paths_free (&paths);
  geniex_free (resolved_id);
  g_free (pull_name);
  return *error == NULL;
}

/* GStreamer element lifecycle and streaming callbacks. */
static gboolean
gst_geniex_start (GstGenieX *self)
{
  gchar *error = NULL;

  g_mutex_lock (&self->lock);
  if (self->model_id == NULL || self->model_id[0] == '\0') {
    error = g_strdup ("Set the model property to a GenieX VLM model ID");
    goto failed;
  }
  if (!gst_geniex_runtime_acquire ()) {
    error = g_strdup ("GenieX initialization failed; see the debug log");
    goto failed;
  }
  self->runtime_acquired = TRUE;
  if (!gst_geniex_load_model (self, &error))
    goto failed;

  g_mutex_unlock (&self->lock);
  return TRUE;

failed:
  if (self->runtime_acquired) {
    self->runtime_acquired = FALSE;
    gst_geniex_runtime_release ();
  }
  g_mutex_unlock (&self->lock);
  GST_ELEMENT_ERROR (self, RESOURCE, FAILED,
      ("Failed to load GenieX VLM"), ("%s", error));
  g_free (error);
  return FALSE;
}

static void
gst_geniex_stop (GstGenieX *self)
{
  g_mutex_lock (&self->lock);
  if (self->vlm != NULL) {
    gint32 rc = geniex_vlm_destroy (self->vlm);
    if (rc != GENIEX_SUCCESS)
      GST_WARNING_OBJECT (self, "VLM destruction failed: %s (%d)",
          gst_geniex_error_message (rc), rc);
    self->vlm = NULL;
  }
  if (self->runtime_acquired) {
    self->runtime_acquired = FALSE;
    gst_geniex_runtime_release ();
  }
  g_mutex_unlock (&self->lock);
}

static GstFlowReturn
gst_geniex_chain (GstPad *pad, GstObject *parent, GstBuffer *input_buffer)
{
  GstGenieX *self = GST_GENIEX (parent);
  GstMapInfo map = GST_MAP_INFO_INIT;
  GstMapInfo output_map = GST_MAP_INFO_INIT;
  geniex_VlmContent contents[2] = { { 0 } };
  geniex_VlmChatMessage message = { 0 };
  geniex_VlmApplyChatTemplateInput template_input = { 0 };
  geniex_VlmApplyChatTemplateOutput template_output = { 0 };
  geniex_GenerationConfig config = { 0 };
  geniex_VlmGenerateInput generate_input = { 0 };
  geniex_VlmGenerateOutput generate_output = { 0 };
  geniex_Path image_path;
  gchar *temp_directory = NULL;
  gchar *temp_image = NULL;
  gchar *error = NULL;
  GstBuffer *output_buffer = NULL;
  GstClockTime pts = GST_BUFFER_PTS (input_buffer);
  GstClockTime dts = GST_BUFFER_DTS (input_buffer);
  GstClockTime duration = GST_BUFFER_DURATION (input_buffer);
  gsize output_size;
  gboolean locked = FALSE;
  GstFlowReturn flow = GST_FLOW_ERROR;
  gint32 rc;

  (void) pad;

  if (!gst_buffer_map (input_buffer, &map, GST_MAP_READ)) {
    error = g_strdup ("Failed to map JPEG input buffer");
    goto done;
  }
  if (map.size == 0 || map.size > G_MAXSSIZE) {
    error = g_strdup ("JPEG input buffer is empty or too large");
    goto done;
  }
  if (!gst_geniex_write_image (map.data, map.size,
          &temp_directory, &temp_image, &error))
    goto done;
  gst_buffer_unmap (input_buffer, &map);
  memset (&map, 0, sizeof (map));

  image_path = temp_image;
  contents[0].type = "text";
  contents[0].text = self->prompt;
  contents[1].type = "image";
  contents[1].text = temp_image;
  message.role = "user";
  message.contents = contents;
  message.content_count = G_N_ELEMENTS (contents);
  template_input.messages = &message;
  template_input.message_count = 1;

  g_mutex_lock (&self->lock);
  locked = TRUE;
  if (self->vlm == NULL) {
    error = g_strdup ("GenieX VLM is not loaded");
    goto done;
  }
  rc = geniex_vlm_reset (self->vlm);
  if (rc != GENIEX_SUCCESS) {
    error = g_strdup_printf ("VLM reset failed: %s (%d)",
        gst_geniex_error_message (rc), rc);
    goto done;
  }
  rc = geniex_vlm_apply_chat_template (self->vlm, &template_input,
      &template_output);
  if (rc != GENIEX_SUCCESS || template_output.formatted_text == NULL) {
    error = g_strdup_printf ("Chat template failed: %s (%d)",
        gst_geniex_error_message (rc), rc);
    goto done;
  }

  config.max_tokens = 128;
  config.image_paths = &image_path;
  config.image_count = 1;
  generate_input.prompt_utf8 = template_output.formatted_text;
  generate_input.config = &config;
  rc = geniex_vlm_generate (self->vlm, &generate_input, &generate_output);
  if (rc != GENIEX_SUCCESS) {
    error = g_strdup_printf ("VLM inference failed: %s (%d)",
        gst_geniex_error_message (rc), rc);
    goto done;
  }
  g_mutex_unlock (&self->lock);
  locked = FALSE;

  output_size = generate_output.full_text != NULL ?
      strlen (generate_output.full_text) : 0;
  output_buffer = gst_buffer_new_allocate (NULL, output_size, NULL);
  if (output_buffer == NULL) {
    error = g_strdup ("Failed to allocate output buffer");
    goto done;
  }
  if (output_size > 0) {
    if (!gst_buffer_map (output_buffer, &output_map, GST_MAP_WRITE)) {
      error = g_strdup ("Failed to map output buffer");
      goto done;
    }
    memcpy (output_map.data, generate_output.full_text, output_size);
    gst_buffer_unmap (output_buffer, &output_map);
  }

  GST_BUFFER_PTS (output_buffer) = pts;
  GST_BUFFER_DTS (output_buffer) = dts;
  GST_BUFFER_DURATION (output_buffer) = duration;
  flow = GST_FLOW_OK;

done:
  if (map.data != NULL)
    gst_buffer_unmap (input_buffer, &map);
  if (locked)
    g_mutex_unlock (&self->lock);
  if (template_output.formatted_text != NULL)
    geniex_free (template_output.formatted_text);
  if (generate_output.full_text != NULL)
    geniex_free (generate_output.full_text);
  gst_geniex_remove_image (temp_directory, temp_image);
  gst_buffer_unref (input_buffer);

  if (flow == GST_FLOW_ERROR) {
    if (output_buffer != NULL)
      gst_buffer_unref (output_buffer);
    if (GST_PAD_IS_FLUSHING (self->srcpad)) {
      flow = GST_FLOW_FLUSHING;
    } else {
      GST_ELEMENT_ERROR (self, STREAM, FAILED,
          ("GenieX VLM inference failed"), ("%s", error));
    }
    g_free (error);
    return flow;
  }

  g_free (error);
  return gst_pad_push (self->srcpad, output_buffer);
}

static gboolean
gst_geniex_sink_event (GstPad *pad, GstObject *parent, GstEvent *event)
{
  GstGenieX *self = GST_GENIEX (parent);
  (void) pad;
  return gst_pad_push_event (self->srcpad, event);
}

static GstStateChangeReturn
gst_geniex_change_state (GstElement *element, GstStateChange transition)
{
  GstGenieX *self = GST_GENIEX (element);
  GstStateChangeReturn result;

  if (transition == GST_STATE_CHANGE_NULL_TO_READY && !gst_geniex_start (self))
    return GST_STATE_CHANGE_FAILURE;

  result = GST_ELEMENT_CLASS (gst_geniex_parent_class)->change_state (
      element, transition);
  if (result == GST_STATE_CHANGE_FAILURE &&
      transition == GST_STATE_CHANGE_NULL_TO_READY)
    gst_geniex_stop (self);
  else if (transition == GST_STATE_CHANGE_READY_TO_NULL &&
      result != GST_STATE_CHANGE_FAILURE)
    gst_geniex_stop (self);

  return result;
}

/* GObject properties, type setup, and plugin registration. */
static void
gst_geniex_set_property (GObject *object, guint property_id,
    const GValue *value, GParamSpec *pspec)
{
  GstGenieX *self = GST_GENIEX (object);

  g_mutex_lock (&self->lock);
  if (self->vlm != NULL) {
    g_mutex_unlock (&self->lock);
    GST_WARNING_OBJECT (self, "Property '%s' cannot change while the VLM is loaded",
        pspec->name);
    return;
  }
  switch (property_id) {
    case PROP_MODEL:
      g_free (self->model_id);
      self->model_id = g_value_dup_string (value);
      break;
    case PROP_PROMPT:
      g_free (self->prompt);
      self->prompt = g_value_dup_string (value);
      break;
    case PROP_DEVICE:
      g_free (self->device);
      self->device = g_value_dup_string (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
  g_mutex_unlock (&self->lock);
}

static void
gst_geniex_get_property (GObject *object, guint property_id,
    GValue *value, GParamSpec *pspec)
{
  GstGenieX *self = GST_GENIEX (object);

  g_mutex_lock (&self->lock);
  switch (property_id) {
    case PROP_MODEL:
      g_value_set_string (value, self->model_id);
      break;
    case PROP_PROMPT:
      g_value_set_string (value, self->prompt);
      break;
    case PROP_DEVICE:
      g_value_set_string (value, self->device);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
  g_mutex_unlock (&self->lock);
}

static void
gst_geniex_finalize (GObject *object)
{
  GstGenieX *self = GST_GENIEX (object);

  gst_geniex_stop (self);
  g_free (self->model_id);
  g_free (self->prompt);
  g_free (self->device);
  g_mutex_clear (&self->lock);
  G_OBJECT_CLASS (gst_geniex_parent_class)->finalize (object);
}

static void
gst_geniex_class_init (GstGenieXClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);

  object_class->set_property = gst_geniex_set_property;
  object_class->get_property = gst_geniex_get_property;
  object_class->finalize = gst_geniex_finalize;

  properties[PROP_MODEL] = g_param_spec_string ("model", "Model ID",
      "GenieX model ID; downloaded automatically if it is not cached",
      NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  properties[PROP_PROMPT] = g_param_spec_string ("prompt", "Prompt",
      "Fixed prompt sent with each image", "Describe this image.",
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  properties[PROP_DEVICE] = g_param_spec_string ("device", "Device",
      "GenieX device alias or concrete device list (auto, cpu, gpu, npu, hybrid)",
      "auto", G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_properties (object_class, G_N_ELEMENTS (properties),
      properties);

  gst_element_class_set_static_metadata (element_class,
      "GenieX vision-language model", "Filter/AI",
      "Runs image understanding through the GenieX VLM API", "QTI");
  gst_element_class_add_static_pad_template (element_class, &sink_template);
  gst_element_class_add_static_pad_template (element_class, &src_template);
  element_class->change_state = GST_DEBUG_FUNCPTR (gst_geniex_change_state);

  GST_DEBUG_CATEGORY_INIT (gst_geniex_debug, "geniex", 0,
      "GenieX vision-language model element");
}

static void
gst_geniex_init (GstGenieX *self)
{
  g_mutex_init (&self->lock);
  self->prompt = g_strdup ("Describe this image.");
  self->device = g_strdup ("auto");
  self->sinkpad = gst_pad_new_from_static_template (&sink_template, "sink");
  gst_pad_set_chain_function (self->sinkpad, GST_DEBUG_FUNCPTR (gst_geniex_chain));
  gst_pad_set_event_function (self->sinkpad,
      GST_DEBUG_FUNCPTR (gst_geniex_sink_event));
  gst_element_add_pad (GST_ELEMENT (self), self->sinkpad);

  self->srcpad = gst_pad_new_from_static_template (&src_template, "src");
  gst_element_add_pad (GST_ELEMENT (self), self->srcpad);
}

static gboolean
plugin_init (GstPlugin *plugin)
{
  return gst_element_register (plugin, "geniex", GST_RANK_NONE,
      GST_TYPE_GENIEX);
}

GST_PLUGIN_DEFINE (
    GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    geniex,
    "GStreamer integration for GenieX vision-language models",
    plugin_init,
    PACKAGE_VERSION,
    PACKAGE_LICENSE,
    PACKAGE_SUMMARY,
    PACKAGE_ORIGIN
)
