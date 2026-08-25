#!/usr/bin/env python3
# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""USB camera YOLOv8 stable cumulative object counter using Python IMSDK.
"""

import argparse
import os
import sys
import json
import math
import re
import threading
from dataclasses import dataclass
from typing import Any, Dict, List, Optional, Sequence, Tuple

import gi

gi.require_version("Gst", "1.0")
gi.require_version("GLib", "2.0")
from gi.repository import Gst, GLib

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
#      tee. -> qtimetamux (video)
#      tee. -> qtimlvconverter -> qtimltflite -> qtimlpostprocess -> tee
#             -> qtimetamux
#             -> qtimlmetaparser -> appsink(count callback)
#    qtimetamux -> qtivoverlay -> qtivtransform -> [videofilter:BGRA] -> cairooverlay -> waylandsink
#
#  The metadata callback performs ID tracking and cumulative ROI counting,
#  while Cairo renders the ROI box and counter HUD.

# =============================================================================
# Configuration
# =============================================================================
USB_CAMERA_DEVICE = args.input_config
WIDTH = 1920
HEIGHT = 1080
FPS = 20

model_base_path = args.model_base_path

# Leave empty to count all classes. Example: {"bottle"} or {"person"}
COUNT_ONLY_LABELS = set()

# Top counter text
COUNT_TEXT_PREFIX = "ROI Objects Counted"

# Center ROI configuration
# Requirement: a center rectangle with full screen height and 1/3 screen width.
# Only detections whose centroid is inside this ROI are sent to the counter.
ROI_X1 = WIDTH / 3.0
ROI_Y1 = 0.0
ROI_X2 = (2.0 * WIDTH) / 3.0
ROI_Y2 = float(HEIGHT)
ROI_BORDER_LINE_WIDTH = 6.0
ROI_FILL_ALPHA = 0.08
# Keep this as "centroid" for accurate factory/conveyor counting.
# The object is counted only when its center point enters the ROI.
ROI_INCLUSION_MODE = "centroid"
# Optional alternate mode: set ROI_INCLUSION_MODE = "intersection" and tune this ratio.
ROI_MIN_INTERSECTION_RATIO = 0.50

# Tracker tuning
# Larger values help avoid double-counting when detection briefly flickers.
MAX_MISSED_FRAMES = 75
MAX_MATCH_DISTANCE_PIXELS = 300.0
MIN_IOU_FOR_MATCH = 0.02

# New tracks must be detected this many times before a ROI visit can increase the counter.
# 4 frames gives faster response for moving factory objects while still filtering single-frame noise.
TRACK_CONFIRMATION_FRAMES = 4

# ROI visit hysteresis.
# A track is counted once per ROI visit. After it is seen outside the ROI or missing
# for this many frames, the next ROI entry is allowed to increment the counter again.
# Use 1 for the fastest hand-test response; use 2-3 for more industrial stability.
ROI_ENTER_CONFIRMATION_FRAMES = 3
ROI_EXIT_RESET_FRAMES = 3

# Same-frame duplicate suppression. This prevents one physical object being counted as 2 objects
# when YOLO produces two overlapping or very-near boxes for it.
DUPLICATE_IOU_THRESHOLD = 0.35
DUPLICATE_CENTER_DISTANCE_PIXELS = 120.0

# Smooth tracked boxes/centroids to reduce jitter while the object is moving.
# Higher value follows new detections faster; lower value smooths more.
BBOX_SMOOTHING_ALPHA = 0.50

# Ignore tiny accidental boxes if needed. Keep 0.0 to disable.
MIN_BBOX_AREA_PIXELS = 0.0

# Confidence filter. Set to 0.0 if your metadata does not include confidence.
MIN_CONFIDENCE = 0.20
DEBUG_METADATA = False
DEBUG_FIRST_N_BUFFERS = 3
DEBUG_TRACKING = False
# =============================================================================
# Data models
# =============================================================================
BBox = Tuple[float, float, float, float]  # x1, y1, x2, y2 in display pixels


@dataclass
class Detection:
    bbox: BBox
    label: Optional[str] = None
    confidence: Optional[float] = None

    @property
    def centroid(self) -> Tuple[float, float]:
        x1, y1, x2, y2 = self.bbox
        return ((x1 + x2) / 2.0, (y1 + y2) / 2.0)


@dataclass
class Track:
    object_id: int
    bbox: BBox
    centroid: Tuple[float, float]
    label: Optional[str]
    missed_frames: int = 0
    age: int = 1
    hits: int = 1
    counted: bool = False
    inside_roi_hits: int = 0
    outside_roi_frames: int = 0
    current_visit_counted: bool = False
    velocity: Tuple[float, float] = (0.0, 0.0)


# =============================================================================
# Cumulative centroid/IoU tracker
# =============================================================================
class CumulativeObjectTracker:
    """Stable Python-side tracker for cumulative object counting.

    """

    def __init__(
        self,
        max_missed_frames: int = MAX_MISSED_FRAMES,
        max_match_distance: float = MAX_MATCH_DISTANCE_PIXELS,
        min_iou_for_match: float = MIN_IOU_FOR_MATCH,
        min_hits_to_count: int = TRACK_CONFIRMATION_FRAMES,
        roi_enter_confirmation_frames: int = ROI_ENTER_CONFIRMATION_FRAMES,
        roi_exit_reset_frames: int = ROI_EXIT_RESET_FRAMES,
        duplicate_iou_threshold: float = DUPLICATE_IOU_THRESHOLD,
        duplicate_center_distance: float = DUPLICATE_CENTER_DISTANCE_PIXELS,
        smoothing_alpha: float = BBOX_SMOOTHING_ALPHA,
    ) -> None:
        self.max_missed_frames = int(max_missed_frames)
        self.max_match_distance = float(max_match_distance)
        self.min_iou_for_match = float(min_iou_for_match)
        self.min_hits_to_count = max(1, int(min_hits_to_count))
        self.roi_enter_confirmation_frames = max(1, int(roi_enter_confirmation_frames))
        self.roi_exit_reset_frames = max(1, int(roi_exit_reset_frames))
        self.duplicate_iou_threshold = float(duplicate_iou_threshold)
        self.duplicate_center_distance = float(duplicate_center_distance)
        self.smoothing_alpha = max(0.0, min(1.0, float(smoothing_alpha)))
        self.next_object_id = 1
        self.total_count = 0
        self.tracks: Dict[int, Track] = {}

    @staticmethod
    def distance(p1: Tuple[float, float], p2: Tuple[float, float]) -> float:  # Returns the Euclidean distance between two 2D points
        return math.hypot(p1[0] - p2[0], p1[1] - p2[1])

    @staticmethod
    def bbox_area(bbox: BBox) -> float:  # Returns the area of a bounding box in pixels
        x1, y1, x2, y2 = bbox
        return max(0.0, x2 - x1) * max(0.0, y2 - y1)

    @staticmethod
    def bbox_diagonal(bbox: BBox) -> float:  # Returns the diagonal length of a bounding box in pixels
        x1, y1, x2, y2 = bbox
        return math.hypot(max(0.0, x2 - x1), max(0.0, y2 - y1))

    @staticmethod
    def iou(a: BBox, b: BBox) -> float:  # Computes the Intersection over Union (IoU) ratio between two bounding boxes
        ax1, ay1, ax2, ay2 = a
        bx1, by1, bx2, by2 = b
        ix1 = max(ax1, bx1)
        iy1 = max(ay1, by1)
        ix2 = min(ax2, bx2)
        iy2 = min(ay2, by2)
        iw = max(0.0, ix2 - ix1)
        ih = max(0.0, iy2 - iy1)
        intersection = iw * ih
        area_a = max(0.0, ax2 - ax1) * max(0.0, ay2 - ay1)
        area_b = max(0.0, bx2 - bx1) * max(0.0, by2 - by1)
        union = area_a + area_b - intersection
        return intersection / union if union > 0 else 0.0

    @staticmethod
    def same_label(track_label: Optional[str], det_label: Optional[str]) -> bool:  # Returns True if both labels match, or if either label is missing
        # If either side has no label, allow matching by geometry only.
        if not track_label or not det_label:
            return True
        return str(track_label).lower() == str(det_label).lower()

    def adaptive_match_distance(self, track: Track, det: Detection) -> float:  # Returns an adaptive max match distance based on object size and missed frames
        # Larger/nearer boxes and recently missed tracks are allowed more motion.
        size_allowance = 0.60 * max(self.bbox_diagonal(track.bbox), self.bbox_diagonal(det.bbox))
        missed_allowance = 25.0 * min(track.missed_frames, 5)
        return max(self.max_match_distance, size_allowance) + missed_allowance

    def predicted_centroid(self, track: Track) -> Tuple[float, float]:  # Predicts the next centroid position of a track using its current velocity
        # Predict one frame ahead. If frames were missed, predict slightly farther.
        multiplier = 1.0 + min(track.missed_frames, 5)
        return (
            track.centroid[0] + track.velocity[0] * multiplier,
            track.centroid[1] + track.velocity[1] * multiplier,
        )

    def smooth_bbox(self, old_bbox: BBox, new_bbox: BBox) -> BBox:  # Blends the old and new bounding boxes using exponential smoothing
        a = self.smoothing_alpha
        return tuple((1.0 - a) * old + a * new for old, new in zip(old_bbox, new_bbox))  # type: ignore[return-value]

    def deduplicate_detections(self, detections: Sequence[Detection]) -> List[Detection]:
        """Keep only one box when YOLO emits duplicate boxes for the same physical object."""
        if not detections:
            return []

        # Prefer higher confidence. If confidence is unavailable, prefer larger boxes.
        ordered = sorted(
            detections,
            key=lambda det: (
                det.confidence if det.confidence is not None else 1.0,
                self.bbox_area(det.bbox),
            ),
            reverse=True,
        )

        selected: List[Detection] = []
        for det in ordered:
            if MIN_BBOX_AREA_PIXELS > 0.0 and self.bbox_area(det.bbox) < MIN_BBOX_AREA_PIXELS:
                continue

            is_duplicate = False
            for kept in selected:
                if not self.same_label(kept.label, det.label):
                    continue
                iou = self.iou(kept.bbox, det.bbox)
                center_distance = self.distance(kept.centroid, det.centroid)
                if iou >= self.duplicate_iou_threshold or center_distance <= self.duplicate_center_distance:
                    is_duplicate = True
                    break

            if not is_duplicate:
                selected.append(det)

        return selected

    def update_roi_visit_state(self, track: Track, inside_roi: bool) -> None:
        """Update ROI-entry state and increment the cumulative counter once per ROI visit.

        This fixes the fast re-entry case: after an object leaves the ROI for a short,
        confirmed time, the same tracked object is allowed to create a new count when
        it enters the ROI again. At the same time, ROI_ENTER_CONFIRMATION_FRAMES and
        ROI_EXIT_RESET_FRAMES prevent normal detector flicker from causing duplicates.
        """
        if inside_roi:
            track.inside_roi_hits += 1
            track.outside_roi_frames = 0
            if (
                not track.current_visit_counted
                and track.hits >= self.min_hits_to_count
                and track.inside_roi_hits >= self.roi_enter_confirmation_frames
            ):
                track.current_visit_counted = True
                track.counted = True
                self.total_count += 1
                if DEBUG_TRACKING:
                    print(f"Counted ROI visit for object ID {track.object_id}. Total = {self.total_count}")
        else:
            track.inside_roi_hits = 0
            track.outside_roi_frames += 1
            if track.outside_roi_frames >= self.roi_exit_reset_frames:
                track.current_visit_counted = False

    def mark_track_outside_roi(self, track: Track) -> None:  # Marks a track as outside the ROI and updates its exit state
        self.update_roi_visit_state(track, False)

    def register(self, det: Detection) -> Track:  # Creates and registers a new track from an unmatched detection
        object_id = self.next_object_id
        self.next_object_id += 1
        track = Track(
            object_id=object_id,
            bbox=det.bbox,
            centroid=det.centroid,
            label=det.label,
            missed_frames=0,
            age=1,
            hits=1,
            counted=False,
            inside_roi_hits=0,
            outside_roi_frames=0,
            current_visit_counted=False,
            velocity=(0.0, 0.0),
        )
        self.tracks[object_id] = track
        self.update_roi_visit_state(track, detection_inside_roi(det))
        if DEBUG_TRACKING:
            status = "counted" if track.current_visit_counted else "pending"
            print(f"New {status} object ID {object_id}. Total = {self.total_count}")
        return track

    def remove_stale_tracks(self) -> None:  # Removes tracks that have been missing for more than the allowed number of frames
        stale_ids = [
            object_id
            for object_id, track in self.tracks.items()
            if track.missed_frames > self.max_missed_frames
        ]
        for object_id in stale_ids:
            if DEBUG_TRACKING:
                print(f"Object ID {object_id} left frame. Total remains {self.total_count}")
            del self.tracks[object_id]

    def looks_like_existing_track(self, det: Detection) -> bool:
        """Guard against registering an extra duplicate when one object generates two boxes."""
        for track in self.tracks.values():
            if not self.same_label(track.label, det.label):
                continue
            iou = self.iou(track.bbox, det.bbox)
            distance = self.distance(self.predicted_centroid(track), det.centroid)
            duplicate_distance = max(self.duplicate_center_distance, 0.35 * self.bbox_diagonal(track.bbox))
            if iou >= 0.20 or distance <= duplicate_distance:
                return True
        return False

    def update_track_from_detection(self, track: Track, det: Detection) -> None:  # Updates a matched track with the new detection's position, velocity, and ROI state
        old_centroid = track.centroid
        det_centroid = det.centroid

        smoothed_bbox = self.smooth_bbox(track.bbox, det.bbox)
        x1, y1, x2, y2 = smoothed_bbox
        smoothed_centroid = ((x1 + x2) / 2.0, (y1 + y2) / 2.0)

        measured_velocity = (det_centroid[0] - old_centroid[0], det_centroid[1] - old_centroid[1])
        track.velocity = (
            0.70 * track.velocity[0] + 0.30 * measured_velocity[0],
            0.70 * track.velocity[1] + 0.30 * measured_velocity[1],
        )
        track.bbox = smoothed_bbox
        track.centroid = smoothed_centroid
        track.label = det.label or track.label
        track.missed_frames = 0
        track.age += 1
        track.hits += 1
        self.update_roi_visit_state(track, detection_inside_roi(det))

    def update(self, detections: Sequence[Detection]) -> Tuple[int, List[Track]]:
        detections = self.deduplicate_detections(detections)

        # No detections: age active tracks as missed, but do not change total count.
        if not detections:
            for track in self.tracks.values():
                track.missed_frames += 1
                self.mark_track_outside_roi(track)
            self.remove_stale_tracks()
            return self.total_count, list(self.tracks.values())

        # No existing tracks: create pending tracks. They are counted only after confirmation.
        if not self.tracks:
            for det in detections:
                self.register(det)
            return self.total_count, list(self.tracks.values())

        # Build all valid track/detection match candidates.
        candidates = []
        track_items = list(self.tracks.items())
        for object_id, track in track_items:
            predicted_centroid = self.predicted_centroid(track)
            for det_index, det in enumerate(detections):
                if not self.same_label(track.label, det.label):
                    continue
                dist = self.distance(predicted_centroid, det.centroid)
                iou = self.iou(track.bbox, det.bbox)
                allowed_distance = self.adaptive_match_distance(track, det)
                # Match if close enough OR overlapping enough.
                if dist <= allowed_distance or iou >= self.min_iou_for_match:
                    # Prefer high IoU, then low distance. Penalize very stale tracks slightly.
                    score = (1.0 - min(iou, 1.0)) * 10000.0 + dist + (track.missed_frames * 20.0)
                    candidates.append((score, object_id, det_index, dist, iou))

        candidates.sort(key=lambda item: item[0])
        matched_tracks = set()
        matched_detections = set()

        # Greedy one-to-one assignment.
        for _, object_id, det_index, _, _ in candidates:
            if object_id in matched_tracks or det_index in matched_detections:
                continue
            det = detections[det_index]
            track = self.tracks[object_id]
            self.update_track_from_detection(track, det)
            matched_tracks.add(object_id)
            matched_detections.add(det_index)

        # Existing tracks not matched in this frame are considered temporarily missing.
        for object_id, track in list(self.tracks.items()):
            if object_id not in matched_tracks:
                track.missed_frames += 1
                self.mark_track_outside_roi(track)

        # Any unmatched detection can be a new object, but first apply one more
        # duplicate guard against all current tracks.
        for det_index, det in enumerate(detections):
            if det_index in matched_detections:
                continue
            if self.looks_like_existing_track(det):
                continue
            self.register(det)

        self.remove_stale_tracks()
        return self.total_count, list(self.tracks.values())
# =============================================================================
# Shared state
# =============================================================================
class CountState:
    def __init__(self) -> None:
        self.lock = threading.Lock()
        self.last_display_count = -1
        self.debug_buffers = 0
        self.tracker = CumulativeObjectTracker()

    def get_count(self) -> int:  # Returns the current cumulative object count in a thread-safe way
        with self.lock:
            return max(0, int(self.last_display_count))

    def update_with_detections(self, detections: Sequence[Detection]) -> Tuple[int, bool]:  # Passes detections to the tracker and returns the updated count and whether it changed
        with self.lock:
            total_count, _active_tracks = self.tracker.update(detections)
            changed = total_count != self.last_display_count
            self.last_display_count = total_count
            return total_count, changed

    def next_debug_buffer_index(self) -> int:  # Returns the next debug buffer index in a thread-safe way
        with self.lock:
            self.debug_buffers += 1
            return self.debug_buffers

STATE = CountState()

# =============================================================================
# JSON metadata helpers
# =============================================================================
LABEL_KEYS = {
    "label",
    "labels",
    "name",
    "class",
    "class_name",
    "class_id",
    "object",
    "type",
}
RECT_KEYS = {
    "bbox",
    "box",
    "rect",
    "rectangle",
    "bounding_box",
    "boundingbox",
    "roi",
}
SCORE_KEYS = {
    "confidence",
    "conf",
    "score",
    "prob",
    "probability",
}
CONTAINER_KEYS = {
    "objects",
    "object_detection",
    "detections",
    "detection",
    "predictions",
    "prediction",
    "results",
    "result",
    "items",
    "regions",
    "rois",
}


def normalize_key(key: Any) -> str:  # Normalises a metadata key to lowercase with underscores for consistent lookup
    return str(key).lower().replace("-", "_").replace(" ", "_")


def normalized_keys(dictionary: Dict[str, Any]) -> set:  # Returns a set of all normalised keys from a dictionary
    return {normalize_key(key) for key in dictionary.keys()}


def get_value(dictionary: Dict[str, Any], wanted_keys: set) -> Optional[Any]:  # Returns the first value from a dict whose key matches any of the wanted keys
    wanted = {normalize_key(key) for key in wanted_keys}
    for key, value in dictionary.items():
        if normalize_key(key) in wanted:
            return value
    return None


def extract_label(dictionary: Dict[str, Any]) -> Optional[str]:  # Extracts the object class label string from a detection dictionary
    value = get_value(dictionary, LABEL_KEYS)
    if value is None:
        return None
    if isinstance(value, str):
        value = value.strip()
        return value if value else None
    if isinstance(value, (int, float)):
        return str(int(value)) if float(value).is_integer() else str(value)
    if isinstance(value, dict):
        return extract_label(value)
    if isinstance(value, list) and value:
        first = value[0]
        if isinstance(first, dict):
            return extract_label(first)
        return str(first)
    return None


def extract_confidence(dictionary: Dict[str, Any]) -> Optional[float]:  # Extracts the detection confidence score from a detection dictionary
    value = get_value(dictionary, SCORE_KEYS)
    if value is None:
        return None
    if isinstance(value, (int, float)):
        return float(value)
    if isinstance(value, str):
        try:
            return float(value.strip())
        except ValueError:
            return None
    if isinstance(value, dict):
        for subvalue in value.values():
            if isinstance(subvalue, (int, float)):
                return float(subvalue)
    if isinstance(value, list):
        for subvalue in value:
            if isinstance(subvalue, (int, float)):
                return float(subvalue)
    return None


def label_allowed(label: Optional[str]) -> bool:  # Returns True if the label is in the allowed set, or if no filter is configured
    if not COUNT_ONLY_LABELS:
        return True
    if not label:
        return False
    allowed = {str(item).lower() for item in COUNT_ONLY_LABELS}
    return str(label).lower() in allowed


def clamp_bbox(bbox: BBox) -> Optional[BBox]:  # Clamps bounding box coordinates to the frame dimensions and returns None if invalid
    x1, y1, x2, y2 = bbox
    x1 = max(0.0, min(float(WIDTH - 1), x1))
    y1 = max(0.0, min(float(HEIGHT - 1), y1))
    x2 = max(0.0, min(float(WIDTH - 1), x2))
    y2 = max(0.0, min(float(HEIGHT - 1), y2))
    if x2 <= x1 or y2 <= y1:
        return None
    return (x1, y1, x2, y2)


def scale_if_normalized(x1: float, y1: float, x2: float, y2: float) -> BBox:  # Scales normalised 0-1 coordinates to pixel coordinates if they appear normalised
    values = [x1, y1, x2, y2]
    if all(-0.01 <= value <= 1.50 for value in values):
        return (x1 * WIDTH, y1 * HEIGHT, x2 * WIDTH, y2 * HEIGHT)
    return (x1, y1, x2, y2)


def parse_bbox_from_sequence(value: Sequence[Any]) -> Optional[BBox]:  # Parses a bounding box from a flat list of 4 numbers in either xyxy or xywh format
    if len(value) < 4:
        return None
    try:
        a, b, c, d = [float(value[i]) for i in range(4)]
    except (TypeError, ValueError):
        return None

    # Heuristic:
    # - [x1, y1, x2, y2] if c/d look like bottom-right coordinates.
    # - otherwise [x, y, width, height].
    if c > a and d > b:
        bbox = scale_if_normalized(a, b, c, d)
    else:
        bbox = scale_if_normalized(a, b, a + c, b + d)
    return clamp_bbox(bbox)


def parse_bbox_from_dict(value: Dict[str, Any]) -> Optional[BBox]:  # Parses a bounding box from a dict using common key name patterns like left/top/right/bottom
    lowered = {normalize_key(k): v for k, v in value.items()}

    def number_for(*names: str) -> Optional[float]:
        for name in names:
            key = normalize_key(name)
            if key in lowered:
                try:
                    return float(lowered[key])
                except (TypeError, ValueError):
                    return None
        return None

    # left/top/right/bottom style
    left = number_for("left", "l", "xmin", "x_min", "x1")
    top = number_for("top", "t", "ymin", "y_min", "y1")
    right = number_for("right", "r", "xmax", "x_max", "x2")
    bottom = number_for("bottom", "b", "ymax", "y_max", "y2")
    if None not in (left, top, right, bottom):
        return clamp_bbox(scale_if_normalized(left, top, right, bottom))

    # x/y/width/height style
    x = number_for("x", "left", "xmin", "x_min")
    y = number_for("y", "top", "ymin", "y_min")
    w = number_for("width", "w")
    h = number_for("height", "h")
    if None not in (x, y, w, h):
        return clamp_bbox(scale_if_normalized(x, y, x + w, y + h))

    # Nested rectangle values may be inside another key.
    for nested_value in value.values():
        bbox = extract_bbox(nested_value)
        if bbox is not None:
            return bbox
    return None


def extract_bbox(value: Any) -> Optional[BBox]:  # Extracts a bounding box from any JSON value, whether a dict, list, or nested structure
    if isinstance(value, dict):
        return parse_bbox_from_dict(value)
    if isinstance(value, (list, tuple)):
        if len(value) >= 4 and not isinstance(value[0], (dict, list, tuple)):
            return parse_bbox_from_sequence(value)
        for item in value:
            bbox = extract_bbox(item)
            if bbox is not None:
                return bbox
    return None


def looks_like_detection(dictionary: Dict[str, Any]) -> bool:  # Returns True if a dictionary looks like a detection object based on its keys
    keys = normalized_keys(dictionary)
    has_rect = bool(keys & {normalize_key(k) for k in RECT_KEYS})
    has_label = bool(keys & {normalize_key(k) for k in LABEL_KEYS})
    has_score = bool(keys & {normalize_key(k) for k in SCORE_KEYS})
    return has_rect or (has_label and has_score)


def detection_from_dict(dictionary: Dict[str, Any]) -> Optional[Detection]:  # Builds a Detection object from a JSON dict, returning None if bbox is missing or filtered out
    # Look for a bbox under known rectangle keys first.
    bbox = None
    for key, value in dictionary.items():
        if normalize_key(key) in {normalize_key(k) for k in RECT_KEYS}:
            bbox = extract_bbox(value)
            if bbox is not None:
                break

    # Some metadata schemas put x/y/w/h directly in the detection object.
    if bbox is None:
        bbox = parse_bbox_from_dict(dictionary)

    if bbox is None:
        return None

    label = extract_label(dictionary)
    if not label_allowed(label):
        return None

    confidence = extract_confidence(dictionary)
    if confidence is not None and confidence < MIN_CONFIDENCE:
        return None

    return Detection(bbox=bbox, label=label, confidence=confidence)


def extract_detections_from_json_object(obj: Any) -> List[Detection]:  # Recursively walks a JSON object and collects all valid Detection instances
    detections: List[Detection] = []

    if obj is None:
        return detections

    if isinstance(obj, list):
        for item in obj:
            detections.extend(extract_detections_from_json_object(item))
        return detections

    if isinstance(obj, dict):
        if looks_like_detection(obj):
            det = detection_from_dict(obj)
            if det is not None:
                return [det]

        # Prefer known containers to avoid walking unrelated metadata too deeply.
        container_found = False
        for key, value in obj.items():
            if normalize_key(key) in {normalize_key(k) for k in CONTAINER_KEYS}:
                container_found = True
                detections.extend(extract_detections_from_json_object(value))
        if container_found:
            return detections

        # Fallback recursive walk.
        for value in obj.values():
            detections.extend(extract_detections_from_json_object(value))

    return detections


def parse_json_records(raw_text: str) -> List[Any]:  # Parses one or more JSON records from a raw metadata text string
    text = raw_text.replace("\x00", "").strip()
    if not text:
        return []

    # Try entire buffer first.
    try:
        return [json.loads(text)]
    except json.JSONDecodeError:
        pass

    # Try line-by-line JSON.
    records = []
    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            records.append(json.loads(line))
        except json.JSONDecodeError:
            pass
    if records:
        return records

    # Try concatenated JSON objects.
    decoder = json.JSONDecoder()
    index = 0
    while index < len(text):
        while index < len(text) and text[index].isspace():
            index += 1
        if index >= len(text):
            break
        try:
            obj, end_index = decoder.raw_decode(text, index)
            records.append(obj)
            index = end_index
        except json.JSONDecodeError:
            index += 1
    return records


def detections_from_text(raw_text: str) -> List[Detection]:  # Parses all detections from a raw metadata text buffer
    records = parse_json_records(raw_text)
    detections: List[Detection] = []
    for record in records:
        detections.extend(extract_detections_from_json_object(record))
    return detections


# =============================================================================
# ROI helpers
# =============================================================================
def roi_bbox() -> BBox:  # Returns the fixed center ROI bounding box as (x1, y1, x2, y2)
    return (ROI_X1, ROI_Y1, ROI_X2, ROI_Y2)

def point_inside_roi(point: Tuple[float, float]) -> bool:  # Returns True if a given (x, y) point is inside the center ROI rectangle
    x, y = point
    x1, y1, x2, y2 = roi_bbox()
    return x1 <= x <= x2 and y1 <= y <= y2

def bbox_intersection_area(a: BBox, b: BBox) -> float:  # Returns the overlapping area between two bounding boxes
    ax1, ay1, ax2, ay2 = a
    bx1, by1, bx2, by2 = b
    ix1 = max(ax1, bx1)
    iy1 = max(ay1, by1)
    ix2 = min(ax2, bx2)
    iy2 = min(ay2, by2)
    return max(0.0, ix2 - ix1) * max(0.0, iy2 - iy1)

def detection_inside_roi(det: Detection) -> bool:
    """Return True only when this detection should be considered for counting."""
    mode = str(ROI_INCLUSION_MODE).lower().strip()
    if mode == "intersection":
        det_area = max(1.0, CumulativeObjectTracker.bbox_area(det.bbox))
        ratio = bbox_intersection_area(det.bbox, roi_bbox()) / det_area
        return ratio >= ROI_MIN_INTERSECTION_RATIO
    # Default/recommended mode: count only when object center is inside ROI.
    return point_inside_roi(det.centroid)

def filter_detections_to_roi(detections: Sequence[Detection]) -> List[Detection]:  # Filters a list of detections to only those whose centroid or area is inside the ROI
    return [det for det in detections if detection_inside_roi(det)]

def draw_roi_and_counter_overlay(cr, _timestamp: int, _duration: int) -> None:
    """Draw the visible center ROI rectangle and the cumulative counter.
    """
    x1, y1, x2, y2 = roi_bbox()
    roi_width = x2 - x1
    roi_height = y2 - y1
    half_line = ROI_BORDER_LINE_WIDTH / 2.0

    try:
        cr.save()

        # Draw center ROI: full height, middle 1/3 screen width.
        cr.set_source_rgba(0.0, 1.0, 0.0, ROI_FILL_ALPHA)
        cr.rectangle(
            x1 + half_line,
            y1 + half_line,
            max(1.0, roi_width - ROI_BORDER_LINE_WIDTH),
            max(1.0, roi_height - ROI_BORDER_LINE_WIDTH),
        )
        cr.fill_preserve()
        cr.set_source_rgba(0.0, 1.0, 0.0, 0.95)
        cr.set_line_width(ROI_BORDER_LINE_WIDTH)
        cr.stroke()

        # Draw top-center cumulative count.
        count = STATE.get_count()
        text = f"{COUNT_TEXT_PREFIX}: {count}"
        font_size = 46
        margin_top = 28
        pad_x = 24
        pad_y = 14

        cr.select_font_face("Sans", 0, 1)  # normal slant, bold weight
        cr.set_font_size(font_size)
        ext = cr.text_extents(text)
        # PyCairo versions may return either a TextExtents object or a tuple:
        # (x_bearing, y_bearing, width, height, x_advance, y_advance).
        try:
            x_bearing = float(ext.x_bearing)
            text_width = float(ext.width)
            text_height = float(ext.height)
        except AttributeError:
            x_bearing = float(ext[0])
            text_width = float(ext[2])
            text_height = float(ext[3])

        text_x = (WIDTH - text_width) / 2.0 - x_bearing
        text_y = margin_top + text_height

        # Dark translucent background for readability.
        bg_x = (WIDTH - text_width) / 2.0 - pad_x
        bg_y = margin_top - pad_y
        bg_w = text_width + (2.0 * pad_x)
        bg_h = text_height + (2.0 * pad_y)
        cr.set_source_rgba(0.0, 0.0, 0.0, 0.65)
        cr.rectangle(bg_x, bg_y, bg_w, bg_h)
        cr.fill()

        # White count text.
        cr.set_source_rgba(1.0, 1.0, 1.0, 1.0)
        cr.move_to(text_x, text_y)
        cr.show_text(text)

        cr.restore()
    except Exception as exc:
        if DEBUG_TRACKING:
            print(f"ROI/counter overlay draw failed: {exc}")

# =============================================================================
# Small helpers
# =============================================================================
def safe_set_property(element: Gst.Element, prop: str, value: Any) -> None:  # Safely sets a GStreamer element property, silently ignoring any errors
    try:
        element.set_property(prop, value)
    except Exception:
        pass

# =============================================================================
# Appsink callback
# =============================================================================
def on_sample(buffer):  # AppSink buffer callback that parses metadata and updates the cumulative object counter
    data = buffer.data()
    if not data:
        return
    raw_text = bytes(data).decode("utf-8", errors="ignore")

    if DEBUG_METADATA:
        idx = STATE.next_debug_buffer_index()
        if idx <= DEBUG_FIRST_N_BUFFERS:
            preview = raw_text.replace("\n", " ")[:1000]
            print(f"[metadata sample {idx}] {preview}")

    detections = detections_from_text(raw_text)
    # Pass all detections to the tracker, not only ROI detections.
    # This lets the tracker know when an object has left the ROI, so a fast
    # re-entry can be counted as a new ROI visit without waiting for the old
    # track to expire. The tracker itself decides whether each detection is
    # inside the center ROI before increasing the counter.
    total_count, changed = STATE.update_with_detections(detections)

    if changed:
        print(f"{COUNT_TEXT_PREFIX}: {total_count}")

# =============================================================================
# Pipeline
# =============================================================================
def create_and_execute_pipeline(device: str = USB_CAMERA_DEVICE) -> None:  # Builds callbacks and executes the full IMSDK YOLOv8 object counting pipeline
    # Camera source (USB V4L2 device).
    source = Element("v4l2src", "source")
    source.set("device", device)

    # Video transform stage.
    transform = Element("qtivtransform", "transform")

    videofilter = (
        VideoFilter()
        .format("NV12")
        .resolution(WIDTH, HEIGHT)
        .framerate(FPS)
    )

    # Stream split (tee).
    split = Element("tee", "split")

    # Queue for branch decoupling/backpressure.
    q_video = Element("queue", "q_video")

    # Queue for branch decoupling/backpressure.
    q_ml_1 = Element("queue", "q_ml_1")

    # ML preprocessor/converter.
    preprocessing = Element("qtimlvconverter", "preprocessing")

    # Queue for branch decoupling/backpressure.
    q_ml_2 = Element("queue", "q_ml_2")

    # TFLite inference stage.
    inferencing = Element("qtimltflite", "inferencing")
    inferencing.set("delegate", "external")
    inferencing.set("external-delegate-path", "libQnnTFLiteDelegate.so")
    inferencing.set("external-delegate-options", "QNNExternalDelegate,backend_type=htp;")
    inferencing.set("model", model_base_path + "/models/yolov8_det_quantized.tflite")

    # Queue for branch decoupling/backpressure.
    q_ml_3 = Element("queue", "q_ml_3")

    # ML postprocess stage.
    postprocessing = Element("qtimlpostprocess", "postprocessing")
    postprocessing.set("module", "yolov8")
    postprocessing.set("labels", model_base_path + "/labels/yolov8.json")

    # Stream split (tee).
    post_split = Element("tee", "post_split")

    # Queue for branch decoupling/backpressure.
    q_meta_to_mux = Element("queue", "q_meta_to_mux")

    mlf = TextFilter()

    # Queue for branch decoupling/backpressure.
    q_meta_to_count = Element("queue", "q_meta_to_count")

    # Metadata parser stage.
    metaparser = Element("qtimlmetaparser", "metaparser")
    metaparser.set("module", "json")

    # Queue for branch decoupling/backpressure.
    q_count_sink = Element("queue", "q_count_sink")

    # AppSink for application callbacks.
    count_sink = AppSink("count_sink")
    count_sink.set("sync", False)
    count_sink.set("max-buffers", 1)
    count_sink.set("drop", True)
    count_sink.set_buffer_consumer(on_sample)

    # Metadata/video muxer.
    mlmuxer = Element("qtimetamux", "mlmuxer")

    # Metadata overlay renderer.
    overlay = Element("qtivoverlay", "overlay")

    # Video transform stage.
    display_transform = Element("qtivtransform", "display_transform")

    bgrafilter = (
        VideoFilter()
        .format("BGRA")
        .resolution(WIDTH, HEIGHT)
        .framerate(FPS)
    )

    # Cairo overlay draw stage.
    roi_overlay = Element("cairooverlay", "roi_overlay")
    roi_overlay.connect_signal(
        "draw",
        lambda _overlay, draw_context, timestamp, duration:
            draw_roi_and_counter_overlay(draw_context, timestamp, duration)
    )

    # Display sink.
    display = Element("waylandsink", "display")
    display.set("sync", True)
    display.set("fullscreen", True)

    pipeline = (
        Pipeline("product_counting")
        .add(source)
        .add(transform)
        .add_stream_filter("videofilter", videofilter)
        .add(split)
        .add(q_video)
        .add(q_ml_1)
        .add(preprocessing)
        .add(q_ml_2)
        .add(inferencing)
        .add(q_ml_3)
        .add(postprocessing)
        .add(post_split)
        .add(q_meta_to_mux)
        .add_stream_filter("mlf", mlf)
        .add(q_meta_to_count)
        .add(metaparser)
        .add(q_count_sink)
        .add(count_sink)
        .add(mlmuxer)
        .add(overlay)
        .add(display_transform)
        .add_stream_filter("bgrafilter", bgrafilter)
        .add(roi_overlay)
        .add(display)
        # Camera -> NV12 filter -> tee
        .link("source", "transform", "videofilter", "split")
        # Display branch: tee -> queue -> muxer
        .link("split", "q_video", "mlmuxer")
        # ML branch: tee -> preprocess -> infer -> postprocess -> mlf -> post_split
        .link("split", "q_ml_1", "preprocessing", "q_ml_2", "inferencing",
              "q_ml_3", "postprocessing", "mlf", "post_split")
        # Metadata -> muxer branch
        .link("post_split", "q_meta_to_mux", "mlmuxer")
        # Metadata -> counting branch
        .link("post_split", "q_meta_to_count", "metaparser", "q_count_sink", "count_sink")
        # Muxer -> overlay -> BGRA convert -> cairooverlay -> display
        .link("mlmuxer", "overlay", "display_transform", "bgrafilter", "roi_overlay", "display")
    )
    print("[INFO] Starting YOLOv8 ROI cumulative object counting pipeline", flush=True)
    print(f"[INFO] Camera:  {device}", flush=True)
    print(f"[INFO] Model:   {model_base_path + '/models/yolov8_det_quantized.tflite'}", flush=True)
    print(f"[INFO] Labels:  {model_base_path + '/labels/yolov8.json'}", flush=True)
    print("[INFO] Parser:  qtimlmetaparser", flush=True)
    print(f"[INFO] ROI:     x=[{int(ROI_X1)}..{int(ROI_X2)}] y=[{int(ROI_Y1)}..{int(ROI_Y2)}]", flush=True)
    if COUNT_ONLY_LABELS:
        labels = " ".join(sorted(str(item) for item in COUNT_ONLY_LABELS))
        print(f"[INFO] Counting only: {labels}", flush=True)
    else:
        print("[INFO] Counting all detected objects", flush=True)
    print("[INFO] Press Ctrl+C to stop", flush=True)

    pipeline.execute()


def main() -> None:  # Entry point that builds the pipeline, connects callbacks, and starts execution
    from qimsdk import ImsdkGstLogMode, ImsdkLogLevel, SetImsdkGstLogMode, SetImsdkLogLevel

    SetImsdkGstLogMode(ImsdkGstLogMode.ImsdkLog)
    SetImsdkLogLevel(ImsdkLogLevel.Debug)

    create_and_execute_pipeline(USB_CAMERA_DEVICE)


if __name__ == "__main__":
    main()
