# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

from __future__ import annotations

import logging
from typing import Any

log = logging.getLogger("imsdk")

_Gst: Any | None = None
_GObject: Any | None = None
_GLib: Any | None = None


def _require_gi() -> tuple[Any, Any, Any]:
    """Loads and caches the required GI repository modules.

    Returns:
        tuple[Any, Any, Any]: Result of the operation.
    """
    global _Gst, _GObject, _GLib

    if _Gst is not None:
        return _Gst, _GObject, _GLib

    import gi  # type: ignore

    gi.require_version("Gst", "1.0")
    gi.require_version("GObject", "2.0")
    gi.require_version("GLib", "2.0")

    from gi.repository import Gst, GObject, GLib  # type: ignore

    Gst.init(None)

    _Gst, _GObject, _GLib = Gst, GObject, GLib
    return _Gst, _GObject, _GLib


def gst() -> Any:
    """Returns the imported Gst module.

    Returns:
        Any: Result of the operation.
    """
    return _require_gi()[0]


def gobject() -> Any:
    """Returns the imported GObject module.

    Returns:
        Any: Result of the operation.
    """
    return _require_gi()[1]


def glib() -> Any:
    """Returns the imported GLib module.

    Returns:
        Any: Result of the operation.
    """
    return _require_gi()[2]


def normalize_property_name(py_key: str) -> str:
    # drop trailing underscore (async_ -> async)
    """Converts Python-safe property names into GObject property names.

    Args:
        py_key: Py key value. Type: str.

    Returns:
        str: Result of the operation.
    """
    if py_key.endswith("_"):
        py_key = py_key[:-1]
    # allow kebab already or convert snake -> kebab
    if "-" in py_key:
        return py_key
    return py_key.replace("_", "-")

def _coerce_value_for_property(pspec: Any, value: Any) -> Any:
    """Performs the coerce value for property operation used by the SDK.

    Args:
        pspec: Pspec value. Type: Any.
        value: Value to assign. Type: Any.

    Returns:
        Any: Result of the operation.
    """
    from ._stream_filter import StreamFilter

    Gst = gst()

    if isinstance(value, StreamFilter):
        return value._get_caps()

    if pspec is None:
        return value

    try:
        vt = pspec.value_type
        vt_name = str(getattr(vt, "name", "") or "")

        # PyGObject may treat a string for GstValueArray as an iterable of chars.
        # Convert "<a, b, c>" to a Python list so set_property receives array items.
        if (
            "GstValueArray" in vt_name
            and isinstance(value, str)
            and value.startswith("<")
            and value.endswith(">")
        ):
            raw_items = [item.strip() for item in value[1:-1].split(",")]
            out_items: list[Any] = []
            for item in raw_items:
                if item == "":
                    continue
                try:
                    out_items.append(int(item))
                    continue
                except Exception:
                    pass
                try:
                    out_items.append(float(item))
                    continue
                except Exception:
                    pass
                out_items.append(item)
            return out_items

        if isinstance(value, str):
            if hasattr(Gst.Structure, "__gtype__") and vt == Gst.Structure.__gtype__:
                s, _ = Gst.Structure.from_string(value)
                if s is None:
                    raise ValueError(f"Invalid GstStructure string: {value!r}")
                return s

            if hasattr(Gst.Caps, "__gtype__") and vt == Gst.Caps.__gtype__:
                return Gst.Caps.from_string(value)
    except Exception:
        pass

    return value


def set_gobject_properties(gobj: Any, **props: Any) -> None:
    """Sets multiple GObject properties with validation and type coercion.

    Args:
        gobj: Gobj value. Type: Any.
        props: Properties to set on the underlying GObject. Type: Any.

    Returns:
        None.
    """
    if not props:
        return

    for k, v in props.items():
        name = normalize_property_name(k)

        pspec = None
        try:
            if hasattr(gobj, "find_property"):
                pspec = gobj.find_property(name)
        except Exception:
            pspec = None

        if pspec is None:
            gtype_name = None
            try:
                gtype_name = gobj.__gtype__.name  # type: ignore[attr-defined]
            except Exception:
                gtype_name = gobj.__class__.__name__

            raise ValueError(
                f"Unknown property '{name}' for object type '{gtype_name}'"
            )

        # If we found a pspec, coerce with type info.
        v2 = _coerce_value_for_property(pspec, v)

        try:
            if isinstance(v2, str) and v2.startswith("<") and v2.endswith(">"):
                Gst = gst()
                Gst.util_set_object_arg(gobj, name, v2)
            else:
                gobj.set_property(name, v2)
        except Exception as e:
            raise RuntimeError(
                f"Failed to set property '{name}' on "
                f"'{getattr(gobj, 'name', gobj)}': {e}"
            ) from e
