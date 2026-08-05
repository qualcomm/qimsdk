/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifndef __GST_VIDEO_CONVERTER_ENGINE_H__
#define __GST_VIDEO_CONVERTER_ENGINE_H__

#include <gst/video/video.h>
#include <gst/allocators/allocators.h>
#include <gst/video/video-utils.h>
#include <gst/video/video-converter-engine-param.h>

G_BEGIN_DECLS

typedef struct _GstVideoConverterEngine GstVideoConverterEngine;

#define GST_TYPE_VIDEO_CONVERTER_ENGINE (gst_video_converter_engine_get_type())
GST_VIDEO_API GType gst_video_converter_engine_get_type (void);

/**
 * GstVideoConverterBackend:
 * @GST_VIDEO_CONVERTER_BACKEND_NONE: Do not use any backend
 * @GST_VIDEO_CONVERTER_BACKEND_C2D: Use C2D based video converter.
 * @GST_VIDEO_CONVERTER_BACKEND_GLES: Use OpenGLES based video converter.
 * @GST_VIDEO_CONVERTER_BACKEND_FCV: Use FastCV based video converter.
 * @GST_VIDEO_CONVERTER_BACKEND_OCV: Use OpenCV based video converter.
 *
 * The backend of the video converter engine.
 */
typedef enum {
  GST_VIDEO_CONVERTER_BACKEND_NONE,
  GST_VIDEO_CONVERTER_BACKEND_C2D,
  GST_VIDEO_CONVERTER_BACKEND_GLES,
  GST_VIDEO_CONVERTER_BACKEND_FCV,
  GST_VIDEO_CONVERTER_BACKEND_OCV,
} GstVideoConverterBackend;

GST_VIDEO_API GType gst_video_converter_backend_get_type (void);
#define GST_TYPE_VIDEO_CONVERTER_BACKEND (gst_video_converter_backend_get_type())

/**
 * gst_video_converter_default_backend:
 *
 * Retrieve the default vide converter backend.
 *
 * Returns: the default #GstVideoConverterBackend
 */
GST_VIDEO_API GstVideoConverterBackend
gst_video_converter_default_backend (void);

/**
 * gst_video_converter_engine_new:
 * @backend: The type of the underlying converter.
 * @settings: (nullable): A #GstStructure with backend specific options.
 *
 * Initialize instance of video converter engine.
 *
 * Returns: (transfer full) (nullable): A new #GstVideoConverterEngine or NULL
 * on failure
 */
GST_VIDEO_API GstVideoConverterEngine *
gst_video_converter_engine_new (GstVideoConverterBackend backend,
                                GstStructure * settings);

/**
 * gst_video_converter_engine_ref: (skip)
 * @engine: (transfer none): A #GstVideoConverterEngine
 *
 * Atomically increments the reference count of @engine by one.
 *
 * Returns: (transfer none): A pointer to the object passed in @engine
 */
GST_VIDEO_API GstVideoConverterEngine *
gst_video_converter_engine_ref (GstVideoConverterEngine * engine);

/**
 * gst_video_converter_engine_unref: (skip)
 * @engine: (transfer none): A #GstVideoConverterEngine
 *
 * Atomically decrements the reference count of @engine by one. If the
 * reference count drops to 0, the engine and its backend are freed.
 */
GST_VIDEO_API void
gst_video_converter_engine_unref (GstVideoConverterEngine * engine);

/**
 * gst_video_converter_engine_compose:
 * @engine: Pointer to video converter engine.
 * @compositions: (array length=n_compositions): Array of composition frames.
 * @n_compositions: Number of compositions.
 * @fence: (skip): Optional fence to be filled if provided and used for async
 *         operation.
 *
 * Submit the a number of video composition which will be executed together.
 *
 * An optional fence object may be passed to be filled by the engine in which
 * case the compsition operations will be performed asynchronously. If the
 * fence object is left NULL then the operation is performed synchronously.
 *
 * Returns: TRUE on success or FALSE on failure
 */
GST_VIDEO_API gboolean
gst_video_converter_engine_compose (GstVideoConverterEngine * engine,
                                    GstVideoComposition ** compositions,
                                    guint n_compositions, gpointer * fence);

/**
 * gst_video_converter_engine_wait_fence:
 * @engine: Pointer to video converter engine.
 * @fence: (skip): Asynchronously fence object associated with a compose request.
 *
 * Wait for the sumbitted to the engine compositions to finish.
 *
 * Returns: TRUE on success or FALSE on failure
 */
GST_VIDEO_API gboolean
gst_video_converter_engine_wait_fence (GstVideoConverterEngine * engine,
                                       gpointer fence);

/**
 * gst_video_converter_engine_flush:
 * @engine: Pointer to video converter engine.
 *
 * Wait for compositions sumbitted to the engine to finish and flush cached data.
 */
GST_VIDEO_API void
gst_video_converter_engine_flush (GstVideoConverterEngine * engine);

G_END_DECLS

#endif // __GST_VIDEO_CONVERTER_ENGINE_H__
