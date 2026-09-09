# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

from __future__ import annotations

from typing import Any

from ._utils import gst


class Buffer:
    """Convenience wrapper around GstBuffer for reading data, resizing writable buffers, and managing timestamps.
    """
    def __init__(self, gst_buffer: Any | None = None, size: int | None = None) -> None:
        """Initializes the object.

        Args:
            gst_buffer: Gst buffer value. Type: Any | None.
            size: Buffer size in bytes. Type: int | None.

        Returns:
            None.
        """
        self._Gst = gst()
        if gst_buffer is not None:
            self._buf: Any | None = gst_buffer
        elif size is not None:
            self._buf = self._Gst.Buffer.new_allocate(None, int(size), None)
        else:
            self._buf = None

    def data(self) -> memoryview | None:
        """Maps the buffer for reading and returns the payload bytes.

        Returns:
            memoryview | None: Result of the operation.
        """
        if not self.valid():
            return None
        flags = self._Gst.MapFlags.WRITE if self.is_writable() else self._Gst.MapFlags.READ
        ok, info = self._buf.map(flags)
        if not ok:
            return None
        try:
            return memoryview(info.data)
        finally:
            self._buf.unmap(info)

    def size(self) -> int:
        """Returns the current buffer size in bytes.

        Returns:
            int: Result of the operation.
        """
        return int(self._buf.get_size()) if self.valid() else 0

    def resize(self, n: int) -> None:
        """Resizes the wrapped buffer when it is writable.

        Args:
            n: Numerator value. Type: int.

        Returns:
            None.
        """
        if not self.valid():
            self._buf = self._Gst.Buffer.new_allocate(None, int(n), None)
            return
        new_buf = self._Gst.Buffer.new_allocate(None, int(n), None)
        old = self.data()
        if old is not None:
            ok, info = new_buf.map(self._Gst.MapFlags.WRITE)
            if ok:
                try:
                    info.data[: min(len(old), int(n))] = old[: min(len(old), int(n))]
                finally:
                    new_buf.unmap(info)
        self._buf = new_buf

    @property
    def pts(self) -> int | None:
        """Returns the presentation timestamp.

        Returns:
            int | None: Result of the operation.
        """
        return self._buf.pts if self.valid() else None

    @pts.setter
    def pts(self, pts_ns: int) -> None:
        """Sets the presentation timestamp on the wrapped buffer."""
        if self.valid():
            self._buf.pts = pts_ns

    @property
    def dts(self) -> int | None:
        """Returns the decode timestamp.

        Returns:
            int | None: Result of the operation.
        """
        return self._buf.dts if self.valid() else None

    @dts.setter
    def dts(self, dts_ns: int) -> None:
        """Sets the decode timestamp on the wrapped buffer."""
        if self.valid():
            self._buf.dts = dts_ns

    @property
    def duration(self) -> int | None:
        """Returns the buffer duration.

        Returns:
            int | None: Result of the operation.
        """
        return self._buf.duration if self.valid() else None

    @duration.setter
    def duration(self, duration_ns: int) -> None:
        """Sets the duration on the wrapped buffer."""
        if self.valid():
            self._buf.duration = duration_ns

    def is_writable(self) -> bool:
        """Returns whether the wrapped GstBuffer is writable.

        Returns:
            bool: Result of the operation.
        """
        return self.valid() and bool(self._buf.is_writable())

    def is_readonly(self) -> bool:
        """Returns whether the wrapped GstBuffer is read-only.

        Returns:
            bool: Result of the operation.
        """
        return self.valid() and not self.is_writable()

    def make_writable(self) -> bool:
        """Ensures the payload is writable, copying the buffer if it is shared.

        Returns:
            bool: True when the buffer is writable afterwards.
        """
        if not self.valid():
            return False
        if not self.is_writable():
            self._buf = self._buf.copy()
        return self.is_writable()

    def valid(self) -> bool:
        """Returns whether this wrapper currently contains a GstBuffer.

        Returns:
            bool: Result of the operation.
        """
        return self._buf is not None

    def get_raw_buffer(self) -> Any | None:
        """Transfers the wrapped GstBuffer reference and clears this wrapper.

        Returns:
            Any | None: Result of the operation.
        """
        buf = self._buf
        self._buf = None
        return buf
