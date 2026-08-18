/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include "qti/qimsdk-logging.h"

#include "qimsdk-logger-internal.h"

#include <array>
#include <atomic>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace qti {
namespace {

const std::pair<std::string, ::GstDebugLevel> kDefaultCategoryLogLevels[] = {
  { "GST_STATE", GST_LEVEL_LOG },
  { "GST_STATES", GST_LEVEL_LOG },
  { "qtibatch", GST_LEVEL_LOG },
  { "qticvimgpyramid", GST_LEVEL_LOG },
  { "qticvoptclflow", GST_LEVEL_LOG },
  { "qtidfs", GST_LEVEL_LOG },
  { "qtidngpacker", GST_LEVEL_LOG },
  { "qtidrmdecryptor", GST_LEVEL_LOG },
  { "qtimetamux", GST_LEVEL_LOG },
  { "qtimetatransform", GST_LEVEL_LOG },
  { "qtimlaconverter", GST_LEVEL_LOG },
  { "qtimldemux", GST_LEVEL_LOG },
  { "qtimlmetaextractor", GST_LEVEL_LOG },
  { "qtimlmetaparser", GST_LEVEL_LOG },
  { "qtimlpostprocess", GST_LEVEL_LOG },
  { "qtimlqnn", GST_LEVEL_LOG },
  { "qtimlsnpe", GST_LEVEL_LOG },
  { "qtimltflite", GST_LEVEL_LOG },
  { "qtimlvconverter", GST_LEVEL_LOG },
  { "qtiobjtracker", GST_LEVEL_LOG },
  { "qtirestrictedzonedbg", GST_LEVEL_LOG },
  { "qtivcomposer", GST_LEVEL_LOG },
  { "qtivoverlay", GST_LEVEL_LOG },
  { "qtivsplit", GST_LEVEL_LOG },
  { "qtivtransform", GST_LEVEL_LOG },
};

std::atomic<ImsdkLogLevel> g_qimsdk_log_level{ ImsdkLogLevel::Info };
std::atomic<ImsdkGstLogMode> g_qimsdk_gst_log_mode{ ImsdkGstLogMode::ImsdkLog };

const char* ToString(ImsdkLogLevel level) {
  switch (level) {
    case ImsdkLogLevel::Error:
      return "ERROR";
    case ImsdkLogLevel::Warning:
      return "WARN";
    case ImsdkLogLevel::Info:
      return "INFO";
    case ImsdkLogLevel::Debug:
      return "DEBUG";
  }
  return "INFO";
}

const char* ToColor(ImsdkLogLevel level) {
  switch (level) {
    case ImsdkLogLevel::Error:
      return "\x1b[31m";  // Red
    case ImsdkLogLevel::Warning:
      return "\x1b[33m";  // Yellow
    case ImsdkLogLevel::Info:
      return "\x1b[32m";  // Green
    case ImsdkLogLevel::Debug:
      return "\x1b[0m";
  }
  return "\x1b[0m";
}

bool IsColorEnabled() {
  const char* v = std::getenv("QIMSDK_LOG_COLOR");
  if (!v || v[0] == '\0') {
    return true;
  }

  const std::string value(v);
  return !(value == "0" || value == "false" || value == "FALSE" ||
           value == "off" || value == "OFF");
}

bool ParseAndPrintStateChangeLog(GObject* object, GstDebugMessage* message) {
  if (!object || !message) return false;

  const gchar* obj_name = GST_OBJECT_NAME(object);
  if (!obj_name || obj_name[0] == '\0') return false;

  std::string pipeline_name;
  if (GST_IS_PIPELINE(object)) {
    pipeline_name = obj_name;
  } else {
    GstObject* parent = GST_OBJECT_PARENT(object);
    while (parent && !GST_IS_PIPELINE(parent)) {
      parent = GST_OBJECT_PARENT(parent);
    }
    if (parent) {
      const gchar* parent_name = GST_OBJECT_NAME(parent);
      if (parent_name && parent_name[0] != '\0') {
        pipeline_name = parent_name;
      }
    }
  }

  if (pipeline_name.empty() ||
      std::string(obj_name).find(pipeline_name) == std::string::npos) {
    return false;
  }

  const gchar* msg = gst_debug_message_get(message);
  if (!msg) return false;

  static const std::regex kStateRegex(
      R"(completed state change to\s+([A-Za-z0-9_]+))");

  std::cmatch m;
  if (!std::regex_search(msg, m, kStateRegex)) return false;

  QIMSDK_LOG_INFO << "[STATE][" << obj_name << "] " << m[1].str();
  return true;
}

bool ParseAndPrintProcessingTimeLog(GstDebugCategory* /*category*/,
                                    GObject* object,
                                    GstDebugMessage* message) {
  if (!message) return false;

  const gchar* msg = gst_debug_message_get(message);
  if (!msg) return false;

  static const std::regex kPerfRegex(
      R"((?:<([^>]+)>\s+)?Performance time\s+([0-9]+(?:\.[0-9]+)?)\s+ms,\s+HW utilization:\s+([^,\]\r\n]+))");

  std::cmatch match;
  if (!std::regex_search(msg, match, kPerfRegex)) return false;

  std::string element_name;
  if (object) {
    const gchar* obj_name = GST_OBJECT_NAME(object);
    if (obj_name && obj_name[0] != '\0') {
      element_name = obj_name;
    }
  }
  if (element_name.empty() && match[1].matched) {
    element_name = match[1].str();
  }

  std::ostringstream line;
  if (!element_name.empty()) {
    line << "[" << element_name << "] ";
  }
  line << "Performance time " << match[2].str() << " ms, HW utilization: "
       << match[3].str();
  QIMSDK_LOG_DEBUG << line.str();
  return true;
}

std::string ElementNameOrUnknown(GstElement* element) {
  if (!element) {
    return "<null-element>";
  }

  const char* element_name = gst_element_get_name(element);
  return (element_name && element_name[0] != '\0') ?
      element_name : "<unknown-element>";
}

std::string PadNameOrUnknown(GstPad* pad) {
  if (!pad) {
    return "<null-pad>";
  }

  const char* pad_name = gst_pad_get_name(pad);
  return (pad_name && pad_name[0] != '\0') ?
      pad_name : "<unknown-pad>";
}

std::string DirectionToString(GstPadDirection direction) {
  return direction == GST_PAD_SRC ? "src" : "sink";
}

const char* PresenceToString(GstPadPresence presence) {
  switch (presence) {
    case GST_PAD_ALWAYS:
      return "always";
    case GST_PAD_SOMETIMES:
      return "sometimes";
    case GST_PAD_REQUEST:
      return "request";
    default:
      return "unknown";
  }
}

GstCaps* CurrentOrTemplateCapsCopy(GstPad* pad, bool* used_template_caps) {
  if (used_template_caps) {
    *used_template_caps = false;
  }

  if (!pad) {
    return nullptr;
  }

  GstCaps* current = gst_pad_get_current_caps(pad);
  if (current) {
    return current;
  }

  GstCaps* query = gst_pad_query_caps(pad, nullptr);
  if (query) {
    return query;
  }

  GstPadTemplate* templ = gst_pad_get_pad_template(pad);
  if (templ && templ->caps) {
    if (used_template_caps) {
      *used_template_caps = true;
    }
    return gst_caps_copy(templ->caps);
  }

  return nullptr;
}

std::string ReducedCapsToString(GstCaps* caps) {
  if (!caps || gst_caps_is_empty(caps)) {
    return "<none>";
  }

  static const std::array<const char*, 11> kRelevantFields = {
      "format", "stream-format", "alignment", "media", "encoding-name",
      "width", "height", "framerate", "rate", "channels", "profile"};

  std::ostringstream out;
  guint n = gst_caps_get_size(caps);
  guint printed = 0;
  for (guint i = 0; i < n; ++i) {
    const GstStructure* st = gst_caps_get_structure(caps, i);
    if (!st) {
      continue;
    }

    if (printed > 0) {
      out << " | ";
    }

    out << gst_structure_get_name(st);
    for (const char* field : kRelevantFields) {
      if (!gst_structure_has_field(st, field)) {
        continue;
      }
      const GValue* value = gst_structure_get_value(st, field);
      if (!value) {
        continue;
      }
      gchar* v = gst_value_serialize(value);
      if (v && v[0] != '\0') {
        out << ", " << field << "=" << v;
      }
      g_free(v);
    }
    printed += 1;
  }

  return printed ? out.str() : "<none>";
}

std::string CurrentOrTemplateCapsString(GstPad* pad, bool* used_template_caps) {
  GstCaps* caps = CurrentOrTemplateCapsCopy(pad, used_template_caps);
  if (!caps) {
    return "<unknown-caps>";
  }

  std::string text = ReducedCapsToString(caps);
  gst_caps_unref(caps);
  return text;
}

std::string CollectUnlinkedPadsCapsSummary(GstElement* pipeline) {
  if (!pipeline) {
    return {};
  }

  std::ostringstream all;
  bool any = false;

  GstIterator* element_iterator = gst_bin_iterate_elements(GST_BIN(pipeline));
  GValue element_value = G_VALUE_INIT;

  while (gst_iterator_next(element_iterator, &element_value)
         == GST_ITERATOR_OK) {
    GstElement* element = GST_ELEMENT(g_value_get_object(&element_value));

    GstIterator* pad_iterator = gst_element_iterate_pads(element);
    GValue pad_value = G_VALUE_INIT;
    while (gst_iterator_next(pad_iterator, &pad_value) == GST_ITERATOR_OK) {
      GstPad* pad = GST_PAD(g_value_get_object(&pad_value));
      if (!gst_pad_is_linked(pad)) {
        bool from_template = false;
        std::string caps = CurrentOrTemplateCapsString(pad, &from_template);

        GstPadTemplate* templ = gst_pad_get_pad_template(pad);
        GstPadPresence presence = templ ? templ->presence : GST_PAD_ALWAYS;

        any = true;
        all << ElementNameOrUnknown(element) << ":"
            << PadNameOrUnknown(pad)
            << " [" << DirectionToString(GST_PAD_DIRECTION(pad))
            << ", " << PresenceToString(presence)
            << "] caps={" << caps << "}"
            << (from_template ? " [template]" : "")
            << "; ";
      }
      g_value_unset(&pad_value);
    }
    gst_iterator_free(pad_iterator);

    g_value_unset(&element_value);
  }
  gst_iterator_free(element_iterator);

  if (!any) {
    return {};
  }

  return all.str();
}

void CollectPipelineGraphData(
    GstElement* pipeline,
    const std::vector<PendingLinkInfo>* pending_links,
    std::vector<std::pair<std::string, gint>>* nodes,
    std::vector<std::tuple<gint, gint, std::string>>* edges) {
  if (!nodes || !edges) {
    return;
  }
  nodes->clear();
  edges->clear();

  std::unordered_map<std::string, gint> element_id_by_name;
  std::set<std::pair<gint, gint>> added_edges;

  GstBin* pipeline_bin = GST_BIN(pipeline);
  GstIterator* element_iterator = gst_bin_iterate_elements(pipeline_bin);
  GValue element_value = G_VALUE_INIT;
  gint next_node_id = 100;

  while (gst_iterator_next(element_iterator, &element_value)
         == GST_ITERATOR_OK) {
    GstElement* element = GST_ELEMENT(g_value_get_object(&element_value));
    std::string element_name = gst_element_get_name(element);

    nodes->push_back({element_name, next_node_id});
    element_id_by_name[element_name] = next_node_id++;

    g_value_unset(&element_value);
  }
  gst_iterator_free(element_iterator);

  auto add_graph_edge =
      [&](gint from_id, gint to_id, std::string label) {
        if (added_edges.insert({from_id, to_id}).second)
          edges->push_back({from_id, to_id, std::move(label)});
      };

  for (const auto& node : *nodes) {
    GstElement* element =
        gst_bin_get_by_name(pipeline_bin, node.first.c_str());
    if (!element)
      continue;

    bool has_outgoing_source_pad = false;

    GstIterator* pad_iterator = gst_element_iterate_pads(element);
    GValue pad_value = G_VALUE_INIT;

    while (gst_iterator_next(pad_iterator, &pad_value) == GST_ITERATOR_OK) {
      GstPad* pad = GST_PAD(g_value_get_object(&pad_value));
      GstPadDirection pad_direction = GST_PAD_DIRECTION(pad);
      const char* pad_name = gst_pad_get_name(pad);

      if (GstPad* peer_pad = gst_pad_get_peer(pad)) {
        GstElement* peer_element = gst_pad_get_parent_element(peer_pad);
        std::string peer_element_name = gst_element_get_name(peer_element);
        const char* peer_pad_name = gst_pad_get_name(peer_pad);

        if (pad_direction == GST_PAD_SRC) {
          has_outgoing_source_pad = true;
          add_graph_edge(
              node.second,
              element_id_by_name[peer_element_name],
              node.first + ":" + pad_name + " → " +
                  peer_element_name + ":" + peer_pad_name);
        } else {
          add_graph_edge(
              element_id_by_name[peer_element_name],
              node.second,
              peer_element_name + ":" + peer_pad_name + " → " +
                  node.first + ":" + pad_name);
        }

        gst_object_unref(peer_pad);
        gst_object_unref(peer_element);
      }

      g_value_unset(&pad_value);
    }
    gst_iterator_free(pad_iterator);

    if (!has_outgoing_source_pad && pending_links) {
      for (const auto& pending_link : *pending_links) {
        if (pending_link.completed || pending_link.upstream_name != node.first)
          continue;

        auto down_it = element_id_by_name.find(pending_link.downstream_name);
        if (down_it == element_id_by_name.end())
          break;

        add_graph_edge(
            node.second,
            down_it->second,
            node.first + ":" +
                (pending_link.src_pad_template.empty() ?
                    "src" : pending_link.src_pad_template) +
                " → " +
                pending_link.downstream_name + ":" +
                (pending_link.sink_pad_template.empty() ?
                    "sink" : pending_link.sink_pad_template));
        break;
      }
    }

    gst_object_unref(element);
  }
}

}  // namespace

void LogUnlinkedPadsCaps(GstElement* pipeline) {
  if (!pipeline) {
    QIMSDK_LOG_ERROR << "Cannot inspect unlinked pads: pipeline is null";
    return;
  }

  if (!GST_IS_BIN(pipeline)) {
    QIMSDK_LOG_ERROR << "Cannot inspect unlinked pads: object is not a bin";
    return;
  }

  std::string all = CollectUnlinkedPadsCapsSummary(pipeline);
  if (all.empty()) {
    return;
  }

  QIMSDK_LOG_ERROR << "[PIPELINE][NOT_LINKED] " << all;
}

void PrintPipelineTopology(
    GstElement* pipeline,
    const std::vector<PendingLinkInfo>* pending_links) {
  if (!pipeline) {
    QIMSDK_LOG_ERROR << "[PIPELINE][TOPOLOGY] pipeline is null";
    return;
  }
  if (!GST_IS_BIN(pipeline)) {
    QIMSDK_LOG_ERROR << "[PIPELINE][TOPOLOGY] object is not a bin";
    return;
  }

  GstIterator* element_iterator = gst_bin_iterate_elements(GST_BIN(pipeline));
  GValue element_value = G_VALUE_INIT;

  while (gst_iterator_next(element_iterator, &element_value)
         == GST_ITERATOR_OK) {
    GstElement* element = GST_ELEMENT(g_value_get_object(&element_value));
    const char* element_name = gst_element_get_name(element);

    QIMSDK_LOG_DEBUG << "Element: " << element_name;

    bool source_connection_printed = false;

    GstIterator* pad_iterator = gst_element_iterate_pads(element);
    GValue pad_value = G_VALUE_INIT;

    while (gst_iterator_next(pad_iterator, &pad_value) == GST_ITERATOR_OK) {
      GstPad* pad = GST_PAD(g_value_get_object(&pad_value));
      GstPadDirection pad_direction = GST_PAD_DIRECTION(pad);

      const char* pad_name = gst_pad_get_name(pad);

      if (GstPad* peer_pad = gst_pad_get_peer(pad)) {
        GstElement* peer_element = gst_pad_get_parent_element(peer_pad);

        const char* peer_element_name = gst_element_get_name(peer_element);
        const char* peer_pad_name = gst_pad_get_name(peer_pad);

        QIMSDK_LOG_DEBUG << "  "
                        << element_name << ":" << pad_name
                        << (pad_direction == GST_PAD_SRC ? " --> " : " <-- ")
                        << peer_element_name << ":" << peer_pad_name;

        if (pad_direction == GST_PAD_SRC) {
          source_connection_printed = true;
        }

        gst_object_unref(peer_element);
        gst_object_unref(peer_pad);
      }

      g_value_unset(&pad_value);
    }
    gst_iterator_free(pad_iterator);

    if (!source_connection_printed && pending_links) {
      for (const auto& pending_link : *pending_links) {
        if (pending_link.completed || pending_link.upstream_name != element_name)
          continue;

        QIMSDK_LOG_DEBUG << "  "
                        << element_name << ":"
                        << (pending_link.src_pad_template.empty() ?
                              "src" : pending_link.src_pad_template)
                        << " --> "
                        << pending_link.downstream_name << ":"
                        << (pending_link.sink_pad_template.empty() ?
                              "sink" : pending_link.sink_pad_template);
        break;
      }
    }

    g_value_unset(&element_value);
  }
  gst_iterator_free(element_iterator);
}

void GeneratePipelineGraph(
    GstElement* pipeline,
    const std::string& filename,
    const std::vector<PendingLinkInfo>* pending_links) {
  if (!pipeline) {
    QIMSDK_LOG_ERROR << "[PIPELINE][GRAPH] pipeline is null";
    return;
  }
  if (!GST_IS_BIN(pipeline)) {
    QIMSDK_LOG_ERROR << "[PIPELINE][GRAPH] object is not a bin";
    return;
  }

  std::vector<std::pair<std::string, gint>> nodes;
  std::vector<std::tuple<gint, gint, std::string>> edges;
  CollectPipelineGraphData(pipeline, pending_links, &nodes, &edges);

  std::ofstream out(filename);
  if (!out)
    throw std::runtime_error("open failed");

  out << "<mxfile host=\"draw.io\">";
  out << "<diagram name=\"Pipeline Graph\">";
  out << "<mxGraphModel><root>";
  out << "<mxCell id=\"0\"/><mxCell id=\"1\" parent=\"0\"/>";

  int start_x = 50;
  int start_y = 50;
  int vertical_spacing = 80;

  for (size_t i = 0; i < nodes.size(); ++i) {
    out << "<mxCell id=\"" << nodes[i].second
        << "\" value=\"" << nodes[i].first
        << "\" vertex=\"1\" parent=\"1\" "
        << "style=\"rounded=1;whiteSpace=wrap;"
           "fillColor=#dae8fc;strokeColor=#6c8ebf;\">"
        << "<mxGeometry x=\"" << start_x
        << "\" y=\"" << start_y + i * vertical_spacing
        << "\" width=\"150\" height=\"40\" as=\"geometry\"/>"
        << "</mxCell>\n";
  }

  for (const auto& edge : edges) {
    out << "<mxCell edge=\"1\" parent=\"1\" source=\"" << std::get<0>(edge)
        << "\" target=\"" << std::get<1>(edge)
        << "\" value=\"" << std::get<2>(edge)
        << "\" style=\"endArrow=block;\">"
        << "<mxGeometry relative=\"1\" as=\"geometry\"/>"
        << "</mxCell>\n";
  }

  out << "</root></mxGraphModel></diagram></mxfile>";
  QIMSDK_LOG_INFO << "[PIPELINE][GRAPH] saved to " << filename;
}

void SetImsdkLogLevel(ImsdkLogLevel level) {
  g_qimsdk_log_level.store(level, std::memory_order_release);
}

void SetImsdkGstLogMode(ImsdkGstLogMode mode) {
  g_qimsdk_gst_log_mode.store(mode, std::memory_order_release);
}

ImsdkLogLevel GetImsdkLogLevel() {
  return g_qimsdk_log_level.load(std::memory_order_acquire);
}

ImsdkGstLogMode GetImsdkGstLogMode() {
  return g_qimsdk_gst_log_mode.load(std::memory_order_acquire);
}

bool ShouldLog(ImsdkLogLevel level) {
  return static_cast<int>(level) <=
         static_cast<int>(GetImsdkLogLevel());
}

void EmitImsdkLog(ImsdkLogLevel level, const std::string& message) {
  if (!ShouldLog(level)) {
    return;
  }
  if (IsColorEnabled()) {
    std::cout << "[QIMSDK][" << ToColor(level) << ToString(level) << "\x1b[0m]";
  } else {
    std::cout << "[QIMSDK][" << ToString(level) << "]";
  }
  if (!message.empty()) {
    std::cout << message;
  }
  std::cout << std::endl;
}

void ImsdkLogger::ConfigureGstParserCategoryLogLevels() {
  for (const auto& [category, level] : kDefaultCategoryLogLevels) {
    gst_debug_set_threshold_for_name(category.c_str(), level);
  }
}

bool ImsdkLogger::ParseAndPrintGstLog(::GstDebugLevel level,
                                              GstDebugCategory* category,
                                              GObject* object,
                                              GstDebugMessage* message) {
  if (ParseAndPrintStateChangeLog(object, message)) return true;
  if (ParseAndPrintProcessingTimeLog(category, object, message)) return true;

  const gchar* msg = message ? gst_debug_message_get(message) : nullptr;
  if (level != GST_LEVEL_ERROR) return false;

  std::ostringstream line;
  if (category) {
    const gchar* cat = gst_debug_category_get_name(category);
    if (cat && cat[0] != '\0') line << "[" << cat << "] ";
  }
  if (object) {
    const gchar* obj_name = GST_OBJECT_NAME(object);
    if (obj_name && obj_name[0] != '\0') line << obj_name << " ";
  }
  if (msg && msg[0] != '\0') line << msg;

  QIMSDK_LOG_ERROR << line.str();
  return true;
}


ImsdkLogMessage::~ImsdkLogMessage() {
  if (ShouldLog(level_)) {
    EmitImsdkLog(level_, stream_.str());
  }
}

std::ostream& ImsdkLogMessage::stream() {
  return stream_;
}

}  // namespace qti
