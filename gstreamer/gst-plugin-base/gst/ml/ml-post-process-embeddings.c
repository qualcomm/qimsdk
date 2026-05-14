/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include "ml-post-process-embeddings.h"

/**
 * GstMLEmbeddings:
 * @refcount: thread safe reference counter
 * @entries: (element-type GstMLEmbedding): A #GArray of #GstMLEmbedding
 *
 * Information describing a group of prediction results beloging to the same batch.
 */
struct _GstMLEmbeddings {
  gatomicrefcount refcount;
  GArray          *entries;
};

G_DEFINE_BOXED_TYPE (GstMLEmbedding, gst_ml_embedding,
    (GBoxedCopyFunc) gst_ml_embedding_copy,
    (GBoxedFreeFunc) gst_ml_embedding_free);

G_DEFINE_BOXED_TYPE (GstMLEmbeddings, gst_ml_embeddings,
    (GBoxedCopyFunc) gst_ml_embeddings_ref,
    (GBoxedFreeFunc) gst_ml_embeddings_unref);


void
gst_ml_embedding_reset (GstMLEmbedding * embedding)
{
  g_return_if_fail (embedding != NULL);

  embedding->n_dims = 0;
  embedding->type = GST_ML_TYPE_UNKNOWN;

  g_clear_pointer (&embedding->embedding, g_array_unref);
  memset (embedding->dims, 0, sizeof(embedding->dims));
}

GstMLEmbedding*
gst_ml_embedding_copy (const GstMLEmbedding * embedding)
{
  GstMLEmbedding *newembedding = NULL;
  guint i = 0;

  g_return_val_if_fail (embedding != NULL, NULL);

  newembedding = g_slice_new (GstMLEmbedding);

  newembedding->embedding = g_array_copy (embedding->embedding);
  newembedding->type = embedding->type;
  newembedding->n_dims = embedding->n_dims;

  for (i = 0; i < embedding->n_dims; ++i)
    newembedding->dims[i] = embedding->dims[i];

  return newembedding;
}

void
gst_ml_embedding_free (GstMLEmbedding * embedding)
{
  if (embedding == NULL)
    return;

  gst_ml_embedding_reset (embedding);
  g_slice_free (GstMLEmbedding, embedding);
}

GstStructure *
gst_ml_embedding_to_structure (GstMLEmbedding * embedding)
{
  GstStructure *structure = NULL;
  const guchar *data = NULL;
  GValue value = G_VALUE_INIT, vdims = G_VALUE_INIT;
  guint idx = 0, size = 0;

  structure = gst_structure_new ("embedding",
    "type", G_TYPE_STRING, gst_ml_type_to_string (embedding->type),
    "num-dims", G_TYPE_UINT, embedding->n_dims, NULL);

  g_value_init (&value, G_TYPE_STRING);

  data = (const guchar*) embedding->embedding->data;
  size = embedding->embedding->len * gst_ml_type_get_size (embedding->type);

  g_value_take_string (&value, g_base64_encode (data, size));
  gst_structure_take_value (structure, "data", &value);

  g_value_init (&vdims, GST_TYPE_ARRAY);
  g_value_init (&value, G_TYPE_UINT);

  for (idx = 0; idx < embedding->n_dims; ++idx) {
    g_value_set_uint (&value, embedding->dims[idx]);
    gst_value_array_append_value(&vdims, &value);
  }

  g_value_unset (&value);
  gst_structure_take_value (structure, "dimensions", &vdims);

  gst_ml_embedding_reset (embedding);
  return structure;
}

GstMLEmbeddings*
gst_ml_embeddings_new (void)
{
  GstMLEmbeddings *embeddings = g_slice_new (GstMLEmbeddings);

  g_atomic_ref_count_init (&embeddings->refcount);
  embeddings->entries =
      g_array_new (FALSE, TRUE, sizeof (GstMLEmbedding));

  g_array_set_clear_func (embeddings->entries,
      (GDestroyNotify) gst_ml_embedding_reset);

  return embeddings;
}

GstMLEmbeddings*
gst_ml_embeddings_new_sized (guint size)
{
  GstMLEmbeddings *embeddings = g_slice_new (GstMLEmbeddings);

  g_atomic_ref_count_init (&embeddings->refcount);
  embeddings->entries =
      g_array_sized_new (FALSE, TRUE, sizeof (GstMLEmbedding), size);

  g_array_set_clear_func (embeddings->entries,
      (GDestroyNotify) gst_ml_embedding_reset);
  g_array_set_size (embeddings->entries, size);

  return embeddings;
}

GstMLEmbeddings*
gst_ml_embeddings_ref (GstMLEmbeddings * embeddings)
{
  g_return_val_if_fail (embeddings != NULL, NULL);
  g_atomic_ref_count_inc (&embeddings->refcount);

  return embeddings;
}

void
gst_ml_embeddings_unref (GstMLEmbeddings * embeddings)
{
  g_return_if_fail (embeddings != NULL);

  if (g_atomic_ref_count_dec (&embeddings->refcount)) {
    g_array_free (embeddings->entries, TRUE);
    g_slice_free (GstMLEmbeddings, embeddings);
  }
}

GstMLEmbeddings *
gst_ml_embeddings_copy (const GstMLEmbeddings * embeddings)
{
  GstMLEmbeddings *newembeddings = NULL;

  g_return_val_if_fail (embeddings != NULL, NULL);

  newembeddings = g_slice_new (GstMLEmbeddings);
  newembeddings->entries = g_array_copy (embeddings->entries);
  g_atomic_ref_count_init (&newembeddings->refcount);

  return newembeddings;
}

void
gst_ml_embeddings_append (GstMLEmbeddings * embeddings,
                               const GstMLEmbedding * embedding)
{
  g_return_if_fail (embeddings != NULL);
  g_array_append_vals (embeddings->entries, embedding, 1);
}

void
gst_ml_embeddings_insert (GstMLEmbeddings * embeddings, guint index,
                               const GstMLEmbedding * embedding)
{
  g_return_if_fail (embeddings != NULL);
  g_array_insert_vals (embeddings->entries, index, embedding, 1);
}

void
gst_ml_embeddings_remove (GstMLEmbeddings * embeddings, guint index)
{
  g_return_if_fail (embeddings != NULL);
  g_array_remove_index (embeddings->entries, index);
}

GstMLEmbedding*
gst_ml_embeddings_entry (GstMLEmbeddings * embeddings, guint index)
{
  g_return_val_if_fail (embeddings != NULL, NULL);
  return &(g_array_index (embeddings->entries, GstMLEmbedding, index));
}

guint
gst_ml_embeddings_size (GstMLEmbeddings * embeddings)
{
  g_return_val_if_fail (embeddings != NULL, 0);
  return embeddings->entries->len;
}

void
gst_ml_embeddings_resize (GstMLEmbeddings * embeddings, guint size)
{
  g_return_if_fail (embeddings != NULL);
  g_array_set_size (embeddings->entries, size);
}
