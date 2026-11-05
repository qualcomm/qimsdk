#
# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear
import typing
import gi
import numpy as np

gi.require_version('Gst', '1.0')

from gi.repository import Gst
from gi.overrides import override
from gi.overrides import _gi_gst_qti  # type: ignore[attr-defined]
_gi_gst_qti


if typing.TYPE_CHECKING:
    # Import stubs for type checking this file.
    from gi.repository import GstQtiML
else:
    from gi.module import get_introspection_module
    GstQtiML = get_introspection_module('GstQtiML')

__all__ = []

_ML_TYPE_TO_NUMPY_DTYPE = {
    GstQtiML.Type.INT8: np.int8,
    GstQtiML.Type.UINT8: np.uint8,
    GstQtiML.Type.INT16: np.int16,
    GstQtiML.Type.UINT16: np.uint16,
    GstQtiML.Type.INT32: np.int32,
    GstQtiML.Type.UINT32: np.uint32,
    GstQtiML.Type.INT64: np.int64,
    GstQtiML.Type.UINT64: np.uint64,
    GstQtiML.Type.FLOAT16: np.float16,
    GstQtiML.Type.FLOAT32: np.float32,
}

class Frame(GstQtiML.Frame):
    def get_tensor(self, index: int) -> np.ndarray:  # type: ignore[override]
        """
        Build a zero-copy NumPy array from GstMapInfo of GstMLFrame:
        """
        mv = _gi_gst_qti.ml_frame_get_memory_view(self, index)

        if mv is None:
            return np.empty(0, dtype=np.uint8)

        tensor = np.frombuffer(mv, dtype=_ML_TYPE_TO_NUMPY_DTYPE[self.info.types[index]])
        n_dimensions = int(self.info.n_dimensions[index])

        # Since multidimensional arrays are squahed when translated calculate offset
        offset = index * int(GstQtiML.TENSOR_MAX_DIMS) if (index > 0) else 0

        dims = [int(self.info.tensors[offset + i]) for i in range(n_dimensions)]
        tensor = tensor.reshape(tuple(dims))  # zero-copy reshape if contiguous

        return tensor

override(Frame)
__all__.append('Frame')


class Keypoint(GstQtiML.Keypoint):
    def __str__(self) -> str:
        string = f"name: {self.name}, confidence: {self.confidence}, "
        string += f"color: {self.color}, x: {self.x}, y: {self.y}"
        return string

    @property
    def name(self) -> str:
        return _gi_gst_qti.ml_keypoint_get_name(self)

    @name.setter
    def name(self, value: str) -> None:
        _gi_gst_qti.ml_keypoint_set_name(self, value)

override(Keypoint)
__all__.append("Keypoint")


class KeypointLink(GstQtiML.KeypointLink):
    def __str__(self) -> str:
        return f"link: [{self.l_kp}] <-> [{self.r_kp}], color: {self.color}"

override(KeypointLink)
__all__.append("KeypointLink")


class Classification(GstQtiML.Classification):
    def __str__(self) -> str:
        string = f"name: {self.name}, confidence: {self.confidence}, "
        string += f"color: {self.color}, xtraparams: {self.xtraparams}"
        return string

    @property
    def name(self) -> str:
        return _gi_gst_qti.ml_classification_get_name(self)

    @name.setter
    def name(self, value: str) -> None:
        _gi_gst_qti.ml_classification_set_name(self, value)

    @property
    def xtraparams(self) -> Gst.Structure:
        return _gi_gst_qti.ml_classification_get_xtraparams(self)

    @xtraparams.setter
    def xtraparams(self, value: Gst.Structure) -> None:
        _gi_gst_qti.ml_classification_set_xtraparams(self, value)

override(Classification)
__all__.append("Classification")


class Detection(GstQtiML.Detection):
    def __str__(self) -> str:
        string = f"name: {self.name}, confidence: {self.confidence}, "
        string += f"color: {self.color}, left: {self.left}, top: {self.top}, "
        string += f"right: {self.right}, bottom: {self.bottom}, landmarks: ["

        if self.landmarks is not None:
            string += ' | '.join(str(k) for k in self.landmarks)
        else:
            string += "None"

        string += f"], xtraparams: {self.xtraparams}"
        return string

    @property
    def name(self) -> str:
        return _gi_gst_qti.ml_detection_get_name(self)

    @name.setter
    def name(self, value: str) -> None:
        _gi_gst_qti.ml_detection_set_name(self, value)

    @property
    def landmarks(self) -> list:
        return _gi_gst_qti.ml_detection_get_landmarks(self)

    @landmarks.setter
    def landmarks(self, value) -> None:
        _gi_gst_qti.ml_detection_set_landmarks(self, value)

    @property
    def xtraparams(self) -> Gst.Structure:
        return _gi_gst_qti.ml_detection_get_xtraparams(self)

    @xtraparams.setter
    def xtraparams(self, value: Gst.Structure) -> None:
        _gi_gst_qti.ml_detection_set_xtraparams(self, value)

override(Detection)
__all__.append("Detection")


class Pose(GstQtiML.Pose):
    def __str__(self) -> str:
        string = f"name: {self.name}, confidence: {self.confidence}, keypoints: ["

        if self.keypoints is not None:
            string += ' | '.join(str(k) for k in self.keypoints)
        else:
            string += "None"

        string += "], links: ["

        if self.links is not None:
            string += ' | '.join(str(k) for k in self.links)
        else:
            string += "None"

        string += f"], xtraparams: {self.xtraparams}"
        return string

    @property
    def name(self) -> str:
        return _gi_gst_qti.ml_pose_get_name(self)

    @name.setter
    def name(self, value: str) -> None:
        _gi_gst_qti.ml_pose_set_name(self, value)

    @property
    def keypoints(self) -> list:
        return _gi_gst_qti.ml_pose_get_keypoints(self)

    @keypoints.setter
    def keypoints(self, value) -> None:
        _gi_gst_qti.ml_pose_set_keypoints(self, value)

    @property
    def links(self) -> list:
        return _gi_gst_qti.ml_pose_get_links(self)

    @links.setter
    def links(self, value) -> None:
        _gi_gst_qti.ml_pose_set_links(self, value)

    @property
    def xtraparams(self) -> Gst.Structure:
        return _gi_gst_qti.ml_pose_get_xtraparams(self)

    @xtraparams.setter
    def xtraparams(self, value: Gst.Structure) -> None:
        _gi_gst_qti.ml_pose_set_xtraparams(self, value)

override(Pose)
__all__.append("Pose")


class Segmentation(GstQtiML.Segmentation):
    def __str__(self) -> str:
        string = f"columns: {self.n_columns}, rows: {self.n_rows}"
        string += f", xtraparams: {self.xtraparams}"
        return string

    @property
    def labels(self) -> list:
        return _gi_gst_qti.ml_segmentation_get_labels(self)

    @labels.setter
    def labels(self, value) -> None:
        _gi_gst_qti.ml_segmentation_set_labels(self, value)

    @property
    def colors(self) -> list:
        return _gi_gst_qti.ml_segmentation_get_colors(self)

    @colors.setter
    def colors(self, value) -> None:
        _gi_gst_qti.ml_segmentation_set_colors(self, value)

    @property
    def xtraparams(self) -> Gst.Structure:
        return _gi_gst_qti.ml_segmentation_get_xtraparams(self)

    @xtraparams.setter
    def xtraparams(self, value: Gst.Structure) -> None:
        _gi_gst_qti.ml_segmentation_set_xtraparams(self, value)

override(Segmentation)
__all__.append("Segmentation")


class DepthMap(GstQtiML.DepthMap):
    def __str__(self) -> str:
        string = f"columns: {self.n_columns}, rows: {self.n_rows}"
        string += f", xtraparams: {self.xtraparams}"
        return string

    @property
    def values(self) -> list:
        return _gi_gst_qti.ml_depth_map_get_values(self)

    @values.setter
    def values(self, value) -> None:
        _gi_gst_qti.ml_depth_map_set_values(self, value)

    @property
    def colors(self) -> list:
        return _gi_gst_qti.ml_depth_map_get_colors(self)

    @colors.setter
    def colors(self, value) -> None:
        _gi_gst_qti.ml_depth_map_set_colors(self, value)

    @property
    def xtraparams(self) -> Gst.Structure:
        return _gi_gst_qti.ml_depth_map_get_xtraparams(self)

    @xtraparams.setter
    def xtraparams(self, value: Gst.Structure) -> None:
        _gi_gst_qti.ml_depth_map_set_xtraparams(self, value)

override(DepthMap)
__all__.append("DepthMap")


def __embedding_meta_get_embedding(self) -> np.ndarray:
    """
    Get the embedding vector of the GstQtiML.EmbeddingMeta as NumPy array.
    """
    data = _gi_gst_qti.ml_embedding_meta_get_embedding(self)
    dtype = _ML_TYPE_TO_NUMPY_DTYPE.get(self.type)

    if data is None or dtype is None:
        return np.empty(0, dtype=np.float32)

    dims = tuple(int(dim) for dim in self.dims[:self.n_dims])
    return np.frombuffer(data, dtype=dtype).reshape(dims)


def __embedding_meta_set_embedding(self, value) -> None:
    """
    Set the embedding vector of the GstQtiML.EmbeddingMeta from NumPy array.
    """
    if value is None:
        _gi_gst_qti.ml_embedding_meta_set_embedding(self, None, 0, ())
        return

    # The meta data is expected to be contiguous and in native byte order.
    value = np.ascontiguousarray(value)

    if not value.dtype.isnative:
        value = value.astype(value.dtype.newbyteorder('='))

    mltype = next((mltype for mltype, dtype in _ML_TYPE_TO_NUMPY_DTYPE.items()
                   if dtype == value.dtype.type), None)

    if mltype is None:
        raise TypeError(f"unsupported embedding data type: {value.dtype}")

    _gi_gst_qti.embedding_meta_set_embedding(
        self, value, int(mltype), value.shape)


GstQtiML.EmbeddingMeta.get_embedding = __embedding_meta_get_embedding
GstQtiML.EmbeddingMeta.set_embedding = __embedding_meta_set_embedding
