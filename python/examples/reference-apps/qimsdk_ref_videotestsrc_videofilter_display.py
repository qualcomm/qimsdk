#!/usr/bin/env python3
# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""Test video test source with stream filter."""

import argparse
import sys

from qimsdk import Element, Pipeline, VideoFilter


class HelpOnErrorArgumentParser(argparse.ArgumentParser):
    def error(self, message):
        self.print_help(sys.stderr)
        sys.stderr.write(f"\n{self.prog}: error: {message}\n")
        sys.exit(2)

#  Example pipeline:
#
#    src -> [vf1] -> transform -> [vf2] -> display
#
#  The pipeline generates synthetic test video frames, restricts them to NV12
#  at 1920x1080/30fps, rotates the frames, restricts the output to 1280x720,
#  and displays the result through Wayland.


def create_and_execute_pipeline() -> None:

    # Generates synthetic test video frames.
    src = (
        Element("videotestsrc", "src")
        .set("pattern", "ball")
    )

    # Restricts the generated stream to NV12 at 1920x1080, 30fps.
    vf1 = VideoFilter().format("NV12").resolution(1920, 1080).framerate(30)

    # Applies geometric transforms to video frames.
    transform = (
        Element("qtivtransform", "transform")
        .set("rotate", "180")
    )

    # Restricts the transformed stream to 1280x720.
    vf2 = VideoFilter().resolution(1280, 720)

    # Render video stream on display.
    display = (
        Element("waylandsink", "display")
        .set("fullscreen", True)
    )

    # Creates the pipeline, adds and links elements, and executes it.
    #
    # Linking is implicit and follows the order in which elements are added.
    pipeline = (
        Pipeline("video-pipeline")
        .add(src)
        .add_stream_filter("vf1", vf1)
        .add(transform)
        .add_stream_filter("vf2", vf2)
        .add(display)
    )

    pipeline.execute()


def main() -> None:

    parser = HelpOnErrorArgumentParser(description=__doc__)
    parser.parse_args()

    from qimsdk import ImsdkGstLogMode, ImsdkLogLevel, SetImsdkGstLogMode, SetImsdkLogLevel
    SetImsdkGstLogMode(ImsdkGstLogMode.ImsdkLog)
    SetImsdkLogLevel(ImsdkLogLevel.Debug)

    create_and_execute_pipeline()


if __name__ == "__main__":
    main()
