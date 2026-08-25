/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <string>
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
static std::string input_config;
static constexpr int FRAME_WIDTH  = 1920;
static constexpr int FRAME_HEIGHT = 1080;
static constexpr int FRAME_FPS    = 30;

// Base path for sample assets (media/, models/, labels/ live under it).
// Set from the --model-base-path argument, or the default location.
static std::string           model_base_path;

static constexpr int    WRIST_ID           = 0;   // MediaPipe wrist landmark id
static constexpr double WRIST_TIMEOUT_SEC  = 0.70;

static constexpr double WRIST_LINE_WIDTH = 10.0;
static constexpr double WRIST_DOT_RADIUS = 13.0;

static constexpr double MAX_STEERING_ANGLE_DEG = 45.0;
static constexpr bool   STEERING_INVERT        = false;
static constexpr double GAME_AUTO_RESTART_SEC  = 2.5;

// "right" or "left": where to put the fallback steering wheel inside the
// game half when no wrist pair is currently detected.
static constexpr const char* STEERING_WHEEL_SIDE = "right";

// =============================================================================
// Basic geometry types
// =============================================================================
using Point = std::pair<double, double>;

static double p_dist(const Point& a, const Point& b) {
    return std::hypot(b.first - a.first, b.second - a.second);
}

static double clampd(double v, double lo, double hi) {
    return std::max(lo, std::min(v, hi));
}

// =============================================================================
// Viewport helpers
// =============================================================================
struct Viewport { double x, y, w, h; };

static Viewport left_viewport() {
    int w = std::max(1, FRAME_WIDTH);
    int h = std::max(1, FRAME_HEIGHT);
    return {0.0, 0.0, static_cast<double>(w / 2), static_cast<double>(h)};
}

static Viewport game_viewport() {
    int w = std::max(1, FRAME_WIDTH);
    int h = std::max(1, FRAME_HEIGHT);
    int half = w / 2;
    return {static_cast<double>(half), 0.0, static_cast<double>(w - half), static_cast<double>(h)};
}

// =============================================================================
// Minimal JSON parser (hand-rolled) — sufficient for the wrist/landmark
// metadata produced by qtimlmetaparser(module=json) for hlandmark results.
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

static std::string norm_str(std::string v) {
    for (auto& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (auto& c : v) if (c == '_' || c == '-') c = ' ';
    size_t b = v.find_first_not_of(' ');
    size_t e = v.find_last_not_of(' ');
    if (b == std::string::npos) return "";
    return v.substr(b, e - b + 1);
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
        for (const auto& k : keys) wanted.insert(norm_str(k));
        for (const auto& kv : obj) {
            if (wanted.count(norm_str(kv.first))) return &kv.second;
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
// between the first '{'/'[' and the last matching '}'/']'.
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
// Wrist extraction
//
// Unlike smartboard (single hand, all 21 landmarks needed), this app only
// needs landmark #0 (the wrist) from up to two separately-tracked hands, so
// the approach here is: find every JSON array that looks like "one hand's
// landmark list" (>=21 points), take element WRIST_ID from each, convert to
// display pixels, then pick the two wrists with the largest horizontal
// separation as the heuristic for disambiguating which two detections are
// the two hands.
// =============================================================================
static std::optional<Point> xy_from_value(const JsonValue& v);

static std::optional<Point> xy_from_object(const JsonValue& d) {
    static const std::vector<std::pair<std::vector<std::string>, std::vector<std::string>>> pairs = {
        {{"x"}, {"y"}}, {{"X"}, {"Y"}}, {{"px"}, {"py"}}, {{"u"}, {"v"}},
        {{"cx"}, {"cy"}}, {{"left"}, {"top"}},
    };
    for (const auto& pr : pairs) {
        if (const JsonValue* xv = d.find(pr.first)) {
            if (const JsonValue* yv = d.find(pr.second)) {
                auto x = xv->as_number();
                auto y = yv->as_number();
                if (x && y) return Point{*x, *y};
            }
        }
    }
    for (const char* key : {"point", "position", "location", "coord", "coordinate",
                             "coordinates", "landmark", "keypoint"}) {
        if (const JsonValue* pv = d.find({key})) {
            if (auto p = xy_from_value(*pv)) return p;
        }
    }
    return std::nullopt;
}

static std::optional<Point> xy_from_value(const JsonValue& v) {
    if (v.type == JsonValue::Object) return xy_from_object(v);
    if (v.type == JsonValue::Array && v.arr.size() >= 2) {
        auto x = v.arr[0].as_number();
        auto y = v.arr[1].as_number();
        if (x && y) return Point{*x, *y};
    }
    return std::nullopt;
}

// Converts a JsonValue array into a list of (x, y) points if it looks like
// a hand's landmark/keypoint list (handles both [{x,y}, ...] and flat
// numeric [x, y, x, y, ...] / [x, y, z, x, y, z, ...] encodings).
static std::vector<Point> points_from_array(const JsonValue& v) {
    std::vector<Point> pts;
    if (v.type != JsonValue::Array) return pts;

    for (const auto& item : v.arr) {
        if (auto p = xy_from_value(item)) pts.push_back(*p);
    }
    if (pts.size() >= 2) return pts;

    std::vector<double> nums;
    for (const auto& item : v.arr) {
        if (auto n = item.as_number()) nums.push_back(*n);
    }
    if (nums.size() >= 42) {
        size_t step = (nums.size() >= 63) ? 3 : 2;
        std::vector<Point> pts2;
        for (size_t idx = 0; idx + 1 < nums.size(); idx += step)
            pts2.push_back({nums[idx], nums[idx + 1]});
        if (pts2.size() >= 2) return pts2;
    }
    return {};
}

// Recursively finds arrays that look like one hand's landmark list.
static void collect_hand_point_lists(const JsonValue& obj, std::vector<std::vector<Point>>& out) {
    if (obj.type == JsonValue::Object) {
        for (const auto& kv : obj.obj) {
            std::string nk = norm_str(kv.first);
            bool looks_like_landmarks =
                nk.find("landmark") != std::string::npos ||
                nk.find("keypoint") != std::string::npos ||
                nk.find("key point") != std::string::npos ||
                nk.find("points") != std::string::npos ||
                nk.find("hand landmarks") != std::string::npos;
            if (looks_like_landmarks) {
                auto pts = points_from_array(kv.second);
                if (pts.size() >= 2) out.push_back(std::move(pts));
            }
            collect_hand_point_lists(kv.second, out);
        }
    } else if (obj.type == JsonValue::Array) {
        auto pts = points_from_array(obj);
        if (pts.size() >= 21) out.push_back(pts);
        for (const auto& item : obj.arr) collect_hand_point_lists(item, out);
    }
}

// Converts a raw landmark coordinate pair into display pixel coordinates.
// Metadata may arrive normalized (0..1) or already in camera pixels; the
// cairo surface here is the same resolution as the camera frame, so no
// extra display-vs-camera rescale is needed.
static std::optional<Point> to_pixels(const Point& raw) {
    double x = raw.first, y = raw.second;
    if (x >= -0.25 && x <= 1.25 && y >= -0.25 && y <= 1.25) {
        x *= FRAME_WIDTH;
        y *= FRAME_HEIGHT;
    }
    x = clampd(std::lround(x), 0, FRAME_WIDTH - 1);
    y = clampd(std::lround(y), 0, FRAME_HEIGHT - 1);
    return Point{x, y};
}

using WristPair = std::pair<Point, Point>;

// Parses the metadata text for up to two hands' wrist landmarks and
// returns the pair with the largest horizontal separation, ordered
// left-to-right.
static std::optional<WristPair> extract_two_wrists(const std::string& text) {
    auto root = load_json(text);
    if (!root) return std::nullopt;

    std::vector<std::vector<Point>> hand_lists;
    collect_hand_point_lists(*root, hand_lists);

    std::vector<Point> wrists;
    for (const auto& pts : hand_lists) {
        if (static_cast<int>(pts.size()) > WRIST_ID) {
            if (auto p = to_pixels(pts[WRIST_ID])) wrists.push_back(*p);
        }
    }

    // Remove near-duplicates (the same hand reported twice).
    std::vector<Point> unique;
    for (const auto& p : wrists) {
        bool dup = false;
        for (const auto& q : unique) {
            if (std::abs(p.first - q.first) <= 4 && std::abs(p.second - q.second) <= 4) { dup = true; break; }
        }
        if (!dup) unique.push_back(p);
    }

    if (unique.size() < 2) return std::nullopt;

    // Choose the pair with the largest horizontal separation.
    double best_score = -1.0;
    std::optional<WristPair> best_pair;
    for (size_t i = 0; i < unique.size(); ++i) {
        for (size_t j = i + 1; j < unique.size(); ++j) {
            double score = std::abs(unique[i].first - unique[j].first);
            if (score > best_score) {
                best_score = score;
                best_pair = WristPair{unique[i], unique[j]};
            }
        }
    }
    if (!best_pair) return std::nullopt;

    if (best_pair->first.first > best_pair->second.first)
        std::swap(best_pair->first, best_pair->second);
    return best_pair;
}

// =============================================================================
// Shared wrist state
//
// Updated (under mutex) once per incoming metadata buffer by the metadata
// AppSink callback (added in a later part); read by the Cairo-bridge AppSink
// callback on every camera frame to drive steering + drawing.
// =============================================================================
static double now_seconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

struct WristState {
    std::mutex mtx;
    std::optional<WristPair> pair;
    double timestamp = 0.0;
};
static WristState g_wrist_state;

// Returns the current wrist pair if it was updated recently enough.
static std::optional<WristPair> current_wrist_pair() {
    std::optional<WristPair> pair;
    double age = 0.0;
    {
        std::lock_guard<std::mutex> lock(g_wrist_state.mtx);
        pair = g_wrist_state.pair;
        age = now_seconds() - g_wrist_state.timestamp;
    }
    if (pair && age <= WRIST_TIMEOUT_SEC) return pair;
    return std::nullopt;
}

// =============================================================================
// Game state / physics
// =============================================================================
struct Obstacle {
    double x, y, w, h;
};

struct GameState {
    bool   initialized  = false;
    double last_time     = 0.0;
    double car_x         = 0.5;
    double car_y         = 0.84;
    double steer_smooth  = 0.0;
    double road_offset   = 0.0;
    double score         = 0.0;
    bool   alive         = true;
    double crash_time    = 0.0;
    std::vector<Obstacle> obstacles;
    double spawn_timer   = 0.0;
    std::mt19937 rng{7};
};
static GameState g_game;

static void reset_game(double now) {
    std::mt19937 rng = g_game.rng;
    g_game = GameState{};
    g_game.rng          = rng;
    g_game.initialized  = true;
    g_game.last_time    = now;
    g_game.car_x        = 0.5;
    g_game.car_y        = 0.84;
    g_game.steer_smooth = 0.0;
    g_game.road_offset  = 0.0;
    g_game.score        = 0.0;
    g_game.alive        = true;
    g_game.crash_time   = 0.0;
    g_game.spawn_timer  = 0.0;
}

// Angle-based steering calc from the wrist-to-wrist vector.
static std::optional<double> steering_from_pair(const std::optional<WristPair>& pair) {
    if (!pair) return std::nullopt;
    double dx = pair->second.first  - pair->first.first;
    double dy = pair->second.second - pair->first.second;
    if (std::abs(dx) < 1.0 && std::abs(dy) < 1.0) return std::nullopt;

    double angle_deg = std::atan2(dy, dx) * 180.0 / M_PI;
    double steer = clampd(angle_deg / MAX_STEERING_ANGLE_DEG, -1.0, 1.0);
    if (STEERING_INVERT) steer = -steer;
    return steer;
}

static Obstacle spawn_obstacle(double w, double h) {
    double road_left = 0.12 * w;
    double road_w    = 0.76 * w;
    double lane_w     = road_w / 3.0;
    std::uniform_int_distribution<int> lane_dist(0, 2);
    int lane = lane_dist(g_game.rng);

    Obstacle o;
    o.x = road_left + lane_w * (lane + 0.5);
    o.y = -0.13 * h;
    o.w = 0.105 * w;
    o.h = 0.115 * h;
    return o;
}

static bool rects_overlap(const Obstacle& a, const Obstacle& b) {
    return !(a.x + a.w < b.x ||
             b.x + b.w < a.x ||
             a.y + a.h < b.y ||
             b.y + b.h < a.y);
}

// Advances car/obstacle physics by one frame's worth of time.
static void update_game(const std::optional<WristPair>& pair) {
    double now = now_seconds();

    if (!g_game.initialized) reset_game(now);

    double dt = now - g_game.last_time;
    dt = clampd(dt, 0.0, 0.060);
    g_game.last_time = now;

    Viewport gv = game_viewport();
    double w = std::max(1.0, gv.w);
    double h = std::max(1.0, gv.h);

    if (!g_game.alive) {
        if (now - g_game.crash_time >= GAME_AUTO_RESTART_SEC) reset_game(now);
        return;
    }

    std::optional<double> raw_steer_opt = steering_from_pair(pair);
    double raw_steer = raw_steer_opt.value_or(0.0);

    g_game.steer_smooth = 0.82 * g_game.steer_smooth + 0.18 * raw_steer;
    double steer = g_game.steer_smooth;

    double road_left  = 0.12 * w;
    double road_w      = 0.76 * w;
    double road_right = road_left + road_w;

    double car_w = 0.105 * w;
    double car_h = 0.125 * h;

    double score = g_game.score;
    double speed_px = (0.38 * h) + std::min(score * 0.008 * h, 0.32 * h);
    double lateral_speed = 0.54 * w;

    g_game.car_x += steer * lateral_speed * dt / w;

    double min_x = (road_left + car_w * 0.55) / w;
    double max_x = (road_right - car_w * 0.55) / w;
    g_game.car_x = clampd(g_game.car_x, min_x, max_x);

    g_game.road_offset = std::fmod(g_game.road_offset + speed_px * dt, std::max(1.0, 0.11 * h));
    g_game.score += dt * 10.0;

    g_game.spawn_timer -= dt;
    if (g_game.spawn_timer <= 0.0) {
        g_game.obstacles.push_back(spawn_obstacle(w, h));
        g_game.spawn_timer = std::max(0.65, 1.35 - g_game.score * 0.006);
    }

    for (auto& obs : g_game.obstacles) obs.y += speed_px * dt;

    g_game.obstacles.erase(
        std::remove_if(g_game.obstacles.begin(), g_game.obstacles.end(),
                        [&](const Obstacle& o) { return o.y >= h + o.h; }),
        g_game.obstacles.end());

    Obstacle car_rect;
    car_rect.x = g_game.car_x * w - car_w * 0.45;
    car_rect.y = g_game.car_y * h - car_h * 0.50;
    car_rect.w = car_w * 0.90;
    car_rect.h = car_h * 0.90;

    for (const auto& obs : g_game.obstacles) {
        if (rects_overlap(car_rect, obs)) {
            g_game.alive = false;
            g_game.crash_time = now;
            break;
        }
    }
}

// =============================================================================
// Cairo drawing helpers
// =============================================================================
static void rounded_rect(cairo_t* cr, double x, double y, double w, double h, double radius) {
    double r = std::min({radius, w / 2.0, h / 2.0});
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r,     r, -M_PI / 2.0, 0.0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0.0,          M_PI / 2.0);
    cairo_arc(cr, x + r,     y + h - r, r, M_PI / 2.0,   M_PI);
    cairo_arc(cr, x + r,     y + r,     r, M_PI,         3.0 * M_PI / 2.0);
    cairo_close_path(cr);
}

static void draw_centered_text(cairo_t* cr, const std::string& text, double x, double y,
                                double size, double r, double g, double b, double a = 1.0,
                                bool bold = false) {
    cairo_save(cr);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                            bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, size);
    cairo_text_extents_t ext;
    cairo_text_extents(cr, text.c_str(), &ext);
    cairo_set_source_rgba(cr, r, g, b, a);
    cairo_move_to(cr, x - ext.width / 2.0 - ext.x_bearing, y - ext.height / 2.0 - ext.y_bearing);
    cairo_show_text(cr, text.c_str());
    cairo_restore(cr);
}

// =============================================================================
// Split-frame background — fills the right (game) half with an opaque
// panel, draws the cyan divider line, and labels each half ("LIVE VIDEO" /
// "CAR GAME"). The left half is left alone so the live camera frame
// underneath remains visible.
// =============================================================================
static void draw_split_frame(cairo_t* cr) {
    Viewport gv = game_viewport();
    double gx = gv.x;

    // Right half: opaque black game panel.
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.96);
    cairo_rectangle(cr, gv.x, gv.y, gv.w, gv.h);
    cairo_fill(cr);

    // Divider.
    cairo_set_source_rgba(cr, 0.0, 0.85, 1.0, 1.0);
    cairo_rectangle(cr, gx - 4, 0, 8, FRAME_HEIGHT);
    cairo_fill(cr);

    // Left label.
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.62);
    rounded_rect(cr, 18, 18, 250, 48, 12);
    cairo_fill(cr);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 27);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
    cairo_move_to(cr, 36, 51);
    cairo_show_text(cr, "LIVE VIDEO");

    // Game label.
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.62);
    rounded_rect(cr, gx + 18, 18, 250, 48, 12);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
    cairo_move_to(cr, gx + 36, 51);
    cairo_show_text(cr, "CAR GAME");
}

// =============================================================================
// Game scene rendering — road/lane-dashes, obstacles, player car sprite
// (with rotated body, racing stripe, glass, "Qualcomm" branding,
// headlights/taillights, wheels), header bar (score/steering/active-status),
// and crash overlay.
// =============================================================================
static void draw_game(cairo_t* cr, const std::optional<WristPair>& pair) {
    Viewport gv = game_viewport();
    double gx = gv.x, gy = gv.y;
    double w = std::max(1.0, gv.w);
    double h = std::max(1.0, gv.h);

    cairo_save(cr);
    cairo_rectangle(cr, gx, gy, w, h);
    cairo_clip(cr);
    cairo_translate(cr, gx, gy);

    double road_left  = 0.12 * w;
    double road_w      = 0.76 * w;
    double road_right = road_left + road_w;
    double lane_w      = road_w / 3.0;

    // Game background.
    cairo_set_source_rgba(cr, 0.02, 0.03, 0.05, 1.0);
    cairo_rectangle(cr, 0, 0, w, h);
    cairo_fill(cr);

    // Road.
    cairo_set_source_rgba(cr, 0.13, 0.13, 0.15, 1.0);
    cairo_rectangle(cr, road_left, 0, road_w, h);
    cairo_fill(cr);

    // Road edges.
    cairo_set_source_rgba(cr, 0.95, 0.95, 0.95, 1.0);
    cairo_rectangle(cr, road_left - 5, 0, 5, h);
    cairo_fill(cr);
    cairo_rectangle(cr, road_right, 0, 5, h);
    cairo_fill(cr);

    // Lane dashes.
    cairo_set_source_rgba(cr, 1.0, 0.93, 0.22, 1.0);
    double dash_h = 0.065 * h;
    double gap    = 0.055 * h;
    double period = dash_h + gap;
    double offset = g_game.road_offset;

    for (double lane_x : {road_left + lane_w, road_left + 2.0 * lane_w}) {
        double y = -period + offset;
        while (y < h + period) {
            rounded_rect(cr, lane_x - 4, y, 8, dash_h, 3);
            cairo_fill(cr);
            y += period;
        }
    }

    // Obstacles.
    for (const auto& obs : g_game.obstacles) {
        rounded_rect(cr, obs.x - obs.w / 2.0, obs.y, obs.w, obs.h, 10);
        cairo_set_source_rgba(cr, 0.85, 0.08, 0.05, 1.0);
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, 0.25, 0.02, 0.02, 1.0);
        cairo_set_line_width(cr, 3);
        cairo_stroke(cr);
    }

    // Player car.
    double car_w = 0.118 * w;
    double car_h = 0.135 * h;
    double cx = g_game.car_x * w;
    double cy = g_game.car_y * h;
    double steer = g_game.steer_smooth;
    double car_angle = steer * (20.0 * M_PI / 180.0);

    // Shadow.
    cairo_save(cr);
    cairo_translate(cr, cx + car_w * 0.08, cy + car_h * 0.08);
    cairo_rotate(cr, car_angle);
    rounded_rect(cr, -car_w * 0.50, -car_h * 0.48, car_w, car_h, 16);
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.36);
    cairo_fill(cr);
    cairo_restore(cr);

    // Car body.
    cairo_save(cr);
    cairo_translate(cr, cx, cy);
    cairo_rotate(cr, car_angle);

    rounded_rect(cr, -car_w / 2.0, -car_h / 2.0, car_w, car_h, 18);
    cairo_set_source_rgba(cr, 0.02, 0.23, 0.95, 1.0);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 0.00, 0.05, 0.22, 1.0);
    cairo_set_line_width(cr, 4);
    cairo_stroke(cr);

    // Racing stripe.
    rounded_rect(cr, -car_w * 0.10, -car_h * 0.47, car_w * 0.20, car_h * 0.94, 7);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.92);
    cairo_fill(cr);

    rounded_rect(cr, -car_w * 0.035, -car_h * 0.47, car_w * 0.07, car_h * 0.94, 4);
    cairo_set_source_rgba(cr, 0.03, 0.15, 0.75, 0.95);
    cairo_fill(cr);

    // Glass.
    rounded_rect(cr, -car_w * 0.32, -car_h * 0.28, car_w * 0.64, car_h * 0.22, 7);
    cairo_set_source_rgba(cr, 0.70, 0.90, 1.0, 0.96);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 0.03, 0.16, 0.32, 0.65);
    cairo_set_line_width(cr, 2);
    cairo_stroke(cr);

    rounded_rect(cr, -car_w * 0.27, car_h * 0.16, car_w * 0.54, car_h * 0.18, 6);
    cairo_set_source_rgba(cr, 0.42, 0.70, 0.94, 0.88);
    cairo_fill(cr);

    // Qualcomm branding: white text on the blue car body, drawn in the
    // rotated car coordinate system so it stays attached to the car.
    {
        const char* brand_text = "Qualcomm";
        cairo_save(cr);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        double brand_font_size = std::max(10.0, car_w * 0.15);
        cairo_set_font_size(cr, brand_font_size);

        cairo_text_extents_t brand_ext;
        cairo_text_extents(cr, brand_text, &brand_ext);
        double max_text_w = car_w * 0.78;
        if (brand_ext.width > max_text_w && brand_ext.width > 0) {
            brand_font_size *= max_text_w / brand_ext.width;
            brand_font_size = std::max(9.0, brand_font_size);
            cairo_set_font_size(cr, brand_font_size);
            cairo_text_extents(cr, brand_text, &brand_ext);
        }

        double brand_x = -brand_ext.width / 2.0 - brand_ext.x_bearing;
        double brand_y = car_h * 0.035 - brand_ext.height / 2.0 - brand_ext.y_bearing;

        // Thin dark outline keeps the white Qualcomm text readable on the car.
        cairo_move_to(cr, brand_x, brand_y);
        cairo_text_path(cr, brand_text);
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.70);
        cairo_set_line_width(cr, std::max(1.2, brand_font_size * 0.12));
        cairo_stroke_preserve(cr);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
        cairo_fill(cr);
        cairo_restore(cr);
    }

    // Lights.
    cairo_set_source_rgba(cr, 1.0, 0.96, 0.45, 1.0);
    rounded_rect(cr, -car_w * 0.39, -car_h * 0.48, car_w * 0.18, car_h * 0.055, 4);
    cairo_fill(cr);
    rounded_rect(cr, car_w * 0.21, -car_h * 0.48, car_w * 0.18, car_h * 0.055, 4);
    cairo_fill(cr);

    cairo_set_source_rgba(cr, 1.0, 0.05, 0.04, 1.0);
    rounded_rect(cr, -car_w * 0.38, car_h * 0.43, car_w * 0.16, car_h * 0.045, 3);
    cairo_fill(cr);
    rounded_rect(cr, car_w * 0.22, car_h * 0.43, car_w * 0.16, car_h * 0.045, 3);
    cairo_fill(cr);

    // Wheels.
    cairo_set_source_rgba(cr, 0.015, 0.015, 0.018, 1.0);
    double wheel_w = car_w * 0.18;
    double wheel_h = car_h * 0.24;
    for (double wx : {-car_w * 0.58, car_w * 0.40}) {
        for (double wy : {-car_h * 0.34, car_h * 0.18}) {
            rounded_rect(cr, wx, wy, wheel_w, wheel_h, 5);
            cairo_fill(cr);
        }
    }

    cairo_restore(cr);

    // Header bar inside game panel.
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.78);
    cairo_rectangle(cr, 0, 0, w, 0.095 * h);
    cairo_fill(cr);

    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 25);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
    cairo_move_to(cr, 24, 39);
    {
        std::ostringstream oss;
        oss << "Score: " << static_cast<int>(g_game.score);
        cairo_show_text(cr, oss.str().c_str());
    }

    cairo_move_to(cr, 185, 39);
    {
        std::ostringstream oss;
        oss.setf(std::ios::showpos);
        oss.precision(2);
        oss << std::fixed << "Steering: " << g_game.steer_smooth;
        cairo_show_text(cr, oss.str().c_str());
    }

    if (!pair) {
        cairo_set_source_rgba(cr, 1.0, 0.25, 0.20, 1.0);
        cairo_move_to(cr, 420, 39);
        cairo_show_text(cr, "Show both wrists");
    } else {
        cairo_set_source_rgba(cr, 0.2, 1.0, 0.2, 1.0);
        cairo_move_to(cr, 420, 39);
        cairo_show_text(cr, "Active");
    }

    if (!g_game.alive) {
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.65);
        cairo_rectangle(cr, 0, 0, w, h);
        cairo_fill(cr);
        draw_centered_text(cr, "CRASH!", w / 2.0, h * 0.44, 64, 1.0, 0.25, 0.20, 1.0, true);
        draw_centered_text(cr, "Auto restart...", w / 2.0, h * 0.53, 32, 1.0, 1.0, 1.0, 1.0, false);
    }

    cairo_restore(cr);
}

// =============================================================================
// Wrist line — draws the green wrist-to-wrist line + dots at the exact
// detected wrist positions, clipped to the left (camera) half.
// =============================================================================
static void draw_wrist_line(cairo_t* cr, const std::optional<WristPair>& pair) {
    if (!pair) return;

    Viewport lv = left_viewport();
    double px1 = pair->first.first,  py1 = pair->first.second;
    double px2 = pair->second.first, py2 = pair->second.second;

    cairo_save(cr);
    cairo_rectangle(cr, lv.x, lv.y, lv.w, lv.h);
    cairo_clip(cr);

    cairo_set_source_rgba(cr, 0.0, 1.0, 0.0, 1.0);
    cairo_set_line_width(cr, WRIST_LINE_WIDTH);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_move_to(cr, px1, py1);
    cairo_line_to(cr, px2, py2);
    cairo_stroke(cr);

    cairo_arc(cr, px1, py1, WRIST_DOT_RADIUS, 0, 2.0 * M_PI);
    cairo_fill(cr);
    cairo_arc(cr, px2, py2, WRIST_DOT_RADIUS, 0, 2.0 * M_PI);
    cairo_fill(cr);

    cairo_restore(cr);
}

// =============================================================================
// Steering wheel — drawn between the user's wrists when both are visible
// (clipped to the left/camera half), or at a fixed fallback position
// inside the game panel corner when no wrist pair is currently detected.
// =============================================================================
static void draw_steering_wheel(cairo_t* cr, const std::optional<WristPair>& pair) {
    Viewport gv = game_viewport();
    Viewport lv = left_viewport();
    double gx = gv.x, gy = gv.y, gw = std::max(1.0, gv.w), gh = std::max(1.0, gv.h);
    double lx = lv.x, ly = lv.y, lw = std::max(1.0, lv.w), lh = std::max(1.0, lv.h);

    double steer = clampd(g_game.steer_smooth, -1.0, 1.0);
    bool active = pair.has_value();
    double alpha = active ? 1.0 : 0.72;

    double cx, cy, radius;
    double clip_x, clip_y, clip_w, clip_h;
    bool show_label;

    if (active) {
        double x1 = pair->first.first,  y1 = pair->first.second;
        double x2 = pair->second.first, y2 = pair->second.second;
        // Exact visual center of both wrists, so the wheel sits between
        // your hands instead of staying fixed in the game corner.
        cx = (x1 + x2) / 2.0;
        cy = (y1 + y2) / 2.0;
        double hand_distance = p_dist(pair->first, pair->second);
        radius = clampd(hand_distance * 0.50, std::min(lw, lh) * 0.085, std::min(lw, lh) * 0.225);
        clip_x = lx; clip_y = ly; clip_w = lw; clip_h = lh;
        show_label = false;
    } else {
        // Fallback position when the wrists are not currently detected.
        std::string side = STEERING_WHEEL_SIDE;
        radius = std::min(gw, gh) * 0.145;
        double margin = std::min(gw, gh) * 0.040;
        if (side == "left") {
            cx = gx + margin + radius;
        } else {
            cx = gx + gw - margin - radius;
        }
        cy = gy + gh - margin - radius;
        cx = clampd(cx, gx + radius + 12, gx + gw - radius - 12);
        cy = clampd(cy, gy + radius + 12, gy + gh - radius - 12);
        clip_x = gx; clip_y = gy; clip_w = gw; clip_h = gh;
        show_label = true;
    }

    cairo_save(cr);
    cairo_rectangle(cr, clip_x, clip_y, clip_w, clip_h);
    cairo_clip(cr);

    // Large dark plate behind wheel.
    double plate_r = radius * 1.18;
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, active ? 0.58 : 0.72);
    cairo_arc(cr, cx, cy, plate_r, 0, 2.0 * M_PI);
    cairo_fill_preserve(cr);
    cairo_set_line_width(cr, std::max(4.0, radius * 0.030));
    cairo_set_source_rgba(cr, 1.0, 0.78, 0.05, alpha);
    cairo_stroke(cr);

    if (show_label) {
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, std::max(22.0, radius * 0.17));
        const char* label = "STEERING";
        cairo_text_extents_t ext;
        cairo_text_extents(cr, label, &ext);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
        cairo_move_to(cr, cx - ext.width / 2.0, cy - radius * 1.33);
        cairo_show_text(cr, label);
    }

    // Rotating wheel.
    cairo_translate(cr, cx, cy);
    cairo_rotate(cr, steer * (120.0 * M_PI / 180.0));
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);

    // Outer ring.
    cairo_set_line_width(cr, radius * 0.18);
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.95);
    cairo_arc(cr, 0, 0, radius, 0, 2.0 * M_PI);
    cairo_stroke(cr);
    cairo_set_line_width(cr, radius * 0.105);
    cairo_set_source_rgba(cr, 1.0, 0.72, 0.02, alpha);
    cairo_arc(cr, 0, 0, radius, 0, 2.0 * M_PI);
    cairo_stroke(cr);
    cairo_set_line_width(cr, radius * 0.035);
    cairo_set_source_rgba(cr, 0.0, 0.95, 1.0, alpha);
    cairo_arc(cr, 0, 0, radius * 0.84, 0, 2.0 * M_PI);
    cairo_stroke(cr);

    // Hub.
    cairo_set_source_rgba(cr, 0.02, 0.03, 0.04, 1.0);
    cairo_arc(cr, 0, 0, radius * 0.25, 0, 2.0 * M_PI);
    cairo_fill_preserve(cr);
    cairo_set_line_width(cr, radius * 0.035);
    cairo_set_source_rgba(cr, 1.0, 0.72, 0.02, alpha);
    cairo_stroke(cr);

    // Three spokes.
    cairo_set_line_width(cr, radius * 0.075);
    cairo_set_source_rgba(cr, 0.0, 0.92, 1.0, alpha);
    for (double deg : {-90.0, 30.0, 150.0}) {
        double angle = deg * M_PI / 180.0;
        cairo_move_to(cr, std::cos(angle) * radius * 0.27, std::sin(angle) * radius * 0.27);
        cairo_line_to(cr, std::cos(angle) * radius * 0.78, std::sin(angle) * radius * 0.78);
        cairo_stroke(cr);
    }

    // Red marker to show rotation clearly.
    cairo_set_source_rgba(cr, 1.0, 0.05, 0.02, 1.0);
    cairo_arc(cr, 0, -radius * 0.88, radius * 0.085, 0, 2.0 * M_PI);
    cairo_fill(cr);
    cairo_restore(cr);

    // Steering percent text. For active mode, keep it close but below the wheel.
    cairo_save(cr);
    cairo_rectangle(cr, clip_x, clip_y, clip_w, clip_h);
    cairo_clip(cr);
    int pct = static_cast<int>(std::lround(steer * 100.0));
    std::ostringstream oss;
    oss.setf(std::ios::showpos);
    oss << pct << "%";
    std::string text = oss.str();
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, std::max(20.0, radius * 0.16));
    cairo_text_extents_t ext;
    cairo_text_extents(cr, text.c_str(), &ext);
    if (active) {
        cairo_set_source_rgba(cr, 0.2, 1.0, 0.2, 1.0);
    } else {
        cairo_set_source_rgba(cr, 1.0, 0.25, 0.15, 1.0);
    }
    cairo_move_to(cr, cx - ext.width / 2.0, cy + radius * 1.32);
    cairo_show_text(cr, text.c_str());
    cairo_restore(cr);
}

// =============================================================================
// Cairo overlay draw callback.
//
// Draw order matters: split-frame background, then the game scene, then
// the steering wheel, then the green wrist line last so the exact wrist
// dots/line remain visible on top of the steering wheel overlay.
// =============================================================================
static void on_cairo_draw(void* draw_context, std::uint64_t /*timestamp*/,
                          std::uint64_t /*duration*/)
{
    if (!draw_context) return;
    cairo_t* cr = static_cast<cairo_t*>(draw_context);

    std::optional<WristPair> pair = current_wrist_pair();
    update_game(pair);

    draw_split_frame(cr);
    draw_game(cr, pair);
    draw_steering_wheel(cr, pair);
    draw_wrist_line(cr, pair);

}

static void on_cairo_draw_signal(void* /*overlay*/, void* draw_context,
                                 std::uint64_t timestamp, std::uint64_t duration,
                                 void* /*user_data*/)
{
    on_cairo_draw(draw_context, timestamp, duration);
}

// =============================================================================
// Metadata AppSink callback — receives per-frame hand-landmark JSON
// metadata text produced by qtimlmetaparser(module=json), parses out up to
// two wrists via extract_two_wrists(), and stores the result (with a
// timestamp) in the shared wrist state under mutex for
// current_wrist_pair() to consume.
// =============================================================================
static long g_meta_buffer_count = 0;

static void on_sample(qti::Buffer buffer)
{
    // Bind to a const reference so the read-only qti::Buffer::data() const
    // overload is used. These buffers are also downstream of a `tee`, so
    // the write-map overload returns null on shared buffers.
    const qti::Buffer& cbuffer = buffer;
    if (!cbuffer.valid() || !cbuffer.data()) return;

    std::string text(reinterpret_cast<const char*>(cbuffer.data()), cbuffer.size());
    ++g_meta_buffer_count;

    auto pair = extract_two_wrists(text);

    if (pair) {
        std::lock_guard<std::mutex> lock(g_wrist_state.mtx);
        g_wrist_state.pair = pair;
        g_wrist_state.timestamp = now_seconds();
    }

    if (g_meta_buffer_count % 60 == 1) {
        if (pair) {
            std::cout << "[WRIST] parsed wrists: (" << pair->first.first << "," << pair->first.second
                       << ") -> (" << pair->second.first << "," << pair->second.second << ")\n";
        } else {
            std::cout << "[WRIST] metadata received (#" << g_meta_buffer_count
                       << "), but two wrists were not parsed. text_len=" << text.size() << "\n";
        }
    }
}
// =============================================================================
// Pipeline construction
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
//    v4l2src -> qtivtransform -> tee
//      tee. -> stage-1 palm detection -> qtimetamux -> qtimetatransform
//      qtimetatransform -> tee -> stage-2 hand landmarks -> qtimetamux
//      qtimetamux -> tee
//        -> qtivoverlay -> qtivtransform -> [vf:BGRA] -> cairooverlay -> waylandsink
//        -> qtimlmetaparser(json) -> appsink(meta_sink)
//
//  Wrist metadata drives steering physics while Cairo renders the split
//  game scene and HUD on the display branch.
void create_and_execute_pipeline()
{
    // Camera + shared video pre-processing
    Element source("v4l2src", "source");
    source.set("device", input_config);

    // Video transform stage.
    Element transform("qtivtransform", "camera_transform");
    transform.set("flip-horizontal", true);
    auto videofilter = VideoFilter()
        .format("NV12")
        .resolution(FRAME_WIDTH, FRAME_HEIGHT)
        .framerate(FRAME_FPS);

    // Stream split (tee).
    Element split("tee", "split");

    // Stage 1: palm detection
    Element q_video_palm = make_queue("q_video_palm");

    Element q_palm_pre = make_queue("q_palm_pre");
    // ML preprocessor/converter.
    Element palm_preproc("qtimlvconverter", "palm_preproc");
    palm_preproc.set("mode", "image-batch-non-cumulative");

    Element q_palm_infer = make_queue("q_palm_infer");
    // TFLite inference stage.
    Element palm_inf("qtimltflite", "palm_inf");
    palm_inf.set("delegate", "gpu");
    palm_inf.set("model", model_base_path + "/models/palm_detection_full.tflite");

    Element q_palm_post = make_queue("q_palm_post");
    // ML postprocess stage.
    Element palm_post("qtimlpostprocess", "palm_post");
    palm_post.set("module", "palmd");
    palm_post.set("results", 2);
    palm_post.set("labels", model_base_path + "/labels/palmd_labels.json");
    palm_post.set("settings", model_base_path + "/labels/palmd_settings.json");

    auto palm_mlf = TextFilter();
    Element q_palm_meta = make_queue("q_palm_meta");

    // Combines the palm-detection video/metadata streams so the ROI
    // transform below can crop the per-hand region for stage 2.
    Element metamux_palm("qtimetamux", "metamux_palm");
    // Metadata transform stage.
    Element palm_roi_transform("qtimetatransform", "palm_roi_transform");
    palm_roi_transform.set("module", "roi-palmd");

    // Stream split (tee).
    Element split_after_palm("tee", "split_after_palm");

    // Stage 2: hand landmarks (21-point)
    Element q_video_final = make_queue("q_video_final");

    Element q_hand_pre = make_queue("q_hand_pre");
    // ML preprocessor/converter.
    Element hand_preproc("qtimlvconverter", "hand_preproc");
    hand_preproc.set("mode", "roi-batch-cumulative");

    Element q_hand_infer = make_queue("q_hand_infer");
    // TFLite inference stage.
    Element hand_inf("qtimltflite", "hand_inf");
    hand_inf.set("delegate", "xnnpack");
    hand_inf.set("model", model_base_path + "/models/hand_landmark_full.tflite");

    Element q_hand_post = make_queue("q_hand_post");
    // ML postprocess stage.
    Element hand_post("qtimlpostprocess", "hand_post");
    hand_post.set("module", "hlandmark");
    hand_post.set("results", 2);
    hand_post.set("labels", model_base_path + "/labels/hlandmarks.json");
    hand_post.set("settings", model_base_path + "/labels/hlandmark_settings.json");

    auto hand_mlf = TextFilter();
    Element q_hand_meta = make_queue("q_hand_meta");

    // Metadata/video muxer.
    Element metamux_final("qtimetamux", "metamux_final");
    // Stream split (tee).
    Element final_split("tee", "final_split");

    // Display branch: overlay -> cairooverlay -> display
    Element q_display = make_queue("q_display");
    // Metadata overlay renderer.
    Element overlay("qtivoverlay", "overlay");
    // Color format conversion stage.
    Element to_cairo("qtivtransform", "to_cairo");
    auto cairofilter = VideoFilter()
        .format("BGRA")
        .resolution(FRAME_WIDTH, FRAME_HEIGHT)
        .framerate(FRAME_FPS);

    // Cairo overlay draw stage.
    Element wrist_draw("cairooverlay", "wrist_draw");
    wrist_draw.connect_signal(
        "draw",
        reinterpret_cast<Element::SignalCallback>(&on_cairo_draw_signal));

    // Display sink.
    Element display("waylandsink", "display");
    display.set("sync", false);
    display.set("fullscreen", true);

    // Metadata branch: hand-landmark JSON -> wrist extraction
    Element q_meta_parse = make_queue("q_meta_parse");
    // Pipeline element.
    Element meta_parser("qtimlmetaparser", "meta_parser");
    meta_parser.set("module", "json");

    // AppSink for application callbacks.
    AppSink meta_sink("meta_sink");
    meta_sink.set("sync", false);
    meta_sink.set("max-buffers", 2);
    meta_sink.set("drop", true);

    // Assemble and link
    Pipeline pipeline("car_game");

    pipeline
        .add(source)
        .add(transform)
        .add_stream_filter("videostream", videofilter)
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
        .add(wrist_draw)
        .add(display)
        // Metadata branch
        .add(q_meta_parse)
        .add(meta_parser)
        .add(meta_sink)
        // Links
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
        .link("final_split", "q_meta_parse", "meta_parser", "meta_sink");

    // Wire up AppSink callback for metadata processing.
    meta_sink.set_buffer_consumer(on_sample);

    std::cout << "[INFO] Starting IMSDK split-screen wrist steering game...\n";
    std::cout << "[INFO] Left = live camera. Right = game. Green line = wrist-to-wrist.\n";
    std::cout << "[INFO] Camera:              " << input_config << "\n";
    std::cout << "[INFO] Palm model:          " << model_base_path + "/models/palm_detection_full.tflite" << "\n";
    std::cout << "[INFO] Hand landmark model: " << model_base_path + "/models/hand_landmark_full.tflite" << "\n";
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
            case 'i':
                input_config = optarg;
                break;
            case 'm':
                model_base_path = optarg;
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
