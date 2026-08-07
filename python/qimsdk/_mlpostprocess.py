# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

from __future__ import annotations

import inspect
from typing import Any, get_type_hints

from ._element import Element

try:
    import gi

    gi.require_version("Gst", "1.0")
    from gi.repository import Gst
except Exception:  # pragma: no cover - runtime dependent
    Gst = None  # type: ignore

try:
    import gi

    gi.require_version("GLib", "2.0")
    gi.require_version("GstQtiML", "1.0")
    from gi.repository import GLib, GstQtiML
except Exception:  # pragma: no cover - runtime dependent
    GLib = None  # type: ignore
    GstQtiML = None  # type: ignore


class MLPostprocess(Element):
    """Wraps qtimlpostprocess and connects Python post-processing callbacks to GStreamer signals.
    """
    def __init__(self, name: str | None = None, *, existing: Any | None = None) -> None:
        """Initializes the object.

        Args:
            name: Name of the element, pad, pipeline, file, or configuration entry. Type: str | None.
            existing: Existing GStreamer element to wrap instead of creating a new one. Type: Any | None.

        Returns:
            None.
        """
        super().__init__("qtimlpostprocess" if existing is None else None, name, existing=existing)
        self._signal_handler_id: int | None = None
        self._wrapped_handler: Any | None = None

    @classmethod
    def cast(cls, element: Any) -> "MLPostprocess":
        """Creates a typed SDK wrapper around an existing GStreamer element.

        Args:
            element: Element value. Type: Any.

        Returns:
            "MLPostprocess": Result of the operation.
        """
        raw = element.get_raw() if hasattr(element, "get_raw") else element
        return cls(existing=raw)

    def _infer_kind(self, handler: Any) -> str:
        """Infer the ML postprocess signal type from callback annotations.

        The output/result argument must be annotated with one of the public
        MLPostprocess marker types, for example ``ObjectDetections`` or
        ``ImageClassifications``. This avoids guessing from callback names or
        parameter names and keeps custom handlers refactor-safe.

        Supported callback forms:

            def callback(mlframe, mlparams, results: ObjectDetections): ...

            def callback(mlpostprocess, mlframe, mlparams, results: ObjectDetections): ...

        Args:
            handler: User callback to register. Type: Any.

        Returns:
            str: qtimlpostprocess output kind, for example ``object-detection``.
        """
        try:
            signature = inspect.signature(handler)
        except (TypeError, ValueError) as exc:
            raise ValueError(
                "Unable to inspect postprocess handler signature. "
                "Please annotate the callback output parameter or set kind explicitly."
            ) from exc

        params = [
            param
            for param in signature.parameters.values()
            if param.kind in (
                inspect.Parameter.POSITIONAL_ONLY,
                inspect.Parameter.POSITIONAL_OR_KEYWORD,
            )
        ]

        if len(params) == 3:
            output_param = params[2]
        elif len(params) == 4:
            output_param = params[3]
        else:
            raise ValueError(
                "Postprocess handler must accept either 3 arguments "
                "(mlframe, mlparams, results) or 4 arguments "
                "(mlpostprocess, mlframe, mlparams, results)"
            )

        try:
            hints = get_type_hints(handler, include_extras=True)
        except Exception:
            hints = getattr(handler, "__annotations__", {})

        annotation = hints.get(output_param.name)
        if annotation is None or annotation is inspect.Parameter.empty:
            raise ValueError(
                "Unable to infer postprocess handler type automatically. "
                f"Please annotate the output parameter '{output_param.name}', for example: "
                "results: ObjectDetections, or set kind explicitly."
            )

        kind = getattr(annotation, "kind", None)
        if kind is None:
            raise ValueError(
                "Unsupported postprocess output annotation. "
                f"Expected an MLPostprocess output marker type, got: {annotation!r}"
            )

        return kind

    def _wrap_handler(self, handler: Any) -> Any:
        """Adapts the user callback to the GStreamer signal callback signature.

        Args:
            handler: User callback to register. Type: Any.

        Returns:
            Any: Result of the operation.
        """
        try:
            arity = len(inspect.signature(handler).parameters)
        except (TypeError, ValueError):
            arity = None

        if arity == 3:
            return lambda _mlpostprocess, mlframe, mlparams, results: handler(mlframe, mlparams, results)

        if arity == 4 or arity is None:
            return handler

        raise ValueError(
            "Postprocess handler must accept either 3 arguments "
            "(mlframe, mlparams, results) or 4 arguments "
            "(mlpostprocess, mlframe, mlparams, results)"
        )

    def set_handler(self, handler: Any, kind: str | None = None) -> "MLPostprocess":
        """Registers a Python callback for ML postprocess output.

        Args:
            handler: User callback to register. Type: Any.
            kind: Kind value. Type: str | None.

        Returns:
            "MLPostprocess": Result of the operation.
        """
        if not callable(handler):
            raise ValueError("Postprocess handler callback is empty")

        resolved_kind = kind if kind is not None else self._infer_kind(handler)

        signal_name = {
            "image-classification": "process-image-classification",
            "audio-classification": "process-audio-classification",
            "object-detection": "process-object-detection",
            "pose-estimation": "process-pose-estimation",
            "depth-estimation": "process-depth-estimation",
            "image-segmentation": "process-segmentation",
            "tensors": "process-tensors",
        }.get(resolved_kind)
        if signal_name is None:
            raise ValueError(f"Unsupported postprocess handler type: {resolved_kind}")

        # Enable external/custom postprocess mode for qtimlpostprocess.
        self.set(module="none")

        if self._signal_handler_id:
            try:
                self._elem.disconnect(self._signal_handler_id)
            except Exception:
                pass
            self._signal_handler_id = None

        self._wrapped_handler = self._wrap_handler(handler)
        self._signal_handler_id = self._elem.connect(signal_name, self._wrapped_handler)
        return self
