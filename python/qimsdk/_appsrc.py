# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

from __future__ import annotations

from typing import Any
from enum import IntEnum

from ._element import Element
from ._buffer import Buffer
from .typing import BufferProducerHandler, EnoughDataHandler
from ._utils import set_gobject_properties


class AppSrc(Element):
    """Wraps a GStreamer appsrc element and provides helpers for pushing buffers and handling flow-control callbacks.
    """
    class Format(IntEnum):
        """Format defines an SDK type used by the IMSDK Python package.
        """
        DEFAULT = 1
        BYTES = 2
        TIME = 3
        BUFFERS = 4
        PERCENT = 5

    def __init__(self, name: str | None = None, *, existing: Any | None = None) -> None:
        """Initializes the object.

        Args:
            name: Name of the element, pad, pipeline, file, or configuration entry. Type: str | None.
            existing: Existing GStreamer element to wrap instead of creating a new one. Type: Any | None.

        Returns:
            None.
        """
        super().__init__("appsrc" if existing is None else None, name, existing=existing)
        self._elem.set_property("emit-signals", True)
        self._buffer_handler: BufferProducerHandler | None = None
        self._enough_handler: EnoughDataHandler | None = None
        self._elem.connect("need-data", self._on_need_data)
        self._elem.connect("enough-data", self._on_enough_data)

    @classmethod
    def cast(cls, element: Any) -> "AppSrc":
        """Creates a typed SDK wrapper around an existing GStreamer element.

        Args:
            element: Element value. Type: Any.

        Returns:
            "AppSrc": Result of the operation.
        """
        raw = element.get_raw() if hasattr(element, "get_raw") else element
        return cls(existing=raw)

    def set(self, *args: Any, **props: Any) -> AppSrc:
        """Sets GObject properties and returns the wrapper for fluent chaining.

        Args:
            args: positional key/value arguments. Type: Any.
            props: Properties to set on the underlying GObject. Type: Any.

        Returns:
            AppSrc: Result of the operation.
        """
        if args:
            if len(args) % 2 != 0:
                raise RuntimeError("AppSrc.set expects property/value pairs")
            for i in range(0, len(args), 2):
                props[str(args[i])] = args[i + 1]
        set_gobject_properties(self._elem, **props)
        return self

    def set_caps(self, caps: Any) -> AppSrc:
        """Sets caps from a StreamFilter, GstCaps, or caps-compatible value.

        Args:
            caps: StreamFilter, GstCaps, or caps-compatible value. Type: Any.

        Returns:
            AppSrc: Result of the operation.
        """
        from ._stream_filter import StreamFilter

        if isinstance(caps, StreamFilter):
            caps = caps._get_caps()
        elif isinstance(caps, str):
            caps = self._Gst.Caps.from_string(caps)

        self._elem.set_property("caps", caps)
        return self

    def set_buffer_producer(self, fn: BufferProducerHandler) -> AppSrc:
        """Registers a callback for incoming samples or requested buffers.

        Args:
            fn: Fn value. Type: BufferProducerHandler.

        Returns:
            AppSrc: Result of the operation.
        """
        self._buffer_handler = fn
        return self

    def set_enough_handler(self, fn: EnoughDataHandler) -> AppSrc:
        """Registers a callback for appsrc enough-data notifications.

        Args:
            fn: Fn value. Type: EnoughDataHandler.

        Returns:
            AppSrc: Result of the operation.
        """
        self._enough_handler = fn
        return self

    def push_buffer(self, buffer: Buffer) -> bool:
        """Pushes a Buffer into appsrc and returns the GStreamer flow result.

        Args:
            buffer: Buffer instance or GstBuffer-compatible object. Type: Buffer.

        Returns:
            bool: Result of the operation.
        """
        gst_buffer = buffer.take_gst_buffer()
        if gst_buffer is None:
            return False
        res = self._elem.emit("push-buffer", gst_buffer)
        return res == self._Gst.FlowReturn.OK

    def end_of_stream(self) -> None:
        """Emits end-of-stream on appsrc.

        Returns:
            None.
        """
        self._elem.emit("end-of-stream")

    def _on_need_data(self, src: object, length: int) -> None:
        """Performs the on need data operation used by the SDK.

        Args:
            src: Source element or pad. Type: object.
            length: Length value. Type: int.

        Returns:
            None.
        """
        try:
            if not self._buffer_handler:
                return
            size = int(length) if length and length > 0 else 1
            buf = Buffer(size=size)
            if self._buffer_handler(buf):
                self.push_buffer(buf)
        except Exception:
            pass

    def _on_enough_data(self, *args: object) -> None:
        """Performs the on enough data operation used by the SDK.

        Args:
            args: positional key/value arguments. Type: object.

        Returns:
            None.
        """
        try:
            if self._enough_handler:
                self._enough_handler()
        except Exception:
            pass
