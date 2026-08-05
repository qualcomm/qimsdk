/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <Python.h>
#include <pygobject-3.0/pygobject.h>
#include <gst/gst.h>
#include <gst/ml/ml-frame.h>
#include <gst/ml/ml-post-process-classification.h>
#include <gst/ml/ml-post-process-detection.h>
#include <gst/ml/ml-post-process-pose.h>
#include <gst/ml/ml-post-process-segmentation.h>
#include <gst/ml/ml-post-process-depth-map.h>
#include <gst/video/video.h>
#include <gst/video/video-utils.h>
#include <gst/video/video-converter-engine.h>

static PyObject *
_gst_ml_frame_get_memory_view (PyObject * self, PyObject * args)
{
  PyTypeObject *gst_ml_frame_type = NULL;
  PyObject *py_mlframe = NULL, *py_mview = NULL;
  GstMLFrame *mlframe = NULL;
  GstMapInfo *mapinfo = NULL;
  gint index = 0, flags = 0;
  gsize size = 0;

  // Look up GstQtiML.Frame and Gst.MapInfo index parameters
  gst_ml_frame_type = pygobject_lookup_class (gst_ml_frame_get_type ());
  if (!PyArg_ParseTuple (args, "O!i", gst_ml_frame_type, &py_mlframe, &index)) {
    PyErr_BadArgument ();
    return NULL;
  }

  // Extract GstMLFrame from Gst.MapInfo parameters
  mlframe = pyg_boxed_get (py_mlframe, GstMLFrame);

  if (index >= (gint) GST_ML_FRAME_N_TENSORS (mlframe))
    Py_RETURN_NONE;

  mapinfo = &(mlframe->mapinfo[index]);

  // Since Python does only support r/o or r/w it has to be changed to either.
  flags = (mapinfo->flags & GST_MAP_WRITE) ? PyBUF_WRITE : PyBUF_READ;

  // Take memoryview with size without the additional padding.
  size = gst_ml_info_tensor_size (&mlframe->info, index);

  if (size > mapinfo->size) {
    PyErr_SetString (PyExc_TypeError, "size mismatch!");
    return NULL;
  }

  py_mview =
      PyMemoryView_FromMemory ((char *) mapinfo->data, size, flags);

  return py_mview;
}

static PyObject *
_gst_ml_keypoint_get_name (PyObject * self, PyObject * args)
{
  PyTypeObject *py_keypoint_type = NULL;
  PyObject *py_keypoint = NULL;
  GstMLKeypoint *keypoint = NULL;

  // Look up GstQtiML.Keypoint parameter
  py_keypoint_type = pygobject_lookup_class (gst_ml_keypoint_get_type ());
  if (!PyArg_ParseTuple (args, "O!", py_keypoint_type, &py_keypoint)) {
    PyErr_BadArgument ();
    return NULL;
  }

  keypoint = pyg_boxed_get (py_keypoint, GstMLKeypoint);

  if (keypoint->name == 0)
    Py_RETURN_NONE;

  return PyUnicode_FromString (g_quark_to_string (keypoint->name));
}

static PyObject *
_gst_ml_keypoint_set_name (PyObject * self, PyObject * args)
{
  PyTypeObject *py_keypoint_type = NULL;
  PyObject *py_keypoint = NULL, *py_str = NULL;
  GstMLKeypoint *keypoint = NULL;

  // Look up GstQtiML.Keypoint and str parameters
  py_keypoint_type = pygobject_lookup_class (gst_ml_keypoint_get_type ());
  if (!PyArg_ParseTuple (args, "O!O", py_keypoint_type, &py_keypoint, &py_str)) {
    PyErr_BadArgument ();
    return NULL;
  }

  if (py_str == Py_None) {
    keypoint->name = 0;
    Py_RETURN_NONE;
  }

  if (!PyUnicode_Check (py_str)) {
    PyErr_SetString (PyExc_TypeError, "value is not string");
    return NULL;
  }

  keypoint = pyg_boxed_get (py_keypoint, GstMLKeypoint);
  keypoint->name = g_quark_from_string (PyUnicode_AsUTF8 (py_str));

  Py_RETURN_NONE;
}

static PyObject *
_gst_ml_classification_get_name (PyObject * self, PyObject * args)
{
  PyTypeObject *py_classification_type = NULL;
  PyObject *py_classification = NULL;
  GstMLClassification *classification = NULL;

  // Look up GstQtiML.Classification parameter
  py_classification_type = pygobject_lookup_class (gst_ml_classification_get_type ());
  if (!PyArg_ParseTuple (args, "O!", py_classification_type, &py_classification)) {
    PyErr_BadArgument ();
    return NULL;
  }

  classification = pyg_boxed_get (py_classification, GstMLClassification);

  if (classification->name == 0)
    Py_RETURN_NONE;

  return PyUnicode_FromString (g_quark_to_string (classification->name));
}

static PyObject *
_gst_ml_classification_set_name (PyObject * self, PyObject * args)
{
  PyTypeObject *py_classification_type = NULL;
  PyObject *py_classification = NULL, *py_str = NULL;
  GstMLClassification *classification = NULL;

  // Look up GstQtiML.Classification and str parameters
  py_classification_type = pygobject_lookup_class (gst_ml_classification_get_type ());
  if (!PyArg_ParseTuple (args, "O!O", py_classification_type, &py_classification,
          &py_str)) {
    PyErr_BadArgument ();
    return NULL;
  }

  if (py_str == Py_None) {
    classification->name = 0;
    Py_RETURN_NONE;
  }

  if (!PyUnicode_Check (py_str)) {
    PyErr_SetString (PyExc_TypeError, "value is not string");
    return NULL;
  }

  classification = pyg_boxed_get (py_classification, GstMLClassification);
  classification->name = g_quark_from_string (PyUnicode_AsUTF8 (py_str));

  Py_RETURN_NONE;
}

static PyObject *
_gst_ml_classification_get_xtraparams (PyObject * self, PyObject * args)
{
  PyTypeObject *py_classification_type = NULL;
  PyObject *py_classification = NULL, *py_structure = NULL;
  GstMLClassification *classification = NULL;

  // Look up GstQtiML.Classification parameter
  py_classification_type = pygobject_lookup_class (gst_ml_classification_get_type ());
  if (!PyArg_ParseTuple (args, "O!", py_classification_type, &py_classification)) {
    PyErr_BadArgument ();
    return NULL;
  }

  classification = pyg_boxed_get (py_classification, GstMLClassification);

  if (classification->xtraparams == NULL)
    Py_RETURN_NONE;

  py_structure = pyg_boxed_new (_gst_structure_type, classification->xtraparams,
      FALSE, FALSE);

  return py_structure;
}

static PyObject *
_gst_ml_classification_set_xtraparams (PyObject * self, PyObject * args)
{
  PyTypeObject *py_classification_type = NULL;
  PyObject *py_classification = NULL, *py_structure = NULL;
  GstMLClassification *classification = NULL;
  GstStructure *structure = NULL;

  // Look up GstQtiML.Classification and Gst.Structure parameters
  py_classification_type = pygobject_lookup_class (gst_ml_classification_get_type ());
  if (!PyArg_ParseTuple (args, "O!O", py_classification_type, &py_classification,
          &py_structure)) {
    PyErr_BadArgument ();
    return NULL;
  }

  if (py_structure == Py_None) {
    g_clear_pointer (&(classification->xtraparams), gst_structure_free);
    Py_RETURN_NONE;
  }

  if (!pyg_boxed_check (py_structure, GST_TYPE_STRUCTURE)) {
    PyErr_SetString (PyExc_TypeError, "value is not Gst.Structure");
    return NULL;
  }

  classification = pyg_boxed_get (py_classification, GstMLClassification);
  structure = pyg_boxed_get (py_structure, GstStructure);

  if (classification->xtraparams == structure)
    Py_RETURN_NONE;

  g_clear_pointer (&(classification->xtraparams), gst_structure_free);
  classification->xtraparams = gst_structure_copy (structure);

  Py_RETURN_NONE;
}

static PyObject *
_gst_ml_detection_get_name (PyObject * self, PyObject * args)
{
  PyTypeObject *py_detection_type = NULL;
  PyObject *py_detection = NULL;
  GstMLDetection *detection = NULL;

  // Look up GstQtiML.Detection parameter
  py_detection_type = pygobject_lookup_class (gst_ml_detection_get_type ());
  if (!PyArg_ParseTuple (args, "O!", py_detection_type, &py_detection)) {
    PyErr_BadArgument ();
    return NULL;
  }

  detection = pyg_boxed_get (py_detection, GstMLDetection);

  if (detection->name == 0)
    Py_RETURN_NONE;

  return PyUnicode_FromString (g_quark_to_string (detection->name));
}

static PyObject *
_gst_ml_detection_set_name (PyObject * self, PyObject * args)
{
  PyTypeObject *py_detection_type = NULL;
  PyObject *py_detection = NULL, *py_str = NULL;
  GstMLDetection *detection = NULL;

  // Look up GstQtiML.Detection and str parameters
  py_detection_type = pygobject_lookup_class (gst_ml_detection_get_type ());
  if (!PyArg_ParseTuple (args, "O!O", py_detection_type, &py_detection,
          &py_str)) {
    PyErr_BadArgument ();
    return NULL;
  }

  if (py_str == Py_None) {
    detection->name = 0;
    Py_RETURN_NONE;
  }

  if (!PyUnicode_Check (py_str)) {
    PyErr_SetString (PyExc_TypeError, "value is not string");
    return NULL;
  }

  detection = pyg_boxed_get (py_detection, GstMLDetection);
  detection->name = g_quark_from_string (PyUnicode_AsUTF8 (py_str));

  Py_RETURN_NONE;
}

static PyObject *
_gst_ml_detection_get_landmarks (PyObject * self, PyObject * args)
{
  PyTypeObject *py_detection_type = NULL;
  PyObject *py_detection = NULL, *py_list = NULL, *py_kp = NULL;
  GstMLDetection *detection = NULL;
  GstMLKeypoint *kp = NULL;
  guint idx = 0;

  // Look up GstQtiML.Detection parameter
  py_detection_type = pygobject_lookup_class (gst_ml_detection_get_type ());
  if (!PyArg_ParseTuple (args, "O!", py_detection_type, &py_detection)) {
    PyErr_BadArgument ();
    return NULL;
  }

  detection = pyg_boxed_get (py_detection, GstMLDetection);

  // Return empty list if no landmarks
  if ((detection->landmarks) == NULL || (detection->landmarks->len == 0))
    Py_RETURN_NONE;

  if ((py_list = PyList_New (detection->landmarks->len)) == NULL) {
    PyErr_NoMemory ();
    return NULL;
  }

  for (idx = 0; idx < detection->landmarks->len; idx++) {
    kp = &(g_array_index (detection->landmarks, GstMLKeypoint, idx));
    py_kp = pyg_boxed_new (GST_TYPE_ML_KEYPOINT, kp, TRUE, TRUE);

    PyList_SET_ITEM (py_list, (Py_ssize_t) idx, py_kp);
  }

  return py_list;
}

static PyObject *
_gst_ml_detection_set_landmarks (PyObject * self, PyObject * args)
{
  PyTypeObject *py_detection_type = NULL;
  PyObject *py_detection = NULL, *py_seq = NULL, *sequence = NULL;
  GstMLDetection *detection = NULL;
  GstMLKeypoint *kp = NULL;
  GArray *landmarks = NULL;
  guint idx = 0, n_entries = 0;

  // Look up GstQtiML.Detection and Sequence|None parameters
  py_detection_type = pygobject_lookup_class (gst_ml_detection_get_type ());
  if (!PyArg_ParseTuple (args, "O!O", py_detection_type, &py_detection, &py_seq)) {
    PyErr_BadArgument ();
    return NULL;
  }

  detection = pyg_boxed_get (py_detection, GstMLDetection);

  if (py_seq == Py_None) {
    g_clear_pointer (&(detection->landmarks), g_array_unref);
    Py_RETURN_NONE;
  }

  sequence = PySequence_Fast (py_seq, "landmarks must be a sequence");
  if (sequence == NULL)
    return NULL;

  if ((n_entries = PySequence_Fast_GET_SIZE (sequence)) == 0) {
    g_clear_pointer (&(detection->landmarks), g_array_unref);
    goto cleanup;
  }

  landmarks = g_array_sized_new (FALSE, FALSE, sizeof (GstMLKeypoint), n_entries);

  for (idx = 0; idx < n_entries; idx++) {
    PyObject *py_kp = PySequence_Fast_GET_ITEM (sequence, idx);

    if (!pyg_boxed_check (py_kp, GST_TYPE_ML_KEYPOINT)) {
      PyErr_SetString (PyExc_TypeError, "sequence value is not GstQtiML.Keypoint");
      return NULL;
    }

    kp = pyg_boxed_get (py_kp, GstMLKeypoint);
    g_array_append_vals (landmarks, kp, 1);
  }

  g_clear_pointer (&(detection->landmarks), g_array_unref);
  detection->landmarks = g_steal_pointer (&(landmarks));

cleanup:
  g_clear_pointer (&(landmarks), g_array_unref);
  Py_DECREF (sequence);

  Py_RETURN_NONE;
}

static PyObject *
_gst_ml_detection_get_xtraparams (PyObject * self, PyObject * args)
{
  PyTypeObject *py_detection_type = NULL;
  PyObject *py_detection = NULL, *py_structure = NULL;
  GstMLDetection *detection = NULL;

  // Look up GstQtiML.Detection parameter
  py_detection_type = pygobject_lookup_class (gst_ml_detection_get_type ());
  if (!PyArg_ParseTuple (args, "O!", py_detection_type, &py_detection)) {
    PyErr_BadArgument ();
    return NULL;
  }

  detection = pyg_boxed_get (py_detection, GstMLDetection);

  if (detection->xtraparams == NULL)
    Py_RETURN_NONE;

  py_structure = pyg_boxed_new (_gst_structure_type, detection->xtraparams,
      FALSE, FALSE);

  return py_structure;
}

static PyObject *
_gst_ml_detection_set_xtraparams (PyObject * self, PyObject * args)
{
  PyTypeObject *py_detection_type = NULL;
  PyObject *py_detection = NULL, *py_structure = NULL;
  GstMLDetection *detection = NULL;
  GstStructure *structure = NULL;

  // Look up GstQtiML.Detection and Gst.Structure parameters
  py_detection_type = pygobject_lookup_class (gst_ml_detection_get_type ());
  if (!PyArg_ParseTuple (args, "O!O", py_detection_type, &py_detection,
          &py_structure)) {
    PyErr_BadArgument ();
    return NULL;
  }

  if (py_structure == Py_None) {
    g_clear_pointer (&(detection->xtraparams), gst_structure_free);
    Py_RETURN_NONE;
  }

  if (!pyg_boxed_check (py_structure, GST_TYPE_STRUCTURE)) {
    PyErr_SetString (PyExc_TypeError, "value is not Gst.Structure");
    return NULL;
  }

  detection = pyg_boxed_get (py_detection, GstMLDetection);
  structure = pyg_boxed_get (py_structure, GstStructure);

  if (detection->xtraparams == structure)
    Py_RETURN_NONE;

  g_clear_pointer (&(detection->xtraparams), gst_structure_free);
  detection->xtraparams = gst_structure_copy (structure);

  Py_RETURN_NONE;
}

static PyObject *
_gst_ml_pose_get_name (PyObject * self, PyObject * args)
{
  PyTypeObject *py_pose_type = NULL;
  PyObject *py_pose = NULL;
  GstMLPose *pose = NULL;

  // Look up GstQtiML.Pose parameter
  py_pose_type = pygobject_lookup_class (gst_ml_pose_get_type ());
  if (!PyArg_ParseTuple (args, "O!", py_pose_type, &py_pose)) {
    PyErr_BadArgument ();
    return NULL;
  }

  pose = pyg_boxed_get (py_pose, GstMLPose);

  if (pose->name == 0)
    Py_RETURN_NONE;

  return PyUnicode_FromString (g_quark_to_string (pose->name));
}

static PyObject *
_gst_ml_pose_set_name (PyObject * self, PyObject * args)
{
  PyTypeObject *py_pose_type = NULL;
  PyObject *py_pose = NULL, *py_str = NULL;
  GstMLPose *pose = NULL;

  // Look up GstQtiML.Pose and str parameters
  py_pose_type = pygobject_lookup_class (gst_ml_pose_get_type ());
  if (!PyArg_ParseTuple (args, "O!O", py_pose_type, &py_pose,
          &py_str)) {
    PyErr_BadArgument ();
    return NULL;
  }

  if (py_str == Py_None) {
    pose->name = 0;
    Py_RETURN_NONE;
  }

  if (!PyUnicode_Check (py_str)) {
    PyErr_SetString (PyExc_TypeError, "value is not string");
    return NULL;
  }

  pose = pyg_boxed_get (py_pose, GstMLPose);
  pose->name = g_quark_from_string (PyUnicode_AsUTF8 (py_str));

  Py_RETURN_NONE;
}

static PyObject *
_gst_ml_pose_get_keypoints (PyObject * self, PyObject * args)
{
  PyTypeObject *py_pose_type = NULL;
  PyObject *py_pose = NULL, *py_list = NULL, *py_kp = NULL;
  GstMLPose *pose = NULL;
  GstMLKeypoint *kp = NULL;
  guint idx = 0;

  // Look up GstQtiML.Pose parameter
  py_pose_type = pygobject_lookup_class (gst_ml_pose_get_type ());
  if (!PyArg_ParseTuple (args, "O!", py_pose_type, &py_pose)) {
    PyErr_BadArgument ();
    return NULL;
  }

  pose = pyg_boxed_get (py_pose, GstMLPose);

  // Return empty list if no keypoints
  if ((pose->keypoints) == NULL || (pose->keypoints->len == 0))
    Py_RETURN_NONE;

  if ((py_list = PyList_New (pose->keypoints->len)) == NULL) {
    PyErr_NoMemory ();
    return NULL;
  }

  for (idx = 0; idx < pose->keypoints->len; idx++) {
    kp = &(g_array_index (pose->keypoints, GstMLKeypoint, idx));
    py_kp = pyg_boxed_new (GST_TYPE_ML_KEYPOINT, kp, TRUE, TRUE);

    PyList_SET_ITEM (py_list, (Py_ssize_t) idx, py_kp);
  }

  return py_list;
}

static PyObject *
_gst_ml_pose_set_keypoints (PyObject * self, PyObject * args)
{
  PyTypeObject *py_pose_type = NULL;
  PyObject *py_pose = NULL, *py_seq = NULL, *sequence = NULL;
  GstMLPose *pose = NULL;
  GstMLKeypoint *kp = NULL;
  GArray *keypoints = NULL;
  guint idx = 0, n_entries = 0;

  // Look up GstQtiML.Pose and Sequence|None parameters
  py_pose_type = pygobject_lookup_class (gst_ml_pose_get_type ());
  if (!PyArg_ParseTuple (args, "O!O", py_pose_type, &py_pose, &py_seq)) {
    PyErr_BadArgument ();
    return NULL;
  }

  pose = pyg_boxed_get (py_pose, GstMLPose);

  if (py_seq == Py_None) {
    g_clear_pointer (&(pose->keypoints), g_array_unref);
    Py_RETURN_NONE;
  }

  sequence = PySequence_Fast (py_seq, "keypoints must be a sequence");
  if (sequence == NULL)
    return NULL;

  if ((n_entries = PySequence_Fast_GET_SIZE (sequence)) == 0) {
    g_clear_pointer (&(pose->keypoints), g_array_unref);
    goto cleanup;
  }

  keypoints = g_array_sized_new (FALSE, FALSE, sizeof (GstMLKeypoint), n_entries);

  for (idx = 0; idx < n_entries; idx++) {
    PyObject *py_kp = PySequence_Fast_GET_ITEM (sequence, idx);

    if (!pyg_boxed_check (py_kp, GST_TYPE_ML_KEYPOINT)) {
      PyErr_SetString (PyExc_TypeError, "sequence value is not GstQtiML.Keypoint");
      return NULL;
    }

    kp = pyg_boxed_get (py_kp, GstMLKeypoint);
    g_array_append_vals (keypoints, kp, 1);
  }

  g_clear_pointer (&(pose->keypoints), g_array_unref);
  pose->keypoints = g_steal_pointer (&(keypoints));

cleanup:
  g_clear_pointer (&(keypoints), g_array_unref);
  Py_DECREF (sequence);

  Py_RETURN_NONE;
}

static PyObject *
_gst_ml_pose_get_links (PyObject * self, PyObject * args)
{
  PyTypeObject *py_pose_type = NULL;
  PyObject *py_pose = NULL, *py_list = NULL, *py_link = NULL;
  GstMLPose *pose = NULL;
  GstMLKeypointLink *link = NULL;
  guint idx = 0;

  // Look up GstQtiML.Pose parameter
  py_pose_type = pygobject_lookup_class (gst_ml_pose_get_type ());
  if (!PyArg_ParseTuple (args, "O!", py_pose_type, &py_pose)) {
    PyErr_BadArgument ();
    return NULL;
  }

  pose = pyg_boxed_get (py_pose, GstMLPose);

  // Return empty list if no links
  if ((pose->links) == NULL || (pose->links->len == 0))
    Py_RETURN_NONE;

  if ((py_list = PyList_New (pose->links->len)) == NULL) {
    PyErr_NoMemory ();
    return NULL;
  }

  for (idx = 0; idx < pose->links->len; idx++) {
    link = &(g_array_index (pose->links, GstMLKeypointLink, idx));
    py_link = pyg_boxed_new (GST_TYPE_ML_KEYPOINT_LINK, link, TRUE, TRUE);

    PyList_SET_ITEM (py_list, (Py_ssize_t) idx, py_link);
  }

  return py_list;
}

static PyObject *
_gst_ml_pose_set_links (PyObject * self, PyObject * args)
{
  PyTypeObject *py_pose_type = NULL;
  PyObject *py_pose = NULL, *py_seq = NULL, *sequence = NULL;
  GstMLPose *pose = NULL;
  GstMLKeypointLink *link = NULL;
  GArray *links = NULL;
  guint idx = 0, n_entries = 0;

  // Look up GstQtiML.Pose and Sequence|None parameters
  py_pose_type = pygobject_lookup_class (gst_ml_pose_get_type ());
  if (!PyArg_ParseTuple (args, "O!O", py_pose_type, &py_pose, &py_seq)) {
    PyErr_BadArgument ();
    return NULL;
  }

  pose = pyg_boxed_get (py_pose, GstMLPose);

  if (py_seq == Py_None) {
    g_clear_pointer (&(pose->links), g_array_unref);
    Py_RETURN_NONE;
  }

  sequence = PySequence_Fast (py_seq, "links must be a sequence");
  if (sequence == NULL)
    return NULL;

  if ((n_entries = PySequence_Fast_GET_SIZE (sequence)) == 0) {
    g_clear_pointer (&(pose->links), g_array_unref);
    goto cleanup;
  }

  links =
      g_array_sized_new (FALSE, FALSE, sizeof (GstMLKeypointLink), n_entries);

  for (idx = 0; idx < n_entries; idx++) {
    PyObject *py_link = PySequence_Fast_GET_ITEM (sequence, idx);

    if (!pyg_boxed_check (py_link, GST_TYPE_ML_KEYPOINT_LINK)) {
      PyErr_SetString (PyExc_TypeError, "sequence value is not GstQtiML.KeypointLink");
      return NULL;
    }

    link = pyg_boxed_get (py_link, GstMLKeypointLink);
    g_array_append_vals (links, link, 1);
  }

  g_clear_pointer (&(pose->links), g_array_unref);
  pose->links = g_steal_pointer (&(links));

cleanup:
  g_clear_pointer (&(links), g_array_unref);
  Py_DECREF (sequence);

  Py_RETURN_NONE;
}

static PyObject *
_gst_ml_pose_get_xtraparams (PyObject * self, PyObject * args)
{
  PyTypeObject *py_pose_type = NULL;
  PyObject *py_pose = NULL, *py_structure = NULL;
  GstMLPose *pose = NULL;

  // Look up GstQtiML.Pose parameter
  py_pose_type = pygobject_lookup_class (gst_ml_pose_get_type ());
  if (!PyArg_ParseTuple (args, "O!", py_pose_type, &py_pose)) {
    PyErr_BadArgument ();
    return NULL;
  }

  pose = pyg_boxed_get (py_pose, GstMLPose);

  if (pose->xtraparams == NULL)
    Py_RETURN_NONE;

  py_structure = pyg_boxed_new (_gst_structure_type, pose->xtraparams,
      FALSE, FALSE);

  return py_structure;
}

static PyObject *
_gst_ml_pose_set_xtraparams (PyObject * self, PyObject * args)
{
  PyTypeObject *py_pose_type = NULL;
  PyObject *py_pose = NULL, *py_structure = NULL;
  GstMLPose *pose = NULL;
  GstStructure *structure = NULL;

  // Look up GstQtiML.Pose and Gst.Structure parameters
  py_pose_type = pygobject_lookup_class (gst_ml_pose_get_type ());
  if (!PyArg_ParseTuple (args, "O!O", py_pose_type, &py_pose, &py_structure)) {
    PyErr_BadArgument ();
    return NULL;
  }

  if (py_structure == Py_None) {
    g_clear_pointer (&(pose->xtraparams), gst_structure_free);
    Py_RETURN_NONE;
  }

  if (!pyg_boxed_check (py_structure, GST_TYPE_STRUCTURE)) {
    PyErr_SetString (PyExc_TypeError, "value is not Gst.Structure");
    return NULL;
  }

  pose = pyg_boxed_get (py_pose, GstMLPose);
  structure = pyg_boxed_get (py_structure, GstStructure);

  if (pose->xtraparams == structure)
    Py_RETURN_NONE;

  g_clear_pointer (&(pose->xtraparams), gst_structure_free);
  pose->xtraparams = gst_structure_copy (structure);

  Py_RETURN_NONE;
}

static PyObject *
_gst_ml_segmentation_get_labels (PyObject * self, PyObject * args)
{
  PyTypeObject *py_segmentation_type = NULL;
  PyObject *py_segmentation = NULL, *py_list = NULL;
  GstMLSegmentation *segmentation = NULL;
  guint idx = 0;

  // Look up GstQtiML.Pose parameter
  py_segmentation_type = pygobject_lookup_class (gst_ml_segmentation_get_type ());
  if (!PyArg_ParseTuple (args, "O!", py_segmentation_type, &py_segmentation)) {
    PyErr_BadArgument ();
    return NULL;
  }

  segmentation = pyg_boxed_get (py_segmentation, GstMLSegmentation);

  // Return empty list if no labels
  if ((segmentation->labels) == NULL || (segmentation->labels->len == 0))
    Py_RETURN_NONE;

  if ((py_list = PyList_New (segmentation->labels->len)) == NULL) {
    PyErr_NoMemory ();
    return NULL;
  }

  for (idx = 0; idx < segmentation->labels->len; idx++) {
    const gchar *label = g_quark_to_string (
        g_array_index (segmentation->labels, GQuark, idx));

    PyList_SET_ITEM (py_list, (Py_ssize_t) idx, PyUnicode_FromString (label));
  }

  return py_list;
}

static PyObject *
_gst_ml_segmentation_set_labels (PyObject * self, PyObject * args)
{
  PyTypeObject *py_segmentation_type = NULL;
  PyObject *py_segmentation = NULL, *py_seq = NULL, *sequence = NULL;
  GstMLSegmentation *segmentation = NULL;
  GArray *labels = NULL;
  guint idx = 0, n_entries = 0;

  // Look up GstQtiML.Pose and Sequence|None parameters
  py_segmentation_type = pygobject_lookup_class (gst_ml_segmentation_get_type ());
  if (!PyArg_ParseTuple (args, "O!O", py_segmentation_type, &py_segmentation,
          &py_seq)) {
    PyErr_BadArgument ();
    return NULL;
  }

  segmentation = pyg_boxed_get (py_segmentation, GstMLSegmentation);

  if (py_seq == Py_None) {
    g_clear_pointer (&(segmentation->labels), g_array_unref);
    Py_RETURN_NONE;
  }

  sequence = PySequence_Fast (py_seq, "labels must be a sequence");
  if (sequence == NULL)
    return NULL;

  if ((n_entries = PySequence_Fast_GET_SIZE (sequence)) == 0) {
    g_clear_pointer (&(segmentation->labels), g_array_unref);
    goto cleanup;
  }

  labels = g_array_sized_new (FALSE, FALSE, sizeof (GQuark), n_entries);

  for (idx = 0; idx < n_entries; idx++) {
    PyObject *py_str = PySequence_Fast_GET_ITEM (sequence, idx);
    GQuark label = 0;

    if (!PyUnicode_Check (py_str)) {
      PyErr_SetString (PyExc_TypeError, "value is not string");
      goto cleanup;
    }

    label = g_quark_from_string (PyUnicode_AsUTF8 (py_str));
    g_array_append_val (labels, label);
  }

  g_clear_pointer (&(segmentation->labels), g_array_unref);
  segmentation->labels = g_steal_pointer (&(labels));

cleanup:
  g_clear_pointer (&(labels), g_array_unref);
  Py_DECREF (sequence);

  Py_RETURN_NONE;
}

static PyObject *
_gst_ml_segmentation_get_colors (PyObject * self, PyObject * args)
{
  PyTypeObject *py_segmentation_type = NULL;
  PyObject *py_segmentation = NULL, *py_list = NULL, *py_long = NULL;
  GstMLSegmentation *segmentation = NULL;
  guint idx = 0;

  // Look up GstQtiML.Pose parameter
  py_segmentation_type = pygobject_lookup_class (gst_ml_segmentation_get_type ());
  if (!PyArg_ParseTuple (args, "O!", py_segmentation_type, &py_segmentation)) {
    PyErr_BadArgument ();
    return NULL;
  }

  segmentation = pyg_boxed_get (py_segmentation, GstMLSegmentation);

  // Return empty list if no color mask
  if ((segmentation->colors) == NULL || (segmentation->colors->len == 0))
    Py_RETURN_NONE;

  if ((py_list = PyList_New (segmentation->colors->len)) == NULL) {
    PyErr_NoMemory ();
    return NULL;
  }

  for (idx = 0; idx < segmentation->colors->len; idx++) {
    py_long = PyLong_FromUnsignedLong (
        g_array_index (segmentation->colors, guint32, idx));

    PyList_SET_ITEM (py_list, (Py_ssize_t) idx, py_long);
  }

  return py_list;
}

static PyObject *
_gst_ml_segmentation_set_colors (PyObject * self, PyObject * args)
{
  PyTypeObject *py_segmentation_type = NULL;
  PyObject *py_segmentation = NULL, *py_seq = NULL, *sequence = NULL;
  GstMLSegmentation *segmentation = NULL;
  GArray *colors = NULL;
  guint idx = 0, n_entries = 0, color = 0;

  // Look up GstQtiML.Pose and Sequence|None parameters
  py_segmentation_type = pygobject_lookup_class (gst_ml_segmentation_get_type ());
  if (!PyArg_ParseTuple (args, "O!O", py_segmentation_type, &py_segmentation,
          &py_seq)) {
    PyErr_BadArgument ();
    return NULL;
  }

  segmentation = pyg_boxed_get (py_segmentation, GstMLSegmentation);

  if (py_seq == Py_None) {
    g_clear_pointer (&(segmentation->colors), g_array_unref);
    Py_RETURN_NONE;
  }

  sequence = PySequence_Fast (py_seq, "color mask must be a sequence");
  if (sequence == NULL)
    return NULL;

  if ((n_entries = PySequence_Fast_GET_SIZE (sequence)) == 0) {
    g_clear_pointer (&(segmentation->colors), g_array_unref);
    goto cleanup;
  }

  colors = g_array_sized_new (FALSE, FALSE, sizeof (guint32), n_entries);

  for (idx = 0; idx < n_entries; idx++) {
    PyObject *py_long = PySequence_Fast_GET_ITEM (sequence, idx);

    if (!PyLong_Check (py_long)) {
      PyErr_SetString (PyExc_TypeError, "value is not integer");
      goto cleanup;
    }

    color = PyLong_AsLong (py_long);
    g_array_append_val (colors, color);
  }

  g_clear_pointer (&(segmentation->colors), g_array_unref);
  segmentation->colors = g_steal_pointer (&(colors));

cleanup:
  g_clear_pointer (&(colors), g_array_unref);
  Py_DECREF (sequence);

  Py_RETURN_NONE;
}

static PyObject *
_gst_ml_segmentation_get_xtraparams (PyObject * self, PyObject * args)
{
  PyTypeObject *py_segmentation_type = NULL;
  PyObject *py_segmentation = NULL, *py_structure = NULL;
  GstMLSegmentation *segmentation = NULL;

  // Look up GstQtiML.Pose parameter
  py_segmentation_type = pygobject_lookup_class (gst_ml_segmentation_get_type ());
  if (!PyArg_ParseTuple (args, "O!", py_segmentation_type, &py_segmentation)) {
    PyErr_BadArgument ();
    return NULL;
  }

  segmentation = pyg_boxed_get (py_segmentation, GstMLSegmentation);

  if (segmentation->xtraparams == NULL)
    Py_RETURN_NONE;

  py_structure = pyg_boxed_new (_gst_structure_type, segmentation->xtraparams,
      FALSE, FALSE);

  return py_structure;
}

static PyObject *
_gst_ml_segmentation_set_xtraparams (PyObject * self, PyObject * args)
{
  PyTypeObject *py_segmentation_type = NULL;
  PyObject *py_segmentation = NULL, *py_structure = NULL;
  GstMLSegmentation *segmentation = NULL;
  GstStructure *structure = NULL;

  // Look up GstQtiML.Pose and Gst.Structure parameters
  py_segmentation_type = pygobject_lookup_class (gst_ml_segmentation_get_type ());
  if (!PyArg_ParseTuple (args, "O!O", py_segmentation_type, &py_segmentation,
          &py_structure)) {
    PyErr_BadArgument ();
    return NULL;
  }

  if (py_structure == Py_None) {
    g_clear_pointer (&(segmentation->xtraparams), gst_structure_free);
    Py_RETURN_NONE;
  }

  if (!pyg_boxed_check (py_structure, GST_TYPE_STRUCTURE)) {
    PyErr_SetString (PyExc_TypeError, "value is not Gst.Structure");
    return NULL;
  }

  segmentation = pyg_boxed_get (py_segmentation, GstMLSegmentation);
  structure = pyg_boxed_get (py_structure, GstStructure);

  if (segmentation->xtraparams == structure)
    Py_RETURN_NONE;

  g_clear_pointer (&(segmentation->xtraparams), gst_structure_free);
  segmentation->xtraparams = gst_structure_copy (structure);

  Py_RETURN_NONE;
}


static PyObject *
_gst_ml_depth_map_get_values (PyObject * self, PyObject * args)
{
  PyTypeObject *py_depth_map_type = NULL;
  PyObject *py_depthmap = NULL, *py_list = NULL;
  GstMLDepthMap *depthmap = NULL;
  guint idx = 0;

  // Look up GstQtiML.Pose parameter
  py_depth_map_type = pygobject_lookup_class (gst_ml_depth_map_get_type ());
  if (!PyArg_ParseTuple (args, "O!", py_depth_map_type, &py_depthmap)) {
    PyErr_BadArgument ();
    return NULL;
  }

  depthmap = pyg_boxed_get (py_depthmap, GstMLDepthMap);

  // Return empty list if no values
  if ((depthmap->values) == NULL || (depthmap->values->len == 0))
    Py_RETURN_NONE;

  if ((py_list = PyList_New (depthmap->values->len)) == NULL) {
    PyErr_NoMemory ();
    return NULL;
  }

  for (idx = 0; idx < depthmap->values->len; idx++) {
    PyList_SET_ITEM (py_list, (Py_ssize_t) idx,
        PyFloat_FromDouble (g_array_index (depthmap->values, gdouble, idx)));
  }

  return py_list;
}

static PyObject *
_gst_ml_depth_map_set_values (PyObject * self, PyObject * args)
{
  PyTypeObject *py_depth_map_type = NULL;
  PyObject *py_depthmap = NULL, *py_seq = NULL, *sequence = NULL;
  GstMLDepthMap *depthmap = NULL;
  GArray *values = NULL;
  guint idx = 0, n_entries = 0;

  // Look up GstQtiML.Pose and Sequence|None parameters
  py_depth_map_type = pygobject_lookup_class (gst_ml_depth_map_get_type ());
  if (!PyArg_ParseTuple (args, "O!O", py_depth_map_type, &py_depthmap, &py_seq)) {
    PyErr_BadArgument ();
    return NULL;
  }

  depthmap = pyg_boxed_get (py_depthmap, GstMLDepthMap);

  if (py_seq == Py_None) {
    g_clear_pointer (&(depthmap->values), g_array_unref);
    Py_RETURN_NONE;
  }

  sequence = PySequence_Fast (py_seq, "values must be a sequence");
  if (sequence == NULL)
    return NULL;

  if ((n_entries = PySequence_Fast_GET_SIZE (sequence)) == 0) {
    g_clear_pointer (&(depthmap->values), g_array_unref);
    goto cleanup;
  }

  values = g_array_sized_new (FALSE, FALSE, sizeof (gdouble), n_entries);

  for (idx = 0; idx < n_entries; idx++) {
    PyObject *py_float = PySequence_Fast_GET_ITEM (sequence, idx);
    gdouble depth = 0;

    if (!PyFloat_Check (py_float)) {
      PyErr_SetString (PyExc_TypeError, "value is not float");
      goto cleanup;
    }

    depth = PyFloat_AsDouble (py_float);
    g_array_append_val (values, depth);
  }

  g_clear_pointer (&(depthmap->values), g_array_unref);
  depthmap->values = g_steal_pointer (&(values));

cleanup:
  g_clear_pointer (&(values), g_array_unref);
  Py_DECREF (sequence);

  Py_RETURN_NONE;
}

static PyObject *
_gst_ml_depth_map_get_colors (PyObject * self, PyObject * args)
{
  PyTypeObject *py_depth_map_type = NULL;
  PyObject *py_depthmap = NULL, *py_list = NULL, *py_long = NULL;
  GstMLDepthMap *depthmap = NULL;
  guint idx = 0;

  // Look up GstQtiML.Pose parameter
  py_depth_map_type = pygobject_lookup_class (gst_ml_depth_map_get_type ());
  if (!PyArg_ParseTuple (args, "O!", py_depth_map_type, &py_depthmap)) {
    PyErr_BadArgument ();
    return NULL;
  }

  depthmap = pyg_boxed_get (py_depthmap, GstMLDepthMap);

  // Return empty list if no color mask
  if ((depthmap->colors) == NULL || (depthmap->colors->len == 0))
    Py_RETURN_NONE;

  if ((py_list = PyList_New (depthmap->colors->len)) == NULL) {
    PyErr_NoMemory ();
    return NULL;
  }

  for (idx = 0; idx < depthmap->colors->len; idx++) {
    py_long = PyLong_FromUnsignedLong (
        g_array_index (depthmap->colors, guint32, idx));

    PyList_SET_ITEM (py_list, (Py_ssize_t) idx, py_long);
  }

  return py_list;
}

static PyObject *
_gst_ml_depth_map_set_colors (PyObject * self, PyObject * args)
{
  PyTypeObject *py_depth_map_type = NULL;
  PyObject *py_depthmap = NULL, *py_seq = NULL, *sequence = NULL;
  GstMLDepthMap *depthmap = NULL;
  GArray *colors = NULL;
  guint idx = 0, n_entries = 0, color = 0;

  // Look up GstQtiML.Pose and Sequence|None parameters
  py_depth_map_type = pygobject_lookup_class (gst_ml_depth_map_get_type ());
  if (!PyArg_ParseTuple (args, "O!O", py_depth_map_type, &py_depthmap,
          &py_seq)) {
    PyErr_BadArgument ();
    return NULL;
  }

  depthmap = pyg_boxed_get (py_depthmap, GstMLDepthMap);

  if (py_seq == Py_None) {
    g_clear_pointer (&(depthmap->colors), g_array_unref);
    Py_RETURN_NONE;
  }

  sequence = PySequence_Fast (py_seq, "color mask must be a sequence");
  if (sequence == NULL)
    return NULL;

  if ((n_entries = PySequence_Fast_GET_SIZE (sequence)) == 0) {
    g_clear_pointer (&(depthmap->colors), g_array_unref);
    goto cleanup;
  }

  colors = g_array_sized_new (FALSE, FALSE, sizeof (guint32), n_entries);

  for (idx = 0; idx < n_entries; idx++) {
    PyObject *py_long = PySequence_Fast_GET_ITEM (sequence, idx);

    if (!PyLong_Check (py_long)) {
      PyErr_SetString (PyExc_TypeError, "value is not integer");
      goto cleanup;
    }

    color = PyLong_AsLong (py_long);
    g_array_append_val (colors, color);
  }

  g_clear_pointer (&(depthmap->colors), g_array_unref);
  depthmap->colors = g_steal_pointer (&(colors));

cleanup:
  g_clear_pointer (&(colors), g_array_unref);
  Py_DECREF (sequence);

  Py_RETURN_NONE;
}

static PyObject *
_gst_ml_depth_map_get_xtraparams (PyObject * self, PyObject * args)
{
  PyTypeObject *py_depth_map_type = NULL;
  PyObject *py_depthmap = NULL, *py_structure = NULL;
  GstMLDepthMap *depthmap = NULL;

  // Look up GstQtiML.Pose parameter
  py_depth_map_type = pygobject_lookup_class (gst_ml_depth_map_get_type ());
  if (!PyArg_ParseTuple (args, "O!", py_depth_map_type, &py_depthmap)) {
    PyErr_BadArgument ();
    return NULL;
  }

  depthmap = pyg_boxed_get (py_depthmap, GstMLDepthMap);

  if (depthmap->xtraparams == NULL)
    Py_RETURN_NONE;

  py_structure = pyg_boxed_new (_gst_structure_type, depthmap->xtraparams,
      FALSE, FALSE);

  return py_structure;
}

static PyObject *
_gst_ml_depth_map_set_xtraparams (PyObject * self, PyObject * args)
{
  PyTypeObject *py_depth_map_type = NULL;
  PyObject *py_depthmap = NULL, *py_structure = NULL;
  GstMLDepthMap *depthmap = NULL;
  GstStructure *structure = NULL;

  // Look up GstQtiML.Pose and Gst.Structure parameters
  py_depth_map_type = pygobject_lookup_class (gst_ml_depth_map_get_type ());
  if (!PyArg_ParseTuple (args, "O!O", py_depth_map_type, &py_depthmap,
          &py_structure)) {
    PyErr_BadArgument ();
    return NULL;
  }

  if (py_structure == Py_None) {
    g_clear_pointer (&(depthmap->xtraparams), gst_structure_free);
    Py_RETURN_NONE;
  }

  if (!pyg_boxed_check (py_structure, GST_TYPE_STRUCTURE)) {
    PyErr_SetString (PyExc_TypeError, "value is not Gst.Structure");
    return NULL;
  }

  depthmap = pyg_boxed_get (py_depthmap, GstMLDepthMap);
  structure = pyg_boxed_get (py_structure, GstStructure);

  if (depthmap->xtraparams == structure)
    Py_RETURN_NONE;

  g_clear_pointer (&(depthmap->xtraparams), gst_structure_free);
  depthmap->xtraparams = gst_structure_copy (structure);

  Py_RETURN_NONE;
}

static PyObject *
_gst_video_region_of_interest_meta_get_quadrilateral (PyObject * self,
    PyObject * args)
{
  PyObject *py_meta = NULL;
  GstVideoRegionOfInterestMeta *meta = NULL;
  GstVideoQuadrilateral quadrilateral;

  // GstVideo.VideoRegionOfInterestMeta is a plain struct without a GType.
  if (!PyArg_ParseTuple (args, "O!", &PyGPointer_Type, &py_meta)) {
    PyErr_BadArgument ();
    return NULL;
  }

  meta = pyg_pointer_get (py_meta, GstVideoRegionOfInterestMeta);

  if (meta == NULL) {
    PyErr_SetString (PyExc_ValueError, "invalid GstVideoRegionOfInterestMeta!");
    return NULL;
  }

  quadrilateral = gst_video_region_of_interest_meta_get_quadrilateral (meta);

  // GstVideoQuadrilateral has no GType so return its 4 points as 8 doubles.
  return Py_BuildValue ("(dddddddd)", quadrilateral.a.x, quadrilateral.a.y,
      quadrilateral.b.x, quadrilateral.b.y, quadrilateral.c.x, quadrilateral.c.y,
      quadrilateral.d.x, quadrilateral.d.y);
}

static PyObject *
_gst_video_composition_get_blits (PyObject * self, PyObject * args)
{
  PyTypeObject *py_composition_type = NULL;
  PyObject *py_composition = NULL;
  GstVideoComposition *composition = NULL;

  py_composition_type = pygobject_lookup_class (gst_video_composition_get_type ());
  if (!PyArg_ParseTuple (args, "O!", py_composition_type, &py_composition)) {
    PyErr_BadArgument ();
    return NULL;
  }

  composition = pyg_boxed_get (py_composition, GstVideoComposition);

  if (composition->blits == NULL)
    Py_RETURN_NONE;

  return pyg_boxed_new (gst_video_blits_get_type (), composition->blits,
      TRUE, TRUE);
}

static PyObject *
_gst_video_composition_set_blits (PyObject * self, PyObject * args)
{
  PyTypeObject *py_composition_type = NULL;
  PyObject *py_composition = NULL, *py_blits = NULL;
  GstVideoComposition *composition = NULL;

  py_composition_type = pygobject_lookup_class (gst_video_composition_get_type ());
  if (!PyArg_ParseTuple (args, "O!O", py_composition_type, &py_composition,
          &py_blits)) {
    PyErr_BadArgument ();
    return NULL;
  }

  composition = pyg_boxed_get (py_composition, GstVideoComposition);

  if (py_blits == Py_None) {
    g_clear_pointer (&(composition->blits), gst_video_blits_unref);
    Py_RETURN_NONE;
  }

  if (!pyg_boxed_check (py_blits, gst_video_blits_get_type ())) {
    PyErr_SetString (PyExc_TypeError, "value must be a GstQtiVideo.Blits");
    return NULL;
  }

  g_clear_pointer (&(composition->blits), gst_video_blits_unref);
  composition->blits =
      gst_video_blits_ref (pyg_boxed_get (py_blits, GstVideoBlits));

  Py_RETURN_NONE;
}

static PyObject *
_gst_video_composition_get_buffer (PyObject * self, PyObject * args)
{
  PyTypeObject *py_composition_type = NULL;
  PyObject *py_composition = NULL;
  GstVideoComposition *composition = NULL;

  py_composition_type = pygobject_lookup_class (gst_video_composition_get_type ());
  if (!PyArg_ParseTuple (args, "O!", py_composition_type, &py_composition)) {
    PyErr_BadArgument ();
    return NULL;
  }

  composition = pyg_boxed_get (py_composition, GstVideoComposition);

  if (composition->buffer == NULL)
    Py_RETURN_NONE;

  return pyg_boxed_new (GST_TYPE_BUFFER, composition->buffer, FALSE, FALSE);
}

static PyObject *
_gst_video_composition_set_buffer (PyObject * self, PyObject * args)
{
  PyTypeObject *py_composition_type = NULL;
  PyObject *py_composition = NULL, *py_buffer = NULL;
  GstVideoComposition *composition = NULL;

  py_composition_type = pygobject_lookup_class (gst_video_composition_get_type ());
  if (!PyArg_ParseTuple (args, "O!O", py_composition_type, &py_composition,
          &py_buffer)) {
    PyErr_BadArgument ();
    return NULL;
  }

  composition = pyg_boxed_get (py_composition, GstVideoComposition);

  if (py_buffer == Py_None) {
    composition->buffer = NULL;
    Py_RETURN_NONE;
  }

  if (!pyg_boxed_check (py_buffer, GST_TYPE_BUFFER)) {
    PyErr_SetString (PyExc_TypeError, "value must be a Gst.Buffer");
    return NULL;
  }

  composition->buffer = pyg_boxed_get (py_buffer, GstBuffer);

  Py_RETURN_NONE;
}

static PyObject *
_gst_video_composition_get_info (PyObject * self, PyObject * args)
{
  PyTypeObject *py_composition_type = NULL;
  PyObject *py_composition = NULL;
  GstVideoComposition *composition = NULL;

  py_composition_type = pygobject_lookup_class (gst_video_composition_get_type ());
  if (!PyArg_ParseTuple (args, "O!", py_composition_type, &py_composition)) {
    PyErr_BadArgument ();
    return NULL;
  }

  composition = pyg_boxed_get (py_composition, GstVideoComposition);

  return pyg_boxed_new (GST_TYPE_VIDEO_INFO, &composition->info, TRUE, TRUE);
}

static PyObject *
_gst_video_composition_set_info (PyObject * self, PyObject * args)
{
  PyTypeObject *py_composition_type = NULL;
  PyObject *py_composition = NULL, *py_info = NULL;
  GstVideoComposition *composition = NULL;

  py_composition_type = pygobject_lookup_class (gst_video_composition_get_type ());
  if (!PyArg_ParseTuple (args, "O!O", py_composition_type, &py_composition,
          &py_info)) {
    PyErr_BadArgument ();
    return NULL;
  }

  composition = pyg_boxed_get (py_composition, GstVideoComposition);

  if (!pyg_boxed_check (py_info, GST_TYPE_VIDEO_INFO)) {
    PyErr_SetString (PyExc_TypeError, "value must be a GstVideo.VideoInfo");
    return NULL;
  }

  composition->info = *pyg_boxed_get (py_info, GstVideoInfo);

  Py_RETURN_NONE;
}

static PyObject *
_gst_video_composition_get_offsets (PyObject * self, PyObject * args)
{
  PyTypeObject *py_composition_type = NULL;
  PyObject *py_composition = NULL, *py_list = NULL;
  GstVideoComposition *composition = NULL;
  guint idx = 0;

  py_composition_type = pygobject_lookup_class (gst_video_composition_get_type ());
  if (!PyArg_ParseTuple (args, "O!", py_composition_type, &py_composition)) {
    PyErr_BadArgument ();
    return NULL;
  }

  composition = pyg_boxed_get (py_composition, GstVideoComposition);

  if ((py_list = PyList_New (GST_VIDEO_MAX_COMPONENTS)) == NULL) {
    PyErr_NoMemory ();
    return NULL;
  }

  for (idx = 0; idx < GST_VIDEO_MAX_COMPONENTS; idx++) {
    PyList_SET_ITEM (py_list, (Py_ssize_t) idx,
        PyFloat_FromDouble (composition->offsets[idx]));
  }

  return py_list;
}

static PyObject *
_gst_video_composition_set_offsets (PyObject * self, PyObject * args)
{
  PyTypeObject *py_composition_type = NULL;
  PyObject *py_composition = NULL, *py_seq = NULL, *sequence = NULL;
  GstVideoComposition *composition = NULL;
  guint idx = 0, n_entries = 0;

  py_composition_type = pygobject_lookup_class (gst_video_composition_get_type ());
  if (!PyArg_ParseTuple (args, "O!O", py_composition_type, &py_composition,
          &py_seq)) {
    PyErr_BadArgument ();
    return NULL;
  }

  composition = pyg_boxed_get (py_composition, GstVideoComposition);

  sequence = PySequence_Fast (py_seq, "offsets must be a sequence");
  if (sequence == NULL)
    return NULL;

  if ((n_entries = PySequence_Fast_GET_SIZE (sequence)) != GST_VIDEO_MAX_COMPONENTS) {
    Py_DECREF (sequence);
    PyErr_Format (PyExc_ValueError, "offsets must have exactly %u entries",
        GST_VIDEO_MAX_COMPONENTS);
    return NULL;
  }

  for (idx = 0; idx < n_entries; idx++) {
    composition->offsets[idx] =
        PyFloat_AsDouble (PySequence_Fast_GET_ITEM (sequence, idx));
  }

  Py_DECREF (sequence);

  Py_RETURN_NONE;
}

static PyObject *
_gst_video_composition_get_scales (PyObject * self, PyObject * args)
{
  PyTypeObject *py_composition_type = NULL;
  PyObject *py_composition = NULL, *py_list = NULL;
  GstVideoComposition *composition = NULL;
  guint idx = 0;

  py_composition_type = pygobject_lookup_class (gst_video_composition_get_type ());
  if (!PyArg_ParseTuple (args, "O!", py_composition_type, &py_composition)) {
    PyErr_BadArgument ();
    return NULL;
  }

  composition = pyg_boxed_get (py_composition, GstVideoComposition);

  if ((py_list = PyList_New (GST_VIDEO_MAX_COMPONENTS)) == NULL) {
    PyErr_NoMemory ();
    return NULL;
  }

  for (idx = 0; idx < GST_VIDEO_MAX_COMPONENTS; idx++) {
    PyList_SET_ITEM (py_list, (Py_ssize_t) idx,
        PyFloat_FromDouble (composition->scales[idx]));
  }

  return py_list;
}

static PyObject *
_gst_video_composition_set_scales (PyObject * self, PyObject * args)
{
  PyTypeObject *py_composition_type = NULL;
  PyObject *py_composition = NULL, *py_seq = NULL, *sequence = NULL;
  GstVideoComposition *composition = NULL;
  guint idx = 0, n_entries = 0;

  py_composition_type = pygobject_lookup_class (gst_video_composition_get_type ());
  if (!PyArg_ParseTuple (args, "O!O", py_composition_type, &py_composition,
          &py_seq)) {
    PyErr_BadArgument ();
    return NULL;
  }

  composition = pyg_boxed_get (py_composition, GstVideoComposition);

  sequence = PySequence_Fast (py_seq, "scales must be a sequence");
  if (sequence == NULL)
    return NULL;

  if ((n_entries = PySequence_Fast_GET_SIZE (sequence)) != GST_VIDEO_MAX_COMPONENTS) {
    Py_DECREF (sequence);
    PyErr_Format (PyExc_ValueError, "scales must have exactly %u entries",
        GST_VIDEO_MAX_COMPONENTS);
    return NULL;
  }

  for (idx = 0; idx < n_entries; idx++) {
    composition->scales[idx] =
        PyFloat_AsDouble (PySequence_Fast_GET_ITEM (sequence, idx));
  }

  Py_DECREF (sequence);

  Py_RETURN_NONE;
}

static PyObject *
_gst_video_converter_engine_compose (PyObject * self, PyObject * args)
{
  PyTypeObject *engine_type = NULL;
  PyObject *py_engine = NULL, *py_compositions = NULL, *py_fence = NULL;
  GstVideoConverterEngine *engine = NULL;
  GstVideoComposition **compositions = NULL;
  gpointer fence = NULL;
  gboolean success = FALSE, is_async = FALSE;
  Py_ssize_t n_compositions = 0, idx = 0;

  engine_type = pygobject_lookup_class (gst_video_converter_engine_get_type ());
  if (!PyArg_ParseTuple (args, "O!Op", engine_type, &py_engine,
          &py_compositions, &is_async)) {
    PyErr_BadArgument ();
    return NULL;
  }

  engine = pyg_boxed_get (py_engine, GstVideoConverterEngine);

  if ((n_compositions = PySequence_Length (py_compositions)) < 0)
    return NULL;

  compositions = g_new0 (GstVideoComposition *, n_compositions);

  for (idx = 0; idx < n_compositions; idx++) {
    PyObject *py_item = PySequence_GetItem (py_compositions, idx);

    if (py_item == NULL) {
      g_free (compositions);
      return NULL;
    }

    if (!pyg_boxed_check (py_item, gst_video_composition_get_type ())) {
      Py_DECREF (py_item);
      g_free (compositions);
      PyErr_SetString (PyExc_TypeError,
          "compositions must be a sequence of GstQtiVideo.Composition");
      return NULL;
    }

    compositions[idx] = pyg_boxed_get (py_item, GstVideoComposition);
    Py_DECREF (py_item);
  }

  success = gst_video_converter_engine_compose (engine, compositions,
      (guint) n_compositions, is_async ? &fence : NULL);

  g_free (compositions);

  if (!success || !is_async)
    return Py_BuildValue ("(OO)", success ? Py_True : Py_False, Py_None);

  if ((py_fence = pyg_pointer_new (G_TYPE_POINTER, fence)) == NULL)
    return NULL;

  return Py_BuildValue ("(ON)", Py_True, py_fence);
}

static PyObject *
_gst_video_converter_engine_wait_fence (PyObject * self, PyObject * args)
{
  PyTypeObject *engine_type = NULL;
  PyObject *py_engine = NULL, *py_fence = NULL;
  GstVideoConverterEngine *engine = NULL;
  gpointer fence = NULL;
  gboolean success = FALSE;

  engine_type = pygobject_lookup_class (gst_video_converter_engine_get_type ());
  if (!PyArg_ParseTuple (args, "O!O", engine_type, &py_engine, &py_fence)) {
    PyErr_BadArgument ();
    return NULL;
  }

  engine = pyg_boxed_get (py_engine, GstVideoConverterEngine);

  if (py_fence == Py_None) {
    success = TRUE;
  } else {
    if (!pyg_pointer_check (py_fence, G_TYPE_POINTER)) {
      PyErr_SetString (PyExc_TypeError, "expected a fence pointer or None");
      return NULL;
    }

    fence = pyg_pointer_get_ptr (py_fence);
    success = gst_video_converter_engine_wait_fence (engine, fence);
  }

  return PyBool_FromLong (success);
}

static PyMethodDef _gi_gst_qti_functions[] = {
    {"ml_frame_get_memory_view", (PyCFunction) _gst_ml_frame_get_memory_view, METH_VARARGS, NULL},
    {"ml_keypoint_get_name", (PyCFunction) _gst_ml_keypoint_get_name, METH_VARARGS, NULL},
    {"ml_keypoint_set_name", (PyCFunction) _gst_ml_keypoint_set_name, METH_VARARGS, NULL},
    {"ml_classification_get_name", (PyCFunction) _gst_ml_classification_get_name, METH_VARARGS, NULL},
    {"ml_classification_set_name", (PyCFunction) _gst_ml_classification_set_name, METH_VARARGS, NULL},
    {"ml_classification_get_xtraparams", (PyCFunction) _gst_ml_classification_get_xtraparams, METH_VARARGS, NULL},
    {"ml_classification_set_xtraparams", (PyCFunction) _gst_ml_classification_set_xtraparams, METH_VARARGS, NULL},
    {"ml_detection_get_name", (PyCFunction) _gst_ml_detection_get_name, METH_VARARGS, NULL},
    {"ml_detection_set_name", (PyCFunction) _gst_ml_detection_set_name, METH_VARARGS, NULL},
    {"ml_detection_get_landmarks", (PyCFunction) _gst_ml_detection_get_landmarks, METH_VARARGS, NULL},
    {"ml_detection_set_landmarks", (PyCFunction) _gst_ml_detection_set_landmarks, METH_VARARGS, NULL},
    {"ml_detection_get_xtraparams", (PyCFunction) _gst_ml_detection_get_xtraparams, METH_VARARGS, NULL},
    {"ml_detection_set_xtraparams", (PyCFunction) _gst_ml_detection_set_xtraparams, METH_VARARGS, NULL},
    {"ml_pose_get_name", (PyCFunction) _gst_ml_pose_get_name, METH_VARARGS, NULL},
    {"ml_pose_set_name", (PyCFunction) _gst_ml_pose_set_name, METH_VARARGS, NULL},
    {"ml_pose_get_keypoints", (PyCFunction) _gst_ml_pose_get_keypoints, METH_VARARGS, NULL},
    {"ml_pose_set_keypoints", (PyCFunction) _gst_ml_pose_set_keypoints, METH_VARARGS, NULL},
    {"ml_pose_get_links", (PyCFunction) _gst_ml_pose_get_links, METH_VARARGS, NULL},
    {"ml_pose_set_links", (PyCFunction) _gst_ml_pose_set_links, METH_VARARGS, NULL},
    {"ml_pose_get_xtraparams", (PyCFunction) _gst_ml_pose_get_xtraparams, METH_VARARGS, NULL},
    {"ml_pose_set_xtraparams", (PyCFunction) _gst_ml_pose_set_xtraparams, METH_VARARGS, NULL},
    {"ml_segmentation_get_labels", (PyCFunction) _gst_ml_segmentation_get_labels, METH_VARARGS, NULL},
    {"ml_segmentation_set_labels", (PyCFunction) _gst_ml_segmentation_set_labels, METH_VARARGS, NULL},
    {"ml_segmentation_get_colors", (PyCFunction) _gst_ml_segmentation_get_colors, METH_VARARGS, NULL},
    {"ml_segmentation_set_colors", (PyCFunction) _gst_ml_segmentation_set_colors, METH_VARARGS, NULL},
    {"ml_segmentation_get_xtraparams", (PyCFunction) _gst_ml_segmentation_get_xtraparams, METH_VARARGS, NULL},
    {"ml_segmentation_set_xtraparams", (PyCFunction) _gst_ml_segmentation_set_xtraparams, METH_VARARGS, NULL},
    {"ml_depth_map_get_values", (PyCFunction) _gst_ml_depth_map_get_values, METH_VARARGS, NULL},
    {"ml_depth_map_set_values", (PyCFunction) _gst_ml_depth_map_set_values, METH_VARARGS, NULL},
    {"ml_depth_map_get_colors", (PyCFunction) _gst_ml_depth_map_get_colors, METH_VARARGS, NULL},
    {"ml_depth_map_set_colors", (PyCFunction) _gst_ml_depth_map_set_colors, METH_VARARGS, NULL},
    {"ml_depth_map_get_xtraparams", (PyCFunction) _gst_ml_depth_map_get_xtraparams, METH_VARARGS, NULL},
    {"ml_depth_map_set_xtraparams", (PyCFunction) _gst_ml_depth_map_set_xtraparams, METH_VARARGS, NULL},
    {"video_region_of_interest_meta_get_quadrilateral", (PyCFunction) _gst_video_region_of_interest_meta_get_quadrilateral, METH_VARARGS, NULL},
    {"video_converter_engine_compose", (PyCFunction) _gst_video_converter_engine_compose, METH_VARARGS, NULL},
    {"video_converter_engine_wait_fence", (PyCFunction) _gst_video_converter_engine_wait_fence, METH_VARARGS, NULL},
    {"video_composition_get_blits", (PyCFunction) _gst_video_composition_get_blits, METH_VARARGS, NULL},
    {"video_composition_set_blits", (PyCFunction) _gst_video_composition_set_blits, METH_VARARGS, NULL},
    {"video_composition_get_buffer", (PyCFunction) _gst_video_composition_get_buffer, METH_VARARGS, NULL},
    {"video_composition_set_buffer", (PyCFunction) _gst_video_composition_set_buffer, METH_VARARGS, NULL},
    {"video_composition_get_info", (PyCFunction) _gst_video_composition_get_info, METH_VARARGS, NULL},
    {"video_composition_set_info", (PyCFunction) _gst_video_composition_set_info, METH_VARARGS, NULL},
    {"video_composition_get_offsets", (PyCFunction) _gst_video_composition_get_offsets, METH_VARARGS, NULL},
    {"video_composition_set_offsets", (PyCFunction) _gst_video_composition_set_offsets, METH_VARARGS, NULL},
    {"video_composition_get_scales", (PyCFunction) _gst_video_composition_get_scales, METH_VARARGS, NULL},
    {"video_composition_set_scales", (PyCFunction) _gst_video_composition_set_scales, METH_VARARGS, NULL},
    {NULL, NULL, 0, NULL}
};

static struct PyModuleDef _gi_gst_qti_module = {
    PyModuleDef_HEAD_INIT,
    "_gi_gst_qti",
    "C helpers for GstQti GI overrides",
    -1,
    _gi_gst_qti_functions,
    NULL,
    NULL,
    NULL,
    NULL
};

PyMODINIT_FUNC PyInit__gi_gst_qti(void)
{
    pygobject_init(3, 0, 0);
    return PyModule_Create(&_gi_gst_qti_module);
}
