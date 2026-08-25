#!/usr/bin/env python3
# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""
IMSDK Split-Screen Wrist-Steering Car Game
==========================================
"""

import argparse
import json
import math
import os
import random
import re
import sys
import threading
import time
import traceback

import gi
gi.require_version("Gst", "1.0")
from gi.repository import Gst

from qimsdk import AppSink, Element, Pipeline, TextFilter, VideoFilter

HOME_PATH = os.environ["HOME"]


class HelpOnErrorArgumentParser(argparse.ArgumentParser):
    def error(self, message):
        self.print_help(sys.stderr)
        sys.stderr.write(f"\n{self.prog}: error: {message}\n")
        sys.exit(2)


parser = HelpOnErrorArgumentParser(
    description="QIMSDK reference app",
    formatter_class=argparse.ArgumentDefaultsHelpFormatter,
)
parser.add_argument("--input-config",
                    default="/dev/video0",
                    help="Input source configuration (camera number, device, or file path)")
parser.add_argument("--model-base-path",
                    default=HOME_PATH + "/Downloads/qimsdk_samples",
                    help="Base model/label path")
args = parser.parse_args()

#  Example pipeline:
#
#    v4l2src -> qtivtransform -> tee
#      tee. -> stage-1 palm detection -> qtimetamux -> qtimetatransform
#      qtimetatransform -> tee -> stage-2 hand landmarks -> qtimetamux
#      qtimetamux -> tee
#        -> qtivoverlay -> qtivtransform -> [videofilter:BGRA] -> cairooverlay -> waylandsink
#        -> qtimlmetaparser(json) -> appsink(metadata callback)
#
#  Wrist metadata drives steering physics while Cairo renders the split
#  game scene and HUD on the display branch.


# =============================================================================
# Configuration
# =============================================================================

CAMERA_DEVICE = args.input_config

FRAME_WIDTH  = 1920
FRAME_HEIGHT = 1080
FRAME_FPS    = 30

model_base_path = args.model_base_path

WRIST_ID          = 0
WRIST_TIMEOUT_SEC = 0.70

WRIST_LINE_WIDTH = 10.0
WRIST_DOT_RADIUS = 13.0

MAX_STEERING_ANGLE_DEG = float(os.environ.get("MAX_STEERING_ANGLE_DEG", "45"))
STEERING_INVERT        = os.environ.get("STEERING_INVERT", "0") == "1"
GAME_AUTO_RESTART_SEC  = float(os.environ.get("GAME_AUTO_RESTART_SEC", "2.5"))
STEERING_WHEEL_SIDE    = os.environ.get("STEERING_WHEEL_SIDE", "right").strip().lower()


# =============================================================================
# Shared state
# =============================================================================

LOCK        = threading.Lock()
WRISTS      = None
WRISTS_TIME = 0.0

DISPLAY_W = FRAME_WIDTH
DISPLAY_H = FRAME_HEIGHT

META_BUFFER_COUNT = 0

GAME = {
    "initialized": False,
    "rng": random.Random(7),
}


# =============================================================================
# Basic helpers
# =============================================================================

def f(value):
    """Attempt to cast *value* to float; return None if the conversion fails."""
    try:
        if value is None:
            return None
        return float(value)
    except Exception:
        return None


def clamp(value, low, high):
    """Return *value* clamped to the closed interval [*low*, *high*]."""
    return max(low, min(high, value))


def norm(key):
    """Normalise a metadata key to lowercase, hyphen-separated form for consistent lookup."""
    return str(key).strip().lower().replace("_", "-").replace(" ", "-")


def left_viewport():
    """Return the (x, y, width, height) rectangle of the left camera panel in display pixels."""
    w = max(1, int(DISPLAY_W))
    h = max(1, int(DISPLAY_H))
    return 0, 0, w // 2, h


def game_viewport():
    """Return the (x, y, width, height) rectangle of the right game panel in display pixels."""
    w = max(1, int(DISPLAY_W))
    h = max(1, int(DISPLAY_H))
    half = w // 2
    return half, 0, w - half, h


# =============================================================================
# Metadata parsing
# =============================================================================

def xy(obj):
    """Extract a float (x, y) tuple from common JSON point representations, or return None."""
    if isinstance(obj, dict):
        for x_key, y_key in (
            ("x", "y"), ("X", "Y"), ("px", "py"),
            ("u", "v"), ("cx", "cy"), ("left", "top"),
        ):
            if x_key in obj and y_key in obj:
                x = f(obj.get(x_key))
                y = f(obj.get(y_key))
                if x is not None and y is not None:
                    return x, y
        for key in ("point", "position", "location", "coord",
                    "coordinate", "coordinates", "landmark", "keypoint"):
            if key in obj:
                p = xy(obj.get(key))
                if p is not None:
                    return p
    elif isinstance(obj, (list, tuple)) and len(obj) >= 2:
        x = f(obj[0])
        y = f(obj[1])
        if x is not None and y is not None:
            return x, y
    return None


def points_from_list(value):
    """Convert a raw JSON list into a list of (x, y) tuples if it encodes landmark coordinates."""
    if not isinstance(value, list):
        return []
    pts = []
    for item in value:
        p = xy(item)
        if p is not None:
            pts.append(p)
    if len(pts) >= 2:
        return pts
    nums = [n for n in (f(item) for item in value) if n is not None]
    if len(nums) >= 42:
        step = 3 if len(nums) >= 63 else 2
        pts2 = [(nums[i], nums[i + 1]) for i in range(0, len(nums) - 1, step)]
        if len(pts2) >= 2:
            return pts2
    return []


def collect_hand_point_lists(obj, out):
    """Recursively traverse a JSON object and append landmark point-lists to *out*."""
    if isinstance(obj, dict):
        for key, value in obj.items():
            nk = norm(key)
            if any(s in nk for s in ("landmark", "keypoint", "key-point", "points", "hand-landmarks")):
                pts = points_from_list(value)
                if len(pts) >= 2:
                    out.append(pts)
            collect_hand_point_lists(value, out)
    elif isinstance(obj, list):
        pts = points_from_list(obj)
        if len(pts) >= 21:
            out.append(pts)
        for item in obj:
            collect_hand_point_lists(item, out)


def json_loads_maybe_many(text):
    """Parse *text* as one JSON object or, if that fails, as multiple newline-delimited objects."""
    text = text.strip()
    if not text:
        return []
    try:
        return [json.loads(text)]
    except Exception:
        pass
    objs = []
    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            objs.append(json.loads(line))
        except Exception:
            continue
    return objs


def fallback_regex_wrists(text):
    """Extract wrist (x, y) pairs from *text* using regex when structured JSON parsing fails."""
    wrists = []
    pattern = r"wrist.{0,160}?x[^0-9.+-]*([+-]?\d+(?:\.\d+)?).{0,80}?y[^0-9.+-]*([+-]?\d+(?:\.\d+)?)"
    for match in re.finditer(pattern, text, re.I | re.S):
        x = f(match.group(1))
        y = f(match.group(2))
        if x is not None and y is not None:
            wrists.append((x, y))
    return wrists


def to_pixels(point):
    """Convert a landmark point (normalised 0-1 or raw camera pixels) to display pixel coordinates."""
    x = f(point[0])
    y = f(point[1])
    if x is None or y is None:
        return None
    if -0.25 <= x <= 1.25 and -0.25 <= y <= 1.25:
        x *= FRAME_WIDTH
        y *= FRAME_HEIGHT
    x *= float(DISPLAY_W) / float(FRAME_WIDTH)
    y *= float(DISPLAY_H) / float(FRAME_HEIGHT)
    return (
        int(clamp(round(x), 0, DISPLAY_W - 1)),
        int(clamp(round(y), 0, DISPLAY_H - 1)),
    )


def extract_two_wrists_from_text(text):
    """Parse raw inference metadata and return the (left_wrist, right_wrist) pixel pair, or None."""
    objs = json_loads_maybe_many(text)
    hand_lists = []
    for obj in objs:
        collect_hand_point_lists(obj, hand_lists)
    wrists = []
    for pts in hand_lists:
        if len(pts) > WRIST_ID:
            p = to_pixels(pts[WRIST_ID])
            if p is not None:
                wrists.append(p)
    unique = []
    for p in wrists:
        if all(abs(p[0] - q[0]) > 4 or abs(p[1] - q[1]) > 4 for q in unique):
            unique.append(p)
    wrists = unique
    if len(wrists) < 2:
        for p in fallback_regex_wrists(text):
            pp = to_pixels(p)
            if pp is not None:
                wrists.append(pp)
    if len(wrists) < 2:
        return None
    best_pair  = None
    best_score = -1
    for i1 in range(len(wrists)):
        for i2 in range(i1 + 1, len(wrists)):
            score = abs(wrists[i1][0] - wrists[i2][0])
            if score > best_score:
                best_score = score
                best_pair  = (wrists[i1], wrists[i2])
    if best_pair is None:
        return None
    a, b = best_pair
    return (a, b) if a[0] <= b[0] else (b, a)


# =============================================================================
# GStreamer callbacks
# =============================================================================

def on_sample(buffer):
    """AppSink callback: parse each metadata buffer and update the shared wrist state."""
    global WRISTS, WRISTS_TIME
    global META_BUFFER_COUNT

    data = buffer.data()
    if not data:
        return
    text = bytes(data).decode("utf-8", "ignore")

    now = time.time()
    META_BUFFER_COUNT += 1

    pair = extract_two_wrists_from_text(text)

    if pair is not None:
        with LOCK:
            WRISTS      = pair
            WRISTS_TIME = now
    if META_BUFFER_COUNT % 60 == 1:
        if pair is not None:
            print(f"[WRIST] parsed wrists: {pair[0]} -> {pair[1]}", flush=True)
        else:
            print(f"[WRIST] metadata received (#{META_BUFFER_COUNT}), but two wrists were not parsed. text_len={len(text)}", flush=True)

    return


def on_cairo_caps_changed(overlay, caps):
    """Cairooverlay caps-changed callback: refresh the global display dimensions."""
    global DISPLAY_W, DISPLAY_H
    try:
        structure  = caps.get_structure(0)
        DISPLAY_W = int(structure.get_value("width"))
        DISPLAY_H = int(structure.get_value("height"))
        print(f"[WRIST] cairo surface: {DISPLAY_W}x{DISPLAY_H}", flush=True)
    except Exception:
        DISPLAY_W = FRAME_WIDTH
        DISPLAY_H = FRAME_HEIGHT


# =============================================================================
# Game logic
# =============================================================================

def reset_game(now=None):
    """Reset all game state to initial values, preserving the existing RNG instance."""
    if now is None:
        now = time.time()
    rng = GAME.get("rng") or random.Random(7)
    GAME.clear()
    GAME.update({
        "initialized":  True,
        "last_time":    now,
        "car_x":        0.5,
        "car_y":        0.84,
        "steer_smooth": 0.0,
        "road_offset":  0.0,
        "score":        0.0,
        "alive":        True,
        "crash_time":   0.0,
        "obstacles":    [],
        "spawn_timer":  0.0,
        "rng":          rng,
    })


def current_wrist_pair():
    """Return the most recent wrist pair if it arrived within WRIST_TIMEOUT_SEC, else None."""
    with LOCK:
        pair = WRISTS
        age  = time.time() - WRISTS_TIME
    if pair is not None and age <= WRIST_TIMEOUT_SEC:
        return pair
    return None


def steering_from_pair(pair):
    """Compute a normalised steering value in [-1, 1] from the wrist-to-wrist tilt angle."""
    if pair is None:
        return None
    (x1, y1), (x2, y2) = pair
    dx = float(x2 - x1)
    dy = float(y2 - y1)
    if abs(dx) < 1.0 and abs(dy) < 1.0:
        return None
    angle_deg = math.degrees(math.atan2(dy, dx))
    steer = clamp(angle_deg / MAX_STEERING_ANGLE_DEG, -1.0, 1.0)
    if STEERING_INVERT:
        steer = -steer
    return steer


def spawn_obstacle(w, h):
    """Create and return a new obstacle dict positioned in a random lane at the top of the road."""
    road_left = 0.12 * w
    road_w    = 0.76 * w
    lane_w    = road_w / 3.0
    lane      = GAME["rng"].randint(0, 2)
    return {
        "x": road_left + lane_w * (lane + 0.5),
        "y": -0.13 * h,
        "w": 0.105 * w,
        "h": 0.115 * h,
    }


def rects_overlap(a, b):
    """Return True if two axis-aligned rectangles, each described by an (x, y, w, h) dict, overlap."""
    return not (
        a["x"] + a["w"] < b["x"] or
        b["x"] + b["w"] < a["x"] or
        a["y"] + a["h"] < b["y"] or
        b["y"] + b["h"] < a["y"]
    )


def update_game(pair):
    """Advance the game simulation by one frame: steer the car, scroll the road, manage obstacles, and detect collisions."""
    now = time.time()
    if not GAME.get("initialized"):
        reset_game(now)
    dt = clamp(now - float(GAME.get("last_time", now)), 0.0, 0.060)
    GAME["last_time"] = now
    _, _, w, h = game_viewport()
    w = max(1, w)
    h = max(1, h)
    if not GAME["alive"]:
        if now - GAME["crash_time"] >= GAME_AUTO_RESTART_SEC:
            reset_game(now)
        return
    raw_steer = steering_from_pair(pair) or 0.0
    GAME["steer_smooth"] = 0.82 * GAME["steer_smooth"] + 0.18 * raw_steer
    steer      = GAME["steer_smooth"]
    road_left  = 0.12 * w
    road_w     = 0.76 * w
    road_right = road_left + road_w
    car_w      = 0.105 * w
    car_h      = 0.125 * h
    score      = float(GAME["score"])
    speed_px   = (0.38 * h) + min(score * 0.008 * h, 0.32 * h)
    GAME["car_x"] += steer * 0.54 * w * dt / float(w)
    GAME["car_x"]  = clamp(
        GAME["car_x"],
        (road_left  + car_w * 0.55) / float(w),
        (road_right - car_w * 0.55) / float(w),
    )
    GAME["road_offset"] = (GAME["road_offset"] + speed_px * dt) % max(1.0, 0.11 * h)
    GAME["score"]      += dt * 10.0
    GAME["spawn_timer"] -= dt
    if GAME["spawn_timer"] <= 0.0:
        GAME["obstacles"].append(spawn_obstacle(w, h))
        GAME["spawn_timer"] = max(0.65, 1.35 - GAME["score"] * 0.006)
    for obs in GAME["obstacles"]:
        obs["y"] += speed_px * dt
    GAME["obstacles"] = [obs for obs in GAME["obstacles"] if obs["y"] < h + obs["h"]]
    car_rect = {
        "x": GAME["car_x"] * w - car_w * 0.45,
        "y": GAME["car_y"] * h - car_h * 0.50,
        "w": car_w * 0.90,
        "h": car_h * 0.90,
    }
    for obs in GAME["obstacles"]:
        if rects_overlap(car_rect, obs):
            GAME["alive"]      = False
            GAME["crash_time"] = now
            break


# =============================================================================
# Drawing helpers
# =============================================================================

def rounded_rect(cr, x, y, w, h, radius):
    """Append a rounded-corner rectangle path to the given Cairo context without stroking or filling."""
    r = min(radius, w / 2.0, h / 2.0)
    cr.new_sub_path()
    cr.arc(x + w - r, y + r,     r, -math.pi / 2.0, 0.0)
    cr.arc(x + w - r, y + h - r, r,  0.0,            math.pi / 2.0)
    cr.arc(x + r,     y + h - r, r,  math.pi / 2.0,  math.pi)
    cr.arc(x + r,     y + r,     r,  math.pi,         3.0 * math.pi / 2.0)
    cr.close_path()


def draw_centered_text(cr, text, x, y, size, r, g, b, a=1.0, bold=False):
    """Render *text* centred on (*x*, *y*) using the specified font size and RGBA colour."""
    cr.save()
    cr.select_font_face("Sans", 0, 1 if bold else 0)
    cr.set_font_size(size)
    ext = cr.text_extents(text)
    cr.set_source_rgba(r, g, b, a)
    cr.move_to(x - ext.width / 2.0 - ext.x_bearing, y - ext.height / 2.0 - ext.y_bearing)
    cr.show_text(text)
    cr.restore()


def draw_split_frame(cr):
    """Draw the static split-screen chrome: dark game panel background, cyan divider, and panel labels."""
    gx, gy, gw, gh = game_viewport()
    cr.set_source_rgba(0.0, 0.0, 0.0, 0.96)
    cr.rectangle(gx, gy, gw, gh)
    cr.fill()
    cr.set_source_rgba(0.0, 0.85, 1.0, 1.0)
    cr.rectangle(gx - 4, 0, 8, DISPLAY_H)
    cr.fill()
    cr.set_source_rgba(0.0, 0.0, 0.0, 0.62)
    rounded_rect(cr, 18, 18, 250, 48, 12)
    cr.fill()
    cr.select_font_face("Sans", 0, 1)
    cr.set_font_size(27)
    cr.set_source_rgba(1.0, 1.0, 1.0, 1.0)
    cr.move_to(36, 51)
    cr.show_text("LIVE VIDEO")
    cr.set_source_rgba(0.0, 0.0, 0.0, 0.62)
    rounded_rect(cr, gx + 18, 18, 250, 48, 12)
    cr.fill()
    cr.set_source_rgba(1.0, 1.0, 1.0, 1.0)
    cr.move_to(gx + 36, 51)
    cr.show_text("CAR GAME")


def draw_game(cr, pair):
    """Draw the complete game scene including the road, lane markings, obstacles, player car, HUD, and crash overlay."""
    gx, gy, w, h = game_viewport()
    w = max(1, w)
    h = max(1, h)
    cr.save()
    cr.rectangle(gx, gy, w, h)
    cr.clip()
    cr.translate(gx, gy)
    road_left  = 0.12 * w
    road_w     = 0.76 * w
    road_right = road_left + road_w
    lane_w     = road_w / 3.0
    cr.set_source_rgba(0.02, 0.03, 0.05, 1.0)
    cr.rectangle(0, 0, w, h)
    cr.fill()
    cr.set_source_rgba(0.13, 0.13, 0.15, 1.0)
    cr.rectangle(road_left, 0, road_w, h)
    cr.fill()
    cr.set_source_rgba(0.95, 0.95, 0.95, 1.0)
    cr.rectangle(road_left - 5, 0, 5, h)
    cr.fill()
    cr.rectangle(road_right, 0, 5, h)
    cr.fill()
    cr.set_source_rgba(1.0, 0.93, 0.22, 1.0)
    dash_h  = 0.065 * h
    gap     = 0.055 * h
    period  = dash_h + gap
    offset  = GAME.get("road_offset", 0.0)
    for lane_x in (road_left + lane_w, road_left + 2.0 * lane_w):
        y = -period + offset
        while y < h + period:
            rounded_rect(cr, lane_x - 4, y, 8, dash_h, 3)
            cr.fill()
            y += period
    for obs in GAME.get("obstacles", []):
        rounded_rect(cr, obs["x"] - obs["w"] / 2.0, obs["y"], obs["w"], obs["h"], 10)
        cr.set_source_rgba(0.85, 0.08, 0.05, 1.0)
        cr.fill_preserve()
        cr.set_source_rgba(0.25, 0.02, 0.02, 1.0)
        cr.set_line_width(3)
        cr.stroke()
    car_w     = 0.118 * w
    car_h     = 0.135 * h
    cx        = GAME.get("car_x", 0.5) * w
    cy        = GAME.get("car_y", 0.84) * h
    steer     = GAME.get("steer_smooth", 0.0)
    car_angle = steer * math.radians(20.0)
    cr.save()
    cr.translate(cx + car_w * 0.08, cy + car_h * 0.08)
    cr.rotate(car_angle)
    rounded_rect(cr, -car_w * 0.50, -car_h * 0.48, car_w, car_h, 16)
    cr.set_source_rgba(0.0, 0.0, 0.0, 0.36)
    cr.fill()
    cr.restore()
    cr.save()
    cr.translate(cx, cy)
    cr.rotate(car_angle)
    rounded_rect(cr, -car_w / 2.0, -car_h / 2.0, car_w, car_h, 18)
    cr.set_source_rgba(0.02, 0.23, 0.95, 1.0)
    cr.fill_preserve()
    cr.set_source_rgba(0.00, 0.05, 0.22, 1.0)
    cr.set_line_width(4)
    cr.stroke()
    rounded_rect(cr, -car_w * 0.10, -car_h * 0.47, car_w * 0.20, car_h * 0.94, 7)
    cr.set_source_rgba(1.0, 1.0, 1.0, 0.92)
    cr.fill()
    rounded_rect(cr, -car_w * 0.035, -car_h * 0.47, car_w * 0.07, car_h * 0.94, 4)
    cr.set_source_rgba(0.03, 0.15, 0.75, 0.95)
    cr.fill()
    rounded_rect(cr, -car_w * 0.32, -car_h * 0.28, car_w * 0.64, car_h * 0.22, 7)
    cr.set_source_rgba(0.70, 0.90, 1.0, 0.96)
    cr.fill_preserve()
    cr.set_source_rgba(0.03, 0.16, 0.32, 0.65)
    cr.set_line_width(2)
    cr.stroke()
    rounded_rect(cr, -car_w * 0.27, car_h * 0.16, car_w * 0.54, car_h * 0.18, 6)
    cr.set_source_rgba(0.42, 0.70, 0.94, 0.88)
    cr.fill()
    brand_text = "Qualcomm"
    cr.save()
    cr.select_font_face("Sans", 0, 1)
    brand_font_size = max(10.0, car_w * 0.15)
    cr.set_font_size(brand_font_size)
    brand_ext  = cr.text_extents(brand_text)
    max_text_w = car_w * 0.78
    if brand_ext.width > max_text_w and brand_ext.width > 0:
        brand_font_size = max(9.0, brand_font_size * max_text_w / brand_ext.width)
        cr.set_font_size(brand_font_size)
        brand_ext = cr.text_extents(brand_text)
    brand_x = -brand_ext.width / 2.0 - brand_ext.x_bearing
    brand_y = car_h * 0.035 - brand_ext.height / 2.0 - brand_ext.y_bearing
    cr.move_to(brand_x, brand_y)
    cr.text_path(brand_text)
    cr.set_source_rgba(0.0, 0.0, 0.0, 0.70)
    cr.set_line_width(max(1.2, brand_font_size * 0.12))
    cr.stroke_preserve()
    cr.set_source_rgba(1.0, 1.0, 1.0, 1.0)
    cr.fill()
    cr.restore()
    cr.set_source_rgba(1.0, 0.96, 0.45, 1.0)
    rounded_rect(cr, -car_w * 0.39, -car_h * 0.48, car_w * 0.18, car_h * 0.055, 4)
    cr.fill()
    rounded_rect(cr, car_w * 0.21, -car_h * 0.48, car_w * 0.18, car_h * 0.055, 4)
    cr.fill()
    cr.set_source_rgba(1.0, 0.05, 0.04, 1.0)
    rounded_rect(cr, -car_w * 0.38, car_h * 0.43, car_w * 0.16, car_h * 0.045, 3)
    cr.fill()
    rounded_rect(cr, car_w * 0.22, car_h * 0.43, car_w * 0.16, car_h * 0.045, 3)
    cr.fill()
    cr.set_source_rgba(0.015, 0.015, 0.018, 1.0)
    wheel_w = car_w * 0.18
    wheel_h = car_h * 0.24
    for wx in (-car_w * 0.58, car_w * 0.40):
        for wy in (-car_h * 0.34, car_h * 0.18):
            rounded_rect(cr, wx, wy, wheel_w, wheel_h, 5)
            cr.fill()
    cr.restore()
    cr.set_source_rgba(0.0, 0.0, 0.0, 0.78)
    cr.rectangle(0, 0, w, 0.095 * h)
    cr.fill()
    cr.select_font_face("Sans", 0, 1)
    cr.set_font_size(25)
    cr.set_source_rgba(1.0, 1.0, 1.0, 1.0)
    cr.move_to(24, 39)
    cr.show_text(f"Score: {int(GAME.get('score', 0))}")
    cr.move_to(185, 39)
    cr.show_text(f"Steering: {GAME.get('steer_smooth', 0.0):+.2f}")
    if pair is None:
        cr.set_source_rgba(1.0, 0.25, 0.20, 1.0)
        cr.move_to(420, 39)
        cr.show_text("Show both wrists")
    else:
        cr.set_source_rgba(0.2, 1.0, 0.2, 1.0)
        cr.move_to(420, 39)
        cr.show_text("Active")
    if not GAME.get("alive", True):
        cr.set_source_rgba(0.0, 0.0, 0.0, 0.65)
        cr.rectangle(0, 0, w, h)
        cr.fill()
        draw_centered_text(cr, "CRASH!",          w / 2.0, h * 0.44, 64, 1.0, 0.25, 0.20, 1.0, True)
        draw_centered_text(cr, "Auto restart...", w / 2.0, h * 0.53, 32, 1.0, 1.0,  1.0,  1.0, False)
    cr.restore()


def draw_wrist_line(cr, pair):
    """Draw a green connecting line and circular dot markers at each detected wrist position."""
    if pair is None:
        return
    lx, ly, lw, lh = left_viewport()
    (x1, y1), (x2, y2) = pair
    cr.save()
    cr.rectangle(lx, ly, lw, lh)
    cr.clip()
    cr.set_source_rgba(0.0, 1.0, 0.0, 1.0)
    cr.set_line_width(WRIST_LINE_WIDTH)
    cr.set_line_cap(1)
    cr.move_to(float(x1), float(y1))
    cr.line_to(float(x2), float(y2))
    cr.stroke()
    cr.arc(float(x1), float(y1), WRIST_DOT_RADIUS, 0, 2.0 * math.pi)
    cr.fill()
    cr.arc(float(x2), float(y2), WRIST_DOT_RADIUS, 0, 2.0 * math.pi)
    cr.fill()
    cr.restore()


def draw_steering_wheel(cr, pair):
    """Draw the animated steering wheel widget: centred between the wrists when active, or in the game corner when idle."""
    gx, gy, gw, gh = game_viewport()
    lx, ly, lw, lh = left_viewport()
    gw = max(1, gw)
    gh = max(1, gh)
    lw = max(1, lw)
    lh = max(1, lh)
    steer  = clamp(float(GAME.get("steer_smooth", 0.0)), -1.0, 1.0)
    active = pair is not None
    alpha  = 1.0 if active else 0.72
    if active:
        (x1, y1), (x2, y2) = pair
        cx            = (float(x1) + float(x2)) / 2.0
        cy            = (float(y1) + float(y2)) / 2.0
        hand_distance = math.hypot(float(x2) - float(x1), float(y2) - float(y1))
        radius        = clamp(hand_distance * 0.50, min(lw, lh) * 0.085, min(lw, lh) * 0.225)
        clip_x, clip_y, clip_w, clip_h = lx, ly, lw, lh
        show_label = False
    else:
        side   = STEERING_WHEEL_SIDE
        radius = min(gw, gh) * 0.145
        margin = min(gw, gh) * 0.040
        cx     = (gx + margin + radius) if side == "left" else (gx + gw - margin - radius)
        cy     = gy + gh - margin - radius
        cx     = clamp(cx, gx + radius + 12, gx + gw - radius - 12)
        cy     = clamp(cy, gy + radius + 12, gy + gh - radius - 12)
        clip_x, clip_y, clip_w, clip_h = gx, gy, gw, gh
        show_label = True
    cr.save()
    cr.rectangle(clip_x, clip_y, clip_w, clip_h)
    cr.clip()
    plate_r = radius * 1.18
    cr.set_source_rgba(0.0, 0.0, 0.0, 0.58 if active else 0.72)
    cr.arc(cx, cy, plate_r, 0, 2.0 * math.pi)
    cr.fill_preserve()
    cr.set_line_width(max(4.0, radius * 0.030))
    cr.set_source_rgba(1.0, 0.78, 0.05, alpha)
    cr.stroke()
    if show_label:
        cr.select_font_face("Sans", 0, 1)
        cr.set_font_size(max(22.0, radius * 0.17))
        label = "STEERING"
        ext   = cr.text_extents(label)
        cr.set_source_rgba(1.0, 1.0, 1.0, 1.0)
        cr.move_to(cx - ext.width / 2.0, cy - radius * 1.33)
        cr.show_text(label)
    cr.translate(cx, cy)
    cr.rotate(steer * math.radians(120.0))
    cr.set_line_cap(1)
    cr.set_line_width(radius * 0.18)
    cr.set_source_rgba(0.0, 0.0, 0.0, 0.95)
    cr.arc(0, 0, radius, 0, 2.0 * math.pi)
    cr.stroke()
    cr.set_line_width(radius * 0.105)
    cr.set_source_rgba(1.0, 0.72, 0.02, alpha)
    cr.arc(0, 0, radius, 0, 2.0 * math.pi)
    cr.stroke()
    cr.set_line_width(radius * 0.035)
    cr.set_source_rgba(0.0, 0.95, 1.0, alpha)
    cr.arc(0, 0, radius * 0.84, 0, 2.0 * math.pi)
    cr.stroke()
    cr.set_source_rgba(0.02, 0.03, 0.04, 1.0)
    cr.arc(0, 0, radius * 0.25, 0, 2.0 * math.pi)
    cr.fill_preserve()
    cr.set_line_width(radius * 0.035)
    cr.set_source_rgba(1.0, 0.72, 0.02, alpha)
    cr.stroke()
    cr.set_line_width(radius * 0.075)
    cr.set_source_rgba(0.0, 0.92, 1.0, alpha)
    for deg in (-90, 30, 150):
        angle = math.radians(deg)
        cr.move_to(math.cos(angle) * radius * 0.27, math.sin(angle) * radius * 0.27)
        cr.line_to(math.cos(angle) * radius * 0.78, math.sin(angle) * radius * 0.78)
        cr.stroke()
    cr.set_source_rgba(1.0, 0.05, 0.02, 1.0)
    cr.arc(0, -radius * 0.88, radius * 0.085, 0, 2.0 * math.pi)
    cr.fill()
    cr.restore()
    cr.save()
    cr.rectangle(clip_x, clip_y, clip_w, clip_h)
    cr.clip()
    pct  = int(round(steer * 100.0))
    text = f"{pct:+d}%"
    cr.select_font_face("Sans", 0, 1)
    cr.set_font_size(max(20.0, radius * 0.16))
    ext = cr.text_extents(text)
    if active:
        cr.set_source_rgba(0.2, 1.0, 0.2, 1.0)
    else:
        cr.set_source_rgba(1.0, 0.25, 0.15, 1.0)
    cr.move_to(cx - ext.width / 2.0, cy + radius * 1.32)
    cr.show_text(text)
    cr.restore()


def on_cairo_draw(cr, timestamp, duration):
    """Cairooverlay draw callback: advance the game state and composite all UI layers onto the frame."""
    pair = current_wrist_pair()
    update_game(pair)
    draw_split_frame(cr)
    draw_game(cr, pair)
    draw_steering_wheel(cr, pair)
    draw_wrist_line(cr, pair)

def make_queue(name: str) -> Element:
    q = Element("queue", name)
    q.set("leaky", 2)
    q.set("max-size-buffers", 2)
    q.set("max-size-bytes", 0)
    q.set("max-size-time", 0)
    return q


# =============================================================================
# Pipeline construction and run
# =============================================================================

def create_and_execute_pipeline(device: str = CAMERA_DEVICE) -> None:
    """Construct and execute the full two-stage hand-tracking game pipeline using explicit Element objects."""

    # -------------------------------------------------------------------------
    # Camera source
    # -------------------------------------------------------------------------
    source = Element("v4l2src", "source")
    source.set("device", device)

    # Video transform stage.
    camera_transform = Element("qtivtransform", "camera_transform")
    camera_transform.set("flip-horizontal", True)

    videostream = (
        VideoFilter()
        .format("NV12")
        .resolution(FRAME_WIDTH, FRAME_HEIGHT)
        .framerate(FRAME_FPS)
    )

    # Stream split (tee).
    split = Element("tee", "split")

    # -------------------------------------------------------------------------
    # Stage 1 — Palm detection branch
    # -------------------------------------------------------------------------
    q_palm_pre = make_queue("q_palm_pre")

    # ML preprocessor/converter.
    palm_preproc = Element("qtimlvconverter", "palm_preproc")
    palm_preproc.set("mode", "image-batch-non-cumulative")

    # Queue for branch decoupling/backpressure.
    q_palm_infer = make_queue("q_palm_infer")

    # TFLite inference stage.
    palm_inf = Element("qtimltflite", "palm_inf")
    palm_inf.set("delegate", "gpu")
    palm_inf.set("model", model_base_path + "/models/palm_detection_full.tflite")

    # Queue for branch decoupling/backpressure.
    q_palm_post = make_queue("q_palm_post")

    # ML postprocess stage.
    palm_post = Element("qtimlpostprocess", "palm_post")
    palm_post.set("module", "palmd")
    palm_post.set("results", 2)
    palm_post.set("labels", model_base_path + "/labels/palmd_labels.json")
    palm_post.set("settings", model_base_path + "/labels/palmd_settings.json")

    palm_mlf = TextFilter()

    # Queue for branch decoupling/backpressure.
    q_palm_meta = make_queue("q_palm_meta")

    # -------------------------------------------------------------------------
    # Palm ROI mux + transform
    # -------------------------------------------------------------------------
    q_video_palm = make_queue("q_video_palm")

    # Metadata/video muxer.
    metamux_palm = Element("qtimetamux", "metamux_palm")

    # Metadata transform stage.
    palm_roi_transform = Element("qtimetatransform", "palm_roi_transform")
    palm_roi_transform.set("module", "roi-palmd")

    # Stream split (tee).
    split_after_palm = Element("tee", "split_after_palm")

    # -------------------------------------------------------------------------
    # Stage 2 — Hand landmark branch
    # -------------------------------------------------------------------------
    q_hand_pre = make_queue("q_hand_pre")

    # ML preprocessor/converter.
    hand_preproc = Element("qtimlvconverter", "hand_preproc")
    hand_preproc.set("mode", "roi-batch-cumulative")

    # Queue for branch decoupling/backpressure.
    q_hand_infer = make_queue("q_hand_infer")

    # TFLite inference stage.
    hand_inf = Element("qtimltflite", "hand_inf")
    hand_inf.set("delegate", "xnnpack")
    hand_inf.set("model", model_base_path + "/models/hand_landmark_full.tflite")

    # Queue for branch decoupling/backpressure.
    q_hand_post = make_queue("q_hand_post")

    # ML postprocess stage.
    hand_post = Element("qtimlpostprocess", "hand_post")
    hand_post.set("module", "hlandmark")
    hand_post.set("results", 2)
    hand_post.set("labels", model_base_path + "/labels/hlandmarks.json")
    hand_post.set("settings", model_base_path + "/labels/hlandmark_settings.json")

    hand_mlf = TextFilter()

    # Queue for branch decoupling/backpressure.
    q_hand_meta = make_queue("q_hand_meta")

    # -------------------------------------------------------------------------
    # Final metadata mux
    # -------------------------------------------------------------------------
    q_video_final = make_queue("q_video_final")

    # Metadata/video muxer.
    metamux_final = Element("qtimetamux", "metamux_final")

    # Stream split (tee).
    final_split = Element("tee", "final_split")

    # -------------------------------------------------------------------------
    # Display branch — cairooverlay draws the split-screen game UI
    # -------------------------------------------------------------------------
    q_display = make_queue("q_display")

    # Metadata overlay renderer.
    overlay = Element("qtivoverlay", "overlay")

    # Color format conversion stage.
    to_cairo = Element("qtivtransform", "to_cairo")

    cairofilter = (
        VideoFilter()
        .format("BGRA")
        .resolution(FRAME_WIDTH, FRAME_HEIGHT)
        .framerate(FRAME_FPS)
    )

    # Cairo overlay draw stage.
    wrist_draw = Element("cairooverlay", "wrist_draw")
    wrist_draw.connect_signal(
        "draw",
        lambda _overlay, draw_context, timestamp, duration:
            on_cairo_draw(draw_context, timestamp, duration),
    )

    display = Element("waylandsink", "display")
    display.set("sync", False)
    display.set("fullscreen", True)

    # -------------------------------------------------------------------------
    # Metadata branch — Python wrist parsing via appsink
    # -------------------------------------------------------------------------
    q_meta_parse = make_queue("q_meta_parse")

    # Pipeline element.
    meta_parser = Element("qtimlmetaparser", "meta_parser")
    meta_parser.set("module", "json")

    # AppSink for application callbacks.
    meta_sink = AppSink("meta_sink")
    meta_sink.set("sync", False)
    meta_sink.set("drop", True)
    meta_sink.set("max-buffers", 2)
    meta_sink.set_buffer_consumer(on_sample)

    # -------------------------------------------------------------------------
    # Assemble pipeline
    # -------------------------------------------------------------------------
    pipeline = (
        Pipeline("car_game")
        .add(source)
        .add(camera_transform)
        .add_stream_filter("videostream", videostream)
        .add(split)
        .add(q_palm_pre)
        .add(palm_preproc)
        .add(q_palm_infer)
        .add(palm_inf)
        .add(q_palm_post)
        .add(palm_post)
        .add_stream_filter("palm_mlf", palm_mlf)
        .add(q_palm_meta)
        .add(q_video_palm)
        .add(metamux_palm)
        .add(palm_roi_transform)
        .add(split_after_palm)
        .add(q_hand_pre)
        .add(hand_preproc)
        .add(q_hand_infer)
        .add(hand_inf)
        .add(q_hand_post)
        .add(hand_post)
        .add_stream_filter("hand_mlf", hand_mlf)
        .add(q_hand_meta)
        .add(q_video_final)
        .add(metamux_final)
        .add(final_split)
        .add(q_display)
        .add(overlay)
        .add(to_cairo)
        .add_stream_filter("cairofilter", cairofilter)
        .add(wrist_draw)
        .add(display)
        .add(q_meta_parse)
        .add(meta_parser)
        .add(meta_sink)
        .link("source", "camera_transform", "videostream", "split")
        .link("split", "q_video_palm", "metamux_palm")
        .link("split", "q_palm_pre", "palm_preproc", "q_palm_infer", "palm_inf",
              "q_palm_post", "palm_post", "palm_mlf", "q_palm_meta", "metamux_palm")
        .link("metamux_palm", "palm_roi_transform", "split_after_palm")
        .link("split_after_palm", "q_video_final", "metamux_final")
        .link("split_after_palm", "q_hand_pre", "hand_preproc", "q_hand_infer", "hand_inf",
              "q_hand_post", "hand_post", "hand_mlf", "q_hand_meta", "metamux_final")
        .link("metamux_final", "final_split")
        .link("final_split", "q_display", "overlay", "to_cairo", "cairofilter", "wrist_draw", "display")
        .link("final_split", "q_meta_parse", "meta_parser", "meta_sink")
    )

    print("[INFO] Starting IMSDK split-screen wrist steering game...", flush=True)
    print("[INFO] Left = live camera. Right = game. Green line = wrist-to-wrist.", flush=True)
    print(f"[INFO] Camera:              {device}", flush=True)
    print(f"[INFO] Palm model:          {model_base_path + '/models/palm_detection_full.tflite'}", flush=True)
    print(f"[INFO] Hand landmark model: {model_base_path + '/models/hand_landmark_full.tflite'}", flush=True)
    print("[INFO] Press Ctrl+C to stop", flush=True)
    pipeline.execute()


def main() -> None:
    """Initialise GStreamer, build the pipeline, connect all callbacks, and run until completion."""
    from qimsdk import ImsdkGstLogMode, ImsdkLogLevel, SetImsdkGstLogMode, SetImsdkLogLevel

    SetImsdkGstLogMode(ImsdkGstLogMode.ImsdkLog)
    SetImsdkLogLevel(ImsdkLogLevel.Debug)

    create_and_execute_pipeline(CAMERA_DEVICE)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\n[INFO] Interrupted by user.", flush=True)
    except Exception as exc:
        print(f"[ERROR] Pipeline failed: {exc}", flush=True)
        traceback.print_exc()
        raise SystemExit(1)
