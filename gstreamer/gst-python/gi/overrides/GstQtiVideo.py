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
