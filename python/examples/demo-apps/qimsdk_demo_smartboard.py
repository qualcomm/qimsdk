#!/usr/bin/env python3
# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""Air Whiteboard — hand-gesture drawing app (letter/shape modes, word drag)."""

import argparse
import os
import sys
import json
import math
import re
import threading
import traceback
from collections import deque

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
#    v4l2src -> qtivtransform -> [videofilter] -> tee
#      tee. -> stage-1 palm detection -> qtimetamux -> qtimetatransform
#      qtimetatransform -> tee -> stage-2 hand landmarks -> qtimetamux -> tee
#        -> display branch (qtivoverlay + cairooverlay) -> waylandsink
#        -> metadata branch (qtimlmetaparser -> appsink callback)
#
#  Landmark metadata drives draw/erase/drag state updates while Cairo draws
#  persistent strokes, shapes, cursors, and HUD each frame.

# ── Hardware / pipeline ───────────────────────────────────────────────────────
CAMERA_DEVICE = args.input_config
FRAME_WIDTH, FRAME_HEIGHT, FRAME_FPS = 1920, 1080, 30

# Direct model and metadata paths.
# Paths are rooted at $HOME/Downloads/qimsdk_samples for consistency
# with the other C++/Python demo applications.
model_base_path = args.model_base_path


# ── Landmark IDs ──────────────────────────────────────────────────────────────
INDEX_TIP_ID, THUMB_TIP_ID, PINKY_TIP_ID = 8, 4, 20

# ── Gesture thresholds ────────────────────────────────────────────────────────
TIP_MARGIN          = 25
EXT_MARGIN          = 25
STABLE_FRAMES       = 2          # ← faster mode switch (was 3)
PINKY_BLOCK_MARGIN  = 5
ERASER_MARGIN       = 15
ERASER_TO_PEN_BLOCK = 8

PINCH_PIXEL_THRESHOLD  = 58      # px — thumb-index distance to count as pinch
PINCH_COOLDOWN_FRAMES  = 4       # small debounce; pinch is mainly used for drag
PINCH_DRAG_GRAB_RADIUS = 220     # px — max distance from index tip to grab an object/word
WORD_CLUSTER_JOIN_RADIUS = 90    # px — joins nearby strokes/letters into one movable word
BOARD_MODE_STABLE_FRAMES = 3       # open/closed palm must persist this many frames

# ── Pen / eraser ──────────────────────────────────────────────────────────────
PEN_DOT_SPACING       = 1        # ← densest interpolation (was 2)
PEN_LINE_WIDTH        = 17
MAX_JUMP_PIXELS       = 180
MAX_PEN_SEGMENTS      = 0        # 0 = unlimited
MAX_SHAPES            = 0
ERASER_RADIUS         = 45
ERASER_PATH_SPACING   = 8

PEN_SMOOTHING_ALPHA    = 0.55    # ← much more responsive (was 0.10)
ERASER_SMOOTHING_ALPHA = 0.45
MIN_DRAW_MOVE          = 1       # ← almost no threshold (was 4)

# ── Shape recognition ─────────────────────────────────────────────────────────
MIN_SHAPE_POINTS      = 22       # minimum raw points before any recognition attempt
MIN_SHAPE_SIZE        = 90       # minimum bounding-box diagonal in pixels
MIN_SHAPE_PATH        = 160      # minimum total path length in pixels
SHAPE_CONF_THRESHOLD  = 0.62
ARROW_CONF_THRESHOLD  = 0.72

# NEW — shape preview is only shown when the live stroke is already "big"
MIN_PREVIEW_POINTS    = 28       # must have at least this many points
MIN_PREVIEW_DIAG      = 110      # bounding-box diagonal must exceed this (pixels)

PRINT_STATUS = True

# ── Fixed pen color ──────────────────────────────────────────────────────────
# Black ink for clean whiteboard output. Change this tuple if you want another
# fixed color. Format: (red, green, blue, alpha), each in the range 0.0–1.0.
FIXED_PEN_COLOR = (0.00, 0.00, 0.00, 1.0)
# ── Handwriting cleanup / beautification ─────────────────────────────────────
HANDWRITING_BEAUTIFY = True
HANDWRITING_SMOOTH_WINDOW = 10
HANDWRITING_RESAMPLE_SPACING = 3
HANDWRITING_MIN_KEEP_DIST = 2

LABEL_TO_ID = {
    "wrist": 0, "thumb cmc": 1, "thumb mcp": 2, "thumb ip": 3, "thumb tip": 4,
    "index finger mcp": 5, "index finger pip": 6, "index finger dip": 7, "index finger tip": 8,
    "middle finger mcp": 9, "middle finger pip": 10, "middle finger dip": 11, "middle finger tip": 12,
    "ring finger mcp": 13, "ring finger pip": 14, "ring finger dip": 15, "ring finger tip": 16,
    "pinky mcp": 17, "pinky pip": 18, "pinky dip": 19, "pinky tip": 20,
}

# ─────────────────────────────────────────────────────────────────────────────
# State
# ─────────────────────────────────────────────────────────────────────────────
class PenState:
    def __init__(self):
        self.lock = threading.Lock()

        # Committed freehand segments: (a, b, rgba, width)
        self.segments = deque(maxlen=MAX_PEN_SEGMENTS or None)
        # Committed clean shapes: dicts from classify_shape()
        self.shapes   = deque(maxlen=MAX_SHAPES or None)

        # Active live stroke
        self.current_points = []
        self.preview_shape  = None
        self.current_color  = FIXED_PEN_COLOR

        # Smoothed positions
        self.prev_pen = self.prev_eraser = None
        self.smooth_pen = self.smooth_eraser = None

        # Mode FSM
        self.mode = self.raw_mode = self.candidate_mode = "OFF"
        self.candidate_count = self.eraser_block = 0
        self.pen_mode = self.eraser_mode = False

        # Finger tip positions (raw / smoothed)
        self.index_point = self.pinky_point = self.thumb_point = None
        self.tip_y = {}
        self.flags = {}

        # Fixed pen color. Color-changing logic was removed for publication.
        self.pen_color = FIXED_PEN_COLOR

        # Board mode: LETTER disables shape recognition; SHAPE enables it
        self.board_mode = "LETTER"
        self.board_candidate_mode = None
        self.board_candidate_count = 0
        self.board_flash = 0

        # Pinch / drag state
        self.pinch_active       = False
        self.pinch_cooldown     = 0
        self.pinch_last_release = 0   # frame counter when pinch was last released
        self.frame_counter      = 0

        # Drag state
        self.drag_mode          = False   # True while holding an object
        self.drag_target_type   = None    # "seg_group" | "shape"
        self.drag_target_idx    = None    # index into shapes deque (for shapes)
        self.drag_seg_indices   = None    # set of segment indices (for freehand)
        self.drag_anchor        = None    # index-tip position when grab started
        self.drag_origin        = None    # original object centre when grab started
        self.drag_selected_count = 0      # number of freehand segments selected for word drag

STATE = PenState()

# ─────────────────────────────────────────────────────────────────────────────
# Tiny helpers
# ─────────────────────────────────────────────────────────────────────────────
def clamp(v, lo, hi): return max(lo, min(int(v), hi))  # Clamps a value between a low and high limit
def norm(v): return str(v).strip().lower().replace("_", " ").replace("-", " ")  # Normalises a string to lowercase with spaces for consistent key matching
def first(d, keys): return next((d[k] for k in keys if isinstance(d, dict) and k in d), None)  # Returns the first matching value from a dict given a list of candidate keys
def num(v):  # Safely converts a value to float, returning None if conversion fails
    try: return float(v)
    except Exception: return None

def pix(v, size):  # Converts a normalised (0-1) or raw value to an integer pixel coordinate
    n = num(v)
    if n is None: return None
    if -0.25 <= n <= 2.0: n *= size
    return int(round(n))

def dist(a, b): return math.hypot(b[0] - a[0], b[1] - a[1])  # Returns the Euclidean distance between two 2D points

def smooth_point(prev, cur, alpha):  # Applies exponential smoothing between previous and current point positions
    if cur is None:  return prev
    if prev is None: return cur
    return (
        int(round(alpha * cur[0] + (1.0 - alpha) * prev[0])),
        int(round(alpha * cur[1] + (1.0 - alpha) * prev[1])),
    )

def set_rgba(ctx, rgba, alpha_mul=1.0):  # Sets the Cairo drawing colour from an RGBA tuple with an optional alpha multiplier
    r, g, b, a = rgba
    ctx.set_source_rgba(r, g, b, max(0.0, min(1.0, a * alpha_mul)))

def load_json(text):  # Attempts to parse a JSON object or array from a raw text string
    cuts = [text]
    for a, b in [("{", "}"), ("[", "]")]:
        i, j = text.find(a), text.rfind(b)
        if i >= 0 and j > i: cuts.append(text[i:j + 1])
    for s in cuts:
        try: return json.loads(s.strip())
        except Exception: pass
    return None

# ─────────────────────────────────────────────────────────────────────────────
# Landmark parsing  (unchanged from original)
# ─────────────────────────────────────────────────────────────────────────────
def point_any(v):  # Extracts an (x, y) pixel point from either a dict or a list/tuple value
    if isinstance(v, dict): return point_dict(v)
    if isinstance(v, (list, tuple)) and len(v) >= 2:
        x, y = pix(v[0], FRAME_WIDTH), pix(v[1], FRAME_HEIGHT)
        if x is not None and y is not None: return x, y
    return None

def point_dict(d):  # Extracts an (x, y) pixel point from a JSON dict using common coordinate key names
    x = first(d, ["x","X","x_pos","xpos","center_x","centerX","px","point_x"])
    y = first(d, ["y","Y","y_pos","ypos","center_y","centerY","py","point_y"])
    if x is not None and y is not None:
        x, y = pix(x, FRAME_WIDTH), pix(y, FRAME_HEIGHT)
        if x is not None and y is not None: return x, y
    r = first(d, ["rectangle","rect","bbox","box","bounding_box","bounding-box"])
    if isinstance(r, (list, tuple)) and len(r) >= 4:
        x1,y1,x2,y2 = pix(r[0],FRAME_WIDTH),pix(r[1],FRAME_HEIGHT),pix(r[2],FRAME_WIDTH),pix(r[3],FRAME_HEIGHT)
        if None not in (x1,y1,x2,y2): return int((x1+x2)/2), int((y1+y2)/2)
    for k in ["point","position","coord","coords","coordinate","location"]:
        p = point_any(d.get(k))
        if p is not None: return p
    return None

def lm_id(d):  # Extracts the landmark index (0-20) from a JSON dict by ID field or label name
    v = first(d, ["id","class_id","classId","label_id","labelId","index","idx"])
    if v is not None:
        try:
            i = int(float(str(v)))
            if 0 <= i <= 20: return i
        except Exception: pass
    label = first(d, ["label","name","class","class_name","display_name"])
    return LABEL_TO_ID.get(norm(label)) if label is not None else None

def collect(obj, out):  # Recursively walks a JSON object and fills a dict mapping landmark IDs to pixel points
    if isinstance(obj, list):
        if len(obj) >= 21 and sum(point_any(x) is not None for x in obj[:21]) >= 15:
            for i, x in enumerate(obj[:21]):
                p = point_any(x)
                if p is not None: out[i] = p
            return
        for x in obj: collect(x, out)
    elif isinstance(obj, dict):
        i, p = lm_id(obj), point_dict(obj)
        if i is not None and p is not None: out[i] = p
        for x in obj.values():
            if isinstance(x, (dict, list)): collect(x, out)

def regex_lms(text):  # Fallback regex parser that extracts landmark (id, x, y) triples from raw text
    out, s = {}, " ".join(text.replace("\n"," ").replace("\r"," ").split())
    pats = [
        r"(?:id|class_id|classId|label_id|labelId|index|idx)\s*[:=]\s*(\d+).{0,220}?(?:x|X)\s*[:=]\s*(-?\d+(?:\.\d+)?).{0,80}?(?:y|Y)\s*[:=]\s*(-?\d+(?:\.\d+)?)",
        r"(?:id|class_id|classId|label_id|labelId|index|idx)\s*[:=]\s*(\d+).{0,220}?(?:y|Y)\s*[:=]\s*(-?\d+(?:\.\d+)?).{0,80}?(?:x|X)\s*[:=]\s*(-?\d+(?:\.\d+)?)",
    ]
    for m in re.finditer(pats[0], s):
        i, x, y = int(m.group(1)), pix(m.group(2), FRAME_WIDTH), pix(m.group(3), FRAME_HEIGHT)
        if 0 <= i <= 20 and x is not None and y is not None: out[i] = (x, y)
    for m in re.finditer(pats[1], s):
        i, y, x = int(m.group(1)), pix(m.group(2), FRAME_HEIGHT), pix(m.group(3), FRAME_WIDTH)
        if 0 <= i <= 20 and x is not None and y is not None: out[i] = (x, y)
    return out

def parse_landmarks(text):  # Parses hand landmark pixel positions from raw metadata text into a {id: (x,y)} dict
    out = {}
    obj = load_json(text)
    if obj is not None: collect(obj, out)
    if len(out) < 21: out.update(regex_lms(text))
    return {i: (clamp(x, 0, FRAME_WIDTH-1), clamp(y, 0, FRAME_HEIGHT-1))
            for i, (x, y) in out.items() if 0 <= i <= 20}

# ─────────────────────────────────────────────────────────────────────────────
# Gesture logic
# ─────────────────────────────────────────────────────────────────────────────
def has(lms, ids): return all(i in lms for i in ids)  # Returns True if all given landmark IDs are present in the landmarks dict
def extended(lms, tip, pip): return tip in lms and pip in lms and lms[tip][1] + EXT_MARGIN < lms[pip][1]  # Returns True if a finger is extended (tip is above its PIP joint)
def flags(lms): return {  # Returns a dict of booleans indicating which fingers are currently extended
    "index":  extended(lms, 8,  6),
    "middle": extended(lms, 12, 10),
    "ring":   extended(lms, 16, 14),
    "pinky":  extended(lms, 20, 18),
}

def highest(lms, active, others, margin):  # Returns True if the active landmark is higher on screen than all other given landmarks
    if not has(lms, [active] + others): return False
    y = lms[active][1]
    return all(y + margin < lms[i][1] for i in others)

def pinky_intent(lms, f):  # Returns True if the pinky finger gesture is detected, indicating eraser intent
    if f.get("pinky"): return True
    if 20 in lms and 18 in lms and lms[20][1] + PINKY_BLOCK_MARGIN < lms[18][1]: return True
    return has(lms, [20, 4, 12, 16]) and highest(lms, 20, [4, 12, 16], ERASER_MARGIN)

def pinch_intent(lms):  # Returns True if thumb and index fingertips are close enough to count as a pinch
    if not has(lms, [THUMB_TIP_ID, INDEX_TIP_ID]): return False
    return dist(lms[THUMB_TIP_ID], lms[INDEX_TIP_ID]) <= PINCH_PIXEL_THRESHOLD

def open_palm_intent(lms, f):
    """Open palm -> LETTER mode.
    Uses the four long fingers. Thumb orientation is intentionally ignored
    because left/right hand and mirror flip can change thumb geometry.
    """
    return (
        f.get("index", False)
        and f.get("middle", False)
        and f.get("ring", False)
        and f.get("pinky", False)
    )

def folded(lms, tip, pip):  # Returns True if a finger is folded (tip is at or below its PIP joint)
    # In image coordinates, larger y means lower on the screen. A folded finger
    # usually has the tip at or below its PIP joint. Keep the margin relaxed
    # because closed fists are noisier than open fingers.
    return tip in lms and pip in lms and lms[tip][1] > lms[pip][1] - 8

def closed_palm_intent(lms, f):
    """Closed palm / fist -> SHAPE mode."""
    if not has(lms, [8, 6, 12, 10, 16, 14, 20, 18]):
        return False
    if f.get("index") or f.get("middle") or f.get("ring") or f.get("pinky"):
        return False
    return (
        folded(lms, 8, 6)
        and folded(lms, 12, 10)
        and folded(lms, 16, 14)
        and folded(lms, 20, 18)
    )

def requested_board_mode(lms, f):  # Returns the board mode requested by the current hand gesture (LETTER, SHAPE, or None)
    if open_palm_intent(lms, f):
        return "LETTER"
    if closed_palm_intent(lms, f):
        return "SHAPE"
    return None

def handle_board_mode_request(state, requested):
    """Stable open/closed palm mode switch."""
    if requested is None:
        state.board_candidate_mode = None
        state.board_candidate_count = 0
        return False
    if requested == state.board_mode:
        state.board_candidate_mode = requested
        state.board_candidate_count = BOARD_MODE_STABLE_FRAMES
        return False
    if state.board_candidate_mode == requested:
        state.board_candidate_count += 1
    else:
        state.board_candidate_mode = requested
        state.board_candidate_count = 1
    if state.board_candidate_count >= BOARD_MODE_STABLE_FRAMES:
        state.board_mode = requested
        state.board_flash = 24
        state.board_candidate_count = 0
        print(f"[INFO] Board mode -> {requested}", flush=True)
        return True
    return False

def raw_mode_detect(lms):  # Detects the raw gesture mode (PEN, ERASER, or OFF) from current landmark positions
    f     = flags(lms)
    pinky = pinky_intent(lms, f)
    eraser = (f["pinky"] and not f["middle"] and not f["ring"]
              and highest(lms, 20, [4, 12, 16], ERASER_MARGIN))
    pen = (f["index"] and not pinky and not f["middle"] and not f["ring"]
           and highest(lms, 8, [4, 12, 16, 20], TIP_MARGIN))
    return ("ERASER" if eraser else "OFF" if pinky else "PEN" if pen else "OFF"), f, pinky

def stable(state, mode):  # Applies a stability filter to prevent flickering between gesture modes
    if mode == "OFF":
        state.mode, state.candidate_mode, state.candidate_count = "OFF", "OFF", 0
        return "OFF"
    if state.mode == mode:
        state.candidate_mode, state.candidate_count = mode, STABLE_FRAMES
        return mode
    state.candidate_count = state.candidate_count + 1 if state.candidate_mode == mode else 1
    state.candidate_mode  = mode
    state.mode = mode if state.candidate_count >= STABLE_FRAMES else "OFF"
    return state.mode

# ─────────────────────────────────────────────────────────────────────────────
# Stroke geometry helpers
# ─────────────────────────────────────────────────────────────────────────────
def path_length(points):  # Calculates the total length of a polyline path through a list of points
    return sum(dist(points[i-1], points[i]) for i in range(1, len(points)))

def bounds(points):  # Returns the (x1, y1, x2, y2) bounding box of a list of points
    xs = [p[0] for p in points]; ys = [p[1] for p in points]
    return min(xs), min(ys), max(xs), max(ys)

def add_densified_segment(seg_deque, a, b, color, width):  # Interpolates densely spaced sub-segments between two points and appends them to the deque
    d = dist(a, b)
    steps = max(1, int(d / PEN_DOT_SPACING))
    prev = a
    for step in range(1, steps + 1):
        t = step / steps
        cur = (int(round(a[0] + (b[0]-a[0])*t)), int(round(a[1] + (b[1]-a[1])*t)))
        seg_deque.append((prev, cur, color, width))
        prev = cur

def commit_freehand_segments(state, points, color):  # Converts a list of stroke points into committed freehand segments stored in state
    if len(points) < 2: return
    for i in range(1, len(points)):
        if dist(points[i-1], points[i]) <= MAX_JUMP_PIXELS:
            add_densified_segment(state.segments, points[i-1], points[i], color, PEN_LINE_WIDTH)

# ─────────────────────────────────────────────────────────────────────────────
# Shape recognition
# ─────────────────────────────────────────────────────────────────────────────
def classify_shape(points, color):  # Analyses a stroke and attempts to recognise it as a circle, rectangle, or arrow
    if len(points) < MIN_SHAPE_POINTS: return None
    x1, y1, x2, y2 = bounds(points)
    w, h  = max(1, x2-x1), max(1, y2-y1)
    diag  = math.hypot(w, h)
    plen  = path_length(points)
    close = dist(points[0], points[-1])

    if diag < MIN_SHAPE_SIZE or plen < MIN_SHAPE_PATH: return None

    candidates = []

    # ── Circle
    cx, cy = (x1+x2)/2.0, (y1+y2)/2.0
    radii  = [math.hypot(p[0]-cx, p[1]-cy) for p in points]
    mean_r = sum(radii) / max(1, len(radii))
    if mean_r > 1:
        radial_std  = (sum((r-mean_r)**2 for r in radii)/len(radii))**0.5
        close_score  = 1.0 - min(close / max(1.0, diag*0.45), 1.0)
        aspect_score = 1.0 - min(abs(w-h) / float(max(w,h)), 1.0)
        radial_score = 1.0 - min(radial_std / max(1.0, mean_r*0.32), 1.0)
        circle_conf  = 0.36*close_score + 0.24*aspect_score + 0.40*radial_score
        if close < diag*0.42 and circle_conf >= SHAPE_CONF_THRESHOLD:
            candidates.append({
                "type":"circle","cx":int(round(cx)),"cy":int(round(cy)),
                "r":int(round((w+h)/4.0)),"color":color,
                "width":PEN_LINE_WIDTH,"confidence":circle_conf,
            })

    # ── Rectangle
    edge_tol  = max(14.0, min(w,h)*0.16)
    edge_hits = sum(1 for x,y in points
                    if min(abs(x-x1),abs(x-x2),abs(y-y1),abs(y-y2)) <= edge_tol)
    edge_score   = edge_hits / float(len(points))
    corner_radius = max(26.0, min(w,h)*0.25)
    corners      = [(x1,y1),(x2,y1),(x2,y2),(x1,y2)]
    corner_count = sum(1 for c in corners if any(dist(p,c)<=corner_radius for p in points))
    close_score  = 1.0 - min(close / max(1.0, diag*0.45), 1.0)
    rect_conf    = 0.35*close_score + 0.40*edge_score + 0.25*(corner_count/4.0)
    if close < diag*0.45 and edge_score >= 0.55 and corner_count >= 3 and rect_conf >= SHAPE_CONF_THRESHOLD:
        candidates.append({
            "type":"rect","x":int(x1),"y":int(y1),"w":int(w),"h":int(h),
            "color":color,"width":PEN_LINE_WIDTH,"confidence":rect_conf,
        })

    # ── Arrow (open, mostly straight, mostly one-direction stroke)
    # Letters/words often have a large left-to-right bounding box, so simply
    # checking start/end displacement is not enough.  Require a straight path
    # with low side deviation and mostly forward motion.
    displacement = dist(points[0], points[-1])
    linearity    = displacement / max(1.0, plen)
    open_score   = min(displacement / max(1.0, diag*0.65), 1.0)

    straight_score = 0.0
    forward_score = 0.0
    if displacement > 1:
        sx, sy = points[0]
        ex, ey = points[-1]
        ux, uy = (ex - sx) / displacement, (ey - sy) / displacement
        # Perpendicular distance of points to the start→end line.
        perp = [abs((p[0] - sx) * uy - (p[1] - sy) * ux) for p in points]
        mean_perp = sum(perp) / max(1, len(perp))
        straight_score = 1.0 - min(mean_perp / max(1.0, diag * 0.22), 1.0)
        # Forward-progress score: arrows mostly move in one direction; letters backtrack.
        proj = [(p[0] - sx) * ux + (p[1] - sy) * uy for p in points]
        forward = sum(max(0.0, proj[i] - proj[i-1]) for i in range(1, len(proj)))
        total = sum(abs(proj[i] - proj[i-1]) for i in range(1, len(proj)))
        forward_score = forward / max(1.0, total)

    arrow_conf = (
        0.40 * min(max((linearity - 0.55) / 0.35, 0.0), 1.0)
        + 0.25 * open_score
        + 0.20 * straight_score
        + 0.15 * forward_score
    )
    if (
        close > diag * 0.45
        and displacement >= MIN_SHAPE_SIZE
        and linearity >= 0.68
        and straight_score >= 0.58
        and forward_score >= 0.72
        and arrow_conf >= ARROW_CONF_THRESHOLD
    ):
        candidates.append({
            "type":"arrow","start":points[0],"end":points[-1],
            "color":color,"width":PEN_LINE_WIDTH,"confidence":arrow_conf,
        })

    if not candidates: return None
    return max(candidates, key=lambda x: x.get("confidence", 0.0))

def should_show_preview(points):
    """Gate: only show live shape preview when stroke is already large enough."""
    if len(points) < MIN_PREVIEW_POINTS: return False
    x1, y1, x2, y2 = bounds(points)
    return math.hypot(x2-x1, y2-y1) >= MIN_PREVIEW_DIAG

def resample_points(points, spacing):  # Resamples a list of points at a fixed distance spacing for uniform density
    if len(points) < 2 or spacing <= 0:
        return list(points)
    out = [points[0]]
    prev = points[0]
    accum = 0.0
    i = 1
    while i < len(points):
        cur = points[i]
        d = dist(prev, cur)
        if d <= 1e-6:
            i += 1
            continue
        if accum + d >= spacing:
            t = (spacing - accum) / d
            nx = int(round(prev[0] + (cur[0] - prev[0]) * t))
            ny = int(round(prev[1] + (cur[1] - prev[1]) * t))
            newp = (nx, ny)
            out.append(newp)
            prev = newp
            accum = 0.0
        else:
            accum += d
            prev = cur
            i += 1
    if out[-1] != points[-1]:
        out.append(points[-1])
    return out

def moving_average_points(points, window):  # Smooths a list of points using a sliding window moving average
    if len(points) < 3 or window <= 1:
        return list(points)
    half = max(1, window // 2)
    out = [points[0]]
    for i in range(1, len(points) - 1):
        lo = max(0, i - half)
        hi = min(len(points), i + half + 1)
        xs = [points[j][0] for j in range(lo, hi)]
        ys = [points[j][1] for j in range(lo, hi)]
        out.append((int(round(sum(xs) / len(xs))), int(round(sum(ys) / len(ys)))))
    out.append(points[-1])
    return out

def beautify_handwriting(points):
    """Light cleanup for letters/words.
    This is not OCR/font conversion; it keeps your handwriting style, but removes
    tiny landmark jitter and makes curves less shaky.
    """
    pts = list(points)
    if not HANDWRITING_BEAUTIFY or len(pts) < 4:
        return pts
    cleaned = [pts[0]]
    for pnt in pts[1:]:
        if dist(cleaned[-1], pnt) >= HANDWRITING_MIN_KEEP_DIST:
            cleaned.append(pnt)
    if len(cleaned) < 4:
        return cleaned
    cleaned = resample_points(cleaned, HANDWRITING_RESAMPLE_SPACING)
    cleaned = moving_average_points(cleaned, HANDWRITING_SMOOTH_WINDOW)
    return cleaned

def finish_current_stroke(state):  # Finalises the active stroke, committing it as a shape or freehand segments depending on board mode
    points = list(state.current_points)
    color  = state.current_color
    board_mode = getattr(state, "board_mode", "LETTER")
    state.current_points = []
    state.preview_shape  = None
    state.prev_pen       = None
    state.smooth_pen     = None
    if len(points) < 2: return

    if board_mode == "SHAPE":
        shape = classify_shape(points, color)
        if shape is not None:
            state.shapes.append(shape)
        else:
            # Fallback: if the rough shape was not confidently recognized, keep
            # the user's raw stroke rather than losing it.
            commit_freehand_segments(state, points, color)
    else:
        # LETTER mode: never auto-convert to shapes. Commit cleaned handwriting.
        commit_freehand_segments(state, beautify_handwriting(points), color)

# ─────────────────────────────────────────────────────────────────────────────
# Erasing
# ─────────────────────────────────────────────────────────────────────────────
def point_seg_dist(p, a, b):  # Returns the shortest distance from point p to the line segment a-b
    px,py = p; x1,y1 = a; x2,y2 = b; dx,dy = x2-x1,y2-y1
    if dx==0 and dy==0: return math.hypot(px-x1, py-y1)
    t = max(0.0, min(1.0, ((px-x1)*dx+(py-y1)*dy)/float(dx*dx+dy*dy)))
    return math.hypot(px-(x1+t*dx), py-(y1+t*dy))

def arrow_segments(shape):  # Returns the three line segments (shaft + two arrowhead lines) that make up an arrow shape
    s, e = shape["start"], shape["end"]
    angle    = math.atan2(e[1]-s[1], e[0]-s[0])
    head_len = max(35, min(70, int(dist(s,e)*0.18)))
    a1 = (int(round(e[0]-head_len*math.cos(angle-math.pi/6))),
          int(round(e[1]-head_len*math.sin(angle-math.pi/6))))
    a2 = (int(round(e[0]-head_len*math.cos(angle+math.pi/6))),
          int(round(e[1]-head_len*math.sin(angle+math.pi/6))))
    return [(s,e),(e,a1),(e,a2)]

def shape_dist(shape, p):  # Returns the distance from point p to the nearest edge of a committed shape
    t = shape.get("type")
    if t == "circle":
        return abs(dist(p,(shape["cx"],shape["cy"])) - shape["r"])
    if t == "rect":
        x,y,w,h = shape["x"],shape["y"],shape["w"],shape["h"]
        segs = [((x,y),(x+w,y)),((x+w,y),(x+w,y+h)),((x+w,y+h),(x,y+h)),((x,y+h),(x,y))]
        return min(point_seg_dist(p,a,b) for a,b in segs)
    if t == "arrow":
        return min(point_seg_dist(p,a,b) for a,b in arrow_segments(shape))
    return 999999.0

def erase(state, p):  # Removes all segments and shapes within the eraser radius of the given point
    points = [p]
    if state.prev_eraser is not None:
        d = math.hypot(p[0]-state.prev_eraser[0], p[1]-state.prev_eraser[1])
        if d <= MAX_JUMP_PIXELS:
            steps = max(1, int(d / ERASER_PATH_SPACING))
            points = [
                (int(round(state.prev_eraser[0]+(p[0]-state.prev_eraser[0])*t/steps)),
                 int(round(state.prev_eraser[1]+(p[1]-state.prev_eraser[1])*t/steps)))
                for t in range(1, steps+1)
            ]
    state.prev_eraser = p
    state.segments = deque(
        (seg for seg in state.segments if all(point_seg_dist(ep,seg[0],seg[1])>ERASER_RADIUS for ep in points)),
        maxlen=state.segments.maxlen)
    state.shapes = deque(
        (sh for sh in state.shapes if all(shape_dist(sh,ep)>ERASER_RADIUS for ep in points)),
        maxlen=state.shapes.maxlen)

# ─────────────────────────────────────────────────────────────────────────────
# Drag / move helpers
# ─────────────────────────────────────────────────────────────────────────────
def shape_centre(shape):  # Returns the centre point (x, y) of a committed shape
    t = shape.get("type")
    if t == "circle": return (shape["cx"], shape["cy"])
    if t == "rect":   return (shape["x"] + shape["w"]//2, shape["y"] + shape["h"]//2)
    if t == "arrow":  return ((shape["start"][0]+shape["end"][0])//2,
                              (shape["start"][1]+shape["end"][1])//2)
    return (0, 0)

def move_shape(shape, dx, dy):
    """Return a new shape dict translated by (dx,dy)."""
    t = shape.get("type")
    s = dict(shape)
    if t == "circle":
        s["cx"] += dx; s["cy"] += dy
    elif t == "rect":
        s["x"] += dx;  s["y"] += dy
    elif t == "arrow":
        s["start"] = (s["start"][0]+dx, s["start"][1]+dy)
        s["end"]   = (s["end"][0]+dx,   s["end"][1]+dy)
    return s

def seg_group_centre(segs):  # Returns the average centre point of a group of freehand segments
    if not segs: return (0, 0)
    xs = [s[0][0] for s in segs] + [s[1][0] for s in segs]
    ys = [s[0][1] for s in segs] + [s[1][1] for s in segs]
    return (sum(xs)//len(xs), sum(ys)//len(ys))

def segment_bbox(seg):  # Returns the (x1, y1, x2, y2) bounding box of a single segment
    a, b = seg[0], seg[1]
    return (min(a[0], b[0]), min(a[1], b[1]), max(a[0], b[0]), max(a[1], b[1]))

def bbox_of_segments(segs, indices):  # Returns the combined bounding box of a set of segments identified by index
    if not indices: return None
    boxes = [segment_bbox(segs[i]) for i in indices]
    return (
        min(b[0] for b in boxes), min(b[1] for b in boxes),
        max(b[2] for b in boxes), max(b[3] for b in boxes),
    )

def bbox_gap(b1, b2):
    """0 if boxes overlap, otherwise Euclidean distance between boxes."""
    dx = max(b1[0] - b2[2], b2[0] - b1[2], 0)
    dy = max(b1[1] - b2[3], b2[1] - b1[3], 0)
    return math.hypot(dx, dy)

def build_word_segment_cluster(segs, seed_idx):
    """Return indices for the nearby freehand cluster/word around seed_idx.

    The old version treated all freehand drawing as one global group and only
    checked the centre of that group. That made a written word hard to grab.
    This clusters nearby tiny line segments around the segment you pinch, so
    pinching one letter moves the whole nearby word, but not the whole canvas.
    """
    if seed_idx is None or seed_idx < 0 or seed_idx >= len(segs):
        return set()

    selected = {seed_idx}
    selected_bbox = bbox_of_segments(segs, selected)
    changed = True

    while changed:
        changed = False
        for i, seg in enumerate(segs):
            if i in selected:
                continue
            b = segment_bbox(seg)
            if bbox_gap(selected_bbox, b) <= WORD_CLUSTER_JOIN_RADIUS:
                selected.add(i)
                selected_bbox = (
                    min(selected_bbox[0], b[0]), min(selected_bbox[1], b[1]),
                    max(selected_bbox[2], b[2]), max(selected_bbox[3], b[3]),
                )
                changed = True

    return selected

def try_grab(state, grab_point):
    """
    Find the nearest committed object within PINCH_DRAG_GRAB_RADIUS and grab it.

    Shapes are grabbed by edge distance. Freehand words are grabbed by nearest
    segment, then expanded into a nearby segment cluster. This is the key fix
    for moving a written word after you finish writing it.
    """
    best_d = PINCH_DRAG_GRAB_RADIUS
    best_type = None
    best_idx = None
    best_seg_cluster = None

    # 1) Check clean shapes.
    for idx, sh in enumerate(state.shapes):
        d = shape_dist(sh, grab_point)
        if d < best_d:
            best_d = d
            best_type = "shape"
            best_idx = idx
            best_seg_cluster = None

    # 2) Check nearest freehand segment, not the centre of the whole canvas.
    segs = list(state.segments)
    nearest_seg_idx = None
    nearest_seg_d = PINCH_DRAG_GRAB_RADIUS
    for idx, seg in enumerate(segs):
        d = point_seg_dist(grab_point, seg[0], seg[1])
        if d < nearest_seg_d:
            nearest_seg_d = d
            nearest_seg_idx = idx

    if nearest_seg_idx is not None and nearest_seg_d < best_d:
        cluster = build_word_segment_cluster(segs, nearest_seg_idx)
        if cluster:
            best_d = nearest_seg_d
            best_type = "seg_group"
            best_idx = nearest_seg_idx
            best_seg_cluster = cluster

    if best_type is None:
        print("[INFO] Pinch: no nearby object/word to grab", flush=True)
        return False

    state.drag_mode = True
    state.drag_target_type = best_type
    state.drag_target_idx = best_idx
    state.drag_anchor = grab_point

    if best_type == "shape":
        state.drag_seg_indices = None
        state.drag_selected_count = 1
        state.drag_origin = shape_centre(list(state.shapes)[best_idx])
    else:
        state.drag_seg_indices = set(best_seg_cluster or [])
        state.drag_selected_count = len(state.drag_seg_indices)
        selected_segs = [segs[i] for i in state.drag_seg_indices]
        state.drag_origin = seg_group_centre(selected_segs)

    print(
        f"[INFO] Drag GRAB: {best_type} dist={best_d:.0f}px selected={state.drag_selected_count}",
        flush=True,
    )
    return True

def apply_drag(state, current_point):  # Moves the currently grabbed shape or segment group by the delta from the drag anchor
    if not state.drag_mode or state.drag_anchor is None: return
    dx = current_point[0] - state.drag_anchor[0]
    dy = current_point[1] - state.drag_anchor[1]

    if state.drag_target_type == "shape":
        shapes_list = list(state.shapes)
        idx = state.drag_target_idx
        if idx < len(shapes_list):
            shapes_list[idx] = move_shape(shapes_list[idx], dx, dy)
            state.shapes = deque(shapes_list, maxlen=state.shapes.maxlen)

    elif state.drag_target_type == "seg_group":
        selected = state.drag_seg_indices or set()
        moved_items = []
        for idx, (a, b, c, w) in enumerate(state.segments):
            if idx in selected:
                moved_items.append(((a[0] + dx, a[1] + dy), (b[0] + dx, b[1] + dy), c, w))
            else:
                moved_items.append((a, b, c, w))
        state.segments = deque(moved_items, maxlen=state.segments.maxlen)

    # Update anchor so next frame delta is relative to THIS position
    state.drag_anchor = current_point

def release_drag(state):  # Clears all drag state when the pinch gesture is released
    state.drag_mode        = False
    state.drag_target_type = None
    state.drag_target_idx  = None
    state.drag_anchor      = None
    state.drag_origin      = None
    state.drag_seg_indices = None
    state.drag_selected_count = 0
    print("[INFO] Drag RELEASED", flush=True)

# ─────────────────────────────────────────────────────────────────────────────
# Main state update  (called from appsink callback)
# ─────────────────────────────────────────────────────────────────────────────
def update_state(lms, state):  # Main per-frame logic that processes landmarks and updates pen, eraser, drag, and board mode state
    raw, f, pinky = raw_mode_detect(lms)
    pinch = pinch_intent(lms)

    with state.lock:
        state.frame_counter += 1
        fc = state.frame_counter

        if state.pinch_cooldown > 0:
            state.pinch_cooldown -= 1
        if state.board_flash > 0:
            state.board_flash -= 1

        raw_index = lms.get(8)
        raw_thumb = lms.get(4)
        raw_pinky = lms.get(20)

        state.raw_mode  = raw
        state.flags     = f
        state.tip_y     = {i: lms[i][1] for i in [4,8,12,16,20] if i in lms}

        # ── OPEN/CLOSED PALM BOARD MODE SWITCH ───────────────────────────
        # OPEN PALM  -> LETTER mode (no shape recognition)
        # CLOSED PALM/FIST -> SHAPE mode (shape recognition enabled)
        requested_mode = requested_board_mode(lms, f)
        if requested_mode is not None:
            finish_current_stroke(state)
            handle_board_mode_request(state, requested_mode)
            state.mode = "SET_" + requested_mode
            state.pen_mode = state.eraser_mode = False
            state.index_point = raw_index
            state.thumb_point = raw_thumb
            state.pinky_point = raw_pinky
            return
        else:
            handle_board_mode_request(state, None)
        # ── PINCH detected: dedicated to drag/move ────────────────────────
        if pinch:
            if not state.pinch_active and state.pinch_cooldown == 0:
                # Commit the live word/stroke first, then immediately try to grab it.
                finish_current_stroke(state)
                grab_pt = raw_index or (0, 0)
                try_grab(state, grab_pt)

            # While pinch is held and drag is active → move object/word.
            if state.drag_mode and raw_index:
                apply_drag(state, raw_index)

            state.pinch_active = True
            state.mode     = "DRAG" if state.drag_mode else "PINCH"
            state.pen_mode = state.eraser_mode = False
            state.index_point = raw_index
            state.thumb_point = raw_thumb
            state.pinky_point = raw_pinky
            return

        else:
            # Pinch just released
            if state.pinch_active:
                state.pinch_last_release = fc
                if state.drag_mode:
                    release_drag(state)
                state.pinch_cooldown = PINCH_COOLDOWN_FRAMES
            state.pinch_active = False

        # ── Normal PEN / ERASER / OFF ──────────────────────────────────────
        mode = raw
        if mode == "ERASER" or pinky:
            state.eraser_block = ERASER_TO_PEN_BLOCK
        elif state.eraser_block > 0:
            state.eraser_block -= 1
        if mode == "PEN" and state.eraser_block > 0:
            mode = "OFF"

        mode = stable(state, mode)

        state.pen_mode    = mode == "PEN"
        state.eraser_mode = mode == "ERASER"
        state.index_point = raw_index
        state.thumb_point = raw_thumb
        state.pinky_point = raw_pinky

        # ── ERASER ────────────────────────────────────────────────────────
        if state.eraser_mode and raw_pinky:
            finish_current_stroke(state)
            p = smooth_point(state.smooth_eraser, raw_pinky, ERASER_SMOOTHING_ALPHA)
            state.smooth_eraser = p
            state.pinky_point   = p
            state.prev_pen = None
            state.smooth_pen = None
            erase(state, p)
            return

        state.prev_eraser   = None
        state.smooth_eraser = None

        # ── PEN ───────────────────────────────────────────────────────────
        if not state.pen_mode or not raw_index:
            finish_current_stroke(state)
            return

        # Fast smoothing so the cursor sticks to the finger
        p = smooth_point(state.smooth_pen, raw_index, PEN_SMOOTHING_ALPHA)
        state.smooth_pen  = p
        state.index_point = p

        if state.prev_pen is None:
            state.current_color  = state.pen_color
            state.current_points = [p]
            state.preview_shape  = None
            state.prev_pen       = p
            return

        d = dist(state.prev_pen, p)

        if d > MAX_JUMP_PIXELS:
            finish_current_stroke(state)
            state.current_color  = state.pen_color
            state.current_points = [p]
            state.preview_shape  = None
            state.prev_pen       = p
            return

        if d < MIN_DRAW_MOVE:
            return

        state.current_points.append(p)
        state.prev_pen = p

        # LETTER mode never shows shape preview. SHAPE mode previews only for
        # large deliberate strokes, so writing words will not flash arrows.
        if state.board_mode == "SHAPE" and should_show_preview(state.current_points):
            state.preview_shape = classify_shape(state.current_points, state.current_color)
        else:
            state.preview_shape = None

# ─────────────────────────────────────────────────────────────────────────────
# Cairo drawing
# ─────────────────────────────────────────────────────────────────────────────
def draw_polyline(ctx, points, color, width, alpha_mul=1.0):  # Draws a connected polyline through a list of points using Cairo
    if not points: return
    set_rgba(ctx, color, alpha_mul)
    ctx.set_line_width(width)
    try: ctx.set_line_cap(1); ctx.set_line_join(1)
    except Exception: pass
    ctx.new_path()
    ctx.move_to(points[0][0], points[0][1])
    for p in points[1:]:
        ctx.line_to(p[0], p[1])
    ctx.stroke()

def draw_shape(ctx, shape, alpha_mul=1.0):  # Draws a committed shape (circle, rectangle, or arrow) onto the Cairo canvas
    color = shape.get("color", (0,0,0,1))
    width = shape.get("width", PEN_LINE_WIDTH)
    set_rgba(ctx, color, alpha_mul)
    ctx.set_line_width(width)
    try: ctx.set_line_cap(1); ctx.set_line_join(1)
    except Exception: pass
    t = shape.get("type")
    if t == "circle":
        ctx.new_path()
        ctx.arc(shape["cx"], shape["cy"], shape["r"], 0, 2*math.pi)
        ctx.stroke()
    elif t == "rect":
        ctx.new_path()
        ctx.rectangle(shape["x"], shape["y"], shape["w"], shape["h"])
        ctx.stroke()
    elif t == "arrow":
        for a, b in arrow_segments(shape):
            ctx.new_path(); ctx.move_to(a[0],a[1]); ctx.line_to(b[0],b[1]); ctx.stroke()

def draw_canvas(overlay, ctx, timestamp, duration, state):  # Cairo overlay callback that renders all strokes, shapes, cursors, and the HUD each frame
    with state.lock:
        segs           = list(state.segments)
        shapes         = list(state.shapes)
        current_points = list(state.current_points)
        preview_shape  = dict(state.preview_shape) if state.preview_shape else None
        mode           = state.mode
        pen            = state.pen_mode
        eraser         = state.eraser_mode
        drag_mode      = state.drag_mode
        ip, pp, tp     = state.index_point, state.pinky_point, state.thumb_point
        seg_count      = len(state.segments)
        shape_count    = len(state.shapes)
        ty             = dict(state.tip_y)
        pen_color      = state.pen_color
        board_mode     = state.board_mode
        board_flash    = state.board_flash

    try:
        ctx.save()

        # ── Committed freehand strokes ─────────────────────────────────────
        if segs:
            # Batch by color+width into continuous paths → no gaps, faster render
            ctx.new_path()
            last_b = None
            last_c = None
            for a, b, color, width in segs:
                if last_c != color:
                    if last_c is not None: ctx.stroke()
                    set_rgba(ctx, color)
                    ctx.set_line_width(width)
                    try: ctx.set_line_cap(1); ctx.set_line_join(1)
                    except Exception: pass
                    ctx.new_path()
                    ctx.move_to(a[0], a[1])
                    last_b = None
                if last_b != a:
                    ctx.move_to(a[0], a[1])
                ctx.line_to(b[0], b[1])
                last_b = b
                last_c = color
            if last_c is not None: ctx.stroke()

        # ── Committed clean shapes ─────────────────────────────────────────
        for shape in shapes:
            draw_shape(ctx, shape, 1.0)

        # ── Live stroke  ───────────────────────────────────────────────────
        if current_points:
            if preview_shape:
                # Ghost of raw stroke (very faint)
                draw_polyline(ctx, current_points,
                              preview_shape.get("color", pen_color),
                              max(3, int(PEN_LINE_WIDTH * 0.5)), 0.20)
                # Dashed clean shape preview
                try:    ctx.set_dash([16.0, 10.0], 0)
                except Exception: pass
                draw_shape(ctx, preview_shape, 0.80)
                try:    ctx.set_dash([], 0)
                except Exception: pass
            else:
                # Solid live stroke — draw as one continuous path (zero gaps).
                # In LETTER mode, lightly smooth the display path too.
                live_points = beautify_handwriting(current_points) if board_mode == "LETTER" else current_points
                draw_polyline(ctx, live_points, pen_color, PEN_LINE_WIDTH, 0.97)

        # ── Index finger dot ───────────────────────────────────────────────
        if ip:
            if pen:
                set_rgba(ctx, pen_color)
                ctx.arc(ip[0], ip[1], 9, 0, 2*math.pi)
                ctx.fill()
            elif drag_mode:
                ctx.set_source_rgba(1, 0.6, 0, 0.9)
                ctx.arc(ip[0], ip[1], 11, 0, 2*math.pi)
                ctx.fill()
            else:
                ctx.set_source_rgba(1, 1, 0, 0.5)
                ctx.arc(ip[0], ip[1], 6, 0, 2*math.pi)
                ctx.fill()

        # ── Thumb dot (visible during pinch/color) ─────────────────────────
        if tp and mode in ("DRAG", "PINCH", "SET_LETTER", "SET_SHAPE"):
            ctx.set_source_rgba(1, 1, 1, 0.75)
            ctx.arc(tp[0], tp[1], 7, 0, 2*math.pi)
            ctx.fill()

        # ── Pinky eraser ───────────────────────────────────────────────────
        if pp:
            if eraser:
                ctx.set_source_rgba(0, 0.45, 1, 0.22)
                ctx.arc(pp[0], pp[1], ERASER_RADIUS, 0, 2*math.pi)
                ctx.fill()
                ctx.set_source_rgba(0, 0.65, 1, 1)
                ctx.set_line_width(4)
                ctx.arc(pp[0], pp[1], ERASER_RADIUS, 0, 2*math.pi)
                ctx.stroke()
            else:
                ctx.set_source_rgba(0, 0.45, 1, 0.45)
                ctx.arc(pp[0], pp[1], 5, 0, 2*math.pi)
                ctx.fill()

        # ── Status HUD ────────────────────────────────────────────────────
        if PRINT_STATUS:
            ctx.set_source_rgba(0, 0, 0, 0.68)
            ctx.rectangle(20, 20, 790, 135)
            ctx.fill()

            ctx.select_font_face("Sans")
            ctx.set_font_size(30)
            if   mode == "PEN":    set_rgba(ctx, pen_color)
            elif mode == "ERASER": ctx.set_source_rgba(0, 0.65, 1, 1)
            elif mode == "DRAG":   ctx.set_source_rgba(1, 0.6, 0, 1)
            elif mode == "SET_SHAPE": ctx.set_source_rgba(0.3, 0.8, 1, 1)
            elif mode == "SET_LETTER": ctx.set_source_rgba(0.2, 1, 0.35, 1)
            elif mode == "PINCH":  ctx.set_source_rgba(1, 0.8, 0.2, 1)
            else:                  ctx.set_source_rgba(1, 1, 0, 1)
            ctx.move_to(35, 58)
            ctx.show_text(f"MODE: {mode}")

            # Board mode label
            if board_mode == "LETTER":
                ctx.set_source_rgba(0.2, 1.0, 0.35, 1.0 if board_flash else 0.85)
            else:
                ctx.set_source_rgba(0.3, 0.8, 1.0, 1.0 if board_flash else 0.85)
            ctx.set_font_size(22)
            ctx.move_to(245, 57)
            ctx.show_text(f"BOARD: {board_mode}")
            preview_label = preview_shape.get("type", "-").upper() if preview_shape else "-"
            ctx.set_font_size(20)
            ctx.set_source_rgba(1, 1, 1, 0.95)
            ctx.move_to(35, 95)
            ctx.show_text(f"preview={preview_label}  shapes={shape_count}  segs={seg_count}")
            ctx.move_to(35, 128)
            ctx.show_text(
                f"open palm=letters  closed palm=shapes  pinch=move word/shape  pinky=eraser"
                f"  y8={ty.get(8,'-')} y20={ty.get(20,'-')}"
            )

        ctx.restore()
    except Exception as e:
        print(f"[WARNING] cairo draw failed: {e}", flush=True)

# =============================================================================
# Callback consumers
# =============================================================================
def on_sample(buffer):  # Receives per-frame hand-landmark metadata and updates the gesture state machine
    try:
        data = buffer.data()
        text = bytes(data).decode("utf-8", errors="ignore").replace("\x00", "").strip() if data else ""
        lms = parse_landmarks(text)
        if lms:
            update_state(lms, STATE)
        else:
            with STATE.lock:
                finish_current_stroke(STATE)
                STATE.mode = "OFF"
                STATE.pen_mode = STATE.eraser_mode = False
                STATE.prev_pen = STATE.prev_eraser = None
                STATE.smooth_pen = STATE.smooth_eraser = None
                STATE.tip_y = {}
    except Exception as e:
        print(f"[WARNING] metadata callback failed: {e}", flush=True)

def make_queue(name: str) -> Element:
    q = Element("queue", name)
    q.set("leaky", 2)
    q.set("max-size-buffers", 2)
    q.set("max-size-bytes", 0)
    q.set("max-size-time", 0)
    return q

#  Example pipeline:
#
#    source -> transform -> [videofilter] -> split
#      split. -> q_video_palm -> metamux_palm
#      split. -> palm_preproc -> palm_inf -> palm_post -> [palm_mlf]
#             -> q_palm_meta -> metamux_palm -> palm_roi_transform -> split_after_palm
#      split_after_palm. -> q_video_final -> metamux_final
#      split_after_palm. -> hand_preproc -> hand_inf -> hand_post -> [hand_mlf]
#                       -> q_hand_meta -> metamux_final -> final_split
#      final_split. -> q_display -> qtivoverlay -> qtivtransform -> [cairofilter]
#                   -> cairooverlay -> waylandsink
#      final_split. -> qtimlmetaparser -> appsink
#
#  The final display branch uses qtivoverlay first for Qualcomm ML overlay
#  metadata and cairooverlay second for persistent virtual ink rendering.
def create_and_execute_pipeline(device: str = CAMERA_DEVICE) -> None:  # Builds callbacks and executes the full IMSDK smartboard pipeline
    # -------------------------------------------------------------------------
    # Source and shared camera stream
    # -------------------------------------------------------------------------
    # Camera source (USB V4L2 device).
    source = Element("v4l2src", "source")
    source.set("device", device)

    # Initial video transform (mirror preview).
    transform = Element("qtivtransform", "transform")
    transform.set("flip-horizontal", True)

    videofilter = (
        VideoFilter()
        .format("NV12")
        .resolution(FRAME_WIDTH, FRAME_HEIGHT)
        .framerate(FRAME_FPS)
    )

    # Split raw camera stream to palm branch + passthrough video branch.
    split = Element("tee", "split")

    # -------------------------------------------------------------------------
    # Palm detector preprocessing / inference / postprocess branch
    # -------------------------------------------------------------------------
    # Queue before palm preprocessing.
    q_palm_pre = make_queue("q_palm_pre")

    # Palm detector preprocessor.
    palm_preproc = Element("qtimlvconverter", "palm_preproc")
    palm_preproc.set("mode", "image-batch-non-cumulative")

    # Queue before palm inference.
    q_palm_infer = make_queue("q_palm_infer")

    # Palm detector inference.
    palm_inf = Element("qtimltflite", "palm_inf")
    palm_inf.set("delegate", "gpu")
    palm_inf.set("model", model_base_path + "/models/palm_detection_full.tflite")

    # Queue before palm postprocess.
    q_palm_post = make_queue("q_palm_post")

    # Palm detector postprocess.
    palm_post = Element("qtimlpostprocess", "palm_post")
    palm_post.set("module", "palmd")
    palm_post.set("results", 1)
    palm_post.set("labels", model_base_path + "/labels/palmd_labels.json")
    palm_post.set("settings", model_base_path + "/labels/palmd_settings.json")

    palm_mlf = TextFilter()

    # Queue carrying palm metadata text.
    q_palm_meta = make_queue("q_palm_meta")

    # Queue for raw video path into palm metadata mux.
    q_video_palm = make_queue("q_video_palm")
    # Muxes video + palm metadata.
    metamux_palm = Element("qtimetamux", "metamux_palm")

    # Converts palm detections into hand ROI metadata.
    palm_roi_transform = Element("qtimetatransform", "palm_roi_transform")
    palm_roi_transform.set("module", "roi-palmd")

    # Split after palm ROI transform into hand branch + final video branch.
    split_after_palm = Element("tee", "split_after_palm")

    # -------------------------------------------------------------------------
    # Hand landmark preprocessing / inference / postprocess branch
    # -------------------------------------------------------------------------
    # Queue before hand-landmark preprocessing.
    q_hand_pre = make_queue("q_hand_pre")

    # Hand-landmark preprocessor (ROI batch mode).
    hand_preproc = Element("qtimlvconverter", "hand_preproc")
    hand_preproc.set("mode", "roi-batch-cumulative")

    # Queue before hand-landmark inference.
    q_hand_infer = make_queue("q_hand_infer")

    # Hand-landmark inference.
    hand_inf = Element("qtimltflite", "hand_inf")
    hand_inf.set("delegate", "xnnpack")
    hand_inf.set("model", model_base_path + "/models/hand_landmark_full.tflite")

    # Queue before hand-landmark postprocess.
    q_hand_post = make_queue("q_hand_post")

    # Hand-landmark postprocess.
    hand_post = Element("qtimlpostprocess", "hand_post")
    hand_post.set("module", "hlandmark")
    hand_post.set("results", 1)
    hand_post.set("labels", model_base_path + "/labels/hlandmarks.json")
    hand_post.set("settings", model_base_path + "/labels/hlandmark_settings.json")

    hand_mlf = TextFilter()

    # Queue carrying hand metadata text.
    q_hand_meta = make_queue("q_hand_meta")

    # Queue for video path into final metadata mux.
    q_video_final = make_queue("q_video_final")
    # Muxes final video + hand metadata.
    metamux_final = Element("qtimetamux", "metamux_final")

    # Split final stream into display + metadata consumer branches.
    final_split = Element("tee", "final_split")

    # -------------------------------------------------------------------------
    # Display branch: Qualcomm overlay first, persistent cairo ink second
    # -------------------------------------------------------------------------
    # Queue for display branch.
    q_display = make_queue("q_display")

    # Qualcomm metadata overlay renderer.
    overlay = Element("qtivoverlay", "overlay")

    # Transform before cairooverlay.
    to_cairo = Element("qtivtransform", "to_cairo")

    # Explicit BGRA caps into cairooverlay (cairooverlay needs a
    # Cairo-drawable format -- BGRA/ARGB -- not autonegotiation).
    cairofilter = VideoFilter().format("BGRA").resolution(FRAME_WIDTH, FRAME_HEIGHT).framerate(FRAME_FPS)

    # Cairo overlay that draws persistent whiteboard content.
    pen_canvas = Element("cairooverlay", "pen_canvas")
    pen_canvas.connect_signal(
        "draw",
        lambda _overlay, draw_context, timestamp, duration:
            draw_canvas(None, draw_context, timestamp, duration, STATE)
    )

    # Display sink.
    display = Element("waylandsink", "display")
    display.set("sync", False)
    display.set("fullscreen", True)

    # -------------------------------------------------------------------------
    # Metadata branch: parse metadata JSON and feed the Python callback
    # -------------------------------------------------------------------------
    # Queue before metadata parser.
    q_meta_parse = make_queue("q_meta_parse")

    # Metadata parser (JSON output).
    meta_parser = Element("qtimlmetaparser", "meta_parser")
    meta_parser.set("module", "json")

    # AppSink receiving parsed metadata for gesture state updates.
    meta_sink = AppSink("meta_sink")
    meta_sink.set("sync", False)
    meta_sink.set("max-buffers", 1)
    meta_sink.set("drop", True)

    meta_sink.set_buffer_consumer(on_sample)

    # -------------------------------------------------------------------------
    # Assemble and link the pipeline.
    # -------------------------------------------------------------------------
    pipeline = (
        Pipeline("smartboard")
        .add(source)
        .add(transform)
        .add_stream_filter("videofilter", videofilter)
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
        .add(pen_canvas)
        .add(display)

        .add(q_meta_parse)
        .add(meta_parser)
        .add(meta_sink)

        # Links
        .link("source", "transform", "videofilter", "split")
        .link("split", "q_video_palm", "metamux_palm")
        .link("split", "q_palm_pre", "palm_preproc", "q_palm_infer", "palm_inf",
              "q_palm_post", "palm_post", "palm_mlf", "q_palm_meta", "metamux_palm")
        .link("metamux_palm", "palm_roi_transform", "split_after_palm")
        .link("split_after_palm", "q_video_final", "metamux_final")
        .link("split_after_palm", "q_hand_pre", "hand_preproc", "q_hand_infer", "hand_inf",
              "q_hand_post", "hand_post", "hand_mlf", "q_hand_meta", "metamux_final")
        .link("metamux_final", "final_split")
        .link("final_split", "q_display", "overlay", "to_cairo", "cairofilter", "pen_canvas", "display")
        .link("final_split", "q_meta_parse", "meta_parser", "meta_sink")
    )

    print("[INFO] Air Whiteboard (smartboard) - letter/shape modes + word drag", flush=True)
    print(f"[INFO] Camera:            {device}", flush=True)
    print(f"[INFO] Palm model:        {model_base_path + '/models/palm_detection_full.tflite'}", flush=True)
    print(f"[INFO] Hand landmark model: {model_base_path + '/models/hand_landmark_full.tflite'}", flush=True)
    print("[INFO] Gestures:", flush=True)
    print("[INFO]   open palm      = LETTER mode", flush=True)
    print("[INFO]   closed palm    = SHAPE mode", flush=True)
    print("[INFO]   index finger   = draw in current board mode", flush=True)
    print("[INFO]   pinch          = grab & drag nearest word/shape", flush=True)
    print("[INFO]   pinky finger   = eraser", flush=True)
    print("[INFO] Press Ctrl+C to stop", flush=True)
    pipeline.execute()

def main() -> None:
    from qimsdk import ImsdkGstLogMode, ImsdkLogLevel, SetImsdkGstLogMode, SetImsdkLogLevel

    SetImsdkGstLogMode(ImsdkGstLogMode.ImsdkLog)
    SetImsdkLogLevel(ImsdkLogLevel.Debug)

    create_and_execute_pipeline(CAMERA_DEVICE)

if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\n[INFO] Interrupted by user.", flush=True)
    except Exception as e:
        print(f"[ERROR] Pipeline failed: {e}", flush=True)
        traceback.print_exc()
        raise SystemExit(1)
