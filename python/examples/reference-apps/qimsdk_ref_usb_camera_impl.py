#!/usr/bin/env python3
# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""USB camera preview using auto-linking."""

import argparse

from qimsdk import Pipeline, VideoFilter

#  Example pipeline:
#
#    source -> transform -> [videofilter] -> display
#
#  The pipeline reads frames from a USB (V4L2) camera, rotates them, restricts
#  the stream to NV12/1080p/30fps, and displays the result through Wayland.


def create_and_execute_pipeline(device: str) -> None:

    # Creates the pipeline, adds and links elements, and executes it.
    #
    # Elements are created on the fly as they are added, and linking is
    # implicit, following the order in which elements are added.
    #
    # sync=false disables strict rendering synchronization to the pipeline clock.
    # fullscreen=true renders the output fullscreen on the target display.
    pipeline = (
        Pipeline("usb-cam-pipeline")
        # Captures frames from the USB camera source.
        .add("v4l2src", "source", "device", device)
        # Applies geometric transforms to video frames.
        .add("qtivtransform", "transform")
        # Stream filter used in the branch link.
        # It defines specific stream characteristics from the supported options.
        .add_stream_filter(
            "videofilter",
            VideoFilter().format("NV12").resolution(1920, 1080).framerate(30),
        )
        # Render video stream on display.
        .add("waylandsink", "display", "sync", False, "fullscreen", True)
    )

    pipeline.execute()


def main() -> None:

    from qimsdk import ImsdkGstLogMode, ImsdkLogLevel, SetImsdkGstLogMode, SetImsdkLogLevel
    SetImsdkGstLogMode(ImsdkGstLogMode.ImsdkLog)
    SetImsdkLogLevel(ImsdkLogLevel.Debug)

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "device", nargs="?", default="/dev/video2",
        help="V4L2 device node to capture from (default: %(default)s)",
    )
    args = parser.parse_args()

    create_and_execute_pipeline(args.device)


if __name__ == "__main__":
    main()
