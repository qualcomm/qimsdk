/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifndef __GST_EMBEDDINGS_META_H__
#define __GST_EMBEDDINGS_META_H__

#include <gst/gst.h>
#include <gst/ml/ml-type.h>

G_BEGIN_DECLS

#define GST_EMBEDDINGS_META_API_TYPE  (gst_embedding_meta_api_get_type())
#define GST_EMBEDDINGS_META_INFO      (gst_embedding_meta_get_info())
#define GST_EMBEDDINGS_META_CAST(obj) ((GstEmbeddingMeta *) obj)

typedef struct _GstEmbeddingMeta GstEmbeddingMeta;

/**
 * GstEmbeddingMeta:
 * @meta: Parent #GstMeta
 * @id: ID corresponding to the memory index inside GstBuffer.
 * @parent_id: Identifier of its parent ROI, used when this meta was derived.
 * @embedding: #GArray Embedding vector.
 * @type: ML data type
 * @n_dims: Number of tensor dimensions
 * @dims: (array fixed-size=GST_ML_TENSOR_MAX_DIMS) (element-type guint):
 *        The dimensions of the @embedding vector
 *
 * Extra buffer metadata describing embeddings.
 */
struct _GstEmbeddingMeta {
  GstMeta   meta;

  guint     id;
  gint      parent_id;

  GArray    *embedding;

  // Tensor parameters
  GstMLType type;
  guint     n_dims;
  guint     dims[GST_ML_TENSOR_MAX_DIMS];
};

GST_API GType
gst_embedding_meta_api_get_type (void);

GST_API const GstMetaInfo *
gst_embedding_meta_get_info (void);

/**
 * gst_buffer_add_embedding_meta:
 * @buffer: A #GstBuffer
 * @embedding: Embedding vector data.
 * @type: Type of vector data.
 * @n_dims: Number of vector dimensions.
 * @dims: The dimensions of the @embedding vector.
 *
 * Attaches GstEmbeddingMeta metadata to @buffer with the given parameters.
 *
 * Returns: (transfer none): the #GstEmbeddingMeta on @buffer.
 */
GST_API GstEmbeddingMeta *
gst_buffer_add_embedding_meta (GstBuffer * buffer, GArray * embedding,
                                GstMLType type, guint n_dims, guint * dims);

/**
 * gst_buffer_get_embedding_meta:
 * @buffer: A #GstBuffer
 *
 * Find the #GstEmbeddingMeta on @buffer with the lowest @id.
 *
 * Buffers can contain multiple #GstEmbeddingMeta metadata items.
 *
 * Returns: (transfer none) (nullable): the #GstEmbeddingMeta with lowest id
 *          (usually 0) or %NULL when there is no such metadata on @buffer.
 */
GST_API GstEmbeddingMeta *
gst_buffer_get_embedding_meta (GstBuffer * buffer);

/**
 * gst_buffer_get_embedding_meta_id:
 * @buffer: A #GstBuffer
 * @id: A metadata id
 *
 * Find the #GstEmbeddingMeta on @buffer with the given @id.
 *
 * Buffers can contain multiple #GstEmbeddingMeta metadata items.
 *
 * Returns: (transfer none) (nullable): the #GstEmbeddingMeta with @id or
 *          %NULL when there is no such metadata on @buffer.
 */
GST_API GstEmbeddingMeta *
gst_buffer_get_embedding_meta_id (GstBuffer * buffer, guint id);

/**
 * gst_buffer_get_embedding_metas_parent_id:
 * @buffer: A #GstBuffer
 * @parent_id: A parent metadata id
 *
 * Find the #GstEmbeddingMeta on @buffer with the given @parent_id.
 *
 * Buffers can contain multiple #GstEmbeddingMeta metadata items.
 *
 * Returns: (transfer full) (element-type GstEmbeddingMeta) (nullable):
 *          list of #GstEmbeddingMeta with @parent_id or %NULL when there
 *          is no such metadata on @buffer.
 */
GST_API GList *
gst_buffer_get_embedding_metas_parent_id (GstBuffer * buffer, const gint parent_id);

G_END_DECLS

#endif /* __GST_EMBEDDINGS_META_H__ */
