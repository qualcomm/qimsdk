# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

from __future__ import annotations

from typing import Any, Optional

from ._element import Element
from ._mlpostprocess import MLPostprocess
from ._mlvconverter import MLVConverter


class MLVideoSNPEBin(Element):
    """Wrapper for qtimlvideosnpebin that exposes preprocess and postprocess handler functionality."""

    def __init__(self, name: str | None = None, *, existing: Any | None = None) -> None:
        super().__init__("qtimlvideosnpebin" if existing is None else None, name, existing=existing)

        self._postprocess: Optional[MLPostprocess] = None
        self._preprocess: Optional[MLVConverter] = None

    @classmethod
    def cast(cls, element: Any) -> "MLVideoSNPEBin":
        raw = element.get_raw() if hasattr(element, "get_raw") else element
        return cls(existing=raw)

    def _find_postprocess(self) -> Any | None:
        """Recursively search for qtimlpostprocess inside the bin."""

        def recurse(bin_elem):
            it = bin_elem.iterate_elements()
            while True:
                try:
                    ok, child = it.next()
                except Exception:
                    break

                if not ok or child is None:
                    break

                factory = child.get_factory()
                if factory and factory.get_name() == "qtimlpostprocess":
                    return child

                if hasattr(child, "iterate_elements"):
                    result = recurse(child)
                    if result:
                        return result

            return None

        return recurse(self._elem)

    def _find_preprocess(self) -> Any | None:
        """Locate the internal 'mlpreprocess' element inside the bin, by name."""
        if not hasattr(self._elem, "get_by_name"):
            return None
        return self._elem.get_by_name("mlpreprocess")

    def _ensure_postprocess(self) -> MLPostprocess:
        if self._postprocess is None:
            post = self._find_postprocess()
            if post is None:
                raise RuntimeError(
                    "qtimlpostprocess not found inside qtimlbin. "
                    "Ensure the pipeline is constructed and elements are realized."
                )

            self._postprocess = MLPostprocess(existing=post)

        return self._postprocess

    def _ensure_preprocess(self) -> MLVConverter:
        if self._preprocess is None:
            pre = self._find_preprocess()
            if pre is None:
                raise RuntimeError(
                    "'mlpreprocess' not found inside qtimlbin. "
                    "Ensure the pipeline is constructed and elements are realized."
                )

            self._preprocess = MLVConverter(existing=pre)

        return self._preprocess

    def set_postprocess_handler(self, handler: Any, kind: str | None = None) -> "MLVideoSNPEBin":
        post = self._ensure_postprocess()
        post.set_handler(handler, kind)
        return self

    def set_preprocess_handler(self, handler: Any) -> "MLVideoSNPEBin":
        """Delegate handler setup to internal MLVConverter."""
        pre = self._ensure_preprocess()
        pre.set_handler(handler)
        return self
