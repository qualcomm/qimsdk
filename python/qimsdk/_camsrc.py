# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

from __future__ import annotations

from enum import IntEnum
from typing import Any

from ._element import Element


class CamSrc(Element):
    """Typed wrapper around a camera source element with capture and cancel-capture helpers.
    """

    class CaptureMode(IntEnum):
        """CaptureMode defines an SDK type used by the IMSDK Python package.
        """
        STILL = 0
        BURST = 1

    def __init__(self, name: str | None = None, *, existing: Any | None = None) -> None:
        """Initializes the object.

        Args:
            name: Name of the element, pad, pipeline, file, or configuration entry. Type: str | None.
            existing: Existing GStreamer element to wrap instead of creating a new one. Type: Any | None.

        Returns:
            None.
        """
        super().__init__("qtiqmmfsrc" if existing is None else None, name, existing=existing)

    @classmethod
    def cast(cls, element: Any) -> "CamSrc":
        """Creates a typed SDK wrapper around an existing GStreamer element.

        Args:
            element: Element value. Type: Any.

        Returns:
            "CamSrc": Result of the operation.
        """
        raw = element.get_raw() if hasattr(element, "get_raw") else element
        return cls(existing=raw)

    @staticmethod
    def _interpret_emit_result(ret: Any) -> bool:
        """Performs the interpret emit result operation used by the SDK.

        Args:
            ret: Ret value. Type: Any.

        Returns:
            bool: Result of the operation.
        """
        if isinstance(ret, bool):
            return ret
        if isinstance(ret, tuple):
            for item in reversed(ret):
                if isinstance(item, bool):
                    return item
        return bool(ret)

    def capture(
        self,
        *,
        mode: CaptureMode = CaptureMode.STILL,
        count: int = 1,
        metadata: Any | None = None,
    ) -> bool:
        """Requests image capture on the wrapped camera source element.

        Args:
            mode: Capture mode or logging mode. Type: CaptureMode.
            count: Number of images or buffers to process. Type: int.
            metadata: Optional capture metadata. Type: Any | None.

        Returns:
            bool: Result of the operation.
        """
        # The metadata argument is currently unused, but is kept to preserve
        # the full signature of the underlying action signal.
        ret = self._elem.emit(
            "capture-image",
            int(mode),
            int(count),
            metadata,
        )
        return self._interpret_emit_result(ret)

    def image_capture(self, *args: Any, **kwargs: Any) -> bool:
        """Requests image capture on the wrapped camera source element.

        Args:
            args: positional key/value arguments. Type: Any.
            kwargs: Python keyword properties. Type: Any.

        Returns:
            bool: Result of the operation.
        """
        metadata = kwargs.pop("metadata", None)
        mode = kwargs.pop("mode", self.CaptureMode.STILL)
        count = kwargs.pop("count", 1)
        if kwargs:
            raise TypeError(f"Unexpected keyword arguments: {', '.join(kwargs.keys())}")

        if len(args) == 1:
            first = args[0]
            if isinstance(first, self.CaptureMode):
                mode = first
            else:
                count = int(first)
        elif len(args) == 2:
            mode, count = args
        elif len(args) == 3:
            mode, count, metadata = args
        elif len(args) > 3:
            raise TypeError("image_capture accepts at most 3 positional arguments")

        if isinstance(mode, int) and not isinstance(mode, self.CaptureMode):
            mode = self.CaptureMode(mode)

        return self.capture(mode=mode, count=int(count), metadata=metadata)

    def cancel_capture(self) -> bool:
        """Cancels an active image capture operation.

        Returns:
            bool: Result of the operation.
        """
        ret = self._elem.emit("cancel-capture")
        return self._interpret_emit_result(ret)
