# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

from __future__ import annotations

import os
import threading
from dataclasses import dataclass
from typing import Dict, Any

import yaml

from ._element import Element
from ._appsink import AppSink
from ._appsrc import AppSrc
from ._camsrc import CamSrc
from ._mlpostprocess import MLPostprocess

from ._stream_filter import (
    StreamFilter,
    VideoFilter,
    ImageFilter,
    H264Filter,
    TensorFilter,
    TextFilter,
    AudioFilter,
)

from ._utils import gst, set_gobject_properties
from ._runtime import runtime
from ._logging import (
    emit_imsdk_log,
    ImsdkLogLevel,
    register_pipeline_name,
    register_element_for_pipeline,
    unregister_pipeline_name,
)
import logging


@dataclass
class _PendingLink:
    """Stores metadata for a deferred element link that will be completed when a dynamic pad appears.
    """

    upstream: Any
    downstream: Any
    sink_pad_template: str = ""
    src_pad_template: str = ""
    uses_request_pad: bool = False
    requested_sink_pad: Any | None = None
    uses_request_src_pad: bool = False
    requested_src_pad: Any | None = None
    completed: bool = False


@dataclass
class _HandlerTrack:
    """Tracks a temporary GStreamer signal handler installed for deferred pad linking.
    """

    upstream: Any
    handler_id: int = 0
    pending_count: int = 0


class Pipeline:
    """Builds, links, runs, debugs, and loads GStreamer pipelines through the IMSDK Python API.
    """
    def __init__(self, name: str) -> None:
        """Initializes the object.

        Args:
            name: Name of the element, pad, pipeline, file, or configuration entry. Type: str.

        Returns:
            None.
        """
        self._Gst = gst()

        self._pipeline = self._Gst.Pipeline.new(name)
        if not self._pipeline:
            raise RuntimeError("Failed to create GstPipeline")
        register_pipeline_name(name)

        self._elements: Dict[str, Any] = {}

        self._eos_requested = False
        self._terminated = False
        self._lock = threading.Lock()
        self._cond = threading.Condition(self._lock)

        self._bus = self._pipeline.get_bus()
        self._bus.add_signal_watch()
        self._bus.connect("message", self._on_bus_message)

        self._shutdown_handler = runtime().add_shutdown_handler(self.stop)
        self._linked = False
        self._eos = False

        self._pending_links: list[_PendingLink] = []
        self._handler_tracks: list[_HandlerTrack] = []
        self._dl_lock = threading.RLock()

    def add(
        self,
        factory_or_element: Any,
        name: str | None = None,
        /,
        *args: Any,
        **props: Any,
    ) -> Pipeline:
        """Adds an element, caps field, or property depending on the receiver type.

        Args:
            factory_or_element: Factory or element value. Type: Any.
            name: Name of the element, pad, pipeline, file, or configuration entry. Type: str | None.
            args: positional key/value arguments. Type: Any.
            props: Properties to set on the underlying GObject. Type: Any.

        Returns:
            Pipeline: Result of the operation.
        """
        if isinstance(factory_or_element, Element):
            elem = factory_or_element.get_raw()
        else:
            elem = self._Gst.ElementFactory.make(factory_or_element, name)
            if not elem:
                raise RuntimeError(
                    f"Failed to create element '{factory_or_element}'"
                )

        elem_name = name or elem.name
        if elem_name in self._elements:
            raise RuntimeError(f"Element '{elem_name}' already exists")

        self._pipeline.add(elem)

        if args:
            if len(args) % 2 != 0:
                raise ValueError("add() expects key/value pairs")
            cpp_props = {}
            for i in range(0, len(args), 2):
                key = args[i]
                value = args[i + 1]
                if not isinstance(key, str):
                    raise TypeError("add() property names must be strings")
                cpp_props[key] = value
            set_gobject_properties(elem, **cpp_props)

        if props:
            set_gobject_properties(elem, **props)

        self._elements[elem_name] = elem
        register_element_for_pipeline(elem_name, self._pipeline.name)
        self._linked = False
        return self

    def add_stream_filter(self, name: str | StreamFilter, caps: StreamFilter | None = None) -> Pipeline:
        """Adds a capsfilter element backed by a StreamFilter.

        Args:
            name: Name of the element, pad, pipeline, file, or configuration entry. Type: str | StreamFilter.
            caps: StreamFilter, GstCaps, or caps-compatible value. Type: StreamFilter | None.

        Returns:
            Pipeline: Result of the operation.
        """
        if caps is None:
            if not isinstance(name, StreamFilter):
                raise RuntimeError("add_stream_filter expects (name, caps) or (caps)")
            caps = name
            elem_name = caps.__class__.__name__.lower()
        else:
            elem_name = str(name)

        if elem_name in self._elements:
            raise RuntimeError(f"Element '{elem_name}' already exists")

        cf = self._Gst.ElementFactory.make("capsfilter", elem_name)
        if not cf:
            raise RuntimeError("Failed to create capsfilter")

        self._pipeline.add(cf)
        cf.set_property("caps", caps._get_caps())

        hint = getattr(caps, "_upstream_pad_hint", None)
        if hint:
            cf._imsdk_upstream_pad_hint = hint

        self._elements[elem_name] = cf
        register_element_for_pipeline(elem_name, self._pipeline.name)
        self._linked = False
        return self

    def link(self, *names: str) -> Pipeline:
        """Links this element to another element.

        Args:
            names: Names value. Type: str.

        Returns:
            Pipeline: Result of the operation.
        """
        if len(names) < 2:
            return self

        for src_name, dst_name in zip(names, names[1:]):
            if src_name not in self._elements:
                raise RuntimeError(f"Unknown element in link(): '{src_name}'")
            if dst_name not in self._elements:
                raise RuntimeError(f"Unknown element in link(): '{dst_name}'")

            src = self._elements[src_name]
            dst = self._elements[dst_name]

            if not self._try_immediate_link_or_defer(src, dst):
                if not self._has_dynamic_src(src):
                    has_deferred_sink, _ = self._has_request_or_sometimes_sink(dst)
                    if not has_deferred_sink:
                        self._log_unlinked_pads_caps()
                        raise RuntimeError(
                            f"Failed to link '{src_name}' -> '{dst_name}'"
                        )

        self._linked = True
        return self

    def _auto_link(self) -> None:
        """Performs the auto link operation used by the SDK.

        Returns:
            None.
        """
        names = list(self._elements.keys())
        if len(names) < 2:
            self._linked = True
            return

        for index, (src_name, dst_name) in enumerate(zip(names, names[1:])):
            src = self._elements[src_name]
            dst = self._elements[dst_name]

            if not self._try_immediate_link_or_defer(src, dst):
                if not self._has_dynamic_src(src):
                    has_deferred_sink, _ = self._has_request_or_sometimes_sink(dst)
                    if not has_deferred_sink:
                        self._log_unlinked_pads_caps()
                        raise RuntimeError(
                            "Failed to link static elements at index "
                            f"{index} and {index + 1}"
                        )

        self._linked = True

    def _element_name(self, elem: Any) -> str:
        """Performs the element name operation used by the SDK.

        Args:
            elem: GStreamer element. Type: Any.

        Returns:
            str: Result of the operation.
        """
        try:
            return str(elem.name)
        except Exception:
            try:
                return str(elem.get_name())
            except Exception:
                return "<unknown>"

    def _pad_name(self, pad: Any) -> str:
        """Builds the pad name represented by this port.

        Args:
            pad: GStreamer pad object. Type: Any.

        Returns:
            str: Result of the operation.
        """
        try:
            return str(pad.name)
        except Exception:
            try:
                return str(pad.get_name())
            except Exception:
                return "<unknown>"

    def _template_name(self, template: Any) -> str:
        """Performs the template name operation used by the SDK.

        Args:
            template: Pad template object. Type: Any.

        Returns:
            str: Result of the operation.
        """
        try:
            return str(template.name_template or "")
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
            try:
                return pad.direction
            except Exception:
                return None

    def _pad_template_presence(self, pad: Any) -> Any:
        """Performs the pad template presence operation used by the SDK.

        Args:
            pad: GStreamer pad object. Type: Any.

        Returns:
            Any: Result of the operation.
        """
        try:
            template = pad.get_pad_template()
            if template is not None:
                return template.presence
        except Exception:
            pass
        try:
            template = pad.get_property("template")
            if template is not None:
                return template.presence
        except Exception:
            pass
        return None

    def _get_static_sink_pad(self, elem: Any) -> Any | None:
        """Performs the get static sink pad operation used by the SDK.

        Args:
            elem: GStreamer element. Type: Any.

        Returns:
            Any | None: Result of the operation.
        """
        pad = elem.get_static_pad("sink")
        if pad is not None:
            return pad

        iterator = elem.iterate_pads()
        while True:
            result, pad = iterator.next()
            if result == self._Gst.IteratorResult.OK:
                if (
                    self._pad_direction(pad) == self._Gst.PadDirection.SINK
                    and self._pad_template_presence(pad) == self._Gst.PadPresence.ALWAYS
                ):
                    return pad
                continue
            if result == self._Gst.IteratorResult.RESYNC:
                iterator = elem.iterate_pads()
                continue
            break
        return None

    def _request_pad(self, elem: Any, template_name: str) -> Any | None:
        """Performs the request pad operation used by the SDK.

        Args:
            elem: GStreamer element. Type: Any.
            template_name: Template name value. Type: str.

        Returns:
            Any | None: Result of the operation.
        """
        if not template_name:
            return None
        try:
            if hasattr(elem, "request_pad_simple"):
                return elem.request_pad_simple(template_name)
            return elem.get_request_pad(template_name)
        except Exception:
            return None

    def _request_sink_pad(self, elem: Any, template_name: str) -> Any | None:
        """Performs the request sink pad operation used by the SDK.

        Args:
            elem: GStreamer element. Type: Any.
            template_name: Template name value. Type: str.

        Returns:
            Any | None: Result of the operation.
        """
        return self._request_pad(elem, template_name)

    def _request_src_pad(self, elem: Any, template_name: str) -> Any | None:
        """Performs the request src pad operation used by the SDK.

        Args:
            elem: GStreamer element. Type: Any.
            template_name: Template name value. Type: str.

        Returns:
            Any | None: Result of the operation.
        """
        return self._request_pad(elem, template_name)

    def _release_request_pad(self, elem: Any, pad: Any | None) -> None:
        """Performs the release request pad operation used by the SDK.

        Args:
            elem: GStreamer element. Type: Any.
            pad: GStreamer pad object. Type: Any | None.

        Returns:
            None.
        """
        if elem is None or pad is None:
            return
        try:
            elem.release_request_pad(pad)
        except Exception:
            pass

    def _pad_is_linked(self, pad: Any | None) -> bool:
        """Performs the pad is linked operation used by the SDK.

        Args:
            pad: GStreamer pad object. Type: Any | None.

        Returns:
            bool: Result of the operation.
        """
        if pad is None:
            return False
        try:
            return bool(pad.is_linked())
        except Exception:
            return False

    def _has_dynamic_src(self, elem: Any) -> bool:
        """Performs the has dynamic src operation used by the SDK.

        Args:
            elem: GStreamer element. Type: Any.

        Returns:
            bool: Result of the operation.
        """
        for template in elem.get_pad_template_list() or []:
            try:
                if (
                    template.direction == self._Gst.PadDirection.SRC
                    and template.presence
                    in (self._Gst.PadPresence.SOMETIMES, self._Gst.PadPresence.REQUEST)
                ):
                    return True
            except Exception:
                continue
        return False

    def _has_request_or_sometimes_sink(self, elem: Any) -> tuple[bool, str]:
        """Performs the has request or sometimes sink operation used by the SDK.

        Args:
            elem: GStreamer element. Type: Any.

        Returns:
            tuple[bool, str]: Result of the operation.
        """
        if self._get_static_sink_pad(elem) is not None:
            return True, ""

        for template in elem.get_pad_template_list() or []:
            try:
                if (
                    template.direction == self._Gst.PadDirection.SINK
                    and template.presence
                    in (self._Gst.PadPresence.REQUEST, self._Gst.PadPresence.SOMETIMES)
                ):
                    return True, self._template_name(template)
            except Exception:
                continue
        return False, ""

    def _connect_pad_added_once(self, upstream: Any) -> _HandlerTrack:
        """Performs the connect pad added once operation used by the SDK.

        Args:
            upstream: Upstream value. Type: Any.

        Returns:
            _HandlerTrack: Result of the operation.
        """
        for track in self._handler_tracks:
            if track.upstream is upstream:
                return track

        handler_id = upstream.connect("pad-added", self._on_pad_added)
        track = _HandlerTrack(upstream=upstream, handler_id=handler_id, pending_count=0)
        self._handler_tracks.append(track)
        return track

    def _downstream_upstream_src_hint(self, downstream: Any) -> str:
        """Performs the downstream upstream src hint operation used by the SDK.

        Args:
            downstream: Downstream value. Type: Any.

        Returns:
            str: Result of the operation.
        """
        return str(getattr(downstream, "_imsdk_upstream_pad_hint", "") or "")

    def _try_link_requested_src_to_sink(
        self,
        up: Any,
        down: Any,
        src_template: str,
        sink_template: str,
    ) -> bool:
        """Performs the try link requested src to sink operation used by the SDK.

        Args:
            up: Upstream GStreamer element. Type: Any.
            down: Downstream GStreamer element. Type: Any.
            src_template: Src template value. Type: str.
            sink_template: Sink template value. Type: str.

        Returns:
            bool: Result of the operation.
        """
        src_pad = self._request_src_pad(up, src_template)
        if src_pad is None:
            return False

        sink_pad = self._get_static_sink_pad(down)
        requested_sink = False
        if sink_pad is None:
            sink_pad = self._request_sink_pad(down, sink_template)
            requested_sink = sink_pad is not None

        if sink_pad is not None:
            if not self._pad_is_linked(sink_pad):
                try:
                    ret = src_pad.link(sink_pad)
                except Exception:
                    ret = None
                if ret == self._Gst.PadLinkReturn.OK:
                    self._pending_links.append(
                        _PendingLink(
                            upstream=up,
                            downstream=down,
                            sink_pad_template=sink_template,
                            src_pad_template=src_template,
                            uses_request_pad=requested_sink,
                            requested_sink_pad=sink_pad if requested_sink else None,
                            uses_request_src_pad=True,
                            requested_src_pad=src_pad,
                            completed=True,
                        )
                    )
                    return True

            if requested_sink:
                self._release_request_pad(down, sink_pad)

        self._release_request_pad(up, src_pad)
        return False

    def _try_immediate_link_or_defer(self, up: Any, down: Any) -> bool:
        """Performs the try immediate link or defer operation used by the SDK.

        Args:
            up: Upstream GStreamer element. Type: Any.
            down: Downstream GStreamer element. Type: Any.

        Returns:
            bool: Result of the operation.
        """
        _, sink_template = self._has_request_or_sometimes_sink(down)
        src_template = self._downstream_upstream_src_hint(down)

        if src_template:
            if self._try_link_requested_src_to_sink(up, down, src_template, sink_template):
                return True

        try:
            if up.link(down):
                return True
        except Exception:
            pass

        up_dynamic = self._has_dynamic_src(up)
        down_dynamic, sink_template = self._has_request_or_sometimes_sink(down)
        if not up_dynamic and not down_dynamic:
            return False

        with self._dl_lock:
            track = self._connect_pad_added_once(up)

            pending = _PendingLink(
                upstream=up,
                downstream=down,
                sink_pad_template=sink_template,
                src_pad_template=src_template,
            )

            if src_template:
                src_pad = self._request_src_pad(up, src_template)
                if src_pad is not None:
                    pending.uses_request_src_pad = True
                    pending.requested_src_pad = src_pad

                    sink_pad = self._get_static_sink_pad(down)
                    requested_sink = False
                    if sink_pad is None:
                        sink_pad = self._request_sink_pad(down, sink_template)
                        requested_sink = sink_pad is not None

                    if sink_pad is not None:
                        if not self._pad_is_linked(sink_pad):
                            try:
                                ret = src_pad.link(sink_pad)
                            except Exception:
                                ret = None
                            if ret == self._Gst.PadLinkReturn.OK:
                                pending.completed = True
                                pending.uses_request_pad = requested_sink
                                pending.requested_sink_pad = sink_pad if requested_sink else None
                                self._pending_links.append(pending)
                                return True

                        if requested_sink:
                            self._release_request_pad(down, sink_pad)

                    self._release_request_pad(up, src_pad)
                    pending.uses_request_src_pad = False
                    pending.requested_src_pad = None

            self._pending_links.append(pending)
            track.pending_count += 1
            return False

    def _on_pad_added(self, src: Any, new_pad: Any) -> None:
        """Completes a deferred link when a dynamic source pad is added.

        Args:
            src: Source element or pad. Type: Any.
            new_pad: New pad value. Type: Any.

        Returns:
            None.
        """
        if self._pad_direction(new_pad) != self._Gst.PadDirection.SRC:
            return

        with self._dl_lock:
            for pending in self._pending_links:
                if pending.completed or pending.upstream is not src:
                    continue

                sink_pad = self._get_static_sink_pad(pending.downstream)
                requested = False
                if sink_pad is None:
                    sink_pad = self._request_sink_pad(
                        pending.downstream, pending.sink_pad_template
                    )
                    requested = sink_pad is not None
                if sink_pad is None:
                    continue

                if self._pad_is_linked(sink_pad):
                    if requested:
                        self._release_request_pad(pending.downstream, sink_pad)
                    continue

                try:
                    ret = new_pad.link(sink_pad)
                except Exception:
                    ret = None

                if ret == self._Gst.PadLinkReturn.OK:
                    pending.completed = True
                    pending.uses_request_pad = requested
                    pending.requested_sink_pad = sink_pad if requested else None

                    for track in list(self._handler_tracks):
                        if track.upstream is src:
                            track.pending_count -= 1
                            if track.pending_count <= 0 and track.handler_id:
                                try:
                                    src.disconnect(track.handler_id)
                                except Exception:
                                    pass
                                self._handler_tracks.remove(track)
                            break
                elif requested:
                    self._release_request_pad(pending.downstream, sink_pad)

    def _disconnect_all_pad_added(self) -> None:
        """Performs the disconnect all pad added operation used by the SDK.

        Returns:
            None.
        """
        with self._dl_lock:
            for track in self._handler_tracks:
                if track.handler_id and track.upstream is not None:
                    try:
                        track.upstream.disconnect(track.handler_id)
                    except Exception:
                        pass
            self._handler_tracks.clear()

    def _cleanup_pending_handlers(self) -> None:
        """Performs the cleanup pending handlers operation used by the SDK.

        Returns:
            None.
        """
        with self._dl_lock:
            for pending in self._pending_links:
                if (
                    pending.completed
                    and pending.uses_request_pad
                    and pending.downstream is not None
                    and pending.requested_sink_pad is not None
                ):
                    self._release_request_pad(pending.downstream, pending.requested_sink_pad)
                    pending.requested_sink_pad = None
                if (
                    pending.completed
                    and pending.uses_request_src_pad
                    and pending.upstream is not None
                    and pending.requested_src_pad is not None
                ):
                    self._release_request_pad(pending.upstream, pending.requested_src_pad)
                    pending.requested_src_pad = None

            self._pending_links = [p for p in self._pending_links if not p.completed]

    def _pending_peer_for_element(self, elem: Any, pad_direction: Any) -> tuple[str | None, str | None]:
        """Performs the pending peer for element operation used by the SDK.

        Args:
            elem: GStreamer element. Type: Any.
            pad_direction: Pad direction value. Type: Any.

        Returns:
            tuple[str | None, str | None]: Result of the operation.
        """
        with self._dl_lock:
            for pending in self._pending_links:
                if pending.completed:
                    continue
                if pad_direction == self._Gst.PadDirection.SRC and pending.upstream is elem:
                    return (
                        self._element_name(pending.downstream),
                        pending.sink_pad_template or "sink",
                    )
                if pad_direction == self._Gst.PadDirection.SINK and pending.downstream is elem:
                    return (
                        self._element_name(pending.upstream),
                        pending.src_pad_template or "src",
                    )
        return None, None


    def _imsdk_debug(self, message: str) -> None:
        """Performs the imsdk debug operation used by the SDK.

        Args:
            message: Message text or GStreamer bus/debug message. Type: str.

        Returns:
            None.
        """
        try:
            emit_imsdk_log(ImsdkLogLevel.Debug, message)
        except Exception:
            logging.getLogger("imsdk").debug(message)

    def _imsdk_error(self, message: str) -> None:
        """Performs the imsdk error operation used by the SDK.

        Args:
            message: Message text or GStreamer bus/debug message. Type: str.

        Returns:
            None.
        """
        try:
            emit_imsdk_log(ImsdkLogLevel.Error, message)
        except Exception:
            logging.getLogger("imsdk").error(message)

    def _pad_direction_name(self, direction: Any) -> str:
        """Performs the pad direction name operation used by the SDK.

        Args:
            direction: Direction value. Type: Any.

        Returns:
            str: Result of the operation.
        """
        if direction == self._Gst.PadDirection.SRC:
            return "src"
        if direction == self._Gst.PadDirection.SINK:
            return "sink"
        return "unknown"

    def _pad_presence_name(self, presence: Any) -> str:
        """Performs the pad presence name operation used by the SDK.

        Args:
            presence: Presence value. Type: Any.

        Returns:
            str: Result of the operation.
        """
        if presence == self._Gst.PadPresence.ALWAYS:
            return "always"
        if presence == self._Gst.PadPresence.SOMETIMES:
            return "sometimes"
        if presence == self._Gst.PadPresence.REQUEST:
            return "request"
        return "unknown"

    def _caps_to_string(self, caps: Any) -> str:
        """Performs the caps to string operation used by the SDK.

        Args:
            caps: StreamFilter, GstCaps, or caps-compatible value. Type: Any.

        Returns:
            str: Result of the operation.
        """
        if caps is None:
            return ""
        try:
            if caps.is_any():
                return "ANY"
            if caps.is_empty():
                return "EMPTY"
        except Exception:
            pass
        try:
            return str(caps.to_string())
        except Exception:
            try:
                return str(caps)
            except Exception:
                return ""

    def _current_or_template_caps_string(self, pad: Any) -> str:
        """Performs the current or template caps string operation used by the SDK.

        Args:
            pad: GStreamer pad object. Type: Any.

        Returns:
            str: Result of the operation.
        """
        caps = None
        try:
            caps = pad.get_current_caps()
        except Exception:
            caps = None
        if caps is not None:
            return self._caps_to_string(caps)

        try:
            caps = pad.query_caps(None)
        except Exception:
            caps = None
        if caps is not None:
            return self._caps_to_string(caps)

        try:
            template = pad.get_pad_template()
            if template is not None:
                caps = template.get_caps()
        except Exception:
            caps = None
        return self._caps_to_string(caps)

    def _iter_element_pads(self, elem: Any):
        """Performs the iter element pads operation used by the SDK.

        Args:
            elem: GStreamer element. Type: Any.
        """
        iterator = elem.iterate_pads()
        while True:
            result, pad = iterator.next()
            if result == self._Gst.IteratorResult.OK:
                yield pad
                continue
            if result == self._Gst.IteratorResult.RESYNC:
                iterator = elem.iterate_pads()
                continue
            break

    def _log_unlinked_pads_caps(self) -> None:
        """Performs the log unlinked pads caps operation used by the SDK.

        Returns:
            None.
        """
        found = False
        for elem_name, elem in self._elements.items():
            for pad in self._iter_element_pads(elem):
                try:
                    if pad.is_linked():
                        continue
                except Exception:
                    pass

                found = True
                pad_name = self._pad_name(pad)
                direction = self._pad_direction_name(self._pad_direction(pad))
                presence = self._pad_presence_name(self._pad_template_presence(pad))
                caps = self._current_or_template_caps_string(pad)
                caps_part = f" caps={{{caps}}}" if caps else " caps={}"
                self._imsdk_error(
                    f"[PIPELINE][NOT_LINKED] {elem_name}:{pad_name} "
                    f"[{direction}, {presence}]{caps_part}"
                )

        with self._dl_lock:
            for pending in self._pending_links:
                if pending.completed:
                    continue
                found = True
                up = self._element_name(pending.upstream)
                down = self._element_name(pending.downstream)
                self._imsdk_error(
                    "[PIPELINE][NOT_LINKED] pending "
                    f"{up}:{pending.src_pad_template or 'src'} -> "
                    f"{down}:{pending.sink_pad_template or 'sink'}"
                )

        if not found:
            self._imsdk_debug("[PIPELINE][NOT_LINKED] no unlinked pads found")

    def get(self, name: str, as_type: Any | None = None) -> Any:
        """Returns a named pipeline element as Element or a typed wrapper.

        Args:
            name: Name of the element, pad, pipeline, file, or configuration entry. Type: str.
            as_type: Optional wrapper class (for example MLVideoTFLiteBin). Type: Any | None.

        Returns:
            Any: Element wrapper by default, or a typed wrapper when as_type is provided.
        """
        raw = self._elements[name]
        if as_type is None:
            return Element(existing=raw)

        cast = getattr(as_type, "cast", None)
        if callable(cast):
            return cast(raw)

        return as_type(existing=raw)

    def prepare(self) -> Pipeline:
        """Moves the pipeline to PAUSED and logs diagnostics on failure.

        Returns:
            Pipeline: Result of the operation.
        """
        if not self._linked:
            self._auto_link()
        ret = self._pipeline.set_state(self._Gst.State.PAUSED)
        if ret == self._Gst.StateChangeReturn.FAILURE:
            self._log_unlinked_pads_caps()
            raise RuntimeError("Failed to set pipeline to PAUSED")
        return self

    def activate(self) -> Pipeline:
        """Moves the pipeline to PLAYING.

        Returns:
            Pipeline: Result of the operation.
        """
        return self.start()

    def deactivate(self) -> Pipeline:
        """Moves the wrapped element to PAUSED state when supported.

        Returns:
            Pipeline: Result of the operation.
        """
        ret = self._pipeline.set_state(self._Gst.State.PAUSED)
        if ret == self._Gst.StateChangeReturn.FAILURE:
            self._log_unlinked_pads_caps()
            raise RuntimeError("Failed to set pipeline to PAUSED")
        return self

    def start(self) -> Pipeline:
        """Starts the pipeline and registers it with the runtime.

        Returns:
            Pipeline: Result of the operation.
        """
        if not self._linked:
            self._auto_link()
        ret = self._pipeline.set_state(self._Gst.State.PLAYING)
        if ret == self._Gst.StateChangeReturn.FAILURE:
            self._log_unlinked_pads_caps()
            raise RuntimeError("Failed to set pipeline to PLAYING")
        runtime().register_pipeline(self)
        return self

    def wait(self) -> Pipeline:
        """Blocks until EOS, error, or shutdown terminates the pipeline.

        Returns:
            Pipeline: Result of the operation.
        """
        with self._cond:
            while not self._terminated:
                self._cond.wait()
        return self

    def eos(self, enabled: bool) -> Pipeline:
        """Controls whether stop() sends and waits for EOS before teardown.

        Args:
            enabled: Whether EOS should be used during stop. Type: bool.

        Returns:
            Pipeline: Result of the operation.
        """
        with self._cond:
            self._eos = bool(enabled)
        return self

    def _terminate_now(self) -> None:
        """Immediately tears down the pipeline and releases runtime handlers.

        Returns:
            None.
        """
        self._disconnect_all_pad_added()

        with self._cond:
            if self._terminated:
                return
            self._terminated = True

        self._pipeline.set_state(self._Gst.State.NULL)
        self._cleanup_pending_handlers()

        try:
            self._bus.disconnect_by_func(self._on_bus_message)
        except Exception:
            pass

        runtime().unregister_pipeline(self)
        if self._shutdown_handler is not None:
            runtime().remove_shutdown_handler(self._shutdown_handler)
            self._shutdown_handler = None

        try:
            unregister_pipeline_name(self._pipeline.name)
        except Exception:
            pass

        with self._cond:
            self._cond.notify_all()

    def stop(self) -> Pipeline:
        """Stops or deactivates the wrapped element.

        Returns:
            Pipeline: Result of the operation.
        """
        with self._cond:
            if self._terminated:
                return self
            use_eos = self._eos
            if self._eos_requested:
                use_eos = True
            else:
                self._eos_requested = use_eos

        if use_eos:
            try:
                self._pipeline.send_event(self._Gst.Event.new_eos())
            except Exception:
                pass
        else:
            self._terminate_now()

        return self

    def execute(self) -> None:
        """Runs the pipeline synchronously by starting it, waiting, and stopping it.

        Returns:
            None.
        """
        self.start().wait().stop()

    def print(self) -> None:
        """Logs the pipeline topology.

        Returns:
            None.
        """
        if not self._linked:
            self._auto_link()
        self._imsdk_debug("[PIPELINE][TOPOLOGY] begin")
        for name, elem in self._elements.items():
            self._imsdk_debug(f"[PIPELINE][TOPOLOGY] Element: {name}")
            source_connection_printed = False
            iterator = elem.iterate_pads()
            while True:
                result, pad = iterator.next()
                if result == self._Gst.IteratorResult.OK:
                    pad_direction = self._pad_direction(pad)
                    pad_name = self._pad_name(pad)
                    peer_elem_name = None
                    peer_pad_name = None

                    peer = pad.get_peer()
                    if peer is not None:
                        peer_elem = peer.get_parent_element()
                        peer_elem_name = self._element_name(peer_elem)
                        peer_pad_name = self._pad_name(peer)
                    else:
                        peer_elem_name, peer_pad_name = self._pending_peer_for_element(
                            elem, pad_direction
                        )
                        if pad_direction == self._Gst.PadDirection.SRC and peer_elem_name:
                            source_connection_printed = True

                    if peer_elem_name:
                        arrow = " --> " if pad_direction == self._Gst.PadDirection.SRC else " <-- "
                        self._imsdk_debug(
                            f"[PIPELINE][TOPOLOGY]   {name}:{pad_name}"
                            f"{arrow}{peer_elem_name}:{peer_pad_name}"
                        )
                    continue
                if result == self._Gst.IteratorResult.RESYNC:
                    iterator = elem.iterate_pads()
                    continue
                break

            if not source_connection_printed:
                with self._dl_lock:
                    for pending in self._pending_links:
                        if pending.completed or pending.upstream is not elem:
                            continue
                        self._imsdk_debug(
                            f"[PIPELINE][TOPOLOGY]   {name}:"
                            f"{pending.src_pad_template or 'src'} --> "
                            f"{self._element_name(pending.downstream)}:"
                            f"{pending.sink_pad_template or 'sink'}"
                        )
                        break

    def generate_graph(self, filename: str) -> None:
        """Writes a draw.io XML graph for the pipeline topology.

        Args:
            filename: Output file path. Type: str.

        Returns:
            None.
        """
        self._imsdk_debug(f"[PIPELINE][GRAPH] generating {filename}")
        if not self._linked:
            self._auto_link()

        nodes: list[tuple[str, int]] = []
        element_id_by_name: dict[str, int] = {}
        edges: list[tuple[int, int, str]] = []
        added_edges: set[tuple[int, int]] = set()
        next_node_id = 100

        for name in self._elements.keys():
            nodes.append((name, next_node_id))
            element_id_by_name[name] = next_node_id
            next_node_id += 1

        def add_graph_edge(from_id: int, to_id: int, label: str) -> None:
            """Performs the add graph edge operation used by the SDK.

            Args:
                from_id: From id value. Type: int.
                to_id: To id value. Type: int.
                label: Label value. Type: str.

            Returns:
                None.
            """
            key = (from_id, to_id)
            if key not in added_edges:
                added_edges.add(key)
                edges.append((from_id, to_id, label))

        for name, elem in self._elements.items():
            node_id = element_id_by_name[name]
            has_outgoing_source_pad = False
            iterator = elem.iterate_pads()
            while True:
                result, pad = iterator.next()
                if result == self._Gst.IteratorResult.OK:
                    pad_direction = self._pad_direction(pad)
                    pad_name = self._pad_name(pad)
                    peer = pad.get_peer()
                    if peer is not None:
                        peer_elem = peer.get_parent_element()
                        peer_elem_name = self._element_name(peer_elem)
                        peer_pad_name = self._pad_name(peer)
                        if peer_elem_name in element_id_by_name:
                            if pad_direction == self._Gst.PadDirection.SRC:
                                has_outgoing_source_pad = True
                                add_graph_edge(
                                    node_id,
                                    element_id_by_name[peer_elem_name],
                                    f"{name}:{pad_name} ? {peer_elem_name}:{peer_pad_name}",
                                )
                            else:
                                add_graph_edge(
                                    element_id_by_name[peer_elem_name],
                                    node_id,
                                    f"{peer_elem_name}:{peer_pad_name} ? {name}:{pad_name}",
                                )
                    continue
                if result == self._Gst.IteratorResult.RESYNC:
                    iterator = elem.iterate_pads()
                    continue
                break

            if not has_outgoing_source_pad:
                with self._dl_lock:
                    for pending in self._pending_links:
                        if pending.completed or pending.upstream is not elem:
                            continue
                        downstream_name = self._element_name(pending.downstream)
                        if downstream_name not in element_id_by_name:
                            break
                        add_graph_edge(
                            node_id,
                            element_id_by_name[downstream_name],
                            f"{name}:{pending.src_pad_template or 'src'} ? "
                            f"{downstream_name}:{pending.sink_pad_template or 'sink'}",
                        )
                        break

        with open(filename, "w", encoding="utf-8") as out:
            out.write('<mxfile host="draw.io"><diagram name="Pipeline Graph">')
            out.write("<mxGraphModel><root>")
            out.write('<mxCell id="0"/><mxCell id="1" parent="0"/>')

            start_x = 50
            start_y = 50
            vertical_spacing = 80

            for index, (node_name, node_id) in enumerate(nodes):
                out.write(
                    f'<mxCell id="{node_id}" value="{node_name}" vertex="1" parent="1" '
                    'style="rounded=1;whiteSpace=wrap;fillColor=#dae8fc;strokeColor=#6c8ebf;">'
                    f'<mxGeometry x="{start_x}" y="{start_y + index * vertical_spacing}" '
                    'width="150" height="40" as="geometry"/></mxCell>\n'
                )

            for index, (from_id, to_id, label) in enumerate(edges, start=10000):
                out.write(
                    f'<mxCell id="{index}" edge="1" parent="1" source="{from_id}" '
                    f'target="{to_id}" value="{label}" style="endArrow=block;">'
                    '<mxGeometry relative="1" as="geometry"/></mxCell>\n'
                )

            out.write("</root></mxGraphModel></diagram></mxfile>")

        self._imsdk_debug(f"[PIPELINE][GRAPH] generated {filename}")

    def _on_bus_message(self, bus: Any, message: Any) -> None:
        """Handles GStreamer bus messages for the pipeline.

        Args:
            bus: Bus value. Type: Any.
            message: Message text or GStreamer bus/debug message. Type: Any.

        Returns:
            None.
        """
        if message.type == self._Gst.MessageType.ERROR:
            self._log_unlinked_pads_caps()

        if message.type in (
            self._Gst.MessageType.ERROR,
            self._Gst.MessageType.EOS,
        ):
            self._terminate_now()

    @classmethod
    def from_yaml(cls, name: str, config: str) -> Pipeline:
        """Creates a Pipeline from a YAML configuration string.

        Args:
            name: Name of the element, pad, pipeline, file, or configuration entry. Type: str.
            config: YAML configuration content. Type: str.

        Returns:
            Pipeline: Result of the operation.
        """
        cfg = cls._load_yaml_config(config)

        pipe = cls(name)
        cls._apply_pipeline_options_from_yaml(pipe, cfg)

        cls._add_elements_from_yaml(pipe, cfg)
        cls._link_from_yaml(pipe, cfg)

        return pipe

    @staticmethod
    def _coerce_yaml_bool(value: Any, field_name: str) -> bool:
        """Parses flexible YAML boolean values.

        Args:
            value: Raw value from YAML. Type: Any.
            field_name: Field name for error reporting. Type: str.

        Returns:
            bool: Parsed boolean value.
        """
        if isinstance(value, bool):
            return value
        if isinstance(value, int):
            return bool(value)
        if isinstance(value, str):
            v = value.strip().lower()
            if v in ("true", "1", "yes", "on"):
                return True
            if v in ("false", "0", "no", "off"):
                return False
        raise ValueError(f"'{field_name}' must be a boolean")

    @classmethod
    def _apply_pipeline_options_from_yaml(cls, pipe: Pipeline, cfg: dict) -> None:
        """Applies top-level pipeline options from YAML.

        Args:
            pipe: Pipeline instance. Type: Pipeline.
            cfg: Parsed YAML configuration. Type: dict.

        Returns:
            None.
        """
        pconf = cfg.get("pipeline") or {}
        if "eos" in pconf:
            pipe.eos(cls._coerce_yaml_bool(pconf.get("eos"), "pipeline.eos"))

    @staticmethod
    def _expand_env_in_value(value: Any) -> Any:
        """Recursively expands environment variables in YAML string values.

        Supports both ``$VAR`` and ``${VAR}`` forms via ``os.path.expandvars``.
        Non-string values are returned unchanged.
        """
        if isinstance(value, str):
            return os.path.expandvars(value)

        if isinstance(value, list):
            return [Pipeline._expand_env_in_value(item) for item in value]

        if isinstance(value, dict):
            return {
                key: Pipeline._expand_env_in_value(item)
                for key, item in value.items()
            }

        return value

    @staticmethod
    def _load_yaml_config(config: str) -> dict:
        """Loads YAML configuration content and expands environment variables.

        Args:
            config: YAML configuration content. Type: str.

        Returns:
            dict: Result of the operation.
        """
        cfg = yaml.safe_load(config) or {}

        return Pipeline._expand_env_in_value(cfg)

    @classmethod
    def _add_elements_from_yaml(cls, pipe: Pipeline, cfg: dict) -> None:
        """Performs the add elements from yaml operation used by the SDK.

        Args:
            pipe: Pipe value. Type: Pipeline.
            cfg: Configuration dictionary section. Type: dict.

        Returns:
            None.
        """
        pconf = cfg.get("pipeline") or {}
        elements = pconf.get("elements") or []

        for elem_cfg in elements:
            cls._add_single_element_from_yaml(pipe, elem_cfg)

    @classmethod
    def _add_single_element_from_yaml(
        cls, pipe: Pipeline, elem_cfg: dict
    ) -> None:
        """Performs the add single element from yaml operation used by the SDK.

        Args:
            pipe: Pipe value. Type: Pipeline.
            elem_cfg: Elem cfg value. Type: dict.

        Returns:
            None.
        """
        if not isinstance(elem_cfg, dict):
            raise ValueError(f"Element entry must be a mapping: {elem_cfg!r}")

        etype = elem_cfg.get("type")
        if not etype:
            raise ValueError(
                f"Element missing required key 'type': {elem_cfg!r}"
            )

        name = elem_cfg.get("name")

        if etype == "filter":
            filt = cls._build_stream_filter_from_section(elem_cfg)
            pipe.add_stream_filter(name or "caps", filt)
        else:
            props = cls._extract_element_props(elem_cfg)
            pipe.add(etype, name, **props)

    @staticmethod
    def _extract_element_props(elem_cfg: dict) -> dict:
        """Performs the extract element props operation used by the SDK.

        Args:
            elem_cfg: Elem cfg value. Type: dict.

        Returns:
            dict: Result of the operation.
        """
        return {
            k: v
            for k, v in elem_cfg.items()
            if k
            not in (
                "type",
                "name",
                "video",
                "image",
                "h264",
                "tensor",
                "text",
                "audio",
                "caps",
                "extra",
            )
        }

    @staticmethod
    def _apply_extra_caps(filter_obj: StreamFilter, cfg: dict) -> StreamFilter:
        """Performs the apply extra caps operation used by the SDK.

        Args:
            filter_obj: Filter obj value. Type: StreamFilter.
            cfg: Configuration dictionary section. Type: dict.

        Returns:
            StreamFilter: Result of the operation.
        """
        extras = cfg.get("extra")
        if isinstance(extras, list):
            for expr in extras:
                filter_obj.add(expr)
        elif isinstance(extras, dict):
            for k, v in extras.items():
                filter_obj.add(f"{k}={v}")
        return filter_obj

    @staticmethod
    def _build_stream_filter_from_section(elem_cfg: dict) -> StreamFilter:
        """Performs the build stream filter from section operation used by the SDK.

        Args:
            elem_cfg: Elem cfg value. Type: dict.

        Returns:
            StreamFilter: Result of the operation.
        """
        if isinstance(elem_cfg.get("caps"), str):
            s = StreamFilter(elem_cfg["caps"])
            return Pipeline._apply_extra_caps(s, elem_cfg)

        if isinstance(elem_cfg.get("video"), dict):
            s = Pipeline._video_stream_from_cfg(elem_cfg["video"])
            return Pipeline._apply_extra_caps(s, elem_cfg["video"])

        if isinstance(elem_cfg.get("image"), dict):
            s = Pipeline._image_stream_from_cfg(elem_cfg["image"])
            return Pipeline._apply_extra_caps(s, elem_cfg["image"])

        if isinstance(elem_cfg.get("h264"), dict):
            s = Pipeline._h264_stream_from_cfg(elem_cfg["h264"])
            return Pipeline._apply_extra_caps(s, elem_cfg["h264"])

        if isinstance(elem_cfg.get("tensor"), dict):
            s = Pipeline._tensor_stream_from_cfg(elem_cfg["tensor"])
            return Pipeline._apply_extra_caps(s, elem_cfg["tensor"])

        if isinstance(elem_cfg.get("audio"), dict):
            return Pipeline._audio_stream_from_cfg(elem_cfg["audio"])

        if elem_cfg.get("text") is not None:
            return TextFilter()

        raise ValueError(
            "filter element must define one of: caps, video, image, h264, tensor, text"
        )

    @staticmethod
    def _audio_stream_from_cfg(a: dict) -> AudioFilter:
        """Performs the audio stream from cfg operation used by the SDK.

        Args:
            a: A value. Type: dict.

        Returns:
            AudioFilter: Result of the operation.
        """
        s = AudioFilter()

        if "format" in a:
            s = s.format(a["format"])

        if "channels" in a:
            s = s.channels(int(a["channels"]))

        if "rate" in a:
            s = s.rate(int(a["rate"]))

        if "layout" in a:
            s = s.layout(a["layout"])

        return s

    @staticmethod
    def _video_stream_from_cfg(v: dict) -> VideoFilter:
        """Performs the video stream from cfg operation used by the SDK.

        Args:
            v: V value. Type: dict.

        Returns:
            VideoFilter: Result of the operation.
        """
        s = VideoFilter()

        if "format" in v:
            s = s.format(v["format"])
        if "width" in v and "height" in v:
            s = s.resolution(int(v["width"]), int(v["height"]))
        if "framerate" in v:
            fr = v["framerate"]
            if isinstance(fr, str) and "/" in fr:
                n, d = fr.split("/", 1)
                s = s.framerate(int(n), int(d))
            else:
                s = s.framerate(int(fr))

        return s

    @staticmethod
    def _tensor_stream_from_cfg(t: dict) -> TensorFilter:
        """Performs the tensor stream from cfg operation used by the SDK.

        Args:
            t: Tensor type value. Type: dict.

        Returns:
            TensorFilter: Result of the operation.
        """
        s = TensorFilter()

        if "type" in t:
            s = s.type(t["type"])

        if "dimensions" in t:
            s = s.dimensions(t["dimensions"])

        return s

    @staticmethod
    def _image_stream_from_cfg(j: dict) -> ImageFilter:
        """Performs the image stream from cfg operation used by the SDK.

        Args:
            j: J value. Type: dict.

        Returns:
            ImageFilter: Result of the operation.
        """
        s = ImageFilter()

        if "format" in j:
            s = s.format(j["format"])
        if "width" in j and "height" in j:
            s = s.resolution(int(j["width"]), int(j["height"]))

        return s

    @staticmethod
    def _h264_stream_from_cfg(h: dict) -> H264Filter:
        """Performs the h264 stream from cfg operation used by the SDK.

        Args:
            h: Height in pixels. Type: dict.

        Returns:
            H264Filter: Result of the operation.
        """
        s = H264Filter()

        if "profile" in h:
            s = s.profile(h["profile"])
        if "level" in h:
            s = s.level(h["level"])
        if "width" in h and "height" in h:
            s = s.resolution(int(h["width"]), int(h["height"]))
        if "framerate" in h:
            fr = h["framerate"]
            if isinstance(fr, str) and "/" in fr:
                n, d = fr.split("/", 1)
                s = s.framerate(int(n), int(d))
            else:
                s = s.framerate(int(fr))

        return s

    @classmethod
    def _link_from_yaml(cls, pipe: Pipeline, cfg: dict) -> None:
        """Performs the link from yaml operation used by the SDK.

        Args:
            pipe: Pipe value. Type: Pipeline.
            cfg: Configuration dictionary section. Type: dict.

        Returns:
            None.
        """
        pconf = cfg.get("pipeline") or {}
        links = pconf.get("links") or []

        for chain in links:
            if not isinstance(chain, (list, tuple)) or len(chain) < 2:
                raise ValueError(
                    f"Link chain must be a list of >=2 element names: {chain!r}"
                )
            pipe.link(*chain)
