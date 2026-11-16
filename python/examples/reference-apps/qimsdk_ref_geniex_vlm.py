#!/usr/bin/env python3
# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""Run GenieX on a synthetic video frame every 10 seconds."""

import argparse


def run(model: str, prompt: str, device: str) -> None:
    from qimsdk import AppSink, Element, ImageFilter, Pipeline, StreamFilter

    inference_count = 0

    def print_response(buffer):
        nonlocal inference_count
        data = buffer.to_bytes()
        if data is None:
            return

        inference_count += 1
        pts = buffer.pts
        timestamp = (
            f"{pts / 1_000_000_000:.1f}s"
            if pts is not None and pts < (1 << 64) - 1
            else "unknown time"
        )
        text = data.decode("utf-8", errors="replace")
        print(f"\n[Inference {inference_count} at video time {timestamp}]\n{text}", flush=True)

    source = Element("videotestsrc", "source").set(
        "is-live", True, "pattern", "ball"
    )
    rate = Element("videorate", "rate").set("drop-only", True)
    sample_queue = Element("queue", "sample-queue").set(
        "leaky", 2, "max-size-buffers", 1, "max-size-bytes", 0, "max-size-time", 0
    )
    encoder = Element("jpegenc", "encoder").set("quality", 85)
    vlm = Element("geniex", "vlm").set(
        "model", model, "prompt", prompt, "device", device
    )
    sink = AppSink("output").set("sync", False).set_buffer_consumer(print_response)

    pipeline = (
        Pipeline("geniex-vlm-video")
        .add(source)
        .add_stream_filter(
            "source-format",
            StreamFilter("video/x-raw,format=I420,width=640,height=480,framerate=30/1"),
        )
        .add(rate)
        .add_stream_filter(
            "inference-rate",
            StreamFilter("video/x-raw,format=I420,width=640,height=480,framerate=1/10"),
        )
        .add(encoder)
        .add_stream_filter("jpeg-format", ImageFilter().format("JPEG"))
        .add(sample_queue)
        .add(vlm)
        .add(sink)
    )

    try:
        pipeline.start()
        print("Running VLM inference every 10 seconds. Press Ctrl+C to stop.", flush=True)
        pipeline.wait()
    except KeyboardInterrupt:
        print("\nStopping video inference.", flush=True)
    finally:
        pipeline.stop()

    if not inference_count:
        raise RuntimeError("Pipeline ended without a VLM response")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--model",
        default="unsloth/Qwen3.5-2B-GGUF:Q4_0",
        help="GenieX model ID (downloaded automatically if not cached)",
    )
    parser.add_argument("--prompt", default="Describe this image.")
    parser.add_argument(
        "--device",
        default="auto",
        help="Device alias/list (default: auto)",
    )
    args = parser.parse_args()

    run(args.model, args.prompt, args.device)


if __name__ == "__main__":
    main()
