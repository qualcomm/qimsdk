/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <qti/qimsdk-pipeline.h>
#include <qti/qimsdk-element.h>
#include <qti/qimsdk-stream-filter.h>

#include <yaml-cpp/yaml.h>

#include <cctype>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

namespace qti {
namespace {

static bool is_integer(const std::string& s) {
  if (s.empty()) return false;

  size_t i = 0;
  if (s[0] == '-' || s[0] == '+') i = 1;

  if (i >= s.size()) return false;

  for (; i < s.size(); ++i)
    if (!std::isdigit(static_cast<unsigned char>(s[i])))
      return false;

  return true;
}

static bool is_float(const std::string& s) {
  bool dot = false;
  bool digit = false;
  size_t i = 0;

  if (s.empty())
    return false;

  if (s[0] == '-' || s[0] == '+') i = 1;

  for (; i < s.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (std::isdigit(c)) {
      digit = true;
      continue;
    }

    if (s[i] == '.' && !dot) {
      dot = true;
      continue;
    }

    return false;
  }
  return digit && dot;
}

static bool parse_fraction(const std::string& s, int& num, int& den) {
  auto pos = s.find('/');

  if (pos == std::string::npos)
    return false;

  const std::string a = s.substr(0, pos);
  const std::string b = s.substr(pos + 1);

  if (!is_integer(a) || !is_integer(b))
    return false;

  num = std::stoi(a);
  den = std::stoi(b);

  if (den == 0) den = 1;

  return true;
}

static bool parse_bool_scalar(const std::string& s, bool& out) {
  std::string v;
  v.reserve(s.size());
  for (const char ch : s) {
    v.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }

  if (v == "true" || v == "1" || v == "yes" || v == "on") {
    out = true;
    return true;
  }
  if (v == "false" || v == "0" || v == "no" || v == "off") {
    out = false;
    return true;
  }
  return false;
}

static std::string expand_environment_variables(const std::string& input) {
  std::string output;
  output.reserve(input.size());

  size_t i = 0;
  while (i < input.size()) {
    if (input[i] != '$') {
      output.push_back(input[i]);
      ++i;
      continue;
    }

    if (i + 1 >= input.size()) {
      output.push_back(input[i]);
      ++i;
      continue;
    }

    std::string token;
    std::string var_name;

    if (input[i + 1] == '{') {
      const size_t close = input.find('}', i + 2);
      if (close == std::string::npos) {
        output.push_back(input[i]);
        ++i;
        continue;
      }

      var_name = input.substr(i + 2, close - (i + 2));
      token = input.substr(i, close - i + 1);
      i = close + 1;
    } else {
      const unsigned char first = static_cast<unsigned char>(input[i + 1]);
      if (!(std::isalpha(first) || input[i + 1] == '_')) {
        output.push_back(input[i]);
        ++i;
        continue;
      }

      size_t j = i + 1;
      while (j < input.size()) {
        const unsigned char c = static_cast<unsigned char>(input[j]);
        if (!(std::isalnum(c) || input[j] == '_')) break;
        ++j;
      }

      var_name = input.substr(i + 1, j - (i + 1));
      token = input.substr(i, j - i);
      i = j;
    }

    if (var_name.empty()) {
      output += token;
      continue;
    }

    const char* value = std::getenv(var_name.c_str());
    if (value) {
      output += value;
    } else {
      output += token;
    }
  }

  return output;
}

static void set_property(Element& element, const std::string& key, const YAML::Node& node) {
  if (key == "type" || key == "name") return;

  if (node.IsScalar()) {
    const std::string value = expand_environment_variables(node.as<std::string>());
    element.set(key, value);
    return;
  }

  element.set(key, YAML::Dump(node));
}

template <typename Filter>
static void apply_add_list(Filter& filter, const YAML::Node& node) {
  if (node && node["add"] && node["add"].IsSequence()) {
    for (const auto& it : node["add"]) {
      filter.add(it.as<std::string>());
    }
  }
}

static void apply_add_list(StreamFilter& filter, const YAML::Node& node) {
  if (node && node["add"] && node["add"].IsSequence()) {
    std::string caps = filter.to_string();
    for (const auto& it : node["add"]) {
      const std::string expr = it.as<std::string>();
      if (caps.empty()) {
        caps = expr;
      } else {
        caps += "," + expr;
      }
    }
    if (!caps.empty()) {
      filter = StreamFilter(caps);
    }
  }
}

template <typename Filter>
static void set_framerate(Filter& filter, const YAML::Node& node) {
  if (!node) return;

  if (node.IsScalar()) {
    const std::string framerate = node.as<std::string>();
    int num = 0, den = 1;

    if (parse_fraction(framerate, num, den)) {
      filter.framerate(num, den);
      return;
    }

    if (is_integer(framerate)) {
      filter.framerate(std::stoi(framerate), 1);
      return;
    }

    if (is_float(framerate)) {
      filter.framerate(static_cast<float>(std::stod(framerate)));
      return;
    }

    try {
      filter.framerate(static_cast<float>(node.as<double>()));
      return;
    } catch (...) {}

  } else if (node.IsSequence() && node.size() == 2) {
    filter.framerate(node[0].as<int>(), node[1].as<int>());
  }
}

static StreamFilter build_stream_filter_from_filter(const YAML::Node& node) {
  if (node["video"]) {
    const YAML::Node value = node["video"];

    VideoFilter filter;
    if (value["format"])
      filter.format(value["format"].as<std::string>());

    if (value["width"] && value["height"])
      filter.resolution(value["width"].as<int>(), value["height"].as<int>());

    if (value["framerate"])
      set_framerate(filter, value["framerate"]);

    if (value["colorimetry"])
      filter.colorimetry(value["colorimetry"].as<std::string>());

    if (value["range"])
      filter.range(value["range"].as<std::string>());

    if (value["interlace"])
      filter.interlace(value["interlace"].as<std::string>());

    if (value["pixel_aspect_ratio"]) {
      const auto par = value["pixel_aspect_ratio"];

      if (par.IsSequence() && par.size() == 2) {
        filter.pixel_aspect_ratio(par[0].as<int>(), par[1].as<int>());
      } else if (par.IsScalar()) {
        int pn=0, pd=1;
        if (parse_fraction(par.as<std::string>(), pn, pd))
          filter.pixel_aspect_ratio(pn, pd);
      }
    }

    apply_add_list(filter, value);
    apply_add_list(filter, node);
    return filter;
  }

  if (node["image"]) {
    const YAML::Node value = node["image"];
    ImageFilter filter;

    if (value["format"])
      filter.format(value["format"].as<std::string>());

    if (value["width"] && value["height"])
      filter.resolution(value["width"].as<int>(), value["height"].as<int>());

    if (value["framerate"])
      set_framerate(filter, value["framerate"]);

    apply_add_list(filter, value);
    apply_add_list(filter, node);
    return filter;
  }

  if (node["h264"]) {
    const YAML::Node value = node["h264"];
    H264Filter filter;

    if (value["width"] && value["height"])
      filter.resolution(value["width"].as<int>(), value["height"].as<int>());

    if (value["framerate"])
      set_framerate(filter,  value["framerate"]);

    if (value["profile"])
      filter.profile(value["profile"].as<std::string>());

    if (value["level"])
      filter.level(value["level"].as<std::string>());

    if (value["stream_format"])
      filter.stream_format(value["stream_format"].as<std::string>());

    if (value["alignment"])
      filter.alignment(value["alignment"].as<std::string>());

    if (value["codec_data"])
      filter.codec_data(value["codec_data"].as<std::string>());

    if (value["set"] && value["set"].IsMap()) {
      for (auto it = value["set"].begin(); it != value["set"].end(); ++it) {
        filter.set(it->first.as<std::string>(), it->second.as<std::string>());
      }
    }

    apply_add_list(filter, value);
    apply_add_list(filter, node);
    return filter;
  }

  if (node["tensor"]) {
    const YAML::Node value = node["tensor"];
    TensorFilter filter;

    if (value["type"])
      filter.type(value["type"].as<std::string>());

    if (value["dimensions"]) {
      const YAML::Node d = value["dimensions"];
      if (!d.IsSequence() || d.size() == 0) {
        throw std::runtime_error("YAML: tensor.dimensions must be a non-empty sequence");
      }

      if (d[0].IsSequence()) {
        std::vector<std::vector<int>> many;
        for (const auto& one : d) {
          std::vector<int> dims;
          for (const auto& x : one) dims.push_back(x.as<int>());
          many.push_back(std::move(dims));
        }
        filter.dimensions(many);
      } else {
        std::vector<int> one;
        for (const auto& x : d) one.push_back(x.as<int>());
        filter.dimensions(one);
      }
    }

    apply_add_list(filter, value);
    apply_add_list(filter, node);
    return filter;
  }

  if (node["text"]) {
    const YAML::Node value = node["text"];
    TextFilter filter;

    apply_add_list(filter, value);
    apply_add_list(filter, node);
    return filter;
  }

  if (node["audio"]) {
    const YAML::Node value = node["audio"];
    AudioFilter filter;

    if (value["format"])
      filter.format(value["format"].as<std::string>());

    if (value["channels"])
      filter.channels(value["channels"].as<int>());

    if (value["rate"])
      filter.rate(value["rate"].as<int>());

    if (value["layout"])
      filter.layout(value["layout"].as<std::string>());

    apply_add_list(filter, value);
    apply_add_list(filter, node);
    return filter;
  }

  if (node["caps"]) {
    StreamFilter filter(node["caps"].as<std::string>());
    apply_add_list(filter, node);
    return filter;
  }

  throw std::runtime_error("YAML: filter requires one of: video/image/h264/tensor/text/audio/caps");
}

}  // namespace

Pipeline::Pipeline(const std::string& name, const std::string& config)
  : Pipeline(name) {

  YAML::Node root = YAML::Load(config);

  YAML::Node p = root["pipeline"];
  if (!p) throw std::runtime_error("YAML: missing root key 'pipeline'");

  if (p["eos"]) {
    bool eos_enabled = true;
    const YAML::Node eos_node = p["eos"];
    if (eos_node.IsScalar()) {
      try {
        eos_enabled = eos_node.as<bool>();
      } catch (...) {
        bool parsed = false;
        if (!parse_bool_scalar(eos_node.as<std::string>(), parsed)) {
          throw std::runtime_error("YAML: 'pipeline.eos' must be bool");
        }
        eos_enabled = parsed;
      }
    } else {
      throw std::runtime_error("YAML: 'pipeline.eos' must be bool");
    }
    eos(eos_enabled);
  }

  YAML::Node elements = p["elements"];
  if (!elements || !elements.IsSequence()) {
    throw std::runtime_error("YAML: 'pipeline.elements' must be a sequence");
  }

  for (const auto& n : elements) {
    if (!n["type"] || !n["name"]) {
      throw std::runtime_error("YAML: each element must have 'type' and 'name'");
    }

    const std::string type = n["type"].as<std::string>();  // factory name
    const std::string name = n["name"].as<std::string>();

    if (type == "filter") {
      StreamFilter sf = build_stream_filter_from_filter(n);
      add_stream_filter(name, sf);
    } else {
      add(type, name);
    }
  }

  for (const auto& n : elements) {
    const std::string type = n["type"].as<std::string>();
    const std::string name = n["name"].as<std::string>();

    if (type == "filter") continue;

    Element element = get(name);
    for (auto it = n.begin(); it != n.end(); ++it) {
      set_property(element, it->first.as<std::string>(), it->second);
    }
  }

  YAML::Node links = p["links"];
  if (links) {
    if (!links.IsSequence()) {
      throw std::runtime_error("YAML: 'pipeline.links' must be a sequence");
    }
    for (const auto& chain : links) {
      if (!chain.IsSequence() || chain.size() < 2) {
        throw std::runtime_error("YAML: each link chain must be a sequence with >=2 items");
      }

      std::vector<std::string> path;
      path.reserve(chain.size());

      for (const auto& item : chain)
        path.push_back(item.as<std::string>());
      link_by_names(path);
    }
  }
}

}  // namespace qti
