/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */
#include <iostream>
#include <string>
#include <vector>
#include <set>
#include <map>
#include <unordered_map>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <limits>
#include <optional>
#include <getopt.h>
#include <cairo/cairo.h>
#include <qti/qimsdk.h>
using namespace qti;

namespace {
// =============================================================================
// Configuration
// =============================================================================
static const std::string     home_path         = std::getenv("HOME") ? std::getenv("HOME") : "";
// Input source configuration, overridden via the --input-config argument.
static std::string           input_config;
static constexpr int         WIDTH             = 1920;
static constexpr int         HEIGHT            = 1080;
static constexpr int         FPS               = 20;
// Base path for sample assets (media/, models/, labels/ live under it).
// Set from the --model-base-path argument, or the default location.
static std::string           model_base_path;
static constexpr const char* COUNT_TEXT_PREFIX = "ROI Objects Counted";
// Leave empty to count all detected objects.
// Example: static const std::set<std::string> COUNT_ONLY_LABELS = {"person"};
static const std::set<std::string> COUNT_ONLY_LABELS = {};

// Center ROI: full screen height, middle third of screen width.
static constexpr double ROI_X1               = WIDTH  / 3.0;
static constexpr double ROI_Y1               = 0.0;
static constexpr double ROI_X2               = (2.0 * WIDTH)  / 3.0;
static constexpr double ROI_Y2               = static_cast<double>(HEIGHT);
static constexpr double ROI_BORDER_LINE_WIDTH = 6.0;
static constexpr double ROI_FILL_ALPHA        = 0.08;
// "centroid": count when the object center enters the ROI (default).
// "intersection": count when enough of the bounding box overlaps the ROI.
static constexpr const char* ROI_INCLUSION_MODE         = "centroid";
static constexpr double      ROI_MIN_INTERSECTION_RATIO = 0.50;

// Tracker tuning
static constexpr int    MAX_MISSED_FRAMES              = 75;
static constexpr double MAX_MATCH_DISTANCE_PIXELS      = 300.0;
static constexpr double MIN_IOU_FOR_MATCH              = 0.02;
static constexpr int    TRACK_CONFIRMATION_FRAMES      = 4;
static constexpr int    ROI_ENTER_CONFIRMATION_FRAMES  = 3;
static constexpr int    ROI_EXIT_RESET_FRAMES          = 3;
static constexpr double DUPLICATE_IOU_THRESHOLD        = 0.35;
static constexpr double DUPLICATE_CENTER_DISTANCE_PX   = 120.0;
static constexpr double BBOX_SMOOTHING_ALPHA           = 0.50;
static constexpr double MIN_BBOX_AREA_PIXELS           = 0.0;
static constexpr double MIN_CONFIDENCE                 = 0.20;

static constexpr bool DEBUG_METADATA        = false;
static constexpr int  DEBUG_FIRST_N_BUFFERS = 3;
static constexpr bool DEBUG_TRACKING        = false;

// =============================================================================
// Data models
// =============================================================================
using BBox = std::array<double, 4>; // x1, y1, x2, y2 in display pixels

struct Detection {
    BBox                    bbox;
    std::string             label;
    double                  confidence = -1.0; // negative means not present

    std::pair<double,double> centroid() const {
        return { (bbox[0] + bbox[2]) / 2.0, (bbox[1] + bbox[3]) / 2.0 };
    }
};

struct Track {
    int                      object_id;
    BBox                     bbox;
    std::pair<double,double> centroid;
    std::string              label;
    int                      missed_frames         = 0;
    int                      age                   = 1;
    int                      hits                  = 1;
    bool                     counted               = false;
    int                      inside_roi_hits       = 0;
    int                      outside_roi_frames    = 0;
    bool                     current_visit_counted = false;
    std::pair<double,double> velocity              = {0.0, 0.0};
};
// =============================================================================
// String / key normalisation
// =============================================================================
static std::string normalize_key(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (c == '-' || c == ' ') out += '_';
        else out += static_cast<char>(std::tolower(c));
    }
    return out;
}
// =============================================================================
// Minimal JSON helpers (hand-rolled, no external dependencies)
// =============================================================================
static void skip_ws(const std::string& s, size_t& i)
{
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
}
static std::string parse_json_string(const std::string& s, size_t& i)
{
    std::string out;
    if (i >= s.size() || s[i] != '"') return out;
    ++i;
    while (i < s.size() && s[i] != '"') {
        if (s[i] == '\\' && i + 1 < s.size()) {
            ++i;
            switch (s[i]) {
                case '"':  out += '"';  break;
                case '\\': out += '\\'; break;
                case '/':  out += '/';  break;
                case 'n':  out += '\n'; break;
                case 'r':  out += '\r'; break;
                case 't':  out += '\t'; break;
                default:   out += s[i]; break;
            }
        } else {
            out += s[i];
        }
        ++i;
    }
    if (i < s.size()) ++i; // skip closing quote
    return out;
}
static std::string parse_json_number(const std::string& s, size_t& i)
{
    size_t start = i;
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) ++i;
    while (i < s.size() &&
           (std::isdigit(static_cast<unsigned char>(s[i])) ||
            s[i] == '.' || s[i] == 'e' || s[i] == 'E' ||
            s[i] == '+' || s[i] == '-'))
        ++i;
    return s.substr(start, i - start);
}
// Forward declaration
struct JsonValue;
static JsonValue parse_json_value(const std::string& s, size_t& i);
// A minimal JSON value representation sufficient for detection counting.
struct JsonValue {
    enum Type { Null, Bool, Number, String, Array, Object };
    Type                                    type  = Null;
    std::string                             str;   // String or Number
    std::vector<JsonValue>                  arr;   // Array
    std::vector<std::pair<std::string, JsonValue>> obj; // Object (ordered)
    // Convenience: find a key in an object (case-insensitive normalised)
    const JsonValue* find(const std::set<std::string>& wanted_keys) const {
        if (type != Object) return nullptr;
        std::set<std::string> norm_wanted;
        for (const auto& k : wanted_keys) norm_wanted.insert(normalize_key(k));
        for (const auto& kv : obj) {
            if (norm_wanted.count(normalize_key(kv.first)))
                return &kv.second;
        }
        return nullptr;
    }
    // Normalised key set of this object
    std::set<std::string> norm_keys() const {
        std::set<std::string> out;
        if (type != Object) return out;
        for (const auto& kv : obj) out.insert(normalize_key(kv.first));
        return out;
    }
};
static JsonValue parse_json_array(const std::string& s, size_t& i);
static JsonValue parse_json_object(const std::string& s, size_t& i);
static JsonValue parse_json_value(const std::string& s, size_t& i)
{
    skip_ws(s, i);
    JsonValue v;
    if (i >= s.size()) return v;
    if (s[i] == '"') {
        v.type = JsonValue::String;
        v.str  = parse_json_string(s, i);
    } else if (s[i] == '{') {
        v = parse_json_object(s, i);
    } else if (s[i] == '[') {
        v = parse_json_array(s, i);
    } else if (s[i] == 't') {
        v.type = JsonValue::Bool; v.str = "true";
        i += 4;
    } else if (s[i] == 'f') {
        v.type = JsonValue::Bool; v.str = "false";
        i += 5;
    } else if (s[i] == 'n') {
        v.type = JsonValue::Null;
        i += 4;
    } else if (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '-') {
        v.type = JsonValue::Number;
        v.str  = parse_json_number(s, i);
    }
    return v;
}
static JsonValue parse_json_array(const std::string& s, size_t& i)
{
    JsonValue v;
    v.type = JsonValue::Array;
    if (i >= s.size() || s[i] != '[') return v;
    ++i;
    while (i < s.size()) {
        skip_ws(s, i);
        if (i >= s.size() || s[i] == ']') { ++i; break; }
        if (s[i] == ',') { ++i; continue; }
        v.arr.push_back(parse_json_value(s, i));
    }
    return v;
}
static JsonValue parse_json_object(const std::string& s, size_t& i)
{
    JsonValue v;
    v.type = JsonValue::Object;
    if (i >= s.size() || s[i] != '{') return v;
    ++i;
    while (i < s.size()) {
        skip_ws(s, i);
        if (i >= s.size() || s[i] == '}') { ++i; break; }
        if (s[i] == ',') { ++i; continue; }
        if (s[i] != '"') { ++i; continue; }
        std::string key = parse_json_string(s, i);
        skip_ws(s, i);
        if (i >= s.size() || s[i] != ':') continue;
        ++i;
        JsonValue val = parse_json_value(s, i);
        v.obj.emplace_back(std::move(key), std::move(val));
    }
    return v;
}
// Parse one or more concatenated JSON values from a raw buffer.
static std::vector<JsonValue> parse_json_records(const std::string& raw)
{
    // Strip null bytes
    std::string text;
    text.reserve(raw.size());
    for (char c : raw) if (c != '\0') text += c;
    std::vector<JsonValue> records;
    size_t i = 0;
    while (i < text.size()) {
        skip_ws(text, i);
        if (i >= text.size()) break;
        if (text[i] == '{' || text[i] == '[') {
            size_t before = i;
            JsonValue v = parse_json_value(text, i);
            if (i > before && v.type != JsonValue::Null)
                records.push_back(std::move(v));
        } else {
            ++i;
        }
    }
    return records;
}
// =============================================================================
// Detection counting logic
// =============================================================================
// Key sets used to identify detection fields regardless of naming convention
static const std::set<std::string> LABEL_KEYS = {
    "label", "labels", "name", "class", "class_name",
    "class_id", "object", "type"
};
static const std::set<std::string> RECT_KEYS = {
    "bbox", "box", "rect", "rectangle",
    "bounding_box", "boundingbox", "roi"
};
static const std::set<std::string> SCORE_KEYS = {
    "confidence", "conf", "score", "prob", "probability"
};
static const std::set<std::string> CONTAINER_KEYS = {
    "objects", "object_detection", "detections", "detection",
    "predictions", "prediction", "results", "result",
    "items", "regions", "rois"
};
// Extracts the label string from a JsonValue object.
static std::string extract_label(const JsonValue& v);
static std::string extract_label(const JsonValue& v)
{
    if (v.type == JsonValue::Object) {
        const JsonValue* lv = v.find(LABEL_KEYS);
        if (!lv) return "";
        return extract_label(*lv);
    }
    if (v.type == JsonValue::String) {
        std::string s = v.str;
        // trim
        size_t b = s.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) return "";
        size_t e = s.find_last_not_of(" \t\r\n");
        return s.substr(b, e - b + 1);
    }
    if (v.type == JsonValue::Number) return v.str;
    if (v.type == JsonValue::Array && !v.arr.empty()) {
        if (v.arr[0].type == JsonValue::Object) return extract_label(v.arr[0]);
        return v.arr[0].str;
    }
    return "";
}
// Returns true if the object looks like a single detection entry.
static bool looks_like_detection(const JsonValue& v)
{
    if (v.type != JsonValue::Object) return false;
    auto keys = v.norm_keys();
    std::set<std::string> norm_rect, norm_label, norm_score;
    for (const auto& k : RECT_KEYS)  norm_rect.insert(normalize_key(k));
    for (const auto& k : LABEL_KEYS) norm_label.insert(normalize_key(k));
    for (const auto& k : SCORE_KEYS) norm_score.insert(normalize_key(k));
    bool has_rect  = false, has_label = false, has_score = false;
    for (const auto& k : keys) {
        if (norm_rect.count(k))  has_rect  = true;
        if (norm_label.count(k)) has_label = true;
        if (norm_score.count(k)) has_score = true;
    }
    return has_rect || (has_label && has_score);
}
// Returns true if the label string passes the COUNT_ONLY_LABELS filter.
static bool label_allowed(const std::string& lbl)
{
    if (COUNT_ONLY_LABELS.empty()) return true;
    if (lbl.empty()) return false;
    std::string lbl_lower;
    for (unsigned char c : lbl) lbl_lower += static_cast<char>(std::tolower(c));
    for (const auto& allowed : COUNT_ONLY_LABELS) {
        std::string a_lower;
        for (unsigned char c : allowed) a_lower += static_cast<char>(std::tolower(c));
        if (lbl_lower == a_lower) return true;
    }
    return false;
}

// Extracts a numeric confidence value from a JsonValue object.
static double extract_confidence(const JsonValue& v)
{
    const JsonValue* sv = v.find(SCORE_KEYS);
    if (!sv) return -1.0;
    if (sv->type == JsonValue::Number || sv->type == JsonValue::String) {
        try { return std::stod(sv->str); } catch (...) {}
    }
    if (sv->type == JsonValue::Object) {
        for (const auto& kv : sv->obj) {
            if (kv.second.type == JsonValue::Number) {
                try { return std::stod(kv.second.str); } catch (...) {}
            }
        }
    }
    if (sv->type == JsonValue::Array) {
        for (const auto& item : sv->arr) {
            if (item.type == JsonValue::Number) {
                try { return std::stod(item.str); } catch (...) {}
            }
        }
    }
    return -1.0;
}

// Clamps a bbox to the display dimensions; returns false if the box is degenerate.
static bool clamp_bbox(BBox& b)
{
    b[0] = std::max(0.0, std::min(static_cast<double>(WIDTH  - 1), b[0]));
    b[1] = std::max(0.0, std::min(static_cast<double>(HEIGHT - 1), b[1]));
    b[2] = std::max(0.0, std::min(static_cast<double>(WIDTH  - 1), b[2]));
    b[3] = std::max(0.0, std::min(static_cast<double>(HEIGHT - 1), b[3]));
    return b[2] > b[0] && b[3] > b[1];
}

// Scales coordinates to pixels if they appear to be normalised (0..1).
static BBox scale_if_normalized(double x1, double y1, double x2, double y2)
{
    if (x1 >= -0.01 && x1 <= 1.50 &&
        y1 >= -0.01 && y1 <= 1.50 &&
        x2 >= -0.01 && x2 <= 1.50 &&
        y2 >= -0.01 && y2 <= 1.50)
        return { x1 * WIDTH, y1 * HEIGHT, x2 * WIDTH, y2 * HEIGHT };
    return { x1, y1, x2, y2 };
}

// Forward declaration needed because parse_bbox_from_dict calls extract_bbox.
static std::optional<BBox> extract_bbox(const JsonValue& v);

// Parses a bbox from a JSON array value [x1,y1,x2,y2] or [x,y,w,h].
static std::optional<BBox> parse_bbox_from_sequence(const JsonValue& v)
{
    if (v.type != JsonValue::Array || v.arr.size() < 4) return std::nullopt;
    double vals[4];
    for (int i = 0; i < 4; ++i) {
        if (v.arr[i].type != JsonValue::Number && v.arr[i].type != JsonValue::String)
            return std::nullopt;
        try { vals[i] = std::stod(v.arr[i].str); } catch (...) { return std::nullopt; }
    }
    BBox b;
    if (vals[2] > vals[0] && vals[3] > vals[1])
        b = scale_if_normalized(vals[0], vals[1], vals[2], vals[3]);
    else
        b = scale_if_normalized(vals[0], vals[1], vals[0] + vals[2], vals[1] + vals[3]);
    if (!clamp_bbox(b)) return std::nullopt;
    return b;
}

// Parses a bbox from a JSON object using left/top/right/bottom or x/y/w/h keys.
static std::optional<BBox> parse_bbox_from_dict(const JsonValue& v)
{
    if (v.type != JsonValue::Object) return std::nullopt;

    auto number_for = [&](std::initializer_list<const char*> names) -> std::optional<double> {
        for (const char* name : names) {
            const JsonValue* found = v.find({name});
            if (found && (found->type == JsonValue::Number || found->type == JsonValue::String)) {
                try { return std::stod(found->str); } catch (...) {}
            }
        }
        return std::nullopt;
    };

    // left/top/right/bottom style
    auto left   = number_for({"left",  "l", "xmin", "x_min", "x1"});
    auto top    = number_for({"top",   "t", "ymin", "y_min", "y1"});
    auto right  = number_for({"right", "r", "xmax", "x_max", "x2"});
    auto bottom = number_for({"bottom","b", "ymax", "y_max", "y2"});
    if (left && top && right && bottom) {
        BBox b = scale_if_normalized(*left, *top, *right, *bottom);
        if (clamp_bbox(b)) return b;
    }

    // x/y/width/height style
    auto x = number_for({"x", "left", "xmin", "x_min"});
    auto y = number_for({"y", "top",  "ymin", "y_min"});
    auto w = number_for({"width",  "w"});
    auto h = number_for({"height", "h"});
    if (x && y && w && h) {
        BBox b = scale_if_normalized(*x, *y, *x + *w, *y + *h);
        if (clamp_bbox(b)) return b;
    }

    // Nested fallback: search values of this object recursively.
    for (const auto& kv : v.obj) {
        auto nested = extract_bbox(kv.second);
        if (nested) return nested;
    }
    return std::nullopt;
}

static std::optional<BBox> extract_bbox(const JsonValue& v)
{
    if (v.type == JsonValue::Object)  return parse_bbox_from_dict(v);
    if (v.type == JsonValue::Array) {
        // Try as a flat numeric sequence first.
        if (!v.arr.empty() &&
            v.arr[0].type != JsonValue::Object &&
            v.arr[0].type != JsonValue::Array)
            return parse_bbox_from_sequence(v);
        // Otherwise recurse into each element.
        for (const auto& item : v.arr) {
            auto b = extract_bbox(item);
            if (b) return b;
        }
    }
    return std::nullopt;
}

// Builds a Detection from a JSON object that looks_like_detection.
static std::optional<Detection> detection_from_dict(const JsonValue& v)
{
    if (v.type != JsonValue::Object) return std::nullopt;

    // Try known rect keys first.
    std::optional<BBox> bbox;
    for (const auto& kv : v.obj) {
        std::set<std::string> rk_norm;
        for (const auto& k : RECT_KEYS) rk_norm.insert(normalize_key(k));
        if (rk_norm.count(normalize_key(kv.first))) {
            bbox = extract_bbox(kv.second);
            if (bbox) break;
        }
    }
    // Fallback: try to parse x/y/w/h or left/top/right/bottom directly.
    if (!bbox) bbox = parse_bbox_from_dict(v);
    if (!bbox) return std::nullopt;

    std::string lbl = extract_label(v);
    if (!label_allowed(lbl)) return std::nullopt;

    double conf = extract_confidence(v);
    if (conf >= 0.0 && conf < MIN_CONFIDENCE) return std::nullopt;

    Detection d;
    d.bbox       = *bbox;
    d.label      = lbl;
    d.confidence = conf;
    return d;
}

// Recursively extracts all Detection objects from a parsed JSON value.
static std::vector<Detection> extract_detections_from_json_object(const JsonValue& v)
{
    std::vector<Detection> out;
    if (v.type == JsonValue::Array) {
        for (const auto& item : v.arr) {
            auto sub = extract_detections_from_json_object(item);
            out.insert(out.end(), sub.begin(), sub.end());
        }
        return out;
    }
    if (v.type != JsonValue::Object) return out;

    if (looks_like_detection(v)) {
        auto d = detection_from_dict(v);
        if (d) out.push_back(*d);
        return out;
    }

    // Prefer known container keys.
    std::set<std::string> norm_containers;
    for (const auto& k : CONTAINER_KEYS) norm_containers.insert(normalize_key(k));
    bool container_found = false;
    for (const auto& kv : v.obj) {
        if (norm_containers.count(normalize_key(kv.first))) {
            container_found = true;
            auto sub = extract_detections_from_json_object(kv.second);
            out.insert(out.end(), sub.begin(), sub.end());
        }
    }
    if (container_found) return out;

    // Fallback: walk all values.
    for (const auto& kv : v.obj) {
        auto sub = extract_detections_from_json_object(kv.second);
        out.insert(out.end(), sub.begin(), sub.end());
    }
    return out;
}

// Top-level: extract all detections from a raw metadata text buffer.
static std::vector<Detection> detections_from_text(const std::string& raw)
{
    std::vector<Detection> out;
    for (const auto& rec : parse_json_records(raw)) {
        auto sub = extract_detections_from_json_object(rec);
        out.insert(out.end(), sub.begin(), sub.end());
    }
    return out;
}
// =============================================================================
// ROI helpers
// =============================================================================
static double bbox_area(const BBox& b)
{
    return std::max(0.0, b[2] - b[0]) * std::max(0.0, b[3] - b[1]);
}

static bool point_inside_roi(double x, double y)
{
    return x >= ROI_X1 && x <= ROI_X2 && y >= ROI_Y1 && y <= ROI_Y2;
}

static double bbox_intersection_area(const BBox& a, const BBox& b)
{
    double ix1 = std::max(a[0], b[0]);
    double iy1 = std::max(a[1], b[1]);
    double ix2 = std::min(a[2], b[2]);
    double iy2 = std::min(a[3], b[3]);
    return std::max(0.0, ix2 - ix1) * std::max(0.0, iy2 - iy1);
}

static bool detection_inside_roi(const Detection& det)
{
    std::string mode(ROI_INCLUSION_MODE);
    if (mode == "intersection") {
        double area = std::max(1.0, bbox_area(det.bbox));
        BBox roi = { ROI_X1, ROI_Y1, ROI_X2, ROI_Y2 };
        return (bbox_intersection_area(det.bbox, roi) / area) >= ROI_MIN_INTERSECTION_RATIO;
    }
    auto [cx, cy] = det.centroid();
    return point_inside_roi(cx, cy);
}

// =============================================================================
// CumulativeObjectTracker
// =============================================================================
class CumulativeObjectTracker
{
public:
    int    total_count = 0;

    CumulativeObjectTracker() = default;

    // Returns (total_count, snapshot of active tracks).
    std::pair<int, std::vector<Track>> update(const std::vector<Detection>& raw_detections)
    {
        auto detections = deduplicate(raw_detections);

        if (detections.empty()) {
            for (auto& [id, t] : tracks_) {
                ++t.missed_frames;
                mark_outside_roi(t);
            }
            remove_stale();
            return { total_count, active_tracks() };
        }

        if (tracks_.empty()) {
            for (const auto& det : detections) register_track(det);
            return { total_count, active_tracks() };
        }

        // Build scored candidates.
        struct Candidate { double score; int id; int det_idx; };
        std::vector<Candidate> candidates;
        std::vector<std::pair<int, Track*>> track_items;
        for (auto& [id, t] : tracks_) track_items.push_back({id, &t});

        for (auto& [id, tp] : track_items) {
            auto pred = predicted_centroid(*tp);
            for (int di = 0; di < static_cast<int>(detections.size()); ++di) {
                const auto& det = detections[di];
                if (!same_label(tp->label, det.label)) continue;
                double dist = distance(pred, det.centroid());
                double iou  = calc_iou(tp->bbox, det.bbox);
                double allowed = adaptive_distance(*tp, det);
                if (dist <= allowed || iou >= MIN_IOU_FOR_MATCH) {
                    double score = (1.0 - std::min(iou, 1.0)) * 10000.0
                                   + dist
                                   + tp->missed_frames * 20.0;
                    candidates.push_back({score, id, di});
                }
            }
        }
        std::sort(candidates.begin(), candidates.end(),
                  [](const Candidate& a, const Candidate& b){ return a.score < b.score; });

        std::set<int> matched_tracks, matched_dets;
        for (const auto& c : candidates) {
            if (matched_tracks.count(c.id) || matched_dets.count(c.det_idx)) continue;
            update_from_detection(tracks_.at(c.id), detections[c.det_idx]);
            matched_tracks.insert(c.id);
            matched_dets.insert(c.det_idx);
        }

        for (auto& [id, t] : tracks_) {
            if (!matched_tracks.count(id)) {
                ++t.missed_frames;
                mark_outside_roi(t);
            }
        }

        for (int di = 0; di < static_cast<int>(detections.size()); ++di) {
            if (matched_dets.count(di)) continue;
            if (!looks_like_existing(detections[di])) register_track(detections[di]);
        }

        remove_stale();
        return { total_count, active_tracks() };
    }

private:
    std::map<int, Track> tracks_;
    int next_id_ = 1;

    static double distance(std::pair<double,double> a, std::pair<double,double> b)
    {
        return std::hypot(a.first - b.first, a.second - b.second);
    }

    static double calc_iou(const BBox& a, const BBox& b)
    {
        double ix1 = std::max(a[0], b[0]), iy1 = std::max(a[1], b[1]);
        double ix2 = std::min(a[2], b[2]), iy2 = std::min(a[3], b[3]);
        double inter = std::max(0.0, ix2 - ix1) * std::max(0.0, iy2 - iy1);
        double ua = bbox_area(a) + bbox_area(b) - inter;
        return ua > 0.0 ? inter / ua : 0.0;
    }

    static double bbox_diagonal(const BBox& b)
    {
        return std::hypot(std::max(0.0, b[2] - b[0]), std::max(0.0, b[3] - b[1]));
    }

    static bool same_label(const std::string& a, const std::string& b)
    {
        if (a.empty() || b.empty()) return true;
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(a[i])) !=
                std::tolower(static_cast<unsigned char>(b[i]))) return false;
        return true;
    }

    double adaptive_distance(const Track& t, const Detection& det) const
    {
        double size_allow = 0.60 * std::max(bbox_diagonal(t.bbox), bbox_diagonal(det.bbox));
        double miss_allow = 25.0 * std::min(t.missed_frames, 5);
        return std::max(MAX_MATCH_DISTANCE_PIXELS, size_allow) + miss_allow;
    }

    std::pair<double,double> predicted_centroid(const Track& t) const
    {
        double mult = 1.0 + std::min(t.missed_frames, 5);
        return { t.centroid.first  + t.velocity.first  * mult,
                 t.centroid.second + t.velocity.second * mult };
    }

    static BBox smooth_bbox(const BBox& old_b, const BBox& new_b)
    {
        double a = BBOX_SMOOTHING_ALPHA;
        return { (1.0-a)*old_b[0] + a*new_b[0],
                 (1.0-a)*old_b[1] + a*new_b[1],
                 (1.0-a)*old_b[2] + a*new_b[2],
                 (1.0-a)*old_b[3] + a*new_b[3] };
    }

    std::vector<Detection> deduplicate(const std::vector<Detection>& dets) const
    {
        // Sort: higher confidence first, then larger area.
        std::vector<const Detection*> ordered;
        for (const auto& d : dets) ordered.push_back(&d);
        std::sort(ordered.begin(), ordered.end(), [](const Detection* a, const Detection* b) {
            double ca = a->confidence >= 0.0 ? a->confidence : 1.0;
            double cb = b->confidence >= 0.0 ? b->confidence : 1.0;
            if (ca != cb) return ca > cb;
            return bbox_area(a->bbox) > bbox_area(b->bbox);
        });

        std::vector<Detection> selected;
        for (const Detection* dp : ordered) {
            if (MIN_BBOX_AREA_PIXELS > 0.0 && bbox_area(dp->bbox) < MIN_BBOX_AREA_PIXELS)
                continue;
            bool dup = false;
            for (const auto& kept : selected) {
                if (!same_label(kept.label, dp->label)) continue;
                if (calc_iou(kept.bbox, dp->bbox) >= DUPLICATE_IOU_THRESHOLD ||
                    distance(kept.centroid(), dp->centroid()) <= DUPLICATE_CENTER_DISTANCE_PX) {
                    dup = true; break;
                }
            }
            if (!dup) selected.push_back(*dp);
        }
        return selected;
    }

    void update_roi_visit(Track& t, bool inside)
    {
        if (inside) {
            ++t.inside_roi_hits;
            t.outside_roi_frames = 0;
            if (!t.current_visit_counted
                && t.hits >= TRACK_CONFIRMATION_FRAMES
                && t.inside_roi_hits >= ROI_ENTER_CONFIRMATION_FRAMES) {
                t.current_visit_counted = true;
                t.counted = true;
                ++total_count;
                if (DEBUG_TRACKING)
                    std::cout << "[TRACK] Counted ROI visit for ID " << t.object_id
                              << ". Total=" << total_count << "\n";
            }
        } else {
            t.inside_roi_hits = 0;
            ++t.outside_roi_frames;
            if (t.outside_roi_frames >= ROI_EXIT_RESET_FRAMES)
                t.current_visit_counted = false;
        }
    }

    void mark_outside_roi(Track& t) { update_roi_visit(t, false); }

    void register_track(const Detection& det)
    {
        Track t;
        t.object_id = next_id_++;
        t.bbox      = det.bbox;
        t.centroid  = det.centroid();
        t.label     = det.label;
        tracks_[t.object_id] = t;
        update_roi_visit(tracks_[t.object_id], detection_inside_roi(det));
        if (DEBUG_TRACKING)
            std::cout << "[TRACK] New ID " << t.object_id
                      << " label=" << t.label << "\n";
    }

    void remove_stale()
    {
        for (auto it = tracks_.begin(); it != tracks_.end(); ) {
            if (it->second.missed_frames > MAX_MISSED_FRAMES) {
                if (DEBUG_TRACKING)
                    std::cout << "[TRACK] ID " << it->first << " left frame.\n";
                it = tracks_.erase(it);
            } else {
                ++it;
            }
        }
    }

    bool looks_like_existing(const Detection& det) const
    {
        for (const auto& [id, t] : tracks_) {
            if (!same_label(t.label, det.label)) continue;
            double dup_dist = std::max(DUPLICATE_CENTER_DISTANCE_PX,
                                       0.35 * bbox_diagonal(t.bbox));
            if (calc_iou(t.bbox, det.bbox) >= 0.20 ||
                distance(predicted_centroid(t), det.centroid()) <= dup_dist)
                return true;
        }
        return false;
    }

    void update_from_detection(Track& t, const Detection& det)
    {
        auto old_centroid = t.centroid;
        BBox sb = smooth_bbox(t.bbox, det.bbox);
        t.bbox     = sb;
        t.centroid = { (sb[0]+sb[2])/2.0, (sb[1]+sb[3])/2.0 };
        auto dc = det.centroid();
        double mvx = dc.first  - old_centroid.first;
        double mvy = dc.second - old_centroid.second;
        t.velocity = { 0.70*t.velocity.first  + 0.30*mvx,
                       0.70*t.velocity.second + 0.30*mvy };
        if (!det.label.empty()) t.label = det.label;
        t.missed_frames = 0;
        ++t.age;
        ++t.hits;
        update_roi_visit(t, detection_inside_roi(det));
    }

    std::vector<Track> active_tracks() const
    {
        std::vector<Track> out;
        out.reserve(tracks_.size());
        for (const auto& [id, t] : tracks_) out.push_back(t);
        return out;
    }
};

// =============================================================================
// CountState — thread-safe wrapper around the tracker
// =============================================================================
struct CountState {
    std::mutex               mtx;
    CumulativeObjectTracker  tracker;
    int                      last_display_count = -1;
    int                      debug_buffer_index =  0;

    // Returns (total_count, changed).
    std::pair<int,bool> update(const std::vector<Detection>& dets)
    {
        std::lock_guard<std::mutex> lk(mtx);
        auto [total, _] = tracker.update(dets);
        bool changed = (total != last_display_count);
        last_display_count = total;
        return { total, changed };
    }

    int get_count()
    {
        std::lock_guard<std::mutex> lk(mtx);
        return std::max(0, last_display_count);
    }
};
static CountState g_state;

// =============================================================================
// Metadata AppSink callback
// =============================================================================
static int g_debug_buffer_index = 0;

static void on_sample(qti::Buffer buffer)
{
    const qti::Buffer& cbuffer = buffer;
    if (!cbuffer.valid() || !cbuffer.data()) return;

    std::string raw_text(reinterpret_cast<const char*>(cbuffer.data()), cbuffer.size());

    if (DEBUG_METADATA) {
        ++g_debug_buffer_index;
        if (g_debug_buffer_index <= DEBUG_FIRST_N_BUFFERS) {
            std::string preview = raw_text.substr(0, std::min<size_t>(700, raw_text.size()));
            for (char& c : preview) if (c == '\n') c = ' ';
            std::cout << "[metadata sample " << g_debug_buffer_index << "] " << preview << "\n";
        }
    }

    auto detections = detections_from_text(raw_text);
    auto [total, changed] = g_state.update(detections);
    if (changed)
        std::cout << COUNT_TEXT_PREFIX << ": " << total << "\n";
}

// =============================================================================
// Cairo overlay draw callback
// =============================================================================
static void on_cairo_draw(void* draw_context, std::uint64_t /*timestamp*/,
                          std::uint64_t /*duration*/)
{
    if (!draw_context) return;
    cairo_t* cr = static_cast<cairo_t*>(draw_context);

    // Draw the translucent green ROI rectangle.
    double half_line = ROI_BORDER_LINE_WIDTH / 2.0;
    double roi_w = ROI_X2 - ROI_X1;
    double roi_h = ROI_Y2 - ROI_Y1;

    cairo_set_source_rgba(cr, 0.0, 1.0, 0.0, ROI_FILL_ALPHA);
    cairo_rectangle(cr,
        ROI_X1 + half_line,
        ROI_Y1 + half_line,
        std::max(1.0, roi_w - ROI_BORDER_LINE_WIDTH),
        std::max(1.0, roi_h - ROI_BORDER_LINE_WIDTH));
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 0.0, 1.0, 0.0, 0.95);
    cairo_set_line_width(cr, ROI_BORDER_LINE_WIDTH);
    cairo_stroke(cr);

    // Draw the top-center cumulative count banner.
    int count = g_state.get_count();
    std::string text = std::string(COUNT_TEXT_PREFIX) + ": " + std::to_string(count);

    const double font_size  = 46.0;
    const double margin_top = 28.0;
    const double pad_x      = 24.0;
    const double pad_y      = 14.0;

    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, font_size);

    cairo_text_extents_t ext;
    cairo_text_extents(cr, text.c_str(), &ext);

    double text_x = (WIDTH  - ext.width)  / 2.0 - ext.x_bearing;
    double text_y =  margin_top + ext.height;

    // Dark translucent background pill.
    double bg_x = (WIDTH - ext.width) / 2.0 - pad_x;
    double bg_y =  margin_top - pad_y;
    double bg_w =  ext.width  + 2.0 * pad_x;
    double bg_h =  ext.height + 2.0 * pad_y;
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.65);
    cairo_rectangle(cr, bg_x, bg_y, bg_w, bg_h);
    cairo_fill(cr);

    // White text.
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
    cairo_move_to(cr, text_x, text_y);
    cairo_show_text(cr, text.c_str());

}

static void on_cairo_draw_signal(void* /*overlay*/, void* draw_context,
                                 std::uint64_t timestamp, std::uint64_t duration,
                                 void* /*user_data*/)
{
    on_cairo_draw(draw_context, timestamp, duration);
}
// =============================================================================
// Pipeline construction
// =============================================================================
static Element make_queue(const std::string& name)
{
    return Element("queue", name);
}

//  Example pipeline:
//
//    v4l2src -> qtivtransform -> tee name=split
//      split. -> qtimetamux (video)
//      split. -> qtimlvconverter -> qtimltflite -> qtimlpostprocess -> tee
//                -> qtimetamux
//                -> qtimlmetaparser -> appsink(count logic)
//    qtimetamux -> qtivoverlay -> qtivtransform(BGRA) -> cairooverlay -> waylandsink
//
//  The metadata callback performs ID tracking and cumulative ROI counting;
//  Cairo draws ROI geometry and the count HUD on every frame.
void create_and_execute_pipeline()
{
    // Camera source (USB V4L2 device).
    Element source("v4l2src", "source");
    source.set("device", input_config);

    // Video transform stage.
    Element transform("qtivtransform", "transform");
    auto videofilter = VideoFilter()
        .format("NV12")
        .resolution(WIDTH, HEIGHT)
        .framerate(FPS);

    // Stream split (tee).
    Element split("tee", "split");

    // Display branch: video to the metadata muxer.
    Element q_video        = make_queue("q_video");

    // ML inference branch.
    Element q_ml_1 = make_queue("q_ml_1");
    // ML preprocessor/converter.
    Element preprocessing("qtimlvconverter", "preprocessing");
    Element q_ml_2 = make_queue("q_ml_2");
    // TFLite inference stage.
    Element inferencing("qtimltflite", "inferencing");
    inferencing.set("delegate", "external");
    inferencing.set("external-delegate-path", "libQnnTFLiteDelegate.so");
    inferencing.set("external-delegate-options", "QNNExternalDelegate,backend_type=htp;");
    inferencing.set("model", model_base_path + "/models/yolov8_det_quantized.tflite");
    Element q_ml_3 = make_queue("q_ml_3");
    // ML postprocess stage.
    Element postprocessing("qtimlpostprocess", "postprocessing");
    postprocessing.set("module", "yolov8");
    postprocessing.set("labels", model_base_path + "/labels/yolov8.json");
    // mlf negotiates text/x-raw caps; must sit before post_split so both
    // downstream branches (muxer and counter) receive the correct caps.
    auto mlf = TextFilter();

    // Splits detection metadata: one copy to the muxer, one to the counter.
    Element post_split("tee", "post_split");

    Element q_meta_to_mux   = make_queue("q_meta_to_mux");
    Element q_meta_to_count = make_queue("q_meta_to_count");

    // Pipeline element.
    Element metaparser_elem("qtimlmetaparser", "metaparser");
    metaparser_elem.set("module", "json");
    Element q_count_sink = make_queue("q_count_sink");

    // Counting AppSink: receives parsed JSON metadata.
    AppSink count_sink("count_sink");
    count_sink.set("sync",  false);
    count_sink.set("max-buffers", 1);
    count_sink.set("drop", true);

    // Muxer + bounding-box overlay.
    Element mlmuxer("qtimetamux", "mlmuxer");
    // Metadata overlay renderer.
    Element overlay("qtivoverlay", "overlay");

    // Convert to BGRA so Cairo can draw on the frame.
    Element display_transform("qtivtransform", "display_transform");
    auto bgrafilter = VideoFilter()
        .format("BGRA")
        .resolution(WIDTH, HEIGHT)
        .framerate(FPS);

    // Cairo overlay draw stage.
    Element roi_overlay("cairooverlay", "roi_overlay");
    roi_overlay.connect_signal(
        "draw",
        reinterpret_cast<Element::SignalCallback>(&on_cairo_draw_signal));

    // Display sink.
    Element display("waylandsink", "display");
    display.set("sync",  true);
    display.set("fullscreen", true);

    Pipeline pipeline("product_counting");
    pipeline
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
        .add_stream_filter("mlf", mlf)
        .add(post_split)
        .add(q_meta_to_mux)
        .add(q_meta_to_count)
        .add(metaparser_elem)
        .add(q_count_sink)
        .add(count_sink)
        .add(mlmuxer)
        .add(overlay)
        .add(display_transform)
        .add_stream_filter("bgrafilter", bgrafilter)
        .add(roi_overlay)
        .add(display)
        // Camera -> NV12 filter -> tee
        .link("source", "transform", "videofilter", "split")
        // Display branch: tee -> queue -> muxer
        .link("split", "q_video", "mlmuxer")
        // ML branch: tee -> preprocess -> infer -> postprocess -> mlf -> post_split
        .link("split", "q_ml_1", "preprocessing", "q_ml_2", "inferencing",
              "q_ml_3", "postprocessing", "mlf", "post_split")
        // Metadata -> muxer branch
        .link("post_split", "q_meta_to_mux", "mlmuxer")
        // Metadata -> counting branch
        .link("post_split", "q_meta_to_count", "metaparser", "q_count_sink", "count_sink")
        // Muxer -> overlay -> BGRA convert -> cairooverlay -> display
        .link("mlmuxer", "overlay", "display_transform", "bgrafilter", "roi_overlay", "display");

    count_sink.set_buffer_consumer(on_sample);

    std::cout << "[INFO] Starting YOLOv8 ROI cumulative object counting pipeline\n";
    std::cout << "[INFO] Camera:  " << input_config << "\n";
    std::cout << "[INFO] Model:   " << model_base_path + "/models/yolov8_det_quantized.tflite" << "\n";
    std::cout << "[INFO] Labels:  " << model_base_path + "/labels/yolov8.json" << "\n";
    std::cout << "[INFO] Parser:  qtimlmetaparser\n";
    std::cout << "[INFO] ROI:     x=[" << ROI_X1 << ".." << ROI_X2
              << "] y=[" << ROI_Y1 << ".." << ROI_Y2 << "]\n";
    if (!COUNT_ONLY_LABELS.empty()) {
        std::cout << "[INFO] Counting only: ";
        for (const auto& l : COUNT_ONLY_LABELS) std::cout << l << " ";
        std::cout << "\n";
    } else {
        std::cout << "[INFO] Counting all detected objects\n";
    }
    std::cout << "[INFO] Press Ctrl+C to stop\n";

    pipeline.execute();
}

}  // namespace

// =============================================================================
// Entry point
// =============================================================================
int main(int argc, char **argv)
{
    if (home_path.empty()) {
        std::cerr << "Error: HOME environment variable is not set." << std::endl;
        return 1;
    }

    // Base path for sample assets; override via --model-base-path argument.
    model_base_path = home_path + "/Downloads/qimsdk_samples";
    input_config = "/dev/video0";

    const std::string default_input_config = input_config;
    const std::string default_model_base_path = model_base_path;

    static struct option long_options[] = {
      {"input-config", required_argument, 0, 'i'},
      {"model-base-path", required_argument, 0, 'm'},
      {"help", no_argument, 0, 'h'},
      {0, 0, 0, 0}
    };

    auto print_usage = [&](std::ostream &out) {
        out << "Usage: " << argv[0] << " [OPTIONS]\n"
            << "\n"
            << "Options:\n"
            << "  -i, --input-config VALUE     Input source configuration (camera number, device, or file path)\n"
            << "                                (default: " << default_input_config << ")\n"
            << "  -m, --model-base-path PATH   Base path for models and labels\n"
            << "                                (default: " << default_model_base_path << ")\n"
            << "  -h, --help                   Show this help message and exit\n";
    };

    opterr = 0;
    int option_index = 0;
    int c;
    while ((c = getopt_long(argc, argv, "m:i:h", long_options, &option_index)) != -1) {
        switch (c) {
            case 'm':
                model_base_path = optarg;
                break;
            case 'i':
                input_config = optarg;
                break;
            case 'h':
                print_usage(std::cout);
                return 0;
            case '?':
            default:
                print_usage(std::cerr);
                return 1;
        }
    }

    if (optind != argc) {
        std::cerr << "Error: unexpected argument '" << argv[optind] << "'\n\n";
        print_usage(std::cerr);
        return 1;
    }

    // Route GStreamer logs through the QIMSDK logger and enable debug output.
    qti::SetImsdkGstLogMode(qti::ImsdkGstLogMode::ImsdkLog);
    qti::SetImsdkLogLevel(qti::ImsdkLogLevel::Debug);

    try {
        create_and_execute_pipeline();
    } catch (const std::exception& ex) {
        std::cerr << "Exception: " << ex.what() << std::endl;
        return 1;
    }
    return 0;
}
