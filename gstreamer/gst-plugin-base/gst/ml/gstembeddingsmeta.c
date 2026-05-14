/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include "gstembeddingsmeta.h"

static gboolean
gst_embedding_meta_init (GstMeta * meta, gpointer params, GstBuffer * buffer)
{
  GstEmbeddingMeta *emeta = GST_EMBEDDINGS_META_CAST (meta);

  emeta->id = 0;
  emeta->parent_id = -1;
  emeta->embedding = NULL;
  emeta->type = GST_ML_TYPE_UNKNOWN;
  emeta->n_dims = 0;

  return TRUE;
}

static void
gst_embedding_meta_free (GstMeta * meta, GstBuffer * buffer)
{
  GstEmbeddingMeta *emeta = GST_EMBEDDINGS_META_CAST (meta);

  if (emeta->embedding != NULL) {
    g_array_free (emeta->embedding, TRUE);
    emeta->embedding = NULL;
  }
}

static gboolean
gst_embedding_meta_transform (GstBuffer * transbuffer, GstMeta * meta,
    GstBuffer * buffer, GQuark type, gpointer data)
{
  GstEmbeddingMeta *dmeta = NULL, *smeta = NULL;

  if (!GST_META_TRANSFORM_IS_COPY (type)) {
    // Return FALSE, if transform type is not supported.
    return FALSE;
  }

  smeta = GST_EMBEDDINGS_META_CAST (meta);
  dmeta = gst_buffer_add_embedding_meta (transbuffer,
      g_array_copy (smeta->embedding), smeta->type, smeta->n_dims, smeta->dims);

  if (NULL == dmeta)
    return FALSE;

  dmeta->id = smeta->id;
  dmeta->parent_id = smeta->parent_id;

  GST_DEBUG ("Duplicate Embedding metadata");
  return TRUE;
}

GType
gst_embedding_meta_api_get_type (void)
{
  static GType gtype = 0;
  static const gchar *tags[] = { GST_META_TAG_MEMORY_STR, NULL };

  if (g_once_init_enter (&gtype)) {
    GType type = gst_meta_api_type_register ("GstEmbeddingMetaAPI", tags);
    g_once_init_leave (&gtype, type);
  }
  return gtype;
}

const GstMetaInfo *
gst_embedding_meta_get_info (void)
{
  static const GstMetaInfo *minfo = NULL;

  if (g_once_init_enter ((GstMetaInfo **) &minfo)) {
    const GstMetaInfo *info = gst_meta_register (
        GST_EMBEDDINGS_META_API_TYPE, "GstEmbeddingMeta",
        sizeof (GstEmbeddingMeta), gst_embedding_meta_init,
        gst_embedding_meta_free, gst_embedding_meta_transform);

    g_once_init_leave ((GstMetaInfo **) &minfo, (GstMetaInfo *) info);
  }
  return minfo;
}

GstEmbeddingMeta *
gst_buffer_add_embedding_meta (GstBuffer * buffer, GArray * embedding,
    GstMLType type, guint n_dims, guint * dims)
{
  g_return_val_if_fail (embedding != NULL, NULL);

  GstEmbeddingMeta *outmeta;
  guint idx = 0;

  outmeta = GST_EMBEDDINGS_META_CAST (
      gst_buffer_add_meta (buffer, GST_EMBEDDINGS_META_INFO, NULL));

  if (NULL == outmeta) {
    GST_ERROR ("Failed to add Embedding meta to buffer %p!", buffer);
    return NULL;
  }

  outmeta->embedding = embedding;
  outmeta->type = type;
  outmeta->n_dims = n_dims;

  for (idx = 0; idx < n_dims; ++idx)
    outmeta->dims[idx] = dims[idx];

  return outmeta;
}

GstEmbeddingMeta *
gst_buffer_get_embedding_meta (GstBuffer * buffer)
{
  const GstMetaInfo *info = GST_EMBEDDINGS_META_INFO;
  gpointer state = NULL;
  GstMeta *meta = NULL;
  GstEmbeddingMeta *outmeta = NULL, *emeta = NULL;

  while ((meta = gst_buffer_iterate_meta (buffer, &state))) {
    if (meta->info->api == info->api) {
      emeta = GST_EMBEDDINGS_META_CAST (meta);

      if (emeta->id == 0)
        return emeta;

      if (outmeta == NULL || emeta->id < outmeta->id)
        outmeta = emeta;
    }
  }
  return outmeta;
}

GstEmbeddingMeta *
gst_buffer_get_embedding_meta_id (GstBuffer * buffer, guint id)
{
  const GstMetaInfo *info = GST_EMBEDDINGS_META_INFO;
  gpointer state = NULL;
  GstMeta *meta = NULL;

  while ((meta = gst_buffer_iterate_meta (buffer, &state))) {
    if (meta->info->api == info->api) {
      if (GST_EMBEDDINGS_META_CAST (meta)->id == id)
        return GST_EMBEDDINGS_META_CAST (meta);
    }
  }
  return NULL;
}

GList *
gst_buffer_get_embedding_metas_parent_id (GstBuffer * buffer, const gint parent_id)
{
  GList *metalist = NULL;
  gpointer state = NULL;
  GstMeta *meta = NULL;

  while ((meta = gst_buffer_iterate_meta (buffer, &state))) {
    if (meta->info->api != GST_EMBEDDINGS_META_API_TYPE)
      continue;

    if (GST_EMBEDDINGS_META_CAST (meta)->parent_id == parent_id)
      metalist = g_list_prepend (metalist, meta);
  }
  return metalist;
}
