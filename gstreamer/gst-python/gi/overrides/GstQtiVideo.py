#
# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear
import typing
import gi

gi.require_version('Gst', '1.0')
gi.require_version('GstVideo', '1.0')

from gi.overrides import override
from gi.overrides import _gi_gst_qti  # type: ignore[attr-defined]
_gi_gst_qti

if typing.TYPE_CHECKING:
    # Import stubs for type checking this file.
    from gi.repository import GstQtiVideo
else:
    from gi.module import get_introspection_module
    GstQtiVideo = get_introspection_module('GstQtiVideo')

__all__ = []


def __region_of_interest_meta_get_quadrilateral(meta) -> GstQtiVideo.Quadrilateral:
    """
    Convert the rectangle of a GstVideo.VideoRegionOfInterestMeta into a
    GstQtiVideo.Quadrilateral, applying the rectangle-affine-matrix if present.
    """
    ax, ay, bx, by, cx, cy, dx, dy = \
        _gi_gst_qti.video_region_of_interest_meta_get_quadrilateral(meta)

    quadrilateral = GstQtiVideo.Quadrilateral()

    quadrilateral.a.x, quadrilateral.a.y = ax, ay
    quadrilateral.b.x, quadrilateral.b.y = bx, by
    quadrilateral.c.x, quadrilateral.c.y = cx, cy
    quadrilateral.d.x, quadrilateral.d.y = dx, dy

    return quadrilateral


GstQtiVideo.region_of_interest_meta_get_quadrilateral = \
    __region_of_interest_meta_get_quadrilateral
__all__.append('region_of_interest_meta_get_quadrilateral')


class ConverterEngine(GstQtiVideo.ConverterEngine):
    def compose(self, compositions, is_async=False):  # type: ignore[override]
        """
        Submit compositions for execution.

        is_async selects synchronous (default) or asynchronous composition.
        """
        return _gi_gst_qti.video_converter_engine_compose(
            self, compositions, is_async)

    def wait_fence(self, fence):
        """Wait for an async compose() to finish. fence=None returns True immediately."""
        return _gi_gst_qti.video_converter_engine_wait_fence(self, fence)

override(ConverterEngine)
__all__.append("ConverterEngine")


class Composition(GstQtiVideo.Composition):
    @property
    def blits(self):
        """Get or set the #GstQtiVideo.Blits used as inputs, or None if unset."""
        return _gi_gst_qti.video_composition_get_blits(self)

    @blits.setter
    def blits(self, value):
        _gi_gst_qti.video_composition_set_blits(self, value)

    @property
    def buffer(self):
        """Get or set the #Gst.Buffer used as output, or None if unset."""
        return _gi_gst_qti.video_composition_get_buffer(self)

    @buffer.setter
    def buffer(self, value):
        _gi_gst_qti.video_composition_set_buffer(self, value)

    @property
    def info(self):
        """Get or set the #GstVideo.VideoInfo used for mapping.

        The composition stores its own copy, modify the returned object and
        assign it back to apply changes.
        """
        return _gi_gst_qti.video_composition_get_info(self)

    @info.setter
    def info(self, value):
        _gi_gst_qti.video_composition_set_info(self, value)

    @property
    def offsets(self):
        """Get or set the component offset factors used in the normalize operation."""
        return _gi_gst_qti.video_composition_get_offsets(self)

    @offsets.setter
    def offsets(self, value):
        _gi_gst_qti.video_composition_set_offsets(self, value)

    @property
    def scales(self):
        """Get or set the component scale factors used in the normalize operation."""
        return _gi_gst_qti.video_composition_get_scales(self)

    @scales.setter
    def scales(self, value):
        _gi_gst_qti.video_composition_set_scales(self, value)

override(Composition)
__all__.append("Composition")
