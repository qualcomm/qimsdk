/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifndef __GST_QTI_ML_POST_PROCESS_EMBEDDING_H__
#define __GST_QTI_ML_POST_PROCESS_EMBEDDING_H__

#include <gst/gst.h>
#include <gst/ml/ml-type.h>

G_BEGIN_DECLS

#define GST_ML_EMBEDDINGS_CAST(obj)      ((GstMLEmbeddings*)(obj))

typedef struct _GstMLEmbedding GstMLEmbedding;
typedef struct _GstMLEmbeddings GstMLEmbeddings;

#define GST_TYPE_ML_EMBEDDING            (gst_ml_embedding_get_type ())
GST_API GType gst_ml_embedding_get_type  (void);

#define GST_TYPE_ML_EMBEDDINGS           (gst_ml_embeddings_get_type ())
GST_API GType gst_ml_embeddings_get_type (void);

/**
 * GstMLEmbedding:
 * @embedding: #GArray Embedding vector.
 * @type: Type of the vector data.
 * @n_dims: Number of vector dimensions.
 * @dims: (array fixed-size=GST_ML_TENSOR_MAX_DIMS) (element-type guint):
 *        The dimensions of the @embedding vector
 *
 * Information describing embedding result from embedding models.
 */
struct _GstMLEmbedding {
  GArray *embedding;

  GstMLType type;
  guint n_dims;
  guint dims[GST_ML_TENSOR_MAX_DIMS];
};

/**
 * gst_ml_embedding_reset:
 * @embedding: A #GstMLEmbedding
 *
 * Helper function for freeing any allocated resources owned by the class.
 */
GST_API void
gst_ml_embedding_reset (GstMLEmbedding * embedding);

/**
 * gst_ml_embedding_copy:
 * @embedding: A #GstMLEmbedding
 *
 * Copy a GstMLEmbedding structure.
 *
 * Returns: (transfer full): A new #GstMLEmbedding.
 */
GST_API GstMLEmbedding *
gst_ml_embedding_copy (const GstMLEmbedding * embedding);

/**
 * gst_ml_embedding_free:
 * @embedding: A #GstMLEmbedding
 *
 * Free a GstMLEmbedding structure previously allocated with
 * gst_ml_embedding_copy().
 */
GST_API void
gst_ml_embedding_free (GstMLEmbedding * embedding);

/**
 * gst_ml_embedding_to_structure:
 * @embedding: A #GstMLEmbedding
 *
 * Converts GstMLEmbedding to a GstStructure representation.
 * All internal fields are reset and allocated memory freed.
 *
 * Returns: (transfer full): A new #GstStructure.
 */
GST_API GstStructure *
gst_ml_embedding_to_structure (GstMLEmbedding * embedding);

/**
 * gst_ml_embeddings_new: (constructor)
 *
 * Allocate a new #GstMLEmbeddings that is also initialized.
 *
 * Returns: (transfer full): A new #GstMLEmbeddings.
 */
GST_API GstMLEmbeddings*
gst_ml_embeddings_new (void);

/**
 * gst_ml_embeddings_new_sized: (constructor)
 * @size: number of elements preallocated
 *
 * Allocate a new #GstMLEmbeddings with @size elements preallocated.
 *
 * Returns: (transfer full): A new #GstMLEmbeddings.
 */

GST_API GstMLEmbeddings*
gst_ml_embeddings_new_sized (guint size);

/**
 * gst_ml_embeddings_ref:
 * @embeddings: A #GstMLEmbeddings
 *
 * Atomically increments the reference count of @embeddings by one.
 * This function is thread-safe and may be called from any thread.
 *
 * Returns: (transfer full): The passed in `GstMLEmbeddings`
 */
GST_API GstMLEmbeddings*
gst_ml_embeddings_ref (GstMLEmbeddings * embeddings);

/**
 * gst_ml_embeddings_unref:
 * @embeddings: (transfer full): A #GstMLEmbeddings
 *
 * Atomically decrements the reference count of @embeddings by one. If the
 * reference count drops to 0, free the GstMLEmbeddings.
 * This function is thread-safe and may be called from any thread.
 */
GST_API void
gst_ml_embeddings_unref (GstMLEmbeddings * embeddings);

/**
 * gst_ml_embeddings_copy:
 * @embeddings: A #GstMLEmbeddings
 *
 * Copy a GstMLEmbeddings structure.
 *
 * Returns: (transfer full): A new #GstMLEmbeddings.
 */
GST_API GstMLEmbeddings *
gst_ml_embeddings_copy (const GstMLEmbeddings * embeddings);

/**
 * gst_ml_embeddings_append:
 * @embeddings: A #GstMLEmbeddings
 * @embedding: A #GstMLEmbedding
 *
 * Adds the value on to the end of the GstMLEmbeddings list.
 * The list will grow in size automatically if necessary.
 */
GST_API void
gst_ml_embeddings_append (GstMLEmbeddings * embeddings,
                               const GstMLEmbedding * embedding);

/**
 * gst_ml_embeddings_insert:
 * @embeddings: A #GstMLEmbeddings
 * @index: the index at which to insert the new element
 * @embedding: A #GstMLEmbedding
 *
 * Insert element into a GstMLEmbeddings at the given index.
 * The list will grow in size automatically if necessary.
 */
GST_API void
gst_ml_embeddings_insert (GstMLEmbeddings * embeddings, guint index,
                               const GstMLEmbedding * embedding);

/**
 * gst_ml_embeddings_remove:
 * @embeddings: A #GstMLEmbeddings
 * @index: the index of the element to remove
 *
 * Removes the element at the given index from the embeddings list.
 * The following elements are moved down one place.
 */
GST_API void
gst_ml_embeddings_remove (GstMLEmbeddings * embeddings, guint index);

/**
 * gst_ml_embeddings_entry:
 * @embeddings: A #GstMLEmbeddings
 * @index: the index of the element to return
 *
 * Returns: (transfer none): the #GstMLEmbedding at the given index.
 */
GST_API GstMLEmbedding*
gst_ml_embeddings_entry (GstMLEmbeddings * embeddings, guint index);

/**
 * gst_ml_embeddings_size:
 * @embeddings: A #GstMLEmbeddings
 *
 * Returns: number of elements in A #GstMLEmbeddings
 */
GST_API guint
gst_ml_embeddings_size (GstMLEmbeddings * embeddings);

/**
 * gst_ml_embeddings_resize:
 * @embeddings: A #GstMLEmbeddings
 * @size: the new size of the GstMLEmbeddings list
 *
 * Sets the size of the array, expanding it if necessary.
 */
GST_API void
gst_ml_embeddings_resize (GstMLEmbeddings * embeddings, guint size);

G_END_DECLS

#endif // __GST_QTI_ML_POST_PROCESS_EMBEDDING_H__
