# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

from __future__ import annotations

from typing import Any, List

from ._element import Element
from ._utils import gst


class MLVideoBlit:
    """Read-only view of one blit entry passed to a preprocess callback.

    Wraps a single ``GstVideo.Blit`` (source quadrilateral, destination
    rectangle, and the input video frame) with the source buffer already
    mapped for reading.
    """

    def __init__(self, blit: Any) -> None:
        """Initializes the object.

        Args:
            blit: Raw GstQtiVideo.Blit boxed struct. Type: Any.

        Returns:
            None.
        """
        self._blit = blit
        self._map_info: Any | None = None

    @property
    def destination(self) -> Any:
        """Returns the destination rectangle (x, y, w, h) in the output frame.

        Returns:
            Any: GstVideo.VideoRectangle with x/y/w/h fields.
        """
        return self._blit.destination

    @property
    def alpha(self) -> int:
        """Returns the blit's global alpha value.

        Returns:
            int: 0 (fully transparent) to 255 (fully opaque).
        """
        return int(self._blit.alpha)

    @property
    def info(self) -> Any:
        """Returns the GstVideo.VideoInfo describing the source buffer layout.

        Returns:
            Any: GstVideo.VideoInfo, or None if the blit carries no buffer.
        """
        return self._blit.info

    def planes(self) -> List[memoryview]:
        """Maps the source buffer for reading and returns one memoryview per plane.

        The mapping is cached for the lifetime of this MLVideoBlit; callers
        should not hold onto the returned memoryviews past the preprocess
        callback's return.

        Returns:
            List[memoryview]: One memoryview per video plane, sliced by
                offset/stride from the underlying GstVideo.VideoInfo. Empty
                if the blit has no buffer or info.
        """
        buf = self._blit.buffer
        info = self._blit.info
        if buf is None or info is None:
            return []

        if self._map_info is None:
            ok, map_info = buf.map(gst().MapFlags.READ)
            if not ok:
                return []
            self._map_info = map_info

        data = memoryview(self._map_info.data)
        finfo = info.finfo
        planes = []
        for i in range(finfo.n_planes):
            height = info.height if i == 0 else (info.height + 1) // 2
            start = info.offset[i]
            end = start + info.stride[i] * height
            planes.append(data[start:end])
        return planes

    def unmap(self) -> None:
        """Releases the source buffer mapping acquired by planes(), if any.

        Returns:
            None.
        """
        if self._map_info is not None:
            self._blit.buffer.unmap(self._map_info)
            self._map_info = None


def _wrap_blits(raw_blits: Any) -> List[MLVideoBlit]:
    """Converts a raw GstQtiVideo.Blits into a list of MLVideoBlit wrappers.

    Args:
        raw_blits: Raw GstQtiVideo.Blits boxed struct. Type: Any.

    Returns:
        List[MLVideoBlit]: Result of the operation.
    """
    if raw_blits is None:
        return []
    return [MLVideoBlit(raw_blits.entry(i)) for i in range(raw_blits.size())]


class MLVConverter(Element):
    """Wraps qtimlvconverter and connects a Python preprocess callback to its 'process' signal."""

    def __init__(self, name: str | None = None, *, existing: Any | None = None) -> None:
        """Initializes the object.

        Args:
            name: Name of the element, pad, pipeline, file, or configuration entry. Type: str | None.
            existing: Existing GStreamer element to wrap instead of creating a new one. Type: Any | None.

        Returns:
            None.
        """
        super().__init__("qtimlvconverter" if existing is None else None, name, existing=existing)
        self._signal_handler_id: int | None = None
        self._wrapped_handler: Any | None = None

    @classmethod
    def cast(cls, element: Any) -> "MLVConverter":
        """Creates a typed SDK wrapper around an existing GStreamer element.

        Args:
            element: Element value. Type: Any.

        Returns:
            "MLVConverter": Result of the operation.
        """
        raw = element.get_raw() if hasattr(element, "get_raw") else element
        return cls(existing=raw)

    def _wrap_handler(self, handler: Any) -> Any:
        """Adapts the user callback to the GStreamer 'process' signal signature.

        Args:
            handler: User callback to register. Type: Any.

        Returns:
            Any: Result of the operation.
        """
        def _on_process(_mlvconverter, raw_blits, outmlsample):
            outmlframe = outmlsample.get_frame(self._Gst.MapFlags.WRITE)
            if outmlframe is None:
                return False

            blits = _wrap_blits(raw_blits)
            try:
                return bool(handler(blits, outmlframe))
            finally:
                for blit in blits:
                    blit.unmap()

        return _on_process

    def set_handler(self, handler: Any) -> "MLVConverter":
        """Registers a Python callback for external preprocessing.

        Args:
            handler: User callback with signature (blits, outmlframe) -> bool. Type: Any.

        Returns:
            "MLVConverter": Result of the operation.
        """
        if not callable(handler):
            raise ValueError("Preprocess handler callback is empty")

        # Enable external/custom preprocess mode for qtimlvconverter.
        self.set(engine="none")

        if self._signal_handler_id:
            try:
                self._elem.disconnect(self._signal_handler_id)
            except Exception:
                pass
            self._signal_handler_id = None

        self._wrapped_handler = self._wrap_handler(handler)
        self._signal_handler_id = self._elem.connect("process", self._wrapped_handler)
        return self
