# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

from __future__ import annotations

from typing import Any

from ._element import Element
from ._buffer import Buffer
from .typing import BufferConsumerHandler, PrerollHandler, EosHandler
from ._utils import set_gobject_properties


class AppSink(Element):
    """Wraps a GStreamer appsink element and exposes Python callbacks for samples, preroll samples, and EOS events.
    """
    def __init__(self, name: str | None = None, *, existing: Any | None = None) -> None:
        """Initializes the object.

        Args:
            name: Name of the element, pad, pipeline, file, or configuration entry. Type: str | None.
            existing: Existing GStreamer element to wrap instead of creating a new one. Type: Any | None.

        Returns:
            None.
        """
        super().__init__("appsink" if existing is None else None, name, existing=existing)
        self._elem.set_property("emit-signals", True)
        self._buffer_handler: BufferConsumerHandler | None = None
        self._preroll_handler: PrerollHandler | None = None
        self._eos_handler: EosHandler | None = None
        self._elem.connect("new-sample", self._on_new_sample)
        self._elem.connect("new-preroll", self._on_new_preroll)
        self._elem.connect("eos", self._on_eos)

    @classmethod
    def cast(cls, element: Any) -> "AppSink":
        """Creates a typed SDK wrapper around an existing GStreamer element.

        Args:
            element: Element value. Type: Any.

        Returns:
            "AppSink": Result of the operation.
        """
        raw = element.get_raw() if hasattr(element, "get_raw") else element
        return cls(existing=raw)

    def set(self, *args: Any, **props: Any) -> AppSink:
        """Sets GObject properties and returns the wrapper for fluent chaining.

        Args:
            args: positional key/value arguments. Type: Any.
            props: Properties to set on the underlying GObject. Type: Any.

        Returns:
            AppSink: Result of the operation.
        """
        if args:
            if len(args) % 2 != 0:
                raise RuntimeError("AppSink.set expects property/value pairs")
            for i in range(0, len(args), 2):
                props[str(args[i])] = args[i + 1]
        set_gobject_properties(self._elem, **props)
        return self

    def set_buffer_consumer(self, fn: BufferConsumerHandler) -> AppSink:
        """Registers a callback for incoming samples or requested buffers.

        Args:
            fn: Fn value. Type: BufferConsumerHandler.

        Returns:
            AppSink: Result of the operation.
        """
        self._buffer_handler = fn
        return self

    def set_preroll_handler(self, fn: PrerollHandler) -> AppSink:
        """Registers a callback for appsink preroll samples.

        Args:
            fn: Fn value. Type: PrerollHandler.

        Returns:
            AppSink: Result of the operation.
        """
        self._preroll_handler = fn
        return self

    def set_eos_handler(self, fn: EosHandler) -> AppSink:
        """Registers a callback for end-of-stream notifications.

        Args:
            fn: Fn value. Type: EosHandler.

        Returns:
            AppSink: Result of the operation.
        """
        self._eos_handler = fn
        return self

    def _on_new_sample(self, *args: object) -> int:
        """Performs the on new sample operation used by the SDK.

        Args:
            args: positional key/value arguments. Type: object.

        Returns:
            int: Result of the operation.
        """
        sample = self._elem.emit("pull-sample")
        if not sample:
            return self._Gst.FlowReturn.EOS
        try:
            if self._buffer_handler:
                self._buffer_handler(Buffer.from_readable_sample(sample))
            return self._Gst.FlowReturn.OK
        except Exception:
            return self._Gst.FlowReturn.ERROR
        finally:
            try:
                sample.unref()
            except Exception:
                pass

    def _on_new_preroll(self, *args: object) -> int:
        """Performs the on new preroll operation used by the SDK.

        Args:
            args: positional key/value arguments. Type: object.

        Returns:
            int: Result of the operation.
        """
        sample = self._elem.emit("pull-preroll")
        if not sample:
            return self._Gst.FlowReturn.EOS
        try:
            ok = (
                self._preroll_handler(Buffer.from_readable_sample(sample))
                if self._preroll_handler
                else True
            )
            return self._Gst.FlowReturn.OK if ok else self._Gst.FlowReturn.ERROR
        except Exception:
            return self._Gst.FlowReturn.ERROR
        finally:
            try:
                sample.unref()
            except Exception:
                pass

    def _on_eos(self, *args: object) -> None:
        """Performs the on eos operation used by the SDK.

        Args:
            args: positional key/value arguments. Type: object.

        Returns:
            None.
        """
        try:
            if self._eos_handler:
                self._eos_handler()
        except Exception:
            pass
