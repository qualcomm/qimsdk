# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

from __future__ import annotations

from typing import Any

from ._utils import gst, set_gobject_properties


class Element:
    """Fluent wrapper around a GstElement used by Pipeline for property setting, linking, and pad access.
    """
    def __init__(self, factory: str | None = None, name: str | None = None, *, existing: Any | None = None) -> None:
        """Initializes the object.

        Args:
            factory: GStreamer element factory name. Type: str | None.
            name: Name of the element, pad, pipeline, file, or configuration entry. Type: str | None.
            existing: Existing GStreamer element to wrap instead of creating a new one. Type: Any | None.

        Returns:
            None.
        """
        self._Gst = gst()
        if existing is not None:
            self._elem = existing
        else:
            if factory is None:
                raise ValueError("Invalid initialization")
            self._elem = self._Gst.ElementFactory.make(factory, name)
            if not self._elem:
                raise RuntimeError(f"Failed to create element '{factory}' as '{name}'")

    @property
    def name(self) -> str:
        """Returns the wrapped element name.

        Returns:
            str: Result of the operation.
        """
        return self._elem.name

    def get_raw(self) -> Any:
        """Returns the underlying GstElement object.

        Returns:
            Any: Result of the operation.
        """
        return self._elem

    def set(self, *args: Any, **props: Any) -> Element:
        """Sets GObject properties and returns the wrapper for fluent chaining.

        Args:
            args: positional key/value arguments. Type: Any.
            props: Properties to set on the underlying GObject. Type: Any.

        Returns:
            Element: Result of the operation.
        """
        if args:
            if len(args) % 2 != 0:
                raise RuntimeError("Element.set expects property/value pairs")
            for i in range(0, len(args), 2):
                props[str(args[i])] = args[i + 1]
        set_gobject_properties(self._elem, **props)
        return self

    def deactivate(self) -> Element:
        """Moves the wrapped element to PAUSED state when supported.

        Returns:
            Element: Result of the operation.
        """
        ret = self._elem.set_state(self._Gst.State.PAUSED)
        if ret == self._Gst.StateChangeReturn.FAILURE:
            raise RuntimeError("Failed to set element to PAUSED")
        return self

    def stop(self) -> Element:
        """Stops or deactivates the wrapped element.

        Returns:
            Element: Result of the operation.
        """
        self._elem.set_state(self._Gst.State.NULL)
        return self

    def sync(self) -> Element:
        """Synchronizes the wrapped element state with its parent.

        Returns:
            Element: Result of the operation.
        """
        if not self._elem.sync_state_with_parent():
            raise RuntimeError("gst_element_sync_state_with_parent() failed")
        return self

    def link(self, downstream: Element, src_pad: str = "src", sink_pad: str = "sink") -> Element:
        """Links this element to another element.

        Args:
            downstream: Downstream value. Type: Element.
            src_pad: Src pad value. Type: str.
            sink_pad: Sink pad value. Type: str.

        Returns:
            Element: Result of the operation.
        """
        if not self._elem.link_pads(src_pad, downstream._elem, sink_pad):
            raise RuntimeError(f"Failed to link pads '{src_pad}' -> '{sink_pad}'")
        return self

    def unlink(self, downstream: Element | str | None = None, src_pad: str = "src", sink_pad: str = "sink") -> Element:
        """Unlinks this element from another element or pad.

        Args:
            downstream: Downstream value. Type: Element | str | None.
            src_pad: Src pad value. Type: str.
            sink_pad: Sink pad value. Type: str.

        Returns:
            Element: Result of the operation.
        """
        if isinstance(downstream, str) or downstream is None:
            pad_name = downstream if isinstance(downstream, str) else src_pad
            sp = self._elem.get_static_pad(pad_name)
            if not sp:
                raise RuntimeError(f"unlink(): cannot get src pad '{pad_name}'")
            peer = sp.get_peer()
            if peer:
                sp.unlink(peer)
                peer.unref()
            sp.unref()
            return self

        sp = self._elem.get_static_pad(src_pad)
        dp = downstream._elem.get_static_pad(sink_pad)
        if not sp or not dp:
            if sp:
                sp.unref()
            if dp:
                dp.unref()
            raise RuntimeError("unlink(): cannot get pads")
        sp.unlink(dp)
        sp.unref()
        dp.unref()
        return self

    def input(self, name_or_id: str | int = 0, id: int | None = None) -> "Port":
        """Returns a Port wrapper for a sink pad.

        Args:
            name_or_id: Name or id value. Type: str | int.
            id: Pad index or identifier. Type: int | None.

        Returns:
            "Port": Result of the operation.
        """
        if isinstance(name_or_id, str):
            return Port(self._elem, True, name_or_id, 0 if id is None else id)
        return Port(self._elem, True, None, int(name_or_id))

    def output(self, name_or_id: str | int = 0, id: int | None = None) -> "Port":
        """Returns a Port wrapper for a source pad.

        Args:
            name_or_id: Name or id value. Type: str | int.
            id: Pad index or identifier. Type: int | None.

        Returns:
            "Port": Result of the operation.
        """
        if isinstance(name_or_id, str):
            return Port(self._elem, False, name_or_id, 0 if id is None else id)
        return Port(self._elem, False, None, int(name_or_id))

    def connect_signal(self, signal_name: str, callback: Any, *user_args: Any) -> int:
        """Connects a generic GObject signal on this element.

        Args:
            signal_name: Signal name to connect. Type: str.
            callback: Callable that handles the signal. Type: Any.
            user_args: Optional user data forwarded by GObject. Type: Any.

        Returns:
            int: GObject signal handler id.
        """
        if not signal_name:
            raise ValueError("Element.connect_signal requires a non-empty signal name")
        if callback is None:
            raise ValueError("Element.connect_signal requires a callback")
        return int(self._elem.connect(signal_name, callback, *user_args))

    def disconnect_signal(self, handler_id: int) -> Element:
        """Disconnects a previously connected GObject signal handler.

        Args:
            handler_id: Signal handler id returned by connect_signal. Type: int.

        Returns:
            Element: Result of the operation.
        """
        if handler_id:
            self._elem.disconnect(int(handler_id))
        return self

class Port:
    """Represents a specific input or output pad on an Element and allows pad-level property configuration.
    """
    def __init__(self, element: Any, is_sink: bool, name: str | None = None, id: int = 0) -> None:
        """Initializes the object.

        Args:
            element: Element value. Type: Any.
            is_sink: Is sink value. Type: bool.
            name: Name of the element, pad, pipeline, file, or configuration entry. Type: str | None.
            id: Pad index or identifier. Type: int.

        Returns:
            None.
        """
        self._elem = element
        self._is_sink = is_sink
        self._name = name
        self._id = id

    def _pad_name(self, pad: Any) -> str:
        """Builds the pad name represented by this port.

        Args:
            pad: GStreamer pad object. Type: Any.

        Returns:
            str: Result of the operation.
        """
        try:
            return pad.get_name()
        except Exception:
            try:
                return str(getattr(pad, "name", ""))
            except Exception:
                return ""

    def _pad_direction(self, pad: Any) -> Any:
        """Returns the GStreamer pad direction represented by this port.

        Args:
            pad: GStreamer pad object. Type: Any.

        Returns:
            Any: Result of the operation.
        """
        try:
            return pad.get_direction()
        except Exception:
            return None

    def _request_pad_by_template(self, template_name: str) -> Any:
        # Prefer request_pad_simple() when available, fall back to get_request_pad().
        """Requests a pad from the parent element using a matching pad template.

        Args:
            template_name: Template name value. Type: str.

        Returns:
            Any: Result of the operation.
        """
        if hasattr(self._elem, "request_pad_simple"):
            return self._elem.request_pad_simple(template_name)
        return self._elem.get_request_pad(template_name)

    def _ensure_pad(self) -> Any:
        """Resolves or requests the GstPad represented by this Port.

        Returns:
            Any: Result of the operation.
        """
        Gst = gst()
        direction = Gst.PadDirection.SINK if self._is_sink else Gst.PadDirection.SRC

        def numeric_suffix(name: str) -> int:
            """Performs the numeric suffix operation used by the SDK.

            Args:
                name: Name of the element, pad, pipeline, file, or configuration entry. Type: str.

            Returns:
                int: Result of the operation.
            """
            m = __import__("re").search(r"(\\d+)$", name or "")
            return int(m.group(1)) if m else -1

        def expected_names() -> list[str]:
            """Performs the expected names operation used by the SDK.

            Returns:
                list[str]: Result of the operation.
            """
            if not self._name:
                return []
            if "%u" in self._name:
                return [self._name.replace("%u", str(self._id))]
            names = [self._name]
            if self._id:
                names.insert(0, f"{self._name}_{self._id}")
            return names

        candidates: list[Any] = []
        it = self._elem.iterate_pads()
        while True:
            result, pad = it.next()
            if result == Gst.IteratorResult.OK:
                if self._pad_direction(pad) == direction:
                    candidates.append(pad)
                continue
            if result == Gst.IteratorResult.RESYNC:
                it = self._elem.iterate_pads()
                candidates = []
                continue
            break

        # 1) Exact name/name_%u match when the user specified a pad name/template.
        for expected in expected_names():
            for pad in candidates:
                if self._pad_name(pad) == expected:
                    return pad

        # 2) Match existing pads by numeric suffix, e.g. sink_1 for input(1).
        for pad in candidates:
            if numeric_suffix(self._pad_name(pad)) == self._id:
                return pad

        # 3) Match by ordinal among existing pads.
        if len(candidates) > self._id:
            return candidates[self._id]

        # 4) Try static/request pads by explicit expected names.
        for expected in expected_names():
            pad = self._elem.get_static_pad(expected)
            if pad and self._pad_direction(pad) == direction:
                return pad
            try:
                pad = self._request_pad_by_template(expected)
                if pad and self._pad_direction(pad) == direction:
                    return pad
            except Exception:
                pass

        # 5) when only an id is supplied, request a compatible
        # pad template such as sink_%u/src_%u. This enables:
        # pipeline.get("composer").input(1).set("alpha", 0.2)
        for template in self._elem.get_pad_template_list() or []:
            try:
                if template.direction != direction:
                    continue
                template_name = template.name_template
                if "%u" in template_name:
                    requested_name = template_name.replace("%u", str(self._id))
                    try:
                        pad = self._elem.get_static_pad(requested_name)
                        if pad and self._pad_direction(pad) == direction:
                            return pad
                    except Exception:
                        pass
                    try:
                        pad = self._request_pad_by_template(requested_name)
                        if pad and self._pad_direction(pad) == direction:
                            return pad
                    except Exception:
                        pass
                    try:
                        pad = self._request_pad_by_template(template_name)
                        if pad and self._pad_direction(pad) == direction:
                            return pad
                    except Exception:
                        pass
                else:
                    try:
                        pad = self._request_pad_by_template(template_name)
                        if pad and self._pad_direction(pad) == direction:
                            return pad
                    except Exception:
                        pass
            except Exception:
                continue

        raise RuntimeError("Failed to resolve target pad")

    def set(self, *args: Any, **props: Any) -> Port:
        """Sets GObject properties and returns the wrapper for fluent chaining.

        Args:
            args: positional key/value arguments. Type: Any.
            props: Properties to set on the underlying GObject. Type: Any.

        Returns:
            Port: Result of the operation.
        """
        if args:
            if len(args) % 2 != 0:
                raise RuntimeError("Port.set expects property/value pairs")
            for i in range(0, len(args), 2):
                props[str(args[i])] = args[i + 1]
        pad = self._ensure_pad()
        set_gobject_properties(pad, **props)
        return self
