/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>
#include <getopt.h>

#include <cairo/cairo.h>

#include <qti/qimsdk.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

using namespace qti;

namespace {

// =============================================================================
// Configuration
// =============================================================================
static const std::string     home_path      = std::getenv("HOME") ? std::getenv("HOME") : "";
// Input source configuration, overridden via the --input-config argument.
static std::string           input_config;
static constexpr int FRAME_WIDTH  = 1920;
static constexpr int FRAME_HEIGHT = 1080;
static constexpr int FRAME_FPS    = 30;

// Base path for sample assets (media/, models/, labels/ live under it).
// Set from the --model-base-path argument, or the default location.
static std::string           model_base_path;

// Landmark IDs (MediaPipe-style 21-point hand model)
static constexpr int INDEX_TIP_ID = 8;
static constexpr int THUMB_TIP_ID = 4;
static constexpr int PINKY_TIP_ID = 20;

// Gesture thresholds
static constexpr double TIP_MARGIN         = 25;
static constexpr double EXT_MARGIN         = 25;
static constexpr int    STABLE_FRAMES      = 2;
static constexpr double PINKY_BLOCK_MARGIN = 5;
static constexpr double ERASER_MARGIN      = 15;
static constexpr int    ERASER_TO_PEN_BLOCK = 8;

static constexpr double PINCH_PIXEL_THRESHOLD    = 58.0;
static constexpr int    PINCH_COOLDOWN_FRAMES    = 4;
static constexpr double PINCH_DRAG_GRAB_RADIUS   = 220.0;
static constexpr double WORD_CLUSTER_JOIN_RADIUS = 90.0;
static constexpr int    BOARD_MODE_STABLE_FRAMES = 3;

// Pen / eraser
static constexpr double PEN_DOT_SPACING     = 1.0;
static constexpr double PEN_LINE_WIDTH      = 17.0;
static constexpr double MAX_JUMP_PIXELS     = 180.0;
static constexpr double ERASER_RADIUS       = 45.0;
static constexpr double ERASER_PATH_SPACING = 8.0;

static constexpr double PEN_SMOOTHING_ALPHA    = 0.55;
static constexpr double ERASER_SMOOTHING_ALPHA = 0.45;
static constexpr double MIN_DRAW_MOVE          = 1.0;

// Shape recognition
static constexpr int    MIN_SHAPE_POINTS     = 22;
static constexpr double MIN_SHAPE_SIZE       = 90.0;
static constexpr double MIN_SHAPE_PATH       = 160.0;
static constexpr double SHAPE_CONF_THRESHOLD = 0.62;
static constexpr double ARROW_CONF_THRESHOLD = 0.72;

static constexpr int    MIN_PREVIEW_POINTS = 28;
static constexpr double MIN_PREVIEW_DIAG   = 110.0;

static constexpr bool PRINT_STATUS = true;

// Fixed pen color (color-changing intentionally removed, v4)
struct Rgba { double r = 0.0, g = 0.0, b = 0.0, a = 1.0; };
static constexpr Rgba FIXED_PEN_COLOR{0.0, 0.0, 0.0, 1.0};
static constexpr const char* FIXED_PEN_NAME = "RED";

// Handwriting cleanup / beautification
static constexpr bool   HANDWRITING_BEAUTIFY           = true;
static constexpr int    HANDWRITING_SMOOTH_WINDOW      = 10;
static constexpr double HANDWRITING_RESAMPLE_SPACING   = 3.0;
static constexpr double HANDWRITING_MIN_KEEP_DIST       = 2.0;

// Landmark label -> id map (MediaPipe hand landmark naming)
static const std::map<std::string, int> LABEL_TO_ID = {
    {"wrist", 0}, {"thumb cmc", 1}, {"thumb mcp", 2}, {"thumb ip", 3}, {"thumb tip", 4},
    {"index finger mcp", 5}, {"index finger pip", 6}, {"index finger dip", 7}, {"index finger tip", 8},
    {"middle finger mcp", 9}, {"middle finger pip", 10}, {"middle finger dip", 11}, {"middle finger tip", 12},
    {"ring finger mcp", 13}, {"ring finger pip", 14}, {"ring finger dip", 15}, {"ring finger tip", 16},
    {"pinky mcp", 17}, {"pinky pip", 18}, {"pinky dip", 19}, {"pinky tip", 20},
};

// =============================================================================
// Basic geometry types
// =============================================================================
using Point = std::pair<double, double>;

static double point_distance(const Point& a, const Point& b) {
    return std::hypot(b.first - a.first, b.second - a.second);
}

// =============================================================================
// Shape representation
// =============================================================================
struct Shape {
    enum class Type { Circle, Rect, Arrow } type = Type::Circle;
    // circle
    double cx = 0, cy = 0, r = 0;
    // rect
    double x = 0, y = 0, w = 0, h = 0;
    // arrow
    Point start{0, 0}, end{0, 0};

    Rgba   color = FIXED_PEN_COLOR;
    double width = PEN_LINE_WIDTH;
    double confidence = 0.0;
};

struct Segment {
    Point a, b;
    Rgba  color;
    double width = PEN_LINE_WIDTH;
};

// =============================================================================
// PenState — full mutable drawing/gesture state
// =============================================================================
struct PenState {
    std::mutex mtx;

    std::deque<Segment> segments;
    std::deque<Shape>   shapes;

    std::vector<Point>      current_points;
    std::optional<Shape>    preview_shape;
    Rgba                    current_color = FIXED_PEN_COLOR;

    std::optional<Point> prev_pen, prev_eraser;
    std::optional<Point> smooth_pen, smooth_eraser;

    // Mode FSM
    std::string mode = "OFF";
    std::string raw_mode = "OFF";
    std::string candidate_mode = "OFF";
    int candidate_count = 0;
    int eraser_block = 0;
    bool pen_mode = false;
    bool eraser_mode = false;

    // Finger tip positions
    std::optional<Point> index_point, pinky_point, thumb_point;
    std::map<int, double> tip_y;
    std::map<std::string, bool> flags;

    // Fixed pen color bookkeeping (kept for structural consistency even
    // though color-cycling is disabled)
    int         color_idx = 0;
    std::string color_name = FIXED_PEN_NAME;
    Rgba        pen_color = FIXED_PEN_COLOR;
    int         color_flash = 0;

    // Board mode: LETTER (freehand only) vs SHAPE (shape recognition)
    std::string              board_mode = "LETTER";
    std::optional<std::string> board_candidate_mode;
    int  board_candidate_count = 0;
    int  board_flash = 0;

    // Pinch / drag state
    bool   pinch_active = false;
    int    pinch_cooldown = 0;
    long   pinch_last_release = 0;
    long   frame_counter = 0;

    bool                  drag_mode = false;
    std::string           drag_target_type;   // "seg_group" | "shape"
    int                   drag_target_idx = -1;
    std::set<int>         drag_seg_indices;
    std::optional<Point>  drag_anchor;
    std::optional<Point>  drag_origin;
    int                   drag_selected_count = 0;
};

static PenState g_state;

// =============================================================================
// Tiny helpers
// =============================================================================
static int clamp_int(double v, int lo, int hi) {
    return std::max(lo, std::min(static_cast<int>(std::lround(v)), hi));
}

static std::string normalize_key(std::string v) {
    for (auto& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (auto& c : v) if (c == '_' || c == '-') c = ' ';
    // trim
    size_t b = v.find_first_not_of(' ');
    size_t e = v.find_last_not_of(' ');
    if (b == std::string::npos) return "";
    return v.substr(b, e - b + 1);
}

static std::optional<Point> smooth_point(const std::optional<Point>& prev,
                                          const std::optional<Point>& cur,
                                          double alpha) {
    if (!cur) return prev;
    if (!prev) return cur;
    return Point{alpha * cur->first  + (1.0 - alpha) * prev->first,
                 alpha * cur->second + (1.0 - alpha) * prev->second};
}

// =============================================================================
// Minimal JSON parser (hand-rolled, same JsonValue style already used
// in objectcount/main.cc) — sufficient for the landmark metadata produced by
// qtimlmetaparser(module=json) for hlandmark results.
// =============================================================================
static void skip_ws(const std::string& s, size_t& i) {
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
}

static std::string parse_json_string(const std::string& s, size_t& i) {
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
    if (i < s.size()) ++i; // closing quote
    return out;
}

static std::string parse_json_number(const std::string& s, size_t& i) {
    size_t start = i;
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) ++i;
    while (i < s.size() &&
           (std::isdigit(static_cast<unsigned char>(s[i])) ||
            s[i] == '.' || s[i] == 'e' || s[i] == 'E' ||
            s[i] == '+' || s[i] == '-'))
        ++i;
    return s.substr(start, i - start);
}

struct JsonValue;
static JsonValue parse_json_value(const std::string& s, size_t& i);

struct JsonValue {
    enum Type { Null, Bool, Number, String, Array, Object };
    Type type = Null;
    std::string str;
    std::vector<JsonValue> arr;
    std::vector<std::pair<std::string, JsonValue>> obj;

    const JsonValue* find(const std::vector<std::string>& keys) const {
        if (type != Object) return nullptr;
        std::set<std::string> wanted;
        for (const auto& k : keys) wanted.insert(normalize_key(k));
        for (const auto& kv : obj) {
            if (wanted.count(normalize_key(kv.first))) return &kv.second;
        }
        return nullptr;
    }

    std::optional<double> as_number() const {
        if (type == Number || type == String) {
            try { return std::stod(str); } catch (...) { return std::nullopt; }
        }
        return std::nullopt;
    }
};

static JsonValue parse_json_array(const std::string& s, size_t& i) {
    JsonValue v; v.type = JsonValue::Array;
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

static JsonValue parse_json_object(const std::string& s, size_t& i) {
    JsonValue v; v.type = JsonValue::Object;
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

static JsonValue parse_json_value(const std::string& s, size_t& i) {
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
        v.type = JsonValue::Bool; v.str = "true"; i += 4;
    } else if (s[i] == 'f') {
        v.type = JsonValue::Bool; v.str = "false"; i += 5;
    } else if (s[i] == 'n') {
        v.type = JsonValue::Null; i += 4;
    } else if (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '-') {
        v.type = JsonValue::Number;
        v.str  = parse_json_number(s, i);
    }
    return v;
}

// Attempts full-text JSON parse; on failure, retries on the substring
// between the first '{'/'[' and the last matching '}'/']' as a fallback
// for text that has extra characters surrounding the JSON payload.
static std::optional<JsonValue> load_json(const std::string& text) {
    auto try_parse = [](const std::string& s) -> std::optional<JsonValue> {
        size_t i = 0;
        skip_ws(s, i);
        if (i >= s.size() || (s[i] != '{' && s[i] != '[')) return std::nullopt;
        JsonValue v = parse_json_value(s, i);
        if (v.type == JsonValue::Null) return std::nullopt;
        return v;
    };
    if (auto v = try_parse(text)) return v;
    for (auto pr : {std::make_pair('{', '}'), std::make_pair('[', ']')}) {
        size_t b = text.find(pr.first);
        size_t e = text.rfind(pr.second);
        if (b != std::string::npos && e != std::string::npos && e > b) {
            if (auto v = try_parse(text.substr(b, e - b + 1))) return v;
        }
    }
    return std::nullopt;
}

// =============================================================================
// Landmark parsing: extracts the 21-point hand landmark map from parsed
// JSON metadata (point/label extraction and id-collection helpers).
// =============================================================================
using LandmarkMap = std::map<int, Point>;

static std::optional<double> to_pixel_coord(const JsonValue& v, double size) {
    auto n = v.as_number();
    if (!n) return std::nullopt;
    double x = *n;
    if (x >= -0.25 && x <= 2.0) x *= size;
    return x;
}

static std::optional<Point> point_dict(const JsonValue& d);

static std::optional<Point> point_any(const JsonValue& v) {
    if (v.type == JsonValue::Object) return point_dict(v);
    if (v.type == JsonValue::Array && v.arr.size() >= 2) {
        auto x = to_pixel_coord(v.arr[0], FRAME_WIDTH);
        auto y = to_pixel_coord(v.arr[1], FRAME_HEIGHT);
        if (x && y) return Point{*x, *y};
    }
    return std::nullopt;
}

static std::optional<Point> point_dict(const JsonValue& d) {
    if (const JsonValue* xv = d.find({"x", "X", "x_pos", "xpos", "center_x", "centerX", "px", "point_x"})) {
        if (const JsonValue* yv = d.find({"y", "Y", "y_pos", "ypos", "center_y", "centerY", "py", "point_y"})) {
            auto x = to_pixel_coord(*xv, FRAME_WIDTH);
            auto y = to_pixel_coord(*yv, FRAME_HEIGHT);
            if (x && y) return Point{*x, *y};
        }
    }
    if (const JsonValue* rv = d.find({"rectangle", "rect", "bbox", "box", "bounding_box", "bounding-box"})) {
        if (rv->type == JsonValue::Array && rv->arr.size() >= 4) {
            auto x1 = to_pixel_coord(rv->arr[0], FRAME_WIDTH);
            auto y1 = to_pixel_coord(rv->arr[1], FRAME_HEIGHT);
            auto x2 = to_pixel_coord(rv->arr[2], FRAME_WIDTH);
            auto y2 = to_pixel_coord(rv->arr[3], FRAME_HEIGHT);
            if (x1 && y1 && x2 && y2)
                return Point{(*x1 + *x2) / 2.0, (*y1 + *y2) / 2.0};
        }
    }
    for (const char* k : {"point", "position", "coord", "coords", "coordinate", "location"}) {
        if (const JsonValue* pv = d.find({k})) {
            if (auto p = point_any(*pv)) return p;
        }
    }
    return std::nullopt;
}

static std::optional<int> landmark_id(const JsonValue& d) {
    if (const JsonValue* v = d.find({"id", "class_id", "classId", "label_id", "labelId", "index", "idx"})) {
        if (auto n = v->as_number()) {
            int i = static_cast<int>(std::lround(*n));
            if (i >= 0 && i <= 20) return i;
        }
    }
    if (const JsonValue* lv = d.find({"label", "name", "class", "class_name", "display_name"})) {
        std::string label = normalize_key(lv->str);
        auto it = LABEL_TO_ID.find(label);
        if (it != LABEL_TO_ID.end()) return it->second;
    }
    return std::nullopt;
}

static void collect(const JsonValue& obj, LandmarkMap& out) {
    if (obj.type == JsonValue::Array) {
        if (obj.arr.size() >= 21) {
            int hits = 0;
            for (size_t k = 0; k < 21 && k < obj.arr.size(); ++k)
                if (point_any(obj.arr[k])) ++hits;
            if (hits >= 15) {
                for (int k = 0; k < 21; ++k) {
                    if (auto p = point_any(obj.arr[k])) out[k] = *p;
                }
                return;
            }
        }
        for (const auto& x : obj.arr) collect(x, out);
    } else if (obj.type == JsonValue::Object) {
        auto id = landmark_id(obj);
        auto p  = point_dict(obj);
        if (id && p) out[*id] = *p;
        for (const auto& kv : obj.obj) {
            if (kv.second.type == JsonValue::Object || kv.second.type == JsonValue::Array)
                collect(kv.second, out);
        }
    }
}

// Regex-free fallback: scans for "id=<n> ... x=<n> ... y=<n>" (or y-before-x)
// patterns directly in the raw text, for cases where JSON parsing didn't
// yield enough landmarks.
static LandmarkMap parse_landmarks_fallback(const std::string& text) {
    LandmarkMap out;
    std::string s;
    s.reserve(text.size());
    bool last_space = false;
    for (char c : text) {
        char cc = std::isspace(static_cast<unsigned char>(c)) ? ' ' : c;
        if (cc == ' ' && last_space) continue;
        s += cc;
        last_space = (cc == ' ');
    }

    auto find_number_after = [&](const std::string& hay, size_t from,
                                  const std::vector<std::string>& keys,
                                  size_t max_span) -> std::optional<std::pair<double, size_t>> {
        size_t best_pos = std::string::npos;
        for (const auto& key : keys) {
            size_t pos = hay.find(key, from);
            if (pos != std::string::npos && pos - from <= max_span &&
                (best_pos == std::string::npos || pos < best_pos)) {
                best_pos = pos;
            }
        }
        if (best_pos == std::string::npos) return std::nullopt;
        size_t j = best_pos;
        // skip key text + separators
        while (j < hay.size() && hay[j] != '=' && hay[j] != ':') ++j;
        if (j < hay.size()) ++j;
        while (j < hay.size() && hay[j] == ' ') ++j;
        size_t start = j;
        if (j < hay.size() && (hay[j] == '-' || hay[j] == '+')) ++j;
        while (j < hay.size() && (std::isdigit(static_cast<unsigned char>(hay[j])) || hay[j] == '.')) ++j;
        if (j == start) return std::nullopt;
        try {
            return std::make_pair(std::stod(hay.substr(start, j - start)), j);
        } catch (...) { return std::nullopt; }
    };

    static const std::vector<std::string> id_keys = {"id=", "id:", "class_id=", "classId=", "label_id=", "labelId=", "index=", "idx="};
    static const std::vector<std::string> x_keys   = {"x=", "x:", "X=", "X:"};
    static const std::vector<std::string> y_keys   = {"y=", "y:", "Y=", "Y:"};

    size_t pos = 0;
    while (pos < s.size()) {
        auto id_match = find_number_after(s, pos, id_keys, s.size());
        if (!id_match) break;
        double id_val = id_match->first;
        size_t after_id = id_match->second;

        auto x_match = find_number_after(s, after_id, x_keys, 220);
        auto y_match = find_number_after(s, after_id, y_keys, 300);
        if (x_match && y_match) {
            int id = static_cast<int>(std::lround(id_val));
            if (id >= 0 && id <= 20) {
                double x = x_match->first, y = y_match->first;
                if (x >= -0.25 && x <= 2.0) x *= FRAME_WIDTH;
                if (y >= -0.25 && y <= 2.0) y *= FRAME_HEIGHT;
                out[id] = Point{x, y};
            }
        }
        pos = after_id + 1;
    }
    return out;
}

// Top-level landmark parser: JSON first, regex fallback if <21 points found,
// then clamp all points to the frame bounds.
static LandmarkMap parse_landmarks(const std::string& text) {
    LandmarkMap out;
    if (auto obj = load_json(text)) collect(*obj, out);
    if (out.size() < 21) {
        auto extra = parse_landmarks_fallback(text);
        for (auto& kv : extra) out[kv.first] = kv.second;
    }
    LandmarkMap clamped;
    for (auto& kv : out) {
        if (kv.first < 0 || kv.first > 20) continue;
        double x = clamp_int(kv.second.first, 0, FRAME_WIDTH - 1);
        double y = clamp_int(kv.second.second, 0, FRAME_HEIGHT - 1);
        clamped[kv.first] = Point{x, y};
    }
    return clamped;
}

// =============================================================================
// Gesture logic: flag computation and gesture intents (pinky/pinch/open
// palm/closed palm), board-mode switching, and raw/stable_mode mode detection.
// =============================================================================
static bool has_ids(const LandmarkMap& lms, const std::vector<int>& ids) {
    for (int i : ids) if (!lms.count(i)) return false;
    return true;
}

static bool extended(const LandmarkMap& lms, int tip, int pip) {
    auto t = lms.find(tip), p = lms.find(pip);
    if (t == lms.end() || p == lms.end()) return false;
    return t->second.second + EXT_MARGIN < p->second.second;
}

static std::map<std::string, bool> compute_flags(const LandmarkMap& lms) {
    return {
        {"index",  extended(lms, 8, 6)},
        {"middle", extended(lms, 12, 10)},
        {"ring",   extended(lms, 16, 14)},
        {"pinky",  extended(lms, 20, 18)},
    };
}

static bool highest(const LandmarkMap& lms, int active, const std::vector<int>& others, double margin) {
    std::vector<int> all_ids = others;
    all_ids.push_back(active);
    if (!has_ids(lms, all_ids)) return false;
    double y = lms.at(active).second;
    for (int i : others) {
        if (!(y + margin < lms.at(i).second)) return false;
    }
    return true;
}

static bool pinky_intent(const LandmarkMap& lms, const std::map<std::string, bool>& f) {
    if (f.at("pinky")) return true;
    auto it20 = lms.find(20), it18 = lms.find(18);
    if (it20 != lms.end() && it18 != lms.end() &&
        it20->second.second + PINKY_BLOCK_MARGIN < it18->second.second)
        return true;
    return has_ids(lms, {20, 4, 12, 16}) && highest(lms, 20, {4, 12, 16}, ERASER_MARGIN);
}

static bool pinch_intent(const LandmarkMap& lms) {
    if (!has_ids(lms, {THUMB_TIP_ID, INDEX_TIP_ID})) return false;
    return point_distance(lms.at(THUMB_TIP_ID), lms.at(INDEX_TIP_ID)) <= PINCH_PIXEL_THRESHOLD;
}

// Open palm -> LETTER mode. Uses the four long fingers only; thumb
// orientation is intentionally ignored (mirror-flip / handedness noise).
static bool open_palm_intent(const std::map<std::string, bool>& f) {
    return f.at("index") && f.at("middle") && f.at("ring") && f.at("pinky");
}

static bool folded(const LandmarkMap& lms, int tip, int pip) {
    auto t = lms.find(tip), p = lms.find(pip);
    if (t == lms.end() || p == lms.end()) return false;
    return t->second.second > p->second.second - 8;
}

// Closed palm / fist -> SHAPE mode.
static bool closed_palm_intent(const LandmarkMap& lms, const std::map<std::string, bool>& f) {
    if (!has_ids(lms, {8, 6, 12, 10, 16, 14, 20, 18})) return false;
    if (f.at("index") || f.at("middle") || f.at("ring") || f.at("pinky")) return false;
    return folded(lms, 8, 6) && folded(lms, 12, 10) && folded(lms, 16, 14) && folded(lms, 20, 18);
}

static std::optional<std::string> requested_board_mode(const LandmarkMap& lms,
                                                         const std::map<std::string, bool>& f) {
    if (open_palm_intent(f)) return std::string("LETTER");
    if (closed_palm_intent(lms, f)) return std::string("SHAPE");
    return std::nullopt;
}

// Stable open/closed palm mode switch; returns true if a switch just
// happened. Must be called with state.mtx already held.
static bool handle_board_mode_request(PenState& state, const std::optional<std::string>& requested) {
    if (!requested) {
        state.board_candidate_mode.reset();
        state.board_candidate_count = 0;
        return false;
    }
    if (*requested == state.board_mode) {
        state.board_candidate_mode = requested;
        state.board_candidate_count = BOARD_MODE_STABLE_FRAMES;
        return false;
    }
    if (state.board_candidate_mode && *state.board_candidate_mode == *requested) {
        state.board_candidate_count += 1;
    } else {
        state.board_candidate_mode = requested;
        state.board_candidate_count = 1;
    }
    if (state.board_candidate_count >= BOARD_MODE_STABLE_FRAMES) {
        state.board_mode = *requested;
        state.board_flash = 24;
        state.board_candidate_count = 0;
        std::cout << "[INFO] Board mode -> " << *requested << "\n";
        return true;
    }
    return false;
}

struct RawModeResult { std::string mode; std::map<std::string, bool> flags; bool pinky = false; };

static RawModeResult raw_mode_detect(const LandmarkMap& lms) {
    RawModeResult r;
    r.flags = compute_flags(lms);
    const auto& f = r.flags;
    r.pinky = pinky_intent(lms, f);

    bool eraser = f.at("pinky") && !f.at("middle") && !f.at("ring") &&
                  highest(lms, 20, {4, 12, 16}, ERASER_MARGIN);
    bool pen = f.at("index") && !r.pinky && !f.at("middle") && !f.at("ring") &&
               highest(lms, 8, {4, 12, 16, 20}, TIP_MARGIN);

    r.mode = eraser ? "ERASER" : (r.pinky ? "OFF" : (pen ? "PEN" : "OFF"));
    return r;
}

// Debounces raw mode transitions over STABLE_FRAMES frames. Must be called
// with state.mtx already held.
static std::string stable_mode(PenState& state, const std::string& mode) {
    if (mode == "OFF") {
        state.mode = "OFF";
        state.candidate_mode = "OFF";
        state.candidate_count = 0;
        return "OFF";
    }
    if (state.mode == mode) {
        state.candidate_mode = mode;
        state.candidate_count = STABLE_FRAMES;
        return mode;
    }
    state.candidate_count = (state.candidate_mode == mode) ? state.candidate_count + 1 : 1;
    state.candidate_mode = mode;
    state.mode = (state.candidate_count >= STABLE_FRAMES) ? mode : "OFF";
    return state.mode;
}

// =============================================================================
// Stroke/shape geometry helpers: path length, bounding box, segment
// densification, freehand-stroke commit, and preview-visibility gating.
// =============================================================================
static double path_length(const std::vector<Point>& points) {
    double total = 0.0;
    for (size_t i = 1; i < points.size(); ++i) total += point_distance(points[i - 1], points[i]);
    return total;
}

struct Bounds { double x1, y1, x2, y2; };

static Bounds bounds_of(const std::vector<Point>& points) {
    double x1 = points[0].first, x2 = points[0].first;
    double y1 = points[0].second, y2 = points[0].second;
    for (const auto& p : points) {
        x1 = std::min(x1, p.first);  x2 = std::max(x2, p.first);
        y1 = std::min(y1, p.second); y2 = std::max(y2, p.second);
    }
    return {x1, y1, x2, y2};
}

static void add_densified_segment(std::deque<Segment>& seg_deque, const Point& a, const Point& b,
                                   const Rgba& color, double width) {
    double d = point_distance(a, b);
    int steps = std::max(1, static_cast<int>(d / PEN_DOT_SPACING));
    Point prev = a;
    for (int step = 1; step <= steps; ++step) {
        double t = static_cast<double>(step) / steps;
        Point cur{a.first + (b.first - a.first) * t, a.second + (b.second - a.second) * t};
        seg_deque.push_back(Segment{prev, cur, color, width});
        prev = cur;
    }
}

static void commit_freehand_segments(PenState& state, const std::vector<Point>& points, const Rgba& color) {
    if (points.size() < 2) return;
    for (size_t i = 1; i < points.size(); ++i) {
        if (point_distance(points[i - 1], points[i]) <= MAX_JUMP_PIXELS)
            add_densified_segment(state.segments, points[i - 1], points[i], color, PEN_LINE_WIDTH);
    }
}

// =============================================================================
// Shape recognition: classifies a freehand stroke as a circle, rectangle,
// or arrow based on geometric confidence scores.
// =============================================================================
static std::optional<Shape> classify_shape(const std::vector<Point>& points, const Rgba& color) {
    if (static_cast<int>(points.size()) < MIN_SHAPE_POINTS) return std::nullopt;
    Bounds b = bounds_of(points);
    double w = std::max(1.0, b.x2 - b.x1), h = std::max(1.0, b.y2 - b.y1);
    double diag = std::hypot(w, h);
    double plen = path_length(points);
    double close = point_distance(points.front(), points.back());

    if (diag < MIN_SHAPE_SIZE || plen < MIN_SHAPE_PATH) return std::nullopt;

    std::vector<Shape> candidates;

    // Circle
    double cx = (b.x1 + b.x2) / 2.0, cy = (b.y1 + b.y2) / 2.0;
    std::vector<double> radii;
    radii.reserve(points.size());
    for (const auto& p : points) radii.push_back(std::hypot(p.first - cx, p.second - cy));
    double mean_r = 0.0;
    for (double r : radii) mean_r += r;
    mean_r /= std::max<size_t>(1, radii.size());
    if (mean_r > 1.0) {
        double var = 0.0;
        for (double r : radii) var += (r - mean_r) * (r - mean_r);
        double radial_std = std::sqrt(var / std::max<size_t>(1, radii.size()));
        double close_score  = 1.0 - std::min(close / std::max(1.0, diag * 0.45), 1.0);
        double aspect_score = 1.0 - std::min(std::abs(w - h) / std::max(w, h), 1.0);
        double radial_score = 1.0 - std::min(radial_std / std::max(1.0, mean_r * 0.32), 1.0);
        double circle_conf  = 0.36 * close_score + 0.24 * aspect_score + 0.40 * radial_score;
        if (close < diag * 0.42 && circle_conf >= SHAPE_CONF_THRESHOLD) {
            Shape s;
            s.type = Shape::Type::Circle;
            s.cx = std::lround(cx); s.cy = std::lround(cy);
            s.r  = std::lround((w + h) / 4.0);
            s.color = color; s.width = PEN_LINE_WIDTH; s.confidence = circle_conf;
            candidates.push_back(s);
        }
    }

    // Rectangle
    double edge_tol = std::max(14.0, std::min(w, h) * 0.16);
    int edge_hits = 0;
    for (const auto& p : points) {
        double dmin = std::min({std::abs(p.first - b.x1), std::abs(p.first - b.x2),
                                 std::abs(p.second - b.y1), std::abs(p.second - b.y2)});
        if (dmin <= edge_tol) ++edge_hits;
    }
    double edge_score = static_cast<double>(edge_hits) / points.size();
    double corner_radius = std::max(26.0, std::min(w, h) * 0.25);
    std::vector<Point> corners = {{b.x1, b.y1}, {b.x2, b.y1}, {b.x2, b.y2}, {b.x1, b.y2}};
    int corner_count = 0;
    for (const auto& c : corners) {
        bool near = false;
        for (const auto& p : points) if (point_distance(p, c) <= corner_radius) { near = true; break; }
        if (near) ++corner_count;
    }
    double close_score_r = 1.0 - std::min(close / std::max(1.0, diag * 0.45), 1.0);
    double rect_conf = 0.35 * close_score_r + 0.40 * edge_score + 0.25 * (corner_count / 4.0);
    if (close < diag * 0.45 && edge_score >= 0.55 && corner_count >= 3 && rect_conf >= SHAPE_CONF_THRESHOLD) {
        Shape s;
        s.type = Shape::Type::Rect;
        s.x = b.x1; s.y = b.y1; s.w = w; s.h = h;
        s.color = color; s.width = PEN_LINE_WIDTH; s.confidence = rect_conf;
        candidates.push_back(s);
    }

    // Arrow
    double displacement = point_distance(points.front(), points.back());
    double linearity = displacement / std::max(1.0, plen);
    double open_score = std::min(displacement / std::max(1.0, diag * 0.65), 1.0);

    double straight_score = 0.0, forward_score = 0.0;
    if (displacement > 1.0) {
        double sx = points.front().first, sy = points.front().second;
        double ex = points.back().first,  ey = points.back().second;
        double ux = (ex - sx) / displacement, uy = (ey - sy) / displacement;
        std::vector<double> perp, proj;
        perp.reserve(points.size()); proj.reserve(points.size());
        for (const auto& p : points) {
            perp.push_back(std::abs((p.first - sx) * uy - (p.second - sy) * ux));
            proj.push_back((p.first - sx) * ux + (p.second - sy) * uy);
        }
        double mean_perp = 0.0;
        for (double v : perp) mean_perp += v;
        mean_perp /= std::max<size_t>(1, perp.size());
        straight_score = 1.0 - std::min(mean_perp / std::max(1.0, diag * 0.22), 1.0);

        double forward = 0.0, total = 0.0;
        for (size_t i = 1; i < proj.size(); ++i) {
            forward += std::max(0.0, proj[i] - proj[i - 1]);
            total   += std::abs(proj[i] - proj[i - 1]);
        }
        forward_score = forward / std::max(1.0, total);
    }

    double arrow_conf =
        0.40 * std::min(std::max((linearity - 0.55) / 0.35, 0.0), 1.0) +
        0.25 * open_score +
        0.20 * straight_score +
        0.15 * forward_score;

    if (close > diag * 0.45 &&
        displacement >= MIN_SHAPE_SIZE &&
        linearity >= 0.68 &&
        straight_score >= 0.58 &&
        forward_score >= 0.72 &&
        arrow_conf >= ARROW_CONF_THRESHOLD) {
        Shape s;
        s.type = Shape::Type::Arrow;
        s.start = points.front(); s.end = points.back();
        s.color = color; s.width = PEN_LINE_WIDTH; s.confidence = arrow_conf;
        candidates.push_back(s);
    }

    if (candidates.empty()) return std::nullopt;
    return *std::max_element(candidates.begin(), candidates.end(),
                              [](const Shape& a, const Shape& b) { return a.confidence < b.confidence; });
}

// Gate: only show live shape preview when the stroke is already large enough.
static bool should_show_preview(const std::vector<Point>& points) {
    if (static_cast<int>(points.size()) < MIN_PREVIEW_POINTS) return false;
    Bounds b = bounds_of(points);
    return std::hypot(b.x2 - b.x1, b.y2 - b.y1) >= MIN_PREVIEW_DIAG;
}

// =============================================================================
// Handwriting cleanup / beautification: resampling, moving-average
// smoothing, and the combined beautify pass applied to freehand strokes
// in LETTER board mode.
// =============================================================================
static std::vector<Point> resample_points(const std::vector<Point>& points, double spacing) {
    if (points.size() < 2 || spacing <= 0.0) return points;
    std::vector<Point> out = {points[0]};
    Point prev = points[0];
    double accum = 0.0;
    size_t i = 1;
    while (i < points.size()) {
        Point cur = points[i];
        double d = point_distance(prev, cur);
        if (d <= 1e-6) { ++i; continue; }
        if (accum + d >= spacing) {
            double t = (spacing - accum) / d;
            Point newp{std::lround(prev.first + (cur.first - prev.first) * t),
                       std::lround(prev.second + (cur.second - prev.second) * t)};
            out.push_back(newp);
            prev = newp;
            accum = 0.0;
        } else {
            accum += d;
            prev = cur;
            ++i;
        }
    }
    if (out.back() != points.back()) out.push_back(points.back());
    return out;
}

static std::vector<Point> moving_average_points(const std::vector<Point>& points, int window) {
    if (points.size() < 3 || window <= 1) return points;
    int half = std::max(1, window / 2);
    std::vector<Point> out = {points[0]};
    for (size_t i = 1; i + 1 < points.size(); ++i) {
        size_t lo = static_cast<size_t>(std::max(0, static_cast<int>(i) - half));
        size_t hi = std::min(points.size(), i + half + 1);
        double sx = 0.0, sy = 0.0;
        for (size_t j = lo; j < hi; ++j) { sx += points[j].first; sy += points[j].second; }
        size_t n = hi - lo;
        out.push_back({std::lround(sx / n), std::lround(sy / n)});
    }
    out.push_back(points.back());
    return out;
}

static std::vector<Point> beautify_handwriting(const std::vector<Point>& points) {
    if (!HANDWRITING_BEAUTIFY || points.size() < 4) return points;
    std::vector<Point> cleaned = {points[0]};
    for (size_t i = 1; i < points.size(); ++i) {
        if (point_distance(cleaned.back(), points[i]) >= HANDWRITING_MIN_KEEP_DIST)
            cleaned.push_back(points[i]);
    }
    if (cleaned.size() < 4) return cleaned;
    cleaned = resample_points(cleaned, HANDWRITING_RESAMPLE_SPACING);
    cleaned = moving_average_points(cleaned, HANDWRITING_SMOOTH_WINDOW);
    return cleaned;
}

// Commits the in-progress stroke — either as a classified shape in SHAPE
// board mode (with freehand fallback), or as beautified freehand strokes in
// LETTER mode. Must be called with state.mtx already held.
static void finish_current_stroke(PenState& state) {
    std::vector<Point> points = state.current_points;
    Rgba color = state.current_color;
    std::string board_mode = state.board_mode;
    state.current_points.clear();
    state.preview_shape.reset();
    state.prev_pen.reset();
    state.smooth_pen.reset();
    if (points.size() < 2) return;

    if (board_mode == "SHAPE") {
        auto shape = classify_shape(points, color);
        if (shape) {
            state.shapes.push_back(*shape);
        } else {
            commit_freehand_segments(state, points, color);
        }
    } else {
        commit_freehand_segments(state, beautify_handwriting(points), color);
    }
}

// =============================================================================
// Erasing: point-to-segment distance, arrow outline segments, shape
// distance, and the erase operation that removes nearby strokes/shapes.
// =============================================================================
static double point_seg_dist(const Point& p, const Point& a, const Point& b) {
    double dx = b.first - a.first, dy = b.second - a.second;
    if (dx == 0.0 && dy == 0.0) return point_distance(p, a);
    double t = ((p.first - a.first) * dx + (p.second - a.second) * dy) / (dx * dx + dy * dy);
    t = std::max(0.0, std::min(1.0, t));
    return std::hypot(p.first - (a.first + t * dx), p.second - (a.second + t * dy));
}

static std::vector<std::pair<Point, Point>> arrow_segments(const Shape& shape) {
    const Point& s = shape.start;
    const Point& e = shape.end;
    double angle = std::atan2(e.second - s.second, e.first - s.first);
    double head_len = std::max(35.0, std::min(70.0, point_distance(s, e) * 0.18));
    Point a1{e.first - head_len * std::cos(angle - M_PI / 6.0),
             e.second - head_len * std::sin(angle - M_PI / 6.0)};
    Point a2{e.first - head_len * std::cos(angle + M_PI / 6.0),
             e.second - head_len * std::sin(angle + M_PI / 6.0)};
    return {{s, e}, {e, a1}, {e, a2}};
}

static double shape_dist(const Shape& shape, const Point& p) {
    switch (shape.type) {
        case Shape::Type::Circle:
            return std::abs(point_distance(p, {shape.cx, shape.cy}) - shape.r);
        case Shape::Type::Rect: {
            double x = shape.x, y = shape.y, w = shape.w, h = shape.h;
            std::vector<std::pair<Point, Point>> segs = {
                {{x, y}, {x + w, y}}, {{x + w, y}, {x + w, y + h}},
                {{x + w, y + h}, {x, y + h}}, {{x, y + h}, {x, y}}};
            double best = std::numeric_limits<double>::max();
            for (auto& s : segs) best = std::min(best, point_seg_dist(p, s.first, s.second));
            return best;
        }
        case Shape::Type::Arrow: {
            double best = std::numeric_limits<double>::max();
            for (auto& s : arrow_segments(shape)) best = std::min(best, point_seg_dist(p, s.first, s.second));
            return best;
        }
    }
    return std::numeric_limits<double>::max();
}

// Must be called with state.mtx already held.
static void erase(PenState& state, const Point& p) {
    std::vector<Point> points = {p};
    if (state.prev_eraser) {
        double d = point_distance(p, *state.prev_eraser);
        if (d <= MAX_JUMP_PIXELS) {
            int steps = std::max(1, static_cast<int>(d / ERASER_PATH_SPACING));
            points.clear();
            for (int t = 1; t <= steps; ++t) {
                double frac = static_cast<double>(t) / steps;
                points.push_back({state.prev_eraser->first + (p.first - state.prev_eraser->first) * frac,
                                   state.prev_eraser->second + (p.second - state.prev_eraser->second) * frac});
            }
        }
    }
    state.prev_eraser = p;

    std::deque<Segment> kept_segs;
    for (const auto& seg : state.segments) {
        bool erase_it = false;
        for (const auto& ep : points) {
            if (point_seg_dist(ep, seg.a, seg.b) <= ERASER_RADIUS) { erase_it = true; break; }
        }
        if (!erase_it) kept_segs.push_back(seg);
    }
    state.segments = std::move(kept_segs);

    std::deque<Shape> kept_shapes;
    for (const auto& sh : state.shapes) {
        bool erase_it = false;
        for (const auto& ep : points) {
            if (shape_dist(sh, ep) <= ERASER_RADIUS) { erase_it = true; break; }
        }
        if (!erase_it) kept_shapes.push_back(sh);
    }
    state.shapes = std::move(kept_shapes);
}

// =============================================================================
// Drag / move helpers: shape/segment-group centres, moving shapes, word
// clustering, and grab/apply/release drag operations.
// =============================================================================
static Point shape_centre(const Shape& shape) {
    switch (shape.type) {
        case Shape::Type::Circle: return {shape.cx, shape.cy};
        case Shape::Type::Rect:   return {shape.x + shape.w / 2.0, shape.y + shape.h / 2.0};
        case Shape::Type::Arrow:  return {(shape.start.first + shape.end.first) / 2.0,
                                           (shape.start.second + shape.end.second) / 2.0};
    }
    return {0, 0};
}

static Shape move_shape(const Shape& shape, double dx, double dy) {
    Shape s = shape;
    switch (s.type) {
        case Shape::Type::Circle: s.cx += dx; s.cy += dy; break;
        case Shape::Type::Rect:   s.x  += dx; s.y  += dy; break;
        case Shape::Type::Arrow:
            s.start.first += dx; s.start.second += dy;
            s.end.first   += dx; s.end.second   += dy;
            break;
    }
    return s;
}

static Point seg_group_centre(const std::vector<Segment>& segs) {
    if (segs.empty()) return {0, 0};
    double sx = 0, sy = 0;
    for (const auto& s : segs) { sx += s.a.first + s.b.first; sy += s.a.second + s.b.second; }
    double n = static_cast<double>(segs.size()) * 2.0;
    return {sx / n, sy / n};
}

static Bounds segment_bbox(const Segment& seg) {
    return {std::min(seg.a.first, seg.b.first), std::min(seg.a.second, seg.b.second),
            std::max(seg.a.first, seg.b.first), std::max(seg.a.second, seg.b.second)};
}

static double bbox_gap(const Bounds& b1, const Bounds& b2) {
    double dx = std::max({b1.x1 - b2.x2, b2.x1 - b1.x2, 0.0});
    double dy = std::max({b1.y1 - b2.y2, b2.y1 - b1.y2, 0.0});
    return std::hypot(dx, dy);
}

// Expands a seed segment index into a nearby freehand cluster ("word") via
// bbox-gap BFS. `segs` is a snapshot vector of the current segments.
static std::set<int> build_word_segment_cluster(const std::vector<Segment>& segs, int seed_idx) {
    if (seed_idx < 0 || seed_idx >= static_cast<int>(segs.size())) return {};

    std::set<int> selected = {seed_idx};
    Bounds selected_bbox = segment_bbox(segs[seed_idx]);
    bool changed = true;
    while (changed) {
        changed = false;
        for (int i = 0; i < static_cast<int>(segs.size()); ++i) {
            if (selected.count(i)) continue;
            Bounds b = segment_bbox(segs[i]);
            if (bbox_gap(selected_bbox, b) <= WORD_CLUSTER_JOIN_RADIUS) {
                selected.insert(i);
                selected_bbox.x1 = std::min(selected_bbox.x1, b.x1);
                selected_bbox.y1 = std::min(selected_bbox.y1, b.y1);
                selected_bbox.x2 = std::max(selected_bbox.x2, b.x2);
                selected_bbox.y2 = std::max(selected_bbox.y2, b.y2);
                changed = true;
            }
        }
    }
    return selected;
}

// Finds the nearest committed object within PINCH_DRAG_GRAB_RADIUS and grabs
// it (shapes checked by edge distance, freehand by nearest segment expanded
// into its nearby cluster). Must be called with state.mtx already held.
static bool try_grab(PenState& state, const Point& grab_point) {
    double best_d = PINCH_DRAG_GRAB_RADIUS;
    std::string best_type;
    int best_idx = -1;
    std::optional<std::set<int>> best_seg_cluster;

    // 1) Clean shapes.
    {
        int idx = 0;
        for (const auto& sh : state.shapes) {
            double d = shape_dist(sh, grab_point);
            if (d < best_d) {
                best_d = d; best_type = "shape"; best_idx = idx; best_seg_cluster.reset();
            }
            ++idx;
        }
    }

    // 2) Nearest freehand segment, expanded into its cluster.
    std::vector<Segment> segs(state.segments.begin(), state.segments.end());
    int nearest_seg_idx = -1;
    double nearest_seg_d = PINCH_DRAG_GRAB_RADIUS;
    for (int i = 0; i < static_cast<int>(segs.size()); ++i) {
        double d = point_seg_dist(grab_point, segs[i].a, segs[i].b);
        if (d < nearest_seg_d) { nearest_seg_d = d; nearest_seg_idx = i; }
    }

    if (nearest_seg_idx >= 0 && nearest_seg_d < best_d) {
        auto cluster = build_word_segment_cluster(segs, nearest_seg_idx);
        if (!cluster.empty()) {
            best_d = nearest_seg_d;
            best_type = "seg_group";
            best_idx = nearest_seg_idx;
            best_seg_cluster = cluster;
        }
    }

    if (best_type.empty()) {
        std::cout << "[INFO] Pinch: no nearby object/word to grab\n";
        return false;
    }

    state.drag_mode = true;
    state.drag_target_type = best_type;
    state.drag_target_idx = best_idx;
    state.drag_anchor = grab_point;

    if (best_type == "shape") {
        state.drag_seg_indices.clear();
        state.drag_selected_count = 1;
        state.drag_origin = shape_centre(state.shapes[best_idx]);
    } else {
        state.drag_seg_indices = best_seg_cluster.value_or(std::set<int>{});
        state.drag_selected_count = static_cast<int>(state.drag_seg_indices.size());
        std::vector<Segment> selected_segs;
        for (int i : state.drag_seg_indices) selected_segs.push_back(segs[i]);
        state.drag_origin = seg_group_centre(selected_segs);
    }

    std::cout << "[INFO] Drag GRAB: " << best_type << " dist=" << static_cast<int>(best_d)
              << "px selected=" << state.drag_selected_count << "\n";
    return true;
}

// Must be called with state.mtx already held.
static void apply_drag(PenState& state, const Point& current_point) {
    if (!state.drag_mode || !state.drag_anchor) return;
    double dx = current_point.first - state.drag_anchor->first;
    double dy = current_point.second - state.drag_anchor->second;

    if (state.drag_target_type == "shape") {
        if (state.drag_target_idx >= 0 && state.drag_target_idx < static_cast<int>(state.shapes.size())) {
            state.shapes[state.drag_target_idx] = move_shape(state.shapes[state.drag_target_idx], dx, dy);
        }
    } else if (state.drag_target_type == "seg_group") {
        for (int idx : state.drag_seg_indices) {
            if (idx < 0 || idx >= static_cast<int>(state.segments.size())) continue;
            state.segments[idx].a.first  += dx; state.segments[idx].a.second += dy;
            state.segments[idx].b.first  += dx; state.segments[idx].b.second += dy;
        }
    }
    state.drag_anchor = current_point;
}

// Must be called with state.mtx already held.
static void release_drag(PenState& state) {
    state.drag_mode = false;
    state.drag_target_type.clear();
    state.drag_target_idx = -1;
    state.drag_anchor.reset();
    state.drag_origin.reset();
    state.drag_seg_indices.clear();
    state.drag_selected_count = 0;
    std::cout << "[INFO] Drag RELEASED\n";
}

// =============================================================================
// Main state update
//
// Called once per metadata sample received by the landmark AppSink.
// This function is the core state machine for user interaction:
// 1) Detect mode requests (LETTER/SHAPE) from open/closed palm.
// 2) Handle pinch as an exclusive "grab + drag" interaction.
// 3) Handle normal PEN/ERASER/OFF transitions with debouncing.
// 4) Update/commit in-flight stroke data for drawing and preview.
//
// Threading contract: may be called from GStreamer callback threads.
// All mutable state transitions are guarded by state.mtx.
// =============================================================================
static void update_state(const LandmarkMap& lms, PenState& state) {
    RawModeResult rm = raw_mode_detect(lms);
    bool pinch = pinch_intent(lms);

    std::lock_guard<std::mutex> lk(state.mtx);

    state.frame_counter += 1;

    if (state.pinch_cooldown > 0) state.pinch_cooldown -= 1;
    if (state.board_flash > 0)    state.board_flash -= 1;

    std::optional<Point> raw_index, raw_thumb, raw_pinky;
    if (auto it = lms.find(8);  it != lms.end()) raw_index = it->second;
    if (auto it = lms.find(4);  it != lms.end()) raw_thumb = it->second;
    if (auto it = lms.find(20); it != lms.end()) raw_pinky = it->second;

    state.raw_mode = rm.mode;
    state.flags    = rm.flags;
    state.tip_y.clear();
    for (int i : {4, 8, 12, 16, 20}) {
        if (auto it = lms.find(i); it != lms.end()) state.tip_y[i] = it->second.second;
    }

    // Open/closed palm switches board mode. The switch is debounced via
    // BOARD_MODE_STABLE_FRAMES in handle_board_mode_request().
    auto requested_mode = requested_board_mode(lms, rm.flags);
    if (requested_mode) {
        finish_current_stroke(state);
        handle_board_mode_request(state, requested_mode);
        state.mode = "SET_" + *requested_mode;
        state.pen_mode = state.eraser_mode = false;
        state.index_point = raw_index;
        state.thumb_point = raw_thumb;
        state.pinky_point = raw_pinky;
        return;
    } else {
        handle_board_mode_request(state, std::nullopt);
    }

    // Pinch is reserved for drag/move. While active, pen/eraser drawing is
    // suppressed to avoid accidental stroke commits during object movement.
    if (pinch) {
        if (!state.pinch_active && state.pinch_cooldown == 0) {
            finish_current_stroke(state);
            Point grab_pt = raw_index.value_or(Point{0, 0});
            try_grab(state, grab_pt);
        }
        if (state.drag_mode && raw_index) {
            apply_drag(state, *raw_index);
        }

        state.pinch_active = true;
        state.mode = state.drag_mode ? "DRAG" : "PINCH";
        state.pen_mode = state.eraser_mode = false;
        state.index_point = raw_index;
        state.thumb_point = raw_thumb;
        state.pinky_point = raw_pinky;
        return;
    } else {
        if (state.pinch_active) {
            state.pinch_last_release = state.frame_counter;
            if (state.drag_mode) release_drag(state);
            state.pinch_cooldown = PINCH_COOLDOWN_FRAMES;
        }
        state.pinch_active = false;
    }

    // Normal PEN / ERASER / OFF path (only when pinch is not active).
    std::string mode = rm.mode;
    if (mode == "ERASER" || rm.pinky) {
        state.eraser_block = ERASER_TO_PEN_BLOCK;
    } else if (state.eraser_block > 0) {
        state.eraser_block -= 1;
    }
    if (mode == "PEN" && state.eraser_block > 0) mode = "OFF";

    mode = stable_mode(state, mode);

    state.pen_mode    = (mode == "PEN");
    state.eraser_mode = (mode == "ERASER");
    state.index_point = raw_index;
    state.thumb_point = raw_thumb;
    state.pinky_point = raw_pinky;

    // Eraser mode: commit any in-progress pen stroke, then erase against a
    // smoothed pinky trajectory.
    if (state.eraser_mode && raw_pinky) {
        finish_current_stroke(state);
        auto p = smooth_point(state.smooth_eraser, raw_pinky, ERASER_SMOOTHING_ALPHA);
        state.smooth_eraser = p;
        state.pinky_point   = p;
        state.prev_pen.reset();
        state.smooth_pen.reset();
        erase(state, *p);
        return;
    }

    state.prev_eraser.reset();
    state.smooth_eraser.reset();

    // Pen mode: start/continue the current stroke and, in SHAPE board mode,
    // keep a live shape preview while the stroke is being drawn.
    if (!state.pen_mode || !raw_index) {
        finish_current_stroke(state);
        return;
    }

    auto p = smooth_point(state.smooth_pen, raw_index, PEN_SMOOTHING_ALPHA);
    state.smooth_pen  = p;
    state.index_point = p;

    if (!state.prev_pen) {
        state.current_color  = state.pen_color;
        state.current_points = {*p};
        state.preview_shape.reset();
        state.prev_pen = p;
        return;
    }

    double d = point_distance(*state.prev_pen, *p);

    if (d > MAX_JUMP_PIXELS) {
        finish_current_stroke(state);
        state.current_color  = state.pen_color;
        state.current_points = {*p};
        state.preview_shape.reset();
        state.prev_pen = p;
        return;
    }

    if (d < MIN_DRAW_MOVE) return;

    state.current_points.push_back(*p);
    state.prev_pen = p;

    if (state.board_mode == "SHAPE" && should_show_preview(state.current_points)) {
        state.preview_shape = classify_shape(state.current_points, state.current_color);
    } else {
        state.preview_shape.reset();
    }
}

// =============================================================================
// Cairo drawing: draw_canvas() renders strokes, shapes, gesture indicators,
// and the status HUD onto the cairooverlay draw context.
// =============================================================================
static void set_rgba(cairo_t* cr, const Rgba& c, double alpha_mul = 1.0) {
    cairo_set_source_rgba(cr, c.r, c.g, c.b, std::max(0.0, std::min(1.0, c.a * alpha_mul)));
}

static void draw_polyline(cairo_t* cr, const std::vector<Point>& points, const Rgba& color,
                           double width, double alpha_mul = 1.0) {
    if (points.empty()) return;
    set_rgba(cr, color, alpha_mul);
    cairo_set_line_width(cr, width);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_new_path(cr);
    cairo_move_to(cr, points[0].first, points[0].second);
    for (size_t i = 1; i < points.size(); ++i) cairo_line_to(cr, points[i].first, points[i].second);
    cairo_stroke(cr);
}

static void draw_shape(cairo_t* cr, const Shape& shape, double alpha_mul = 1.0) {
    set_rgba(cr, shape.color, alpha_mul);
    cairo_set_line_width(cr, shape.width);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    switch (shape.type) {
        case Shape::Type::Circle:
            cairo_new_path(cr);
            cairo_arc(cr, shape.cx, shape.cy, shape.r, 0, 2 * M_PI);
            cairo_stroke(cr);
            break;
        case Shape::Type::Rect:
            cairo_new_path(cr);
            cairo_rectangle(cr, shape.x, shape.y, shape.w, shape.h);
            cairo_stroke(cr);
            break;
        case Shape::Type::Arrow:
            for (auto& seg : arrow_segments(shape)) {
                cairo_new_path(cr);
                cairo_move_to(cr, seg.first.first, seg.first.second);
                cairo_line_to(cr, seg.second.first, seg.second.second);
                cairo_stroke(cr);
            }
            break;
    }
}

// Snapshots PenState under lock, then draws everything onto `cr` (a Cairo
// context already bound to the current frame's BGRA image surface).
static void draw_canvas(cairo_t* cr, PenState& state) {
    std::vector<Segment> segs;
    std::vector<Shape>   shapes;
    std::vector<Point>   current_points;
    std::optional<Shape> preview_shape;
    std::string mode, board_mode;
    bool pen = false, eraser = false, drag_mode = false;
    std::optional<Point> ip, pp, tp;
    size_t seg_count = 0, shape_count = 0;
    std::map<int, double> ty;
    Rgba pen_color;
    int board_flash = 0;

    {
        std::lock_guard<std::mutex> lk(state.mtx);
        segs.assign(state.segments.begin(), state.segments.end());
        shapes.assign(state.shapes.begin(), state.shapes.end());
        current_points = state.current_points;
        preview_shape  = state.preview_shape;
        mode           = state.mode;
        pen            = state.pen_mode;
        eraser         = state.eraser_mode;
        drag_mode      = state.drag_mode;
        ip = state.index_point; pp = state.pinky_point; tp = state.thumb_point;
        seg_count   = state.segments.size();
        shape_count = state.shapes.size();
        ty          = state.tip_y;
        pen_color   = state.pen_color;
        board_mode  = state.board_mode;
        board_flash = state.board_flash;
    }

    cairo_save(cr);

    // Committed freehand strokes
    if (!segs.empty()) {
        bool have_path = false;
        std::optional<Point> last_b;
        std::optional<Rgba>  last_c;
        auto same_color = [](const Rgba& a, const Rgba& b) {
            return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
        };
        for (const auto& seg : segs) {
            if (!last_c || !same_color(*last_c, seg.color)) {
                if (have_path) cairo_stroke(cr);
                set_rgba(cr, seg.color);
                cairo_set_line_width(cr, seg.width);
                cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
                cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
                cairo_new_path(cr);
                cairo_move_to(cr, seg.a.first, seg.a.second);
                have_path = true;
                last_b.reset();
            }
            if (!last_b || last_b->first != seg.a.first || last_b->second != seg.a.second) {
                cairo_move_to(cr, seg.a.first, seg.a.second);
            }
            cairo_line_to(cr, seg.b.first, seg.b.second);
            last_b = seg.b;
            last_c = seg.color;
        }
        if (have_path) cairo_stroke(cr);
    }

    // Committed clean shapes
    for (const auto& shape : shapes) draw_shape(cr, shape, 1.0);

    // Live stroke
    if (!current_points.empty()) {
        if (preview_shape) {
            draw_polyline(cr, current_points, preview_shape->color,
                          std::max(3.0, PEN_LINE_WIDTH * 0.5), 0.20);
            double dashes[2] = {16.0, 10.0};
            cairo_set_dash(cr, dashes, 2, 0);
            draw_shape(cr, *preview_shape, 0.80);
            cairo_set_dash(cr, nullptr, 0, 0);
        } else {
            std::vector<Point> live_points =
                (board_mode == "LETTER") ? beautify_handwriting(current_points) : current_points;
            draw_polyline(cr, live_points, pen_color, PEN_LINE_WIDTH, 0.97);
        }
    }

    // Index finger dot
    if (ip) {
        if (pen) {
            set_rgba(cr, pen_color);
            cairo_arc(cr, ip->first, ip->second, 9, 0, 2 * M_PI);
            cairo_fill(cr);
        } else if (drag_mode) {
            cairo_set_source_rgba(cr, 1, 0.6, 0, 0.9);
            cairo_arc(cr, ip->first, ip->second, 11, 0, 2 * M_PI);
            cairo_fill(cr);
        } else {
            cairo_set_source_rgba(cr, 1, 1, 0, 0.5);
            cairo_arc(cr, ip->first, ip->second, 6, 0, 2 * M_PI);
            cairo_fill(cr);
        }
    }

    // Thumb dot (pinch/drag/board-mode-switch)
    if (tp && (mode == "DRAG" || mode == "PINCH" || mode == "SET_LETTER" || mode == "SET_SHAPE")) {
        cairo_set_source_rgba(cr, 1, 1, 1, 0.75);
        cairo_arc(cr, tp->first, tp->second, 7, 0, 2 * M_PI);
        cairo_fill(cr);
    }

    // Pinky eraser
    if (pp) {
        if (eraser) {
            cairo_set_source_rgba(cr, 0, 0.45, 1, 0.22);
            cairo_arc(cr, pp->first, pp->second, ERASER_RADIUS, 0, 2 * M_PI);
            cairo_fill(cr);
            cairo_set_source_rgba(cr, 0, 0.65, 1, 1);
            cairo_set_line_width(cr, 4);
            cairo_arc(cr, pp->first, pp->second, ERASER_RADIUS, 0, 2 * M_PI);
            cairo_stroke(cr);
        } else {
            cairo_set_source_rgba(cr, 0, 0.45, 1, 0.45);
            cairo_arc(cr, pp->first, pp->second, 5, 0, 2 * M_PI);
            cairo_fill(cr);
        }
    }

    // Status HUD
    if (PRINT_STATUS) {
        cairo_set_source_rgba(cr, 0, 0, 0, 0.68);
        cairo_rectangle(cr, 20, 20, 790, 135);
        cairo_fill(cr);

        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 30);
        if (mode == "PEN")            set_rgba(cr, pen_color);
        else if (mode == "ERASER")    cairo_set_source_rgba(cr, 0, 0.65, 1, 1);
        else if (mode == "DRAG")      cairo_set_source_rgba(cr, 1, 0.6, 0, 1);
        else if (mode == "SET_SHAPE") cairo_set_source_rgba(cr, 0.3, 0.8, 1, 1);
        else if (mode == "SET_LETTER") cairo_set_source_rgba(cr, 0.2, 1, 0.35, 1);
        else if (mode == "PINCH")     cairo_set_source_rgba(cr, 1, 0.8, 0.2, 1);
        else                          cairo_set_source_rgba(cr, 1, 1, 0, 1);
        cairo_move_to(cr, 35, 58);
        cairo_show_text(cr, ("MODE: " + mode).c_str());

        if (board_mode == "LETTER")
            cairo_set_source_rgba(cr, 0.2, 1.0, 0.35, board_flash ? 1.0 : 0.85);
        else
            cairo_set_source_rgba(cr, 0.3, 0.8, 1.0, board_flash ? 1.0 : 0.85);
        cairo_set_font_size(cr, 22);
        cairo_move_to(cr, 245, 57);
        cairo_show_text(cr, ("BOARD: " + board_mode).c_str());

        std::string preview_label = "-";
        if (preview_shape) {
            switch (preview_shape->type) {
                case Shape::Type::Circle: preview_label = "CIRCLE"; break;
                case Shape::Type::Rect:   preview_label = "RECT";   break;
                case Shape::Type::Arrow:  preview_label = "ARROW";  break;
            }
        }
        cairo_set_font_size(cr, 20);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.95);
        cairo_move_to(cr, 35, 95);
        {
            std::ostringstream oss;
            oss << "preview=" << preview_label << "  shapes=" << shape_count << "  segs=" << seg_count;
            cairo_show_text(cr, oss.str().c_str());
        }
        cairo_move_to(cr, 35, 128);
        {
            std::ostringstream oss;
            oss << "open palm=letters  closed palm=shapes  pinch=move word/shape  pinky=eraser"
                << "  y8=" << (ty.count(8) ? std::to_string(static_cast<int>(ty[8])) : "-")
                << " y20=" << (ty.count(20) ? std::to_string(static_cast<int>(ty[20])) : "-");
            cairo_show_text(cr, oss.str().c_str());
        }
    }

    cairo_restore(cr);
}

static void on_cairo_draw_signal(void* /*overlay*/, void* draw_context,
                                 std::uint64_t /*timestamp*/, std::uint64_t /*duration*/,
                                 void* /*user_data*/) {
    if (!draw_context) return;
    draw_canvas(static_cast<cairo_t*>(draw_context), g_state);
}

// =============================================================================
// Callback consumers
// =============================================================================

// meta_sink: receives per-frame hand-landmark JSON metadata text produced by
// qtimlmetaparser(module=json); parses it and drives the gesture state
// machine.
static long g_landmark_buffer_count = 0;

static void on_sample(qti::Buffer buffer)
{
    // Bind to a const reference so the read-only qti::Buffer::data() const
    // overload is used. These buffers are downstream of a `tee` (shared
    // across multiple branches, GStreamer refcount > 1), and the non-const
    // (write-map) overload fails/returns null on shared buffers.
    const qti::Buffer& cbuffer = buffer;
    if (!cbuffer.valid() || !cbuffer.data()) {
        std::cout << "[DEBUG] on_sample: invalid/empty buffer\n";
        return;
    }
    std::string text(reinterpret_cast<const char*>(cbuffer.data()), cbuffer.size());

    LandmarkMap lms = parse_landmarks(text);

    ++g_landmark_buffer_count;
    if (g_landmark_buffer_count % 30 == 1) {
        std::cout << "[DEBUG] on_sample: count=" << g_landmark_buffer_count
                  << " text_len=" << text.size() << " landmarks_found=" << lms.size() << "\n";
    }

    if (!lms.empty()) {
        update_state(lms, g_state);
    } else {
        // If metadata is missing/invalid for this frame, gracefully drop back
        // to OFF and commit any in-flight stroke. This avoids "stuck" draw or
        // drag states when detections temporarily disappear.
        std::lock_guard<std::mutex> lk(g_state.mtx);
        finish_current_stroke(g_state);
        g_state.mode = "OFF";
        g_state.pen_mode = g_state.eraser_mode = false;
        g_state.prev_pen.reset();
        g_state.prev_eraser.reset();
        g_state.smooth_pen.reset();
        g_state.smooth_eraser.reset();
        g_state.tip_y.clear();
    }
}

// =============================================================================
// Pipeline construction and run
// =============================================================================
static Element make_queue(const std::string& name) {
    Element q("queue", name);
    q.set("leaky", 2);
    q.set("max-size-buffers", 2);
    q.set("max-size-bytes", 0);
    q.set("max-size-time", static_cast<std::uint64_t>(0));
    return q;
}

//  Example pipeline:
//
//    v4l2src -> qtivtransform -> [vf:NV12] -> tee name=split
//      split. -> stage-1 palm detection:
//               qtimlvconverter -> qtimltflite -> qtimlpostprocess
//               -> [mlf:text] -> qtimetamux
//      qtimetamux -> qtimetatransform(roi-palmd) -> tee name=split_after_palm
//      split_after_palm. -> stage-2 hand landmarks:
//                           qtimlvconverter -> qtimltflite -> qtimlpostprocess
//                           -> [mlf:text] -> qtimetamux -> tee name=final_split
//      final_split. -> display branch:
//                      qtivoverlay -> qtivtransform -> [vf:BGRA] -> cairooverlay -> waylandsink
//      final_split. -> metadata branch:
//                      qtimlmetaparser(json) -> appsink(meta_sink)
//
//  The pipeline runs hand tracking and renders a gesture-driven smartboard
//  overlay. Metadata drives a state machine (draw/erase/drag/mode switch),
//  while the Cairo bridge draws persistent strokes and shapes on each frame.
void create_and_execute_pipeline()
{
    // Camera + shared video pre-processing
    // Camera source (USB V4L2 device).
    Element source("v4l2src", "source");
    source.set("device", input_config);

    // Initial video transform (mirror preview).
    Element transform("qtivtransform", "transform");
    transform.set("flip-horizontal", true);
    auto videofilter = VideoFilter()
        .format("NV12")
        .resolution(FRAME_WIDTH, FRAME_HEIGHT)
        .framerate(FRAME_FPS);

    // Split raw camera stream to palm branch + passthrough video branch.
    Element split("tee", "split");

    // Stage 1: palm detection
    // Queue for raw video path into palm metadata mux.
    Element q_video_palm = make_queue("q_video_palm");

    // Queue before palm preprocessing.
    Element q_palm_pre = make_queue("q_palm_pre");
    // Palm detector preprocessor.
    Element palm_preproc("qtimlvconverter", "palm_preproc");
    palm_preproc.set("mode", "image-batch-non-cumulative");

    // Queue before palm inference.
    Element q_palm_infer = make_queue("q_palm_infer");
    // Palm detector inference.
    Element palm_inf("qtimltflite", "palm_inf");
    palm_inf.set("delegate", "gpu");
    palm_inf.set("model", model_base_path + "/models/palm_detection_full.tflite");

    // Queue before palm postprocess.
    Element q_palm_post = make_queue("q_palm_post");
    // Palm detector postprocess.
    Element palm_post("qtimlpostprocess", "palm_post");
    palm_post.set("module", "palmd");
    palm_post.set("results", 1);
    palm_post.set("labels", model_base_path + "/labels/palmd_labels.json");
    palm_post.set("settings", model_base_path + "/labels/palmd_settings.json");

    auto palm_mlf = TextFilter();
    // Queue carrying palm metadata text.
    Element q_palm_meta = make_queue("q_palm_meta");

    // Combines the palm-detection video/metadata streams so the ROI
    // transform below can crop the per-hand region for stage 2.
    // Muxes video + palm metadata.
    Element metamux_palm("qtimetamux", "metamux_palm");
    // Converts palm detections into hand ROI metadata.
    Element palm_roi_transform("qtimetatransform", "palm_roi_transform");
    palm_roi_transform.set("module", "roi-palmd");

    // Split after palm ROI transform into hand branch + final video branch.
    Element split_after_palm("tee", "split_after_palm");

    // Stage 2: hand landmarks (21-point)
    // Queue for video path into final metadata mux.
    Element q_video_final = make_queue("q_video_final");

    // Queue before hand-landmark preprocessing.
    Element q_hand_pre = make_queue("q_hand_pre");
    // Hand-landmark preprocessor (ROI batch mode).
    Element hand_preproc("qtimlvconverter", "hand_preproc");
    hand_preproc.set("mode", "roi-batch-cumulative");

    // Queue before hand-landmark inference.
    Element q_hand_infer = make_queue("q_hand_infer");
    // Hand-landmark inference.
    Element hand_inf("qtimltflite", "hand_inf");
    hand_inf.set("delegate", "xnnpack");
    hand_inf.set("model", model_base_path + "/models/hand_landmark_full.tflite");

    // Queue before hand-landmark postprocess.
    Element q_hand_post = make_queue("q_hand_post");
    // Hand-landmark postprocess.
    Element hand_post("qtimlpostprocess", "hand_post");
    hand_post.set("module", "hlandmark");
    hand_post.set("results", 1);
    hand_post.set("labels", model_base_path + "/labels/hlandmarks.json");
    hand_post.set("settings", model_base_path + "/labels/hlandmark_settings.json");

    auto hand_mlf = TextFilter();
    // Queue carrying hand metadata text.
    Element q_hand_meta = make_queue("q_hand_meta");

    // Muxes final video + hand metadata.
    Element metamux_final("qtimetamux", "metamux_final");
    // Split final stream into display + metadata consumer branches.
    Element final_split("tee", "final_split");

    // Display branch: overlay -> cairooverlay -> display
    // Queue for display branch.
    Element q_display = make_queue("q_display");
    // Qualcomm metadata overlay renderer.
    Element overlay("qtivoverlay", "overlay");
    // Transform before cairooverlay.
    Element to_cairo("qtivtransform", "to_cairo");
    auto cairofilter = VideoFilter()
        .format("BGRA")
        .resolution(FRAME_WIDTH, FRAME_HEIGHT)
        .framerate(FRAME_FPS);

    // Cairo overlay that draws persistent whiteboard content.
    Element pen_canvas("cairooverlay", "pen_canvas");
    pen_canvas.connect_signal(
        "draw",
        reinterpret_cast<Element::SignalCallback>(&on_cairo_draw_signal));

    // Display sink.
    Element display("waylandsink", "display");
    display.set("sync", false);
    display.set("fullscreen", true);

    // Metadata branch: hand-landmark JSON -> gesture state machine
    // Queue before metadata parser.
    Element q_meta_parse = make_queue("q_meta_parse");
    // Metadata parser (JSON output).
    Element meta_parser("qtimlmetaparser", "meta_parser");
    meta_parser.set("module", "json");

    // AppSink receiving parsed metadata for gesture state updates.
    AppSink meta_sink("meta_sink");
    meta_sink.set("sync", false);
    meta_sink.set("max-buffers", 1);
    meta_sink.set("drop", true);

    // Assemble and link
    Pipeline pipeline("smartboard");

    pipeline
        .add(source)
        .add(transform)
        .add_stream_filter("videofilter", videofilter)
        .add(split)
        // Stage 1: palm detection
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
        // Stage 2: hand landmarks
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
        // Display branch
        .add(q_display)
        .add(overlay)
        .add(to_cairo)
        .add_stream_filter("cairofilter", cairofilter)
        .add(pen_canvas)
        .add(display)
        // Metadata branch
        .add(q_meta_parse)
        .add(meta_parser)
        .add(meta_sink)
        // Links
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
        .link("final_split", "q_meta_parse", "meta_parser", "meta_sink");

    // Wire up callbacks:
    // - meta_sink drives the gesture state machine from landmark metadata.
    // - pen_canvas draw callback renders whiteboard state on each frame.
    meta_sink.set_buffer_consumer(on_sample);

    std::cout << "[INFO] Air Whiteboard (smartboard) — letter/shape modes + word drag\n";
    std::cout << "[INFO] Camera:            " << input_config << "\n";
    std::cout << "[INFO] Palm model:        " << model_base_path + "/models/palm_detection_full.tflite" << "\n";
    std::cout << "[INFO] Hand landmark model: " << model_base_path + "/models/hand_landmark_full.tflite" << "\n";
    std::cout << "[INFO] Gestures:\n";
    std::cout << "[INFO]   open palm      = LETTER mode\n";
    std::cout << "[INFO]   closed palm    = SHAPE mode\n";
    std::cout << "[INFO]   index finger   = draw in current board mode\n";
    std::cout << "[INFO]   pinch          = grab & drag nearest word/shape\n";
    std::cout << "[INFO]   pinky finger   = eraser\n";
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
