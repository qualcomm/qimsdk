#!/usr/bin/env python3
# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""Two-hand fingertip video-region demo (reference-style structure)."""

import json
import math
import os
import re
import argparse
import sys
import threading
import traceback

import cairo
import cv2
import numpy as np

from qimsdk import AppSink, Element, Pipeline, TextFilter, VideoFilter

# =============================================================================
# Configuration
# =============================================================================

class HelpOnErrorArgumentParser(argparse.ArgumentParser):
    def error(self, message):
        self.print_help(sys.stderr)
        sys.stderr.write(f"\n{self.prog}: error: {message}\n")
        sys.exit(2)


parser = HelpOnErrorArgumentParser(
    description="QIMSDK reference app",
    formatter_class=argparse.ArgumentDefaultsHelpFormatter,
)
parser.add_argument(
    "-i",
    "--input-config",
    default="/dev/video0",
    help="Input source configuration (camera number, device, or file path)",
)
parser.add_argument(
    "-m",
    "--model-base-path",
    default=os.environ["HOME"] + "/Downloads/qimsdk_samples",
    help="Base model/label path",
)
args = parser.parse_args()


CAMERA_DEVICE = args.input_config
FRAME_WIDTH, FRAME_HEIGHT, FRAME_FPS = 1920, 1080, 20

model_base_path = args.model_base_path

THUMB_TIP_ID = 4
INDEX_TIP_ID = 8

POINT_SMOOTHING_ALPHA = 0.35
MISS_HOLD_FRAMES = 5
MIN_POLYGON_AREA = 1200

# Landmark labels used by parser fallback.
LABEL_TO_ID = {
    "wrist": 0,
    "thumb cmc": 1,
    "thumb mcp": 2,
    "thumb ip": 3,
    "thumb tip": 4,
    "index finger mcp": 5,
    "index finger pip": 6,
    "index finger dip": 7,
    "index finger tip": 8,
    "middle finger mcp": 9,
    "middle finger pip": 10,
    "middle finger dip": 11,
    "middle finger tip": 12,
    "ring finger mcp": 13,
    "ring finger pip": 14,
    "ring finger dip": 15,
    "ring finger tip": 16,
    "pinky mcp": 17,
    "pinky pip": 18,
    "pinky dip": 19,
    "pinky tip": 20,
}


# =============================================================================
# State
# =============================================================================

class FourFingerState:
    def __init__(self):
        self.lock = threading.Lock()
        self.visible = False
        self.status = "WAITING FOR TWO HANDS"
        self.hand_count = 0
        self.miss_count = 0

        self.points = {
            "left_index": None,
            "right_index": None,
            "right_thumb": None,
            "left_thumb": None,
        }

        self.video_frame = None
        self.video_status = "VIDEO NOT STARTED"
        self.video_error = ""
        self.video_frame_index = 0
        self.video_draw_error = ""


STATE = FourFingerState()
CLIP_SINK = None


def on_clip_sample(buffer):
    if buffer is None:
        return

    data = buffer.data()
    if not data:
        return

    if CLIP_SINK is None:
        return

    try:
        caps = CLIP_SINK.get_raw().get_static_pad("sink").get_current_caps()
        s = caps.get_structure(0)
        w = int(s.get_value("width"))
        h = int(s.get_value("height"))
        fmt = str(s.get_value("format")) if s.has_field("format") else "UNKNOWN"
        if fmt != "BGR":
            raise ValueError(f"unexpected format '{fmt}', expected 'BGR'")

        row_stride = len(data) // h
        if row_stride < w * 3:
            raise ValueError(f"row stride too small: {row_stride}")

        frame = np.ndarray((h, w, 3), dtype=np.uint8, buffer=data, strides=(row_stride, 3, 1)).copy()
        with STATE.lock:
            STATE.video_frame = frame
            STATE.video_frame_index += 1
            STATE.video_status = "VIDEO PLAYING"
            STATE.video_error = ""

    except Exception as exc:
        with STATE.lock:
            STATE.video_status = "VIDEO SAMPLE ERROR"
            STATE.video_error = str(exc)


# =============================================================================
# Helpers
# =============================================================================

def clamp(v, lo, hi):
    return max(lo, min(int(v), hi))


def num(v):
    try:
        return float(v)
    except Exception:
        return None


def norm(v):
    return str(v).strip().lower().replace("_", " ").replace("-", " ")


def first(d, keys):
    return next((d[k] for k in keys if isinstance(d, dict) and k in d), None)


def pix(v, size):
    n = num(v)
    if n is None:
        return None
    if -0.25 <= n <= 2.0:
        n *= size
    return int(round(n))


def smooth_point(prev, cur, alpha):
    if cur is None:
        return prev
    if prev is None:
        return cur
    return (
        int(round(alpha * cur[0] + (1.0 - alpha) * prev[0])),
        int(round(alpha * cur[1] + (1.0 - alpha) * prev[1])),
    )


def hand_center(hand):
    pts = list(hand.values())
    if not pts:
        return 0.0, 0.0
    return (
        sum(p[0] for p in pts) / float(len(pts)),
        sum(p[1] for p in pts) / float(len(pts)),
    )


def polygon_area(points):
    if not points or len(points) < 3:
        return 0.0
    area = 0.0
    n = len(points)
    for i in range(n):
        x1, y1 = points[i]
        x2, y2 = points[(i + 1) % n]
        area += x1 * y2 - x2 * y1
    return abs(area) * 0.5


def draw_polygon_path(ctx, points):
    ctx.new_path()
    ctx.move_to(points[0][0], points[0][1])
    for p in points[1:]:
        ctx.line_to(p[0], p[1])
    ctx.close_path()


def load_json(text):
    cuts = [text]
    for a, b in [("{", "}"), ("[", "]")]:
        i = text.find(a)
        j = text.rfind(b)
        if i >= 0 and j > i:
            cuts.append(text[i:j + 1])
    for s in cuts:
        try:
            return json.loads(s.strip())
        except Exception:
            pass
    return None


def point_any(v):
    if isinstance(v, dict):
        return point_dict(v)
    if isinstance(v, (list, tuple)) and len(v) >= 2:
        x = pix(v[0], FRAME_WIDTH)
        y = pix(v[1], FRAME_HEIGHT)
        if x is not None and y is not None:
            return x, y
    return None


def point_dict(d):
    x = first(d, ["x", "X", "x_pos", "xpos", "center_x", "centerX", "px", "point_x"])
    y = first(d, ["y", "Y", "y_pos", "ypos", "center_y", "centerY", "py", "point_y"])
    if x is not None and y is not None:
        x = pix(x, FRAME_WIDTH)
        y = pix(y, FRAME_HEIGHT)
        if x is not None and y is not None:
            return x, y

    r = first(d, ["rectangle", "rect", "bbox", "box", "bounding_box", "bounding-box"])
    if isinstance(r, (list, tuple)) and len(r) >= 4:
        x1 = pix(r[0], FRAME_WIDTH)
        y1 = pix(r[1], FRAME_HEIGHT)
        x2 = pix(r[2], FRAME_WIDTH)
        y2 = pix(r[3], FRAME_HEIGHT)
        if None not in (x1, y1, x2, y2):
            return int((x1 + x2) / 2), int((y1 + y2) / 2)

    for k in ["point", "position", "coord", "coords", "coordinate", "location"]:
        if isinstance(d, dict) and k in d:
            p = point_any(d.get(k))
            if p is not None:
                return p
    return None


def lm_id(d):
    v = first(d, ["id", "class_id", "classId", "label_id", "labelId", "index", "idx"])
    if v is not None:
        try:
            i = int(float(str(v)))
            if 0 <= i <= 20:
                return i
        except Exception:
            pass
    label = first(d, ["label", "name", "class", "class_name", "display_name"])
    if label is not None:
        return LABEL_TO_ID.get(norm(label))
    return None


def sequence_to_hands(seq):
    hands = []
    if not isinstance(seq, list) or not seq:
        return hands

    explicit_ids = []
    for item in seq:
        if isinstance(item, dict):
            i = lm_id(item)
            if i is not None:
                explicit_ids.append(i)

    if len(explicit_ids) >= 15:
        current = {}
        for item in seq:
            if not isinstance(item, dict):
                continue
            i = lm_id(item)
            p = point_dict(item)
            if i is None or p is None:
                continue
            if i in current and len(current) >= 15:
                hands.append(current)
                current = {}
            current[i] = p
            if len(current) >= 21:
                hands.append(current)
                current = {}
        if len(current) >= 15:
            hands.append(current)
        return hands

    point_count = sum(1 for item in seq if point_any(item) is not None)
    if point_count >= 15:
        for offset in range(0, len(seq), 21):
            chunk = seq[offset:offset + 21]
            if len(chunk) < 15:
                continue
            hand = {}
            for i, item in enumerate(chunk[:21]):
                p = point_any(item)
                if p is not None:
                    hand[i] = p
            if len(hand) >= 15:
                hands.append(hand)
    return hands


def collect_hands(obj, hands):
    if isinstance(obj, list):
        before = len(hands)
        for item in obj:
            if isinstance(item, (dict, list)):
                collect_hands(item, hands)
        if len(hands) == before:
            for h in sequence_to_hands(obj):
                hands.append(h)
        return

    if isinstance(obj, dict):
        known_keys = [
            "landmarks", "hand_landmarks", "keypoints", "points", "joints",
            "results", "detections", "objects", "predictions", "children",
        ]
        before = len(hands)
        for key in known_keys:
            if key in obj and isinstance(obj[key], (dict, list)):
                collect_hands(obj[key], hands)

        numeric_hand = {}
        for k, v in obj.items():
            try:
                i = int(k)
            except Exception:
                continue
            if 0 <= i <= 20:
                p = point_any(v)
                if p is not None:
                    numeric_hand[i] = p
        if len(numeric_hand) >= 15:
            hands.append(numeric_hand)
            return

        if len(hands) > before:
            return
        for v in obj.values():
            if isinstance(v, (dict, list)):
                collect_hands(v, hands)


def regex_lms(text):
    out = {}
    s = " ".join(text.replace("\n", " ").replace("\r", " ").split())
    patterns = [
        r"(?:id|class_id|classId|label_id|labelId|index|idx)\s*[:=]\s*(\d+).{0,220}?(?:x|X)\s*[:=]\s*(-?\d+(?:\.\d+)?).{0,80}?(?:y|Y)\s*[:=]\s*(-?\d+(?:\.\d+)?)",
        r"(?:id|class_id|classId|label_id|labelId|index|idx)\s*[:=]\s*(\d+).{0,220}?(?:y|Y)\s*[:=]\s*(-?\d+(?:\.\d+)?).{0,80}?(?:x|X)\s*[:=]\s*(-?\d+(?:\.\d+)?)",
    ]
    for m in re.finditer(patterns[0], s):
        i = int(m.group(1))
        x = pix(m.group(2), FRAME_WIDTH)
        y = pix(m.group(3), FRAME_HEIGHT)
        if 0 <= i <= 20 and x is not None and y is not None:
            out[i] = (x, y)
    for m in re.finditer(patterns[1], s):
        i = int(m.group(1))
        y = pix(m.group(2), FRAME_HEIGHT)
        x = pix(m.group(3), FRAME_WIDTH)
        if 0 <= i <= 20 and x is not None and y is not None:
            out[i] = (x, y)
    return out


def clean_hand(h):
    out = {}
    for i, p in h.items():
        try:
            i = int(i)
        except Exception:
            continue
        if not (0 <= i <= 20) or p is None:
            continue
        x, y = p
        out[i] = (clamp(x, 0, FRAME_WIDTH - 1), clamp(y, 0, FRAME_HEIGHT - 1))
    return out


def dedupe_hands(hands):
    unique = []
    for h in sorted(hands, key=lambda x: len(x), reverse=True):
        if THUMB_TIP_ID not in h or INDEX_TIP_ID not in h:
            continue
        cx, cy = hand_center(h)
        duplicate = False
        for u in unique:
            ux, uy = hand_center(u)
            if math.hypot(cx - ux, cy - uy) < 35:
                duplicate = True
                break
        if not duplicate:
            unique.append(h)
    return unique


def parse_hands(text):
    hands = []
    obj = load_json(text)
    if obj is not None:
        collect_hands(obj, hands)

    cleaned = []
    for h in hands:
        ch = clean_hand(h)
        if len(ch) >= 2 and THUMB_TIP_ID in ch and INDEX_TIP_ID in ch:
            cleaned.append(ch)
    cleaned = dedupe_hands(cleaned)

    if len(cleaned) < 2:
        fallback = clean_hand(regex_lms(text))
        if THUMB_TIP_ID in fallback and INDEX_TIP_ID in fallback:
            cleaned.append(fallback)
        cleaned = dedupe_hands(cleaned)
    return cleaned


def update_state_from_hands(hands):
    valid = [h for h in hands if THUMB_TIP_ID in h and INDEX_TIP_ID in h]
    valid.sort(key=lambda h: hand_center(h)[0])

    with STATE.lock:
        STATE.hand_count = len(valid)

        if len(valid) >= 2:
            left_hand = valid[0]
            right_hand = valid[-1]

            raw_points = {
                "left_index": left_hand[INDEX_TIP_ID],
                "right_index": right_hand[INDEX_TIP_ID],
                "right_thumb": right_hand[THUMB_TIP_ID],
                "left_thumb": left_hand[THUMB_TIP_ID],
            }

            for key, p in raw_points.items():
                STATE.points[key] = smooth_point(STATE.points.get(key), p, POINT_SMOOTHING_ALPHA)

            STATE.visible = True
            STATE.status = "VIDEO REGION ACTIVE"
            STATE.miss_count = 0

        else:
            STATE.miss_count += 1
            if STATE.miss_count > MISS_HOLD_FRAMES:
                STATE.visible = False
                STATE.status = "WAITING FOR TWO HANDS"
                for key in STATE.points:
                    STATE.points[key] = None


def draw_video_in_polygon(ctx, points):
    frame = None
    with STATE.lock:
        if STATE.video_frame is not None:
            frame = STATE.video_frame.copy()

    if frame is None:
        return False

    try:
        xs = [clamp(p[0], 0, FRAME_WIDTH - 1) for p in points]
        ys = [clamp(p[1], 0, FRAME_HEIGHT - 1) for p in points]
        x0, x1 = min(xs), max(xs)
        y0, y1 = min(ys), max(ys)
        w = max(2, x1 - x0)
        h = max(2, y1 - y0)

        resized = cv2.resize(frame, (w, h), interpolation=cv2.INTER_LINEAR)
        bgra = cv2.cvtColor(resized, cv2.COLOR_BGR2BGRA)
        bgra[:, :, 3] = 255
        bgra = np.ascontiguousarray(bgra)

        surface = cairo.ImageSurface.create_for_data(
            bgra, cairo.FORMAT_ARGB32, w, h, w * 4
        )

        ctx.save()
        draw_polygon_path(ctx, points)
        ctx.clip()
        ctx.set_source_surface(surface, x0, y0)
        ctx.paint()
        ctx.restore()
        surface.flush()

        with STATE.lock:
            STATE.video_draw_error = ""
        return True

    except Exception as exc:
        with STATE.lock:
            STATE.video_draw_error = str(exc)
        return False


def draw_points_and_border(ctx, points):
    ctx.set_source_rgba(0.0, 0.0, 0.0, 1.0)
    ctx.set_line_width(7.0)
    draw_polygon_path(ctx, points)
    ctx.stroke()

    for p in points:
        ctx.set_source_rgba(0.0, 0.0, 0.0, 1.0)
        ctx.arc(p[0], p[1], 12.0, 0, 2 * math.pi)
        ctx.fill()


def draw_status_box(ctx):
    with STATE.lock:
        state_status = STATE.status
        hand_count = STATE.hand_count
        video_status = STATE.video_status
        video_error = STATE.video_error
        frame_index = STATE.video_frame_index
        draw_error = STATE.video_draw_error

    ctx.save()
    ctx.set_source_rgba(0.0, 0.0, 0.0, 0.72)
    ctx.rectangle(20, 20, 760, 130)
    ctx.fill()

    ctx.select_font_face("Sans")
    ctx.set_font_size(28)
    ctx.set_source_rgba(0.0, 1.0, 0.0, 1.0)
    ctx.move_to(35, 58)
    ctx.show_text(f"RECTANGLE VIDEO: {state_status}")

    ctx.set_font_size(21)
    ctx.set_source_rgba(1.0, 1.0, 1.0, 0.95)
    ctx.move_to(35, 92)
    ctx.show_text(f"hands={hand_count} video={video_status} frame={frame_index}")

    if video_error:
        ctx.set_source_rgba(1.0, 0.6, 0.2, 1.0)
        ctx.move_to(35, 122)
        ctx.show_text(f"video error: {video_error[:70]}")
    elif draw_error:
        ctx.set_source_rgba(1.0, 0.6, 0.2, 1.0)
        ctx.move_to(35, 122)
        ctx.show_text(f"draw error: {draw_error[:70]}")
    else:
        ctx.set_source_rgba(0.8, 0.9, 1.0, 1.0)
        ctx.move_to(35, 122)
        ctx.show_text(model_base_path + "/media/ai_demo_sample.mp4")

    ctx.restore()


def on_cairo_draw(_overlay, ctx, _timestamp, _duration):
    with STATE.lock:
        visible = STATE.visible
        points = [
            STATE.points.get("left_index"),
            STATE.points.get("right_index"),
            STATE.points.get("right_thumb"),
            STATE.points.get("left_thumb"),
        ]

    if visible and all(p is not None for p in points):
        area = polygon_area(points)
        if area >= MIN_POLYGON_AREA:
            draw_video_in_polygon(ctx, points)
            draw_points_and_border(ctx, points)

    draw_status_box(ctx)


# =============================================================================
# Pipeline
# =============================================================================

def create_and_execute_pipeline() -> None:

    #  Example pipelines:
    #
    #    clip: src -> demux -> parse -> decoder -> convert -> [clipfilter:BGR] -> clip_sink(appsink)
    #
    #    main: source -> transform -> [videostream] -> split
    #      split. -> q_video_palm -> metamux_palm
    #      split. -> q_palm_pre -> palm_preproc -> q_palm_infer -> palm_inf -> q_palm_post
    #             -> palm_post -> [palm_mlf] -> q_palm_meta -> metamux_palm
    #      metamux_palm -> palm_roi_transform -> split_after_palm
    #      split_after_palm. -> q_video_final -> metamux_final
    #      split_after_palm. -> q_hand_pre -> hand_preproc -> q_hand_infer -> hand_inf -> q_hand_post
    #                       -> hand_post -> [hand_mlf] -> q_hand_meta -> metamux_final
    #      metamux_final -> final_split
    #      final_split. -> q_display -> qtivoverlay -> to_cairo -> [cairofilter] -> video_region_canvas
    #                   -> to_display -> waylandsink
    #      final_split. -> q_meta_parse -> qtimlmetaparser -> meta_sink(appsink)
    #
    #  Hand-landmark metadata drives fingertip polygon points while Cairo draws
    #  the decoded clip frame clipped inside the polygon.

    # -------------------------------------------------------------------------
    # Clip decode pipeline (appsink consumer callback)
    # -------------------------------------------------------------------------

    # Clip file source.
    clip_source = Element("filesrc", "clip_src").set("location", model_base_path + "/media/ai_demo_sample.mp4")

    # Clip MP4 demuxer.
    clip_demux = Element("qtdemux", "clip_demux")

    # Clip H264 parser.
    clip_parse = Element("h264parse", "clip_parse")

    # Clip hardware decoder.
    clip_decoder = (
        Element("v4l2h264dec", "clip_decoder")
        .set("capture-io-mode", 4)
        .set("output-io-mode", 4)
    )

    # Clip color convert.
    clip_convert = Element("qtivtransform", "clip_convert")

    # Clip appsink format filter.
    clip_filter = VideoFilter().format("BGR")

    # Clip appsink element.
    clip_sink = AppSink("clip_sink")

    clip_sink.set("sync", False)
    clip_sink.set("max-buffers", 1)
    clip_sink.set("drop", True)

    global CLIP_SINK
    CLIP_SINK = clip_sink
    clip_sink.set_buffer_consumer(on_clip_sample)

    clip_pipeline = (
        Pipeline("video-region-clip")
        .add(clip_source)
        .add(clip_demux)
        .add(clip_parse)
        .add(clip_decoder)
        .add(clip_convert)
        .add_stream_filter("clip_filter", clip_filter)
        .add(clip_sink)
        .link("clip_src", "clip_demux", "clip_parse", "clip_decoder", "clip_convert", "clip_filter", "clip_sink")
    )

    with STATE.lock:
        STATE.video_status = "OPENING VIDEO..."
        STATE.video_error = ""

    # -------------------------------------------------------------------------
    # Main camera + ML + display pipeline
    # -------------------------------------------------------------------------

    # Camera source element.
    source = Element("v4l2src", "source")
    source.set("device", CAMERA_DEVICE)

    # Camera transform element.
    transform = Element("qtivtransform", "transform")
    transform.set("flip-horizontal", True)

    # Camera stream caps filter.
    videostream = (
        VideoFilter()
        .format("NV12")
        .resolution(FRAME_WIDTH, FRAME_HEIGHT)
        .framerate(FRAME_FPS)
    )

    # Camera tee split element.
    split = Element("tee", "split")

    # Queue for video path into palm metamux.
    q_video_palm = Element("queue", "q_video_palm")

    # Queue before palm preprocessor.
    q_palm_pre = Element("queue", "q_palm_pre")

    # Palm preprocessor element.
    palm_preproc = Element("qtimlvconverter", "palm_preproc")
    palm_preproc.set("mode", "image-batch-non-cumulative")

    # Queue before palm inference.
    q_palm_infer = Element("queue", "q_palm_infer")

    # Palm inference element.
    palm_inf = Element("qtimltflite", "palm_inf")
    palm_inf.set("delegate", "gpu")
    palm_inf.set("model", model_base_path + "/models/palm_detection_full.tflite")

    # Queue before palm postprocess.
    q_palm_post = Element("queue", "q_palm_post")

    # Palm postprocess element.
    palm_post = Element("qtimlpostprocess", "palm_post")
    palm_post.set("module", "palmd")
    palm_post.set("results", 2)
    palm_post.set("labels", model_base_path + "/labels/palmd_labels.json")
    palm_post.set("settings", model_base_path + "/labels/palmd_settings.json")

    # Palm text metadata filter.
    palm_mlf = TextFilter()

    # Queue for palm metadata.
    q_palm_meta = Element("queue", "q_palm_meta")

    # Palm metadata mux element.
    metamux_palm = Element("qtimetamux", "metamux_palm")

    # Palm ROI transform element.
    palm_roi_transform = Element("qtimetatransform", "palm_roi_transform")
    palm_roi_transform.set("module", "roi-palmd")

    # Tee after palm ROI transform.
    split_after_palm = Element("tee", "split_after_palm")

    # Queue for video path into final metamux.
    q_video_final = Element("queue", "q_video_final")

    # Queue before hand preprocessor.
    q_hand_pre = Element("queue", "q_hand_pre")

    # Hand preprocessor element.
    hand_preproc = Element("qtimlvconverter", "hand_preproc")
    hand_preproc.set("mode", "roi-batch-cumulative")

    # Queue before hand inference.
    q_hand_infer = Element("queue", "q_hand_infer")

    # Hand inference element.
    hand_inf = Element("qtimltflite", "hand_inf")
    hand_inf.set("delegate", "xnnpack")
    hand_inf.set("model", model_base_path + "/models/hand_landmark_full.tflite")

    # Queue before hand postprocess.
    q_hand_post = Element("queue", "q_hand_post")

    # Hand postprocess element.
    hand_post = Element("qtimlpostprocess", "hand_post")
    hand_post.set("module", "hlandmark")
    hand_post.set("results", 2)
    hand_post.set("labels", model_base_path + "/labels/hlandmarks.json")
    hand_post.set("settings", model_base_path + "/labels/hlandmark_settings.json")

    # Hand text metadata filter.
    hand_mlf = TextFilter()

    # Queue for hand metadata.
    q_hand_meta = Element("queue", "q_hand_meta")

    # Final metadata mux element.
    metamux_final = Element("qtimetamux", "metamux_final")

    # Final tee split element.
    final_split = Element("tee", "final_split")

    # Queue for display branch.
    q_display = Element("queue", "q_display")

    # Qualcomm metadata overlay element.
    overlay = Element("qtivoverlay", "overlay")

    # Transform before cairooverlay.
    to_cairo = Element("qtivtransform", "to_cairo")

    # BGRA caps filter for cairooverlay.
    cairofilter = VideoFilter().format("BGRA").resolution(FRAME_WIDTH, FRAME_HEIGHT).framerate(FRAME_FPS)

    # Cairo overlay element.
    video_region_canvas = Element("cairooverlay", "video_region_canvas")
    video_region_canvas.connect_signal("draw", on_cairo_draw)

    # Transform after cairooverlay.
    to_display = Element("qtivtransform", "to_display")

    # Display sink element.
    display = Element("waylandsink", "display")
    display.set("sync", False)
    display.set("fullscreen", True)

    # Queue before metadata parser.
    q_meta_parse = Element("queue", "q_meta_parse")

    # Metadata parser element.
    meta_parser = Element("qtimlmetaparser", "meta_parser")
    meta_parser.set("module", "json")

    # Metadata appsink element.
    meta_sink = AppSink("meta_sink")
    meta_sink.set("sync", False)
    meta_sink.set("max-buffers", 1)
    meta_sink.set("drop", True)

    def on_meta_sample(buffer):
        if buffer is None:
            return
        data = buffer.data()
        if not data:
            return
        text = bytes(data).decode("utf-8", errors="ignore").replace("\x00", "").strip()
        hands = parse_hands(text)
        update_state_from_hands(hands)

    meta_sink.set_buffer_consumer(on_meta_sample)

    pipeline = (
        Pipeline("two-hand-fingertip-video-region")
        .add(source)
        .add(transform)
        .add_stream_filter("videostream", videostream)
        .add(split)
        .add(q_video_palm)
        .add(q_palm_pre)
        .add(palm_preproc)
        .add(q_palm_infer)
        .add(palm_inf)
        .add(q_palm_post)
        .add(palm_post)
        .add_stream_filter("palm_mlf", palm_mlf)
        .add(q_palm_meta)
        .add(metamux_palm)
        .add(palm_roi_transform)
        .add(split_after_palm)
        .add(q_video_final)
        .add(q_hand_pre)
        .add(hand_preproc)
        .add(q_hand_infer)
        .add(hand_inf)
        .add(q_hand_post)
        .add(hand_post)
        .add_stream_filter("hand_mlf", hand_mlf)
        .add(q_hand_meta)
        .add(metamux_final)
        .add(final_split)
        .add(q_display)
        .add(overlay)
        .add(to_cairo)
        .add_stream_filter("cairofilter", cairofilter)
        .add(video_region_canvas)
        .add(to_display)
        .add(display)
        .add(q_meta_parse)
        .add(meta_parser)
        .add(meta_sink)
        .link("source", "transform", "videostream", "split")
        .link("split", "q_video_palm", "metamux_palm")
        .link("split", "q_palm_pre", "palm_preproc", "q_palm_infer", "palm_inf",
              "q_palm_post", "palm_post", "palm_mlf", "q_palm_meta", "metamux_palm")
        .link("metamux_palm", "palm_roi_transform", "split_after_palm")
        .link("split_after_palm", "q_video_final", "metamux_final")
        .link("split_after_palm", "q_hand_pre", "hand_preproc", "q_hand_infer", "hand_inf",
              "q_hand_post", "hand_post", "hand_mlf", "q_hand_meta", "metamux_final")
        .link("metamux_final", "final_split")
        .link("final_split", "q_display", "overlay", "to_cairo", "cairofilter", "video_region_canvas", "to_display", "display")
        .link("final_split", "q_meta_parse", "meta_parser", "meta_sink")
    )

    print("[INFO] Starting two-hand fingertip video region application...", flush=True)
    print(f"[INFO] Camera:     {CAMERA_DEVICE}", flush=True)
    print(f"[INFO] Video path: {model_base_path + '/media/ai_demo_sample.mp4'}", flush=True)

    try:
        clip_pipeline.start()
        pipeline.execute()
    finally:
        clip_pipeline.stop()


def main() -> None:
    from qimsdk import ImsdkGstLogMode, ImsdkLogLevel, SetImsdkGstLogMode, SetImsdkLogLevel
    SetImsdkGstLogMode(ImsdkGstLogMode.ImsdkLog)
    SetImsdkLogLevel(ImsdkLogLevel.Debug)

    create_and_execute_pipeline()

if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\n[INFO] Interrupted by user.", flush=True)
    except Exception as exc:
        print(f"[ERROR] Pipeline failed: {exc}", flush=True)
        traceback.print_exc()
        raise SystemExit(1)
