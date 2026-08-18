# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

from __future__ import annotations

import signal
import threading
from typing import Any, Set

from .typing import ShutdownHandler
from ._utils import gst, glib
from ._logging import (
    configure_gst_debug_for_runtime,
    install_gst_debug_hook,
    mark_runtime_started,
    restore_gst_debug_hook,
)


_JOIN_TIMEOUT = 2


class _Runtime:
    """Owns the shared GLib main loop, signal handlers, and registered active pipelines.
    """
    def __init__(self) -> None:
        """Initializes the object.

        Returns:
            None.
        """
        self._GLib = glib()
        self._Gst = gst()

        configure_gst_debug_for_runtime(self._Gst)
        install_gst_debug_hook()
        mark_runtime_started()

        self._loop = self._GLib.MainLoop()

        self._thread = threading.Thread(target=self._loop.run, name="imsdk-glib", daemon=True)

        self._pipelines: Set[Any] = set()
        self._shutdown_requested = False
        self._lock = threading.Lock()

        self._shutdown_handlers: list[ShutdownHandler] = []

        self._thread.start()

        self._sig_sources = []

        if hasattr(self._GLib, "unix_signal_source_new"):
            for sig in (signal.SIGINT, signal.SIGTERM):
                try:
                    src = self._GLib.unix_signal_source_new(sig)
                    try:
                        src.set_callback(self._on_unix_signal)
                    except TypeError:
                        src.set_callback(self._on_unix_signal, None)
                    src.attach(None)
                    self._sig_sources.append(src)
                except Exception:
                    pass
        else:
            try:
                import signal as _py_signal

                def _py_handler(signum, frame):
                    """Performs the py handler operation used by the SDK.

                    Args:
                        signum: Signum value.
                        frame: Frame value.
                    """
                    try:
                        self.request_shutdown()
                    except Exception:
                        pass

                _py_signal.signal(_py_signal.SIGINT, _py_handler)
                _py_signal.signal(_py_signal.SIGTERM, _py_handler)
            except Exception:
                pass

    def register_pipeline(self, pipeline: Any) -> None:
        """Performs the register pipeline operation used by the SDK.

        Args:
            pipeline: Pipeline value. Type: Any.

        Returns:
            None.
        """
        with self._lock:
            self._pipelines.add(pipeline)

    def unregister_pipeline(self, pipeline: Any) -> None:
        """Performs the unregister pipeline operation used by the SDK.

        Args:
            pipeline: Pipeline value. Type: Any.

        Returns:
            None.
        """
        finalize_loop = False
        with self._lock:
            self._pipelines.discard(pipeline)
            if self._shutdown_requested and not self._pipelines:
                finalize_loop = True
        if finalize_loop:
            self._finalize_loop()

    def add_shutdown_handler(self, handler: ShutdownHandler) -> ShutdownHandler:
        """Performs the add shutdown handler operation used by the SDK.

        Args:
            handler: User callback to register. Type: ShutdownHandler.

        Returns:
            ShutdownHandler: Result of the operation.
        """
        self._shutdown_handlers.append(handler)
        return handler

    def remove_shutdown_handler(self, handler: ShutdownHandler) -> None:
        """Performs the remove shutdown handler operation used by the SDK.

        Args:
            handler: User callback to register. Type: ShutdownHandler.

        Returns:
            None.
        """
        if handler in self._shutdown_handlers:
            self._shutdown_handlers.remove(handler)

    def request_shutdown(self) -> None:
        """Performs the request shutdown operation used by the SDK.

        Returns:
            None.
        """
        finalize_loop = False
        with self._lock:
            if self._shutdown_requested:
                return
            self._shutdown_requested = True
            if not self._pipelines:
                finalize_loop = True

        for handler in list(self._shutdown_handlers):
            try:
                handler()
            except Exception:
                pass

        if finalize_loop:
            self._finalize_loop()

    def stop(self) -> None:
        """Stops or deactivates the wrapped element.

        Returns:
            None.
        """
        self.request_shutdown()
        self._join()

    def _finalize_loop(self) -> None:
        """Performs the finalize loop operation used by the SDK.

        Returns:
            None.
        """
        if self._loop.is_running():
            self._loop.quit()
        restore_gst_debug_hook()
        self._join()

    def _join(self) -> None:
        """Performs the join operation used by the SDK.

        Returns:
            None.
        """
        if threading.current_thread() is not self._thread and self._thread.is_alive():
            self._thread.join(timeout=_JOIN_TIMEOUT)

    def _on_unix_signal(self, *args) -> bool:
        """Performs the on unix signal operation used by the SDK.

        Args:
            args: positional key/value arguments.

        Returns:
            bool: Result of the operation.
        """
        try:
            self.request_shutdown()
        except BaseException:
            pass
        return True


_RUNTIME: _Runtime | None = None


def runtime() -> _Runtime:
    """Returns the shared runtime singleton.

    Returns:
        _Runtime: Result of the operation.
    """
    global _RUNTIME
    if _RUNTIME is None:
        _RUNTIME = _Runtime()
    return _RUNTIME
