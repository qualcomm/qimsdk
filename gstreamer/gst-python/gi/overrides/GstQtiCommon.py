#
# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear
import typing
import gi

gi.require_version('Gst', '1.0')

from gi.overrides import _gi_gst_qti  # type: ignore[attr-defined]
_gi_gst_qti

if typing.TYPE_CHECKING:
    # Import stubs for type checking this file.
    from gi.repository import GstQtiCommon
else:
    from gi.module import get_introspection_module
    GstQtiCommon = get_introspection_module('GstQtiCommon')

__all__ = []


def __buffer_get_metas(buffer, meta_class) -> list:
    """
    Get all metas of the given meta struct class (e.g. GstQtiVideo.ClassificationMeta)
    attached to the Gst.Buffer, in attachment order.
    """
    return _gi_gst_qti.buffer_get_metas(buffer, meta_class.get_info().api, meta_class)


GstQtiCommon.buffer_get_metas = __buffer_get_metas
__all__.append('buffer_get_metas')
