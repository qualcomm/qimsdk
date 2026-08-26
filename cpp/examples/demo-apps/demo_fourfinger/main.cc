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
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <set>
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
static const std::string home_path = std::getenv("HOME") ? std::getenv("HOME") : "";
// Input source configuration, overridden via the --input-config argument.
static std::string input_config;

static constexpr int FRAME_WIDTH  = 1920;
static constexpr int FRAME_HEIGHT = 1080;
static constexpr int FRAME_FPS    = 20;

// Base path for sample assets (media/, models/, labels/ live under it).
// Set from the --model-base-path argument, or the default location.
static std::string model_base_path;

static constexpr int THUMB_TIP_ID = 4;
static constexpr int INDEX_TIP_ID = 8;

static constexpr double POINT_SMOOTHING_ALPHA = 0.35;
static constexpr int MISS_HOLD_FRAMES = 5;
static constexpr double MIN_POLYGON_AREA = 1200.0;

static constexpr int CLIP_WIDTH = 1920;
static constexpr int CLIP_HEIGHT = 1080;

using Point = std::pair<int, int>;
using Hand = std::map<int, Point>;

// Landmark label -> id map (MediaPipe hand landmark naming)
static const std::map<std::string, int> LABEL_TO_ID = {
    {"wrist", 0}, {"thumb cmc", 1}, {"thumb mcp", 2}, {"thumb ip", 3}, {"thumb tip", 4},
    {"index finger mcp", 5}, {"index finger pip", 6}, {"index finger dip", 7}, {"index finger tip", 8},
    {"middle finger mcp", 9}, {"middle finger pip", 10}, {"middle finger dip", 11}, {"middle finger tip", 12},
    {"ring finger mcp", 13}, {"ring finger pip", 14}, {"ring finger dip", 15}, {"ring finger tip", 16},
    {"pinky mcp", 17}, {"pinky pip", 18}, {"pinky dip", 19}, {"pinky tip", 20},
};

struct FourFingerState {
    std::mutex mtx;

    bool visible = false;
    std::string status = "WAITING FOR TWO HANDS";
    int hand_count = 0;
    int miss_count = 0;

    std::optional<Point> left_index;
    std::optional<Point> right_index;
    std::optional<Point> right_thumb;
    std::optional<Point> left_thumb;

    std::shared_ptr<std::vector<std::uint8_t>> video_rgbx;
    int video_width = CLIP_WIDTH;
    int video_height = CLIP_HEIGHT;
    bool has_video_frame = false;

    std::string video_status = "VIDEO NOT STARTED";
    std::string video_error;
    std::string video_draw_error;
    std::uint64_t video_frame_index = 0;
};

static FourFingerState g_state;

// =============================================================================
// Helpers
// =============================================================================
static Element make_queue(const std::string& name) {
    Element q("queue", name);
    q.set("leaky", 2);
    q.set("max-size-buffers", 2);
    q.set("max-size-bytes", 0);
    q.set("max-size-time", static_cast<std::uint64_t>(0));
    return q;
}

static int clamp_int(int v, int lo, int hi) {
    return std::max(lo, std::min(v, hi));
}

static std::optional<int> to_int_px(double value, int size) {
    double n = value;
    if (n >= -0.25 && n <= 2.0) {
        n *= static_cast<double>(size);
    }
    if (!std::isfinite(n)) return std::nullopt;
    return static_cast<int>(std::lround(n));
}

static std::optional<int> to_int_px(const std::string& value, int size) {
    try {
        return to_int_px(std::stod(value), size);
    } catch (...) {
        return std::nullopt;
    }
}

static std::optional<Point> smooth_point(const std::optional<Point>& prev,
                                         const std::optional<Point>& cur,
                                         double alpha) {
    if (!cur) return prev;
    if (!prev) return cur;

    int x = static_cast<int>(std::lround(alpha * static_cast<double>(cur->first) +
                                         (1.0 - alpha) * static_cast<double>(prev->first)));
    int y = static_cast<int>(std::lround(alpha * static_cast<double>(cur->second) +
                                         (1.0 - alpha) * static_cast<double>(prev->second)));
    return Point{x, y};
}

static std::pair<double, double> hand_center(const Hand& hand) {
    if (hand.empty()) return {0.0, 0.0};

    double sx = 0.0;
    double sy = 0.0;
    for (const auto& [_, p] : hand) {
        sx += static_cast<double>(p.first);
        sy += static_cast<double>(p.second);
    }
    return {sx / static_cast<double>(hand.size()), sy / static_cast<double>(hand.size())};
}

static double polygon_area(const std::vector<Point>& points) {
    if (points.size() < 3) return 0.0;

    double area = 0.0;
    for (size_t i = 0; i < points.size(); ++i) {
        const auto& p1 = points[i];
        const auto& p2 = points[(i + 1) % points.size()];
        area += static_cast<double>(p1.first) * static_cast<double>(p2.second) -
                static_cast<double>(p2.first) * static_cast<double>(p1.second);
    }
    return std::abs(area) * 0.5;
}

static void draw_polygon_path(cairo_t* cr, const std::vector<Point>& points) {
    if (!cr || points.empty()) return;

    cairo_new_path(cr);
    cairo_move_to(cr, points[0].first, points[0].second);
    for (size_t i = 1; i < points.size(); ++i) {
        cairo_line_to(cr, points[i].first, points[i].second);
    }
    cairo_close_path(cr);
}

static std::string normalize_key(std::string v) {
    for (auto& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (auto& c : v) if (c == '_' || c == '-') c = ' ';
    size_t b = v.find_first_not_of(' ');
    size_t e = v.find_last_not_of(' ');
    if (b == std::string::npos) return "";
    return v.substr(b, e - b + 1);
}

// =============================================================================
// Minimal JSON parser
// =============================================================================
struct JsonValue;
static JsonValue parse_json_value(const std::string& s, size_t& i);

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
    if (i < s.size()) ++i;
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

static std::optional<Point> point_any(const JsonValue& v);

static std::optional<Point> point_dict(const JsonValue& d) {
    if (const JsonValue* xv = d.find({"x", "X", "x_pos", "xpos", "center_x", "centerX", "px", "point_x"})) {
        if (const JsonValue* yv = d.find({"y", "Y", "y_pos", "ypos", "center_y", "centerY", "py", "point_y"})) {
            auto xn = xv->as_number();
            auto yn = yv->as_number();
            if (xn && yn) {
                auto x = to_int_px(*xn, FRAME_WIDTH);
                auto y = to_int_px(*yn, FRAME_HEIGHT);
                if (x && y) return Point{*x, *y};
            }
        }
    }

    if (const JsonValue* rv = d.find({"rectangle", "rect", "bbox", "box", "bounding_box", "bounding-box"})) {
        if (rv->type == JsonValue::Array && rv->arr.size() >= 4) {
            auto x1n = rv->arr[0].as_number();
            auto y1n = rv->arr[1].as_number();
            auto x2n = rv->arr[2].as_number();
            auto y2n = rv->arr[3].as_number();
            if (x1n && y1n && x2n && y2n) {
                auto x1 = to_int_px(*x1n, FRAME_WIDTH);
                auto y1 = to_int_px(*y1n, FRAME_HEIGHT);
                auto x2 = to_int_px(*x2n, FRAME_WIDTH);
                auto y2 = to_int_px(*y2n, FRAME_HEIGHT);
                if (x1 && y1 && x2 && y2) return Point{(*x1 + *x2) / 2, (*y1 + *y2) / 2};
            }
        }
    }

    for (const char* key : {"point", "position", "coord", "coords", "coordinate", "location"}) {
        if (const JsonValue* pv = d.find({key})) {
            if (auto p = point_any(*pv)) return p;
        }
    }
    return std::nullopt;
}

static std::optional<Point> point_any(const JsonValue& v) {
    if (v.type == JsonValue::Object) return point_dict(v);
    if (v.type == JsonValue::Array && v.arr.size() >= 2) {
        auto xn = v.arr[0].as_number();
        auto yn = v.arr[1].as_number();
        if (xn && yn) {
            auto x = to_int_px(*xn, FRAME_WIDTH);
            auto y = to_int_px(*yn, FRAME_HEIGHT);
            if (x && y) return Point{*x, *y};
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
        auto it = LABEL_TO_ID.find(normalize_key(lv->str));
        if (it != LABEL_TO_ID.end()) return it->second;
    }
    return std::nullopt;
}

static void sequence_to_hands(const JsonValue& seq, std::vector<Hand>& hands) {
    if (seq.type != JsonValue::Array || seq.arr.empty()) return;

    int explicit_ids = 0;
    for (const auto& item : seq.arr) {
        if (item.type == JsonValue::Object) {
            if (landmark_id(item)) ++explicit_ids;
        }
    }

    if (explicit_ids >= 15) {
        Hand current;
        for (const auto& item : seq.arr) {
            if (item.type != JsonValue::Object) continue;
            auto id = landmark_id(item);
            auto p = point_dict(item);
            if (!id || !p) continue;
            if (current.count(*id) && current.size() >= 15) {
                hands.push_back(current);
                current.clear();
            }
            current[*id] = *p;
            if (current.size() >= 21) {
                hands.push_back(current);
                current.clear();
            }
        }
        if (current.size() >= 15) hands.push_back(current);
        return;
    }

    int point_count = 0;
    for (const auto& item : seq.arr) if (point_any(item)) ++point_count;
    if (point_count >= 15) {
        for (size_t off = 0; off < seq.arr.size(); off += 21) {
            Hand hand;
            size_t end = std::min(seq.arr.size(), off + 21);
            if (end - off < 15) continue;
            for (size_t i = off; i < end && i < off + 21; ++i) {
                if (auto p = point_any(seq.arr[i])) hand[static_cast<int>(i - off)] = *p;
            }
            if (hand.size() >= 15) hands.push_back(hand);
        }
    }
}

static void collect_hands(const JsonValue& obj, std::vector<Hand>& hands) {
    if (obj.type == JsonValue::Array) {
        size_t before = hands.size();
        for (const auto& item : obj.arr) {
            if (item.type == JsonValue::Object || item.type == JsonValue::Array) {
                collect_hands(item, hands);
            }
        }
        if (hands.size() == before) sequence_to_hands(obj, hands);
        return;
    }

    if (obj.type == JsonValue::Object) {
        static const std::vector<std::string> known_keys = {
            "landmarks", "hand_landmarks", "keypoints", "points", "joints",
            "results", "detections", "objects", "predictions", "children",
        };
        size_t before = hands.size();
        for (const auto& key : known_keys) {
            if (const JsonValue* v = obj.find({key})) {
                if (v->type == JsonValue::Object || v->type == JsonValue::Array) collect_hands(*v, hands);
            }
        }

        Hand numeric_hand;
        for (const auto& kv : obj.obj) {
            try {
                int id = std::stoi(kv.first);
                if (id < 0 || id > 20) continue;
                if (auto p = point_any(kv.second)) numeric_hand[id] = *p;
            } catch (...) {
            }
        }
        if (numeric_hand.size() >= 15) {
            hands.push_back(numeric_hand);
            return;
        }
        if (hands.size() > before) return;
        for (const auto& kv : obj.obj) {
            if (kv.second.type == JsonValue::Object || kv.second.type == JsonValue::Array) collect_hands(kv.second, hands);
        }
    }
}

static Hand clean_hand(const Hand& in) {
    Hand out;
    for (const auto& [id, p] : in) {
        if (id < 0 || id > 20) continue;
        out[id] = {
            clamp_int(p.first, 0, FRAME_WIDTH - 1),
            clamp_int(p.second, 0, FRAME_HEIGHT - 1),
        };
    }
    return out;
}

static std::vector<Hand> dedupe_hands(std::vector<Hand> hands) {
    std::sort(hands.begin(), hands.end(), [](const Hand& a, const Hand& b) {
        return a.size() > b.size();
    });

    std::vector<Hand> unique;
    for (const auto& hand : hands) {
        if (!hand.count(THUMB_TIP_ID) || !hand.count(INDEX_TIP_ID)) continue;

        auto [cx, cy] = hand_center(hand);
        bool duplicate = false;
        for (const auto& u : unique) {
            auto [ux, uy] = hand_center(u);
            if (std::hypot(cx - ux, cy - uy) < 35.0) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) unique.push_back(hand);
    }
    return unique;
}

static std::vector<Hand> parse_hands(const std::string& text) {
    std::vector<Hand> hands;
    if (auto obj = load_json(text)) {
        collect_hands(*obj, hands);
    }

    std::vector<Hand> cleaned;
    for (const auto& h : hands) {
        Hand ch = clean_hand(h);
        if (ch.size() >= 2 && ch.count(THUMB_TIP_ID) && ch.count(INDEX_TIP_ID)) {
            cleaned.push_back(std::move(ch));
        }
    }
    cleaned = dedupe_hands(std::move(cleaned));

    if (cleaned.size() < 2) {
        static const std::regex p1(
            R"((?:id|class_id|classId|label_id|labelId|index|idx)\s*[:=]\s*(\d+).{0,220}?(?:x|X)\s*[:=]\s*(-?\d+(?:\.\d+)?).{0,80}?(?:y|Y)\s*[:=]\s*(-?\d+(?:\.\d+)?))",
            std::regex::icase);
        static const std::regex p2(
            R"((?:id|class_id|classId|label_id|labelId|index|idx)\s*[:=]\s*(\d+).{0,220}?(?:y|Y)\s*[:=]\s*(-?\d+(?:\.\d+)?).{0,80}?(?:x|X)\s*[:=]\s*(-?\d+(?:\.\d+)?))",
            std::regex::icase);

        Hand fallback;
        for (std::sregex_iterator it(text.begin(), text.end(), p1), end; it != end; ++it) {
            int id = std::stoi((*it)[1].str());
            auto x = to_int_px((*it)[2].str(), FRAME_WIDTH);
            auto y = to_int_px((*it)[3].str(), FRAME_HEIGHT);
            if (id < 0 || id > 20 || !x || !y) continue;
            fallback[id] = {clamp_int(*x, 0, FRAME_WIDTH - 1), clamp_int(*y, 0, FRAME_HEIGHT - 1)};
        }
        for (std::sregex_iterator it(text.begin(), text.end(), p2), end; it != end; ++it) {
            int id = std::stoi((*it)[1].str());
            auto y = to_int_px((*it)[2].str(), FRAME_HEIGHT);
            auto x = to_int_px((*it)[3].str(), FRAME_WIDTH);
            if (id < 0 || id > 20 || !x || !y) continue;
            fallback[id] = {clamp_int(*x, 0, FRAME_WIDTH - 1), clamp_int(*y, 0, FRAME_HEIGHT - 1)};
        }

        if (fallback.count(THUMB_TIP_ID) && fallback.count(INDEX_TIP_ID)) cleaned.push_back(std::move(fallback));
        cleaned = dedupe_hands(std::move(cleaned));
    }

    return cleaned;
}

static void update_state_from_hands(const std::vector<Hand>& hands) {
    std::vector<Hand> valid;
    for (const auto& h : hands) {
        if (h.count(THUMB_TIP_ID) && h.count(INDEX_TIP_ID)) {
            valid.push_back(h);
        }
    }

    std::sort(valid.begin(), valid.end(), [](const Hand& a, const Hand& b) {
        return hand_center(a).first < hand_center(b).first;
    });

    std::lock_guard<std::mutex> lock(g_state.mtx);
    g_state.hand_count = static_cast<int>(valid.size());

    if (valid.size() >= 2) {
        const Hand& left = valid.front();
        const Hand& right = valid.back();

        g_state.left_index = smooth_point(g_state.left_index, left.at(INDEX_TIP_ID), POINT_SMOOTHING_ALPHA);
        g_state.right_index = smooth_point(g_state.right_index, right.at(INDEX_TIP_ID), POINT_SMOOTHING_ALPHA);
        g_state.right_thumb = smooth_point(g_state.right_thumb, right.at(THUMB_TIP_ID), POINT_SMOOTHING_ALPHA);
        g_state.left_thumb = smooth_point(g_state.left_thumb, left.at(THUMB_TIP_ID), POINT_SMOOTHING_ALPHA);

        g_state.visible = true;
        g_state.status = "VIDEO REGION ACTIVE";
        g_state.miss_count = 0;
    } else {
        g_state.miss_count += 1;
        if (g_state.miss_count > MISS_HOLD_FRAMES) {
            g_state.visible = false;
            g_state.status = "WAITING FOR TWO HANDS";
            g_state.left_index.reset();
            g_state.right_index.reset();
            g_state.right_thumb.reset();
            g_state.left_thumb.reset();
        }
    }
}

// =============================================================================
// AppSink callbacks
// =============================================================================
static void on_clip_sample(qti::Buffer buffer)
{
    const qti::Buffer& cbuffer = buffer;
    if (!cbuffer.valid() || !cbuffer.data()) return;

    const std::size_t min_bgr = static_cast<std::size_t>(CLIP_WIDTH) *
                                static_cast<std::size_t>(CLIP_HEIGHT) * 3;

    if (cbuffer.size() < min_bgr) {
        std::lock_guard<std::mutex> lock(g_state.mtx);
        g_state.video_status = "VIDEO SAMPLE ERROR";
        g_state.video_error = "buffer is too small for requested array";
        return;
    }

    const std::size_t src_stride = cbuffer.size() / static_cast<std::size_t>(CLIP_HEIGHT);
    if (src_stride < static_cast<std::size_t>(CLIP_WIDTH) * 3) {
        std::lock_guard<std::mutex> lock(g_state.mtx);
        g_state.video_status = "VIDEO SAMPLE ERROR";
        g_state.video_error = "row stride too small for BGR frame";
        return;
    }

    std::vector<std::uint8_t> rgbx(static_cast<std::size_t>(CLIP_WIDTH) *
                                   static_cast<std::size_t>(CLIP_HEIGHT) * 4);

    const std::uint8_t* src = reinterpret_cast<const std::uint8_t*>(cbuffer.data());
    for (int y = 0; y < CLIP_HEIGHT; ++y) {
        const std::uint8_t* row = src + static_cast<std::size_t>(y) * src_stride;
        std::uint8_t* dst = rgbx.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(CLIP_WIDTH) * 4;
        for (int x = 0; x < CLIP_WIDTH; ++x) {
            const std::size_t s = static_cast<std::size_t>(x) * 3;
            const std::size_t d = static_cast<std::size_t>(x) * 4;
            dst[d + 0] = row[s + 0];
            dst[d + 1] = row[s + 1];
            dst[d + 2] = row[s + 2];
            dst[d + 3] = 0xFF;
        }
    }

    std::lock_guard<std::mutex> lock(g_state.mtx);
    g_state.video_rgbx = std::make_shared<std::vector<std::uint8_t>>(std::move(rgbx));
    g_state.video_width = CLIP_WIDTH;
    g_state.video_height = CLIP_HEIGHT;
    g_state.has_video_frame = true;
    g_state.video_status = "VIDEO PLAYING";
    g_state.video_error.clear();
    g_state.video_frame_index += 1;
}

static void on_sample(qti::Buffer buffer)
{
    const qti::Buffer& cbuffer = buffer;
    if (!cbuffer.valid() || !cbuffer.data()) return;

    std::string text(reinterpret_cast<const char*>(cbuffer.data()), cbuffer.size());
    auto hands = parse_hands(text);
    update_state_from_hands(hands);
}

// =============================================================================
// Cairo draw
// =============================================================================
static bool draw_video_in_polygon(cairo_t* cr, const std::vector<Point>& points)
{
    std::shared_ptr<std::vector<std::uint8_t>> frame;
    int src_w = 0;
    int src_h = 0;

    {
        std::lock_guard<std::mutex> lock(g_state.mtx);
        if (!g_state.has_video_frame || !g_state.video_rgbx || g_state.video_rgbx->empty()) {
            return false;
        }
        frame = g_state.video_rgbx;
        src_w = g_state.video_width;
        src_h = g_state.video_height;
    }

    int x0 = std::numeric_limits<int>::max();
    int y0 = std::numeric_limits<int>::max();
    int x1 = std::numeric_limits<int>::min();
    int y1 = std::numeric_limits<int>::min();
    for (const auto& p : points) {
        x0 = std::min(x0, clamp_int(p.first, 0, FRAME_WIDTH - 1));
        y0 = std::min(y0, clamp_int(p.second, 0, FRAME_HEIGHT - 1));
        x1 = std::max(x1, clamp_int(p.first, 0, FRAME_WIDTH - 1));
        y1 = std::max(y1, clamp_int(p.second, 0, FRAME_HEIGHT - 1));
    }

    int box_w = std::max(2, x1 - x0);
    int box_h = std::max(2, y1 - y0);

    cairo_surface_t* surface = cairo_image_surface_create_for_data(
        frame->data(), CAIRO_FORMAT_RGB24, src_w, src_h, src_w * 4);

    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surface);
        std::lock_guard<std::mutex> lock(g_state.mtx);
        g_state.video_draw_error = "cairo surface create failed";
        return false;
    }

    cairo_save(cr);
    draw_polygon_path(cr, points);
    cairo_clip(cr);

    cairo_translate(cr, x0, y0);
    cairo_scale(cr,
        static_cast<double>(box_w) / static_cast<double>(src_w),
        static_cast<double>(box_h) / static_cast<double>(src_h));
    cairo_set_source_surface(cr, surface, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
    cairo_paint(cr);

    cairo_restore(cr);
    cairo_surface_destroy(surface);

    std::lock_guard<std::mutex> lock(g_state.mtx);
    g_state.video_draw_error.clear();
    return true;
}

static void draw_points_and_border(cairo_t* cr, const std::vector<Point>& points)
{
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 1.0);
    cairo_set_line_width(cr, 7.0);
    draw_polygon_path(cr, points);
    cairo_stroke(cr);

    for (const auto& p : points) {
        cairo_arc(cr, p.first, p.second, 12.0, 0.0, 2.0 * M_PI);
        cairo_fill(cr);
    }
}

static void draw_status_box(cairo_t* cr)
{
    std::string status;
    std::string video_status;
    std::string video_error;
    std::string draw_error;
    std::uint64_t frame_index = 0;
    int hand_count = 0;

    {
        std::lock_guard<std::mutex> lock(g_state.mtx);
        status = g_state.status;
        video_status = g_state.video_status;
        video_error = g_state.video_error;
        draw_error = g_state.video_draw_error;
        frame_index = g_state.video_frame_index;
        hand_count = g_state.hand_count;
    }

    cairo_save(cr);
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.72);
    cairo_rectangle(cr, 20, 20, 760, 130);
    cairo_fill(cr);

    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);

    cairo_set_font_size(cr, 28);
    cairo_set_source_rgba(cr, 0.0, 1.0, 0.0, 1.0);
    cairo_move_to(cr, 35, 58);
    cairo_show_text(cr, ("RECTANGLE VIDEO: " + status).c_str());

    cairo_set_font_size(cr, 21);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.95);
    cairo_move_to(cr, 35, 92);
    cairo_show_text(cr, ("hands=" + std::to_string(hand_count) +
                        " video=" + video_status +
                        " frame=" + std::to_string(frame_index)).c_str());

    if (!video_error.empty()) {
        cairo_set_source_rgba(cr, 1.0, 0.6, 0.2, 1.0);
        cairo_move_to(cr, 35, 122);
        cairo_show_text(cr, ("video error: " + video_error).c_str());
    } else if (!draw_error.empty()) {
        cairo_set_source_rgba(cr, 1.0, 0.6, 0.2, 1.0);
        cairo_move_to(cr, 35, 122);
        cairo_show_text(cr, ("draw error: " + draw_error).c_str());
    } else {
        cairo_set_source_rgba(cr, 0.8, 0.9, 1.0, 1.0);
        cairo_move_to(cr, 35, 122);
        cairo_show_text(cr, (model_base_path + "/media/ai_demo_sample.mp4").c_str());
    }

    cairo_restore(cr);
}

static void on_cairo_draw_signal(void* /*overlay*/, void* draw_context,
                                 std::uint64_t /*timestamp*/, std::uint64_t /*duration*/,
                                 void* /*user_data*/) {
    if (!draw_context) return;
    cairo_t* cr = static_cast<cairo_t*>(draw_context);

    bool visible = false;
    std::vector<Point> points;

    {
        std::lock_guard<std::mutex> lock(g_state.mtx);
        visible = g_state.visible;
        if (g_state.left_index) points.push_back(*g_state.left_index);
        if (g_state.right_index) points.push_back(*g_state.right_index);
        if (g_state.right_thumb) points.push_back(*g_state.right_thumb);
        if (g_state.left_thumb) points.push_back(*g_state.left_thumb);
    }

    if (visible && points.size() == 4 && polygon_area(points) >= MIN_POLYGON_AREA) {
        draw_video_in_polygon(cr, points);
        draw_points_and_border(cr, points);
    }

    draw_status_box(cr);
}

// =============================================================================
// Pipeline construction and run
// =============================================================================

//  Example pipelines:
//
//    clip: src -> demux -> parse -> decoder -> transform -> [vf:BGR] -> clip_sink(appsink)
//
//    main: source -> transform -> [videostream] -> split
//      split. -> q_video_palm -> metamux_palm
//      split. -> q_palm_pre -> palm_preproc -> q_palm_infer -> palm_inf -> q_palm_post
//             -> palm_post -> [palm_mlf:text] -> q_palm_meta -> metamux_palm
//      metamux_palm -> palm_roi_transform -> split_after_palm
//      split_after_palm. -> q_video_final -> metamux_final
//      split_after_palm. -> q_hand_pre -> hand_preproc -> q_hand_infer -> hand_inf -> q_hand_post
//                       -> hand_post -> [hand_mlf:text] -> q_hand_meta -> metamux_final
//      metamux_final -> final_split
//      final_split. -> q_display -> qtivoverlay -> to_cairo -> [cairofilter:BGRA] -> video_region_canvas -> display
//      final_split. -> q_meta_parse -> qtimlmetaparser(json) -> meta_sink(appsink)
//
//  Hand-landmark metadata drives fingertip polygon points while Cairo draws
//  the decoded clip frame clipped inside that polygon.
void create_and_execute_pipeline()
{
    // -------------------------------------------------------------------------
    // Clip decode pipeline (appsink consumer callback)
    // -------------------------------------------------------------------------

    // Clip file source.
    Element clip_source("filesrc", "clip_src");
    clip_source.set("location", model_base_path + "/media/ai_demo_sample.mp4");

    // Clip MP4 demuxer.
    Element clip_demux("qtdemux", "clip_demux");

    // Clip H264 parser.
    Element clip_parse("h264parse", "clip_parse");

    // Clip hardware decoder.
    Element clip_decoder("v4l2h264dec", "clip_decoder");
    clip_decoder.set("capture-io-mode", 4);
    clip_decoder.set("output-io-mode", 4);

    // Clip color transform.
    Element clip_transform("qtivtransform", "clip_transform");

    // Clip appsink format filter.
    auto clip_filter = VideoFilter()
        .format("BGR")
        .resolution(CLIP_WIDTH, CLIP_HEIGHT);

    // Clip appsink element.
    AppSink clip_sink("clip_sink");
    clip_sink.set("sync", false);
    clip_sink.set("max-buffers", 1);
    clip_sink.set("drop", true);
    clip_sink.set_buffer_consumer(on_clip_sample);

    Pipeline clip_pipeline("fourfinger_clip");
    clip_pipeline
        .add(clip_source)
        .add(clip_demux)
        .add(clip_parse)
        .add(clip_decoder)
        .add(clip_transform)
        .add_stream_filter("clip_filter", clip_filter)
        .add(clip_sink)
        .link("clip_src", "clip_demux", "clip_parse", "clip_decoder", "clip_transform", "clip_filter", "clip_sink");

    {
        std::lock_guard<std::mutex> lock(g_state.mtx);
        g_state.video_status = "OPENING VIDEO...";
        g_state.video_error.clear();
    }

    // -------------------------------------------------------------------------
    // Main camera + ML + display pipeline
    // -------------------------------------------------------------------------

    // Camera source element.
    Element source("v4l2src", "source");
    source.set("device", input_config);

    // Camera transform element.
    Element transform("qtivtransform", "transform");
    transform.set("flip-horizontal", true);

    // Camera stream caps filter.
    auto videostream = VideoFilter()
        .format("NV12")
        .resolution(FRAME_WIDTH, FRAME_HEIGHT)
        .framerate(FRAME_FPS);

    // Camera tee split element.
    Element split("tee", "split");

    // Queue for video path into palm metamux.
    Element q_video_palm = make_queue("q_video_palm");

    // Queue before palm preprocessor.
    Element q_palm_pre = make_queue("q_palm_pre");

    // Palm preprocessor element.
    Element palm_preproc("qtimlvconverter", "palm_preproc");
    palm_preproc.set("mode", "image-batch-non-cumulative");

    // Queue before palm inference.
    Element q_palm_infer = make_queue("q_palm_infer");

    // Palm inference element.
    Element palm_inf("qtimltflite", "palm_inf");
    palm_inf.set("delegate", "gpu");
    palm_inf.set("model", model_base_path + "/models/palm_detection_full.tflite");

    // Queue before palm postprocess.
    Element q_palm_post = make_queue("q_palm_post");

    // Palm postprocess element.
    Element palm_post("qtimlpostprocess", "palm_post");
    palm_post.set("module", "palmd");
    palm_post.set("results", 2);
    palm_post.set("labels", model_base_path + "/labels/palmd_labels.json");
    palm_post.set("settings", model_base_path + "/labels/palmd_settings.json");

    // Palm text metadata filter.
    auto palm_mlf = TextFilter();

    // Queue for palm metadata.
    Element q_palm_meta = make_queue("q_palm_meta");

    // Palm metadata mux element.
    Element metamux_palm("qtimetamux", "metamux_palm");

    // Palm ROI transform element.
    Element palm_roi_transform("qtimetatransform", "palm_roi_transform");
    palm_roi_transform.set("module", "roi-palmd");

    // Tee after palm ROI transform.
    Element split_after_palm("tee", "split_after_palm");

    // Queue for video path into final metamux.
    Element q_video_final = make_queue("q_video_final");

    // Queue before hand preprocessor.
    Element q_hand_pre = make_queue("q_hand_pre");

    // Hand preprocessor element.
    Element hand_preproc("qtimlvconverter", "hand_preproc");
    hand_preproc.set("mode", "roi-batch-cumulative");

    // Queue before hand inference.
    Element q_hand_infer = make_queue("q_hand_infer");

    // Hand inference element.
    Element hand_inf("qtimltflite", "hand_inf");
    hand_inf.set("delegate", "xnnpack");
    hand_inf.set("model", model_base_path + "/models/hand_landmark_full.tflite");

    // Queue before hand postprocess.
    Element q_hand_post = make_queue("q_hand_post");

    // Hand postprocess element.
    Element hand_post("qtimlpostprocess", "hand_post");
    hand_post.set("module", "hlandmark");
    hand_post.set("results", 2);
    hand_post.set("labels", model_base_path + "/labels/hlandmarks.json");
    hand_post.set("settings", model_base_path + "/labels/hlandmark_settings.json");

    // Hand text metadata filter.
    auto hand_mlf = TextFilter();

    // Queue for hand metadata.
    Element q_hand_meta = make_queue("q_hand_meta");

    // Final metadata mux element.
    Element metamux_final("qtimetamux", "metamux_final");

    // Final tee split element.
    Element final_split("tee", "final_split");

    // Queue for display branch.
    Element q_display = make_queue("q_display");

    // Qualcomm metadata overlay element.
    Element overlay("qtivoverlay", "overlay");

    // Transform before cairooverlay.
    Element to_cairo("qtivtransform", "to_cairo");

    // BGRA caps filter for cairooverlay.
    auto cairofilter = VideoFilter()
        .format("BGRA")
        .resolution(FRAME_WIDTH, FRAME_HEIGHT)
        .framerate(FRAME_FPS);

    // Cairo overlay element.
    Element video_region_canvas("cairooverlay", "video_region_canvas");
    video_region_canvas.connect_signal(
        "draw",
        reinterpret_cast<Element::SignalCallback>(&on_cairo_draw_signal));

    // Display sink element.
    Element display("waylandsink", "display");
    display.set("sync", false);
    display.set("fullscreen", true);

    // Queue before metadata parser.
    Element q_meta_parse = make_queue("q_meta_parse");

    // Metadata parser element.
    Element meta_parser("qtimlmetaparser", "meta_parser");
    meta_parser.set("module", "json");

    // Metadata appsink element.
    AppSink meta_sink("meta_sink");
    meta_sink.set("sync", false);
    meta_sink.set("max-buffers", 1);
    meta_sink.set("drop", true);
    meta_sink.set_buffer_consumer(on_sample);

    Pipeline pipeline("fourfinger");
    pipeline
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
        .link("final_split", "q_display", "overlay", "to_cairo", "cairofilter", "video_region_canvas", "display")
        .link("final_split", "q_meta_parse", "meta_parser", "meta_sink");

    std::cout << "[INFO] Starting two-hand fingertip video region application...\n";
    std::cout << "[INFO] Camera:     " << input_config << "\n";
    std::cout << "[INFO] Video path: " << model_base_path + "/media/ai_demo_sample.mp4" << "\n";

    clip_pipeline.start();
    try {
        pipeline.execute();
    } catch (...) {
        clip_pipeline.stop();
        throw;
    }
    clip_pipeline.stop();
}

}  // namespace

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
