# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

from typing import Protocol, runtime_checkable


@runtime_checkable
class BufferConsumerHandler(Protocol):
    """Protocol for callbacks that consume buffers from AppSink.
    """

    def __call__(self, buffer: "Buffer") -> None: ...


@runtime_checkable
class PrerollHandler(Protocol):
    """Protocol for callbacks that consume preroll samples from AppSink.
    """

    def __call__(self, buffer: "Buffer") -> bool: ...


@runtime_checkable
class EosHandler(Protocol):
    """Protocol for callbacks invoked when AppSink receives EOS.
    """

    def __call__(self) -> None: ...


@runtime_checkable
class BufferProducerHandler(Protocol):
    """Protocol for callbacks that produce buffers for AppSrc.
    """

    def __call__(self, buffer: "Buffer") -> bool: ...


@runtime_checkable
class EnoughDataHandler(Protocol):
    """Protocol for callbacks invoked when AppSrc has enough queued data.
    """

    def __call__(self) -> None: ...


@runtime_checkable
class ShutdownHandler(Protocol):
    """Protocol for callbacks invoked during runtime shutdown.
    """

    def __call__(self) -> None: ...

class MLPostprocessOutput:
    """Base marker type for MLPostprocess callback output containers.

    Subclasses are used only as callback annotations so MLPostprocess can
    select the correct qtimlpostprocess signal without guessing from function
    or parameter names.
    """

    kind: str


class ImageClassifications(MLPostprocessOutput):
    """Marker for image-classification postprocess output callbacks."""

    kind = "image-classification"


class AudioClassifications(MLPostprocessOutput):
    """Marker for audio-classification postprocess output callbacks."""

    kind = "audio-classification"


class ObjectDetections(MLPostprocessOutput):
    """Marker for object-detection postprocess output callbacks."""

    kind = "object-detection"


class Poses(MLPostprocessOutput):
    """Marker for pose-estimation postprocess output callbacks."""

    kind = "pose-estimation"


class DepthMaps(MLPostprocessOutput):
    """Marker for depth-estimation postprocess output callbacks."""

    kind = "depth-estimation"


class Segmentations(MLPostprocessOutput):
    """Marker for image-segmentation postprocess output callbacks."""

    kind = "image-segmentation"


class Tensors(MLPostprocessOutput):
    """Marker for raw tensor postprocess output callbacks."""

    kind = "tensors"

