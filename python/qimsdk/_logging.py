# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

from __future__ import annotations

import logging
import os
import re
import threading
import time
from enum import IntEnum
from typing import Any

from ._utils import gst


class ImsdkGstLogMode(IntEnum):
    """Selects whether GStreamer logs are emitted in native GST format or parsed into IMSDK format.
    """

    GstLog = 0
    ImsdkLog = 1


class ImsdkLogLevel(IntEnum):
    """Defines the IMSDK logging verbosity levels.
    """

    Error = 0
    Warning = 1
    Info = 2
    Debug = 3


_GST_LOG_MODE = ImsdkGstLogMode.ImsdkLog
_LOG_LEVEL = ImsdkLogLevel.Info
_LOG_HANDLER_INSTALLED = False
_RUNTIME_STARTED = False
_GST_HOOK_INSTALLED = False
_PIPELINE_ELEMENT_MAP: dict[str, str] = {}
if "HOME" not in os.environ:
    raise EnvironmentError("Error: HOME environment variable is not set.")

_GST_DEBUG_FILE_PATH = f"{os.environ['HOME']}/Downloads/gst.log"
_GST_FILE_READER_THREAD: threading.Thread | None = None
_GST_FILE_READER_STOP = threading.Event()
_GST_FILE_POLL_SEC = 0.05

_STATE_RE = re.compile(r"completed state change to\s+([A-Za-z0-9_]+)")
_PERF_RE = re.compile(
    r"(?:<([^>]+)>\s+)?Performance time\s+([0-9]+(?:\.[0-9]+)?)\s+ms,\s+HW utilization:\s+([^,\]\r\n]+)"
)
_GST_FILE_PAYLOAD_RE = re.compile(r"^[^:]+:\d+:[^:]+:(.*)$")

_DEFAULT_CATEGORY_LOG_LEVELS = (
    "GST_STATE",
    "GST_STATES",
    "qtibatch",
    "qticvimgpyramid",
    "qticvoptclflow",
    "qtidfs",
    "qtidngpacker",
    "qtidrmdecryptor",
    "qtimetamux",
    "qtimetatransform",
    "qtimlaconverter",
    "qtimldemux",
    "qtimlmetaextractor",
    "qtimlmetaparser",
    "qtimlpostprocess",
    "qtimlqnn",
    "qtimlsnpe",
    "qtimltflite",
    "qtimlvconverter",
    "qtiobjtracker",
    "qtirestrictedzonedbg",
    "qtivcomposer",
    "qtivoverlay",
    "qtivsplit",
    "qtivtransform",
)


def _log_level_name(level: ImsdkLogLevel) -> str:
    """Performs the log level name operation used by the SDK.

    Args:
        level: IMSDK or GStreamer log level. Type: ImsdkLogLevel.

    Returns:
        str: Result of the operation.
    """
    return {
        ImsdkLogLevel.Error: "ERROR",
        ImsdkLogLevel.Warning: "WARN",
        ImsdkLogLevel.Info: "INFO",
        ImsdkLogLevel.Debug: "DEBUG",
    }.get(level, "INFO")


def _log_level_color(level: ImsdkLogLevel) -> str:
    """Performs the log level color operation used by the SDK.

    Args:
        level: IMSDK or GStreamer log level. Type: ImsdkLogLevel.

    Returns:
        str: Result of the operation.
    """
    return {
        ImsdkLogLevel.Error: "\x1b[31m",
        ImsdkLogLevel.Warning: "\x1b[33m",
        ImsdkLogLevel.Info: "\x1b[32m",
        ImsdkLogLevel.Debug: "\x1b[0m",
    }.get(level, "\x1b[0m")


def _is_color_enabled() -> bool:
    """Performs the is color enabled operation used by the SDK.

    Returns:
        bool: Result of the operation.
    """
    value = os.getenv("IMSDK_LOG_COLOR")
    if not value:
        return True
    return value not in {"0", "false", "FALSE", "off", "OFF"}


class _ImsdkFormatter(logging.Formatter):
    """Formats Python logging records using the IMSDK log prefix and optional color output.
    """
    def format(self, record: logging.LogRecord) -> str:
        """Sets the stream format value.

        Args:
            record: Record value. Type: logging.LogRecord.

        Returns:
            str: Result of the operation.
        """
        try:
            level = ImsdkLogLevel(record.imsdk_level)
        except Exception:
            py_level = record.levelno
            if py_level >= logging.ERROR:
                level = ImsdkLogLevel.Error
            elif py_level >= logging.WARNING:
                level = ImsdkLogLevel.Warning
            elif py_level >= logging.INFO:
                level = ImsdkLogLevel.Info
            else:
                level = ImsdkLogLevel.Debug

        label = _log_level_name(level)
        if _is_color_enabled():
            prefix = f"[IMSDK][{_log_level_color(level)}{label}\x1b[0m]"
        else:
            prefix = f"[IMSDK][{label}]"

        return prefix + (record.getMessage() or "")


def _ensure_logging_handler() -> logging.Logger:
    """Performs the ensure logging handler operation used by the SDK.

    Returns:
        logging.Logger: Result of the operation.
    """
    global _LOG_HANDLER_INSTALLED

    logger = logging.getLogger("imsdk")
    if not _LOG_HANDLER_INSTALLED:
        handler = logging.StreamHandler()
        handler.setFormatter(_ImsdkFormatter())
        logger.addHandler(handler)
        logger.propagate = False
        _LOG_HANDLER_INSTALLED = True

    return logger


def emit_imsdk_log(level: ImsdkLogLevel, message: str) -> None:
    """Emits a message through the IMSDK logger.

    Args:
        level: IMSDK or GStreamer log level. Type: ImsdkLogLevel.
        message: Message text or GStreamer bus/debug message. Type: str.

    Returns:
        None.
    """

    logger = _ensure_logging_handler()
    py_level = {
        ImsdkLogLevel.Error: logging.ERROR,
        ImsdkLogLevel.Warning: logging.WARNING,
        ImsdkLogLevel.Info: logging.INFO,
        ImsdkLogLevel.Debug: logging.DEBUG,
    }[ImsdkLogLevel(level)]
    logger.log(py_level, message, extra={"imsdk_level": int(level)})


# Backward-compatible private alias used by older internal modules.
_emit_imsdk_log = emit_imsdk_log


def SetImsdkLogLevel(level: ImsdkLogLevel) -> None:
    """Sets IMSDK logger verbosity.

    Args:
        level: IMSDK or GStreamer log level. Type: ImsdkLogLevel.

    Returns:
        None.
    """

    global _LOG_LEVEL

    _LOG_LEVEL = ImsdkLogLevel(level)
    py_level = {
        ImsdkLogLevel.Error: logging.ERROR,
        ImsdkLogLevel.Warning: logging.WARNING,
        ImsdkLogLevel.Info: logging.INFO,
        ImsdkLogLevel.Debug: logging.DEBUG,
    }[_LOG_LEVEL]
    _ensure_logging_handler().setLevel(py_level)


def SetImsdkGstLogMode(mode: ImsdkGstLogMode) -> None:
    """Selects how GStreamer debug messages are handled before runtime startup.

    Args:
        mode: Capture mode or logging mode. Type: ImsdkGstLogMode.

    Returns:
        None.
    """

    global _GST_LOG_MODE

    if _RUNTIME_STARTED:
        return
    _GST_LOG_MODE = ImsdkGstLogMode(mode)
    if _GST_LOG_MODE == ImsdkGstLogMode.ImsdkLog:
        _ensure_gst_debug_file_env()


def mark_runtime_started() -> None:
    """Marks the runtime as started so logging mode is locked.

    Returns:
        None.
    """

    global _RUNTIME_STARTED
    _RUNTIME_STARTED = True


def _configure_gst_parser_category_log_levels() -> None:
    """Performs the configure gst parser category log levels operation used by the SDK.

    Returns:
        None.
    """
    Gst = gst()
    for category in _DEFAULT_CATEGORY_LOG_LEVELS:
        try:
            Gst.debug_set_threshold_for_name(category, Gst.DebugLevel.LOG)
        except Exception:
            pass


def _ensure_gst_debug_file_env() -> None:
    """Ensures GStreamer writes debug output to the SDK log file."""
    os.environ["GST_DEBUG_FILE"] = _GST_DEBUG_FILE_PATH


def _extract_obj_and_message_from_gst_file_rest(rest: str) -> tuple[str, str]:
    """Extracts object name and message payload from a GST_DEBUG_FILE line."""
    payload = rest.strip()
    match = _GST_FILE_PAYLOAD_RE.match(payload)
    if match:
        payload = match.group(1).strip()

    if payload.startswith("<"):
        end_index = payload.find(">")
        if end_index > 1:
            return payload[1:end_index], payload[end_index + 1:].strip()

    return "", payload


def _parse_and_print_gst_file_line(line: str, Gst: Any) -> None:
    """Parses a single line from GST_DEBUG_FILE and emits IMSDK logs."""
    text = line.strip()
    if not text:
        return

    parts = text.split(None, 5)
    if len(parts) < 6:
        return

    level_name = parts[3].upper()
    category_name = parts[4]
    obj_name, message = _extract_obj_and_message_from_gst_file_rest(parts[5])
    if not message:
        return

    level_map = {
        "ERROR": int(Gst.DebugLevel.ERROR),
        "WARNING": int(Gst.DebugLevel.WARNING),
        "FIXME": int(Gst.DebugLevel.WARNING),
        "INFO": int(Gst.DebugLevel.INFO),
        "DEBUG": int(Gst.DebugLevel.DEBUG),
        "LOG": int(Gst.DebugLevel.LOG),
        "TRACE": int(Gst.DebugLevel.TRACE),
    }
    level = level_map.get(level_name, int(Gst.DebugLevel.INFO))

    pipeline_name = _find_pipeline_name_for_obj(obj_name)
    _parse_and_print_gst_log(level, category_name, obj_name, pipeline_name, message, Gst)


def _gst_debug_file_reader_loop() -> None:
    """Tails GST_DEBUG_FILE and parses new lines asynchronously."""
    Gst = gst()
    while not _GST_FILE_READER_STOP.is_set():
        try:
            with open(_GST_DEBUG_FILE_PATH, "r", encoding="utf-8", errors="replace") as file:
                while not _GST_FILE_READER_STOP.is_set():
                    line = file.readline()
                    if not line:
                        time.sleep(_GST_FILE_POLL_SEC)
                        continue
                    try:
                        _parse_and_print_gst_file_line(line, Gst)
                    except Exception:
                        pass
        except FileNotFoundError:
            time.sleep(_GST_FILE_POLL_SEC)
        except Exception:
            time.sleep(_GST_FILE_POLL_SEC)


def configure_gst_debug_for_runtime(Gst: Any) -> None:
    """Configures GStreamer debug thresholds for the active logging mode.

    Args:
        Gst: Imported Gst module. Type: Any.

    Returns:
        None.
    """

    Gst.debug_set_active(True)

    if _GST_LOG_MODE == ImsdkGstLogMode.ImsdkLog:
        try:
            Gst.debug_set_default_threshold(Gst.DebugLevel.ERROR)
        except Exception:
            pass
        _configure_gst_parser_category_log_levels()
        return

    try:
        gst_level = {
            ImsdkLogLevel.Error: Gst.DebugLevel.ERROR,
            ImsdkLogLevel.Warning: Gst.DebugLevel.WARNING,
            ImsdkLogLevel.Info: Gst.DebugLevel.INFO,
            ImsdkLogLevel.Debug: Gst.DebugLevel.DEBUG,
        }[_LOG_LEVEL]
        Gst.debug_set_default_threshold(gst_level)
    except Exception:
        pass


def _parse_and_print_state_change_log(obj_name: Any, pipeline_name: Any, message: Any, Gst: Any) -> bool:
    """Performs the parse and print state change log operation used by the SDK.

    Args:
        obj_name: Obj name value. Type: Any.
        pipeline_name: Pipeline name value. Type: Any.
        message: Message text or GStreamer bus/debug message. Type: Any.
        Gst: Imported Gst module. Type: Any.

    Returns:
        bool: Result of the operation.
    """
    if obj_name is None or message is None:
        return False
    if not obj_name or not pipeline_name:
        return False
    if pipeline_name not in obj_name and obj_name != pipeline_name:
        return False

    match = _STATE_RE.search(message)
    if not match:
        return False

    emit_imsdk_log(ImsdkLogLevel.Info, f"[STATE][{obj_name}] {match.group(1)}")
    return True


def _parse_and_print_processing_time_log(obj_name: Any, message: Any) -> bool:
    """Performs the parse and print processing time log operation used by the SDK.

    Args:
        obj_name: Obj name value. Type: Any.
        message: Message text or GStreamer bus/debug message. Type: Any.

    Returns:
        bool: Result of the operation.
    """
    if message is None:
        return False

    match = _PERF_RE.search(message)
    if not match:
        return False

    element_name = obj_name or (match.group(1) or "")
    line = ""
    if element_name:
        line += f"[{element_name}] "
    line += f"Performance time {match.group(2)} ms, HW utilization: {match.group(3)}"
    emit_imsdk_log(ImsdkLogLevel.Debug, line)
    return True


def register_pipeline_name(name: str) -> None:
    """Registers a pipeline name so the pipeline maps to itself.

    Args:
        name: Pipeline name. Type: str.

    Returns:
        None.
    """
    _PIPELINE_ELEMENT_MAP[name] = name


def register_element_for_pipeline(elem_name: str, pipeline_name: str) -> None:
    """Registers an element name so it can be resolved to its pipeline.

    Args:
        elem_name: GStreamer element name. Type: str.
        pipeline_name: Owning pipeline name. Type: str.

    Returns:
        None.
    """
    _PIPELINE_ELEMENT_MAP[elem_name] = pipeline_name


def unregister_pipeline_name(name: str) -> None:
    """Removes all entries belonging to a pipeline when it is destroyed.

    Args:
        name: Pipeline name to unregister. Type: str.

    Returns:
        None.
    """
    keys = [k for k, v in _PIPELINE_ELEMENT_MAP.items() if v == name]
    for k in keys:
        del _PIPELINE_ELEMENT_MAP[k]


def _find_pipeline_name_for_obj(obj_name: str | None) -> str | None:
    """Looks up the pipeline name for an object using the safe name registry.

    Avoids traversing GObject parent pointers (which can segfault in debug
    callbacks when objects are being freed).

    Args:
        obj_name: GStreamer object name. Type: str | None.

    Returns:
        str | None: Matching pipeline name, or None.
    """
    if not obj_name:
        return None
    return _PIPELINE_ELEMENT_MAP.get(obj_name)


def _get_obj_name(obj):
    """Get object name"""
    if obj is None:
        return None

    try:
        if hasattr(obj, "name"):
            return obj.name
    except Exception:
        return None

    return None


def _parse_and_print_gst_log(level: Any, category: Any, obj_name: Any, pipeline_name: Any, message: Any, Gst: Any) -> bool:
    """Performs the parse and print gst log operation used by the SDK.

    Args:
        level: IMSDK or GStreamer log level. Type: Any.
        category: GStreamer debug category. Type: Any.
        obj_name: Obj name value. Type: Any.
        pipeline_name: Pipeline name value. Type: Any.
        message: Message text or GStreamer bus/debug message. Type: Any.
        Gst: Imported Gst module. Type: Any.

    Returns:
        bool: Result of the operation.
    """
    if _parse_and_print_state_change_log(obj_name, pipeline_name, message, Gst):
        return True
    if _parse_and_print_processing_time_log(obj_name, message):
        return True

    try:
        is_error = level == Gst.DebugLevel.ERROR or int(level) == int(Gst.DebugLevel.ERROR)
    except Exception:
        is_error = False

    if not is_error:
        return False

    parts: list[str] = []
    if category:
        parts.append(f"[{category}]")
    if obj_name:
        parts.append(str(obj_name))
    if message:
        parts.append(str(message))

    emit_imsdk_log(ImsdkLogLevel.Error, " ".join(parts))
    return True


def _safe_category_name(category: Any) -> str:
    """Performs the safe category name operation used by the SDK.

    Args:
        category: GStreamer debug category. Type: Any.

    Returns:
        str: Result of the operation.
    """
    try:
        if category is None:
            return ""
        return category.get_name()
    except Exception:
        return ""


def _safe_debug_message(message: Any) -> str:
    """Performs the safe debug message operation used by the SDK.

    Args:
        message: Message text or GStreamer bus/debug message. Type: Any.

    Returns:
        str: Result of the operation.
    """
    try:
        if message is None:
            return ""
        return message.get()
    except Exception:
        return ""


def _gst_debug_log_function(category, level, file, function, line, obj, message, user_data):
    """Performs the gst debug log function operation used by the SDK.

    Args:
        category: GStreamer debug category.
        level: IMSDK or GStreamer log level.
        file: Source file supplied by GStreamer debug logging.
        function: Function name supplied by GStreamer debug logging.
        line: Source line supplied by GStreamer debug logging.
        obj: GStreamer object associated with a log message.
        message: Message text or GStreamer bus/debug message.
        user_data: Opaque user data passed by GStreamer.
    """
    Gst = gst()

    if _GST_LOG_MODE == ImsdkGstLogMode.GstLog:
        try:
            Gst.debug_log_default(category, level, file, function, line, obj, message, None)
        except Exception:
            pass
        return

    try:
        obj_name = _get_obj_name(obj)
        pipeline_name = _find_pipeline_name_for_obj(obj_name)
        category_name = _safe_category_name(category)
        debug_message = _safe_debug_message(message)
        _parse_and_print_gst_log(int(level), category_name, obj_name, pipeline_name, debug_message, Gst)
    except Exception:
        pass


def install_gst_debug_hook() -> None:
    """Installs the IMSDK GStreamer debug hook when available.

    Returns:
        None.
    """

    global _GST_HOOK_INSTALLED, _GST_FILE_READER_THREAD

    if _GST_HOOK_INSTALLED:
        return

    # Do not attach log callback in case of GST logs
    if _GST_LOG_MODE != ImsdkGstLogMode.ImsdkLog:
        return

    try:
        with open(_GST_DEBUG_FILE_PATH, "w", encoding="utf-8"):
            pass
    except Exception:
        pass

    _GST_FILE_READER_STOP.clear()
    _GST_FILE_READER_THREAD = threading.Thread(
        target=_gst_debug_file_reader_loop,
        name="imsdk-gst-log-reader",
        daemon=True,
    )
    _GST_FILE_READER_THREAD.start()
    _GST_HOOK_INSTALLED = True


def restore_gst_debug_hook() -> None:
    """Restores the default GStreamer debug logger when possible.

    Returns:
        None.
    """

    global _GST_HOOK_INSTALLED, _GST_FILE_READER_THREAD

    if not _GST_HOOK_INSTALLED:
        return

    _GST_FILE_READER_STOP.set()
    if _GST_FILE_READER_THREAD is not None and threading.current_thread() is not _GST_FILE_READER_THREAD:
        _GST_FILE_READER_THREAD.join(timeout=2)
    try:
        os.remove(_GST_DEBUG_FILE_PATH)
    except Exception:
        pass
    _GST_FILE_READER_THREAD = None

    _GST_HOOK_INSTALLED = False
