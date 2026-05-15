#include "tze/neural_signal_router.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace tze {
namespace {

std::string trim(std::string_view value) {
    std::size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start]))) {
        ++start;
    }
    std::size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    return std::string(value.substr(start, end - start));
}

std::string escape_json(std::string_view value) {
    std::string escaped;
    for (char c : value) {
        switch (c) {
            case '\\': escaped += "\\\\"; break;
            case '"': escaped += "\\\""; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default: escaped.push_back(c); break;
        }
    }
    return escaped;
}

std::string extract_json_string(std::string_view text, std::string_view key) {
    const std::string needle = "\"" + std::string(key) + "\"";
    const std::size_t start = text.find(needle);
    if (start == std::string::npos) {
        return {};
    }
    std::size_t cursor = text.find(':', start + needle.size());
    if (cursor == std::string::npos) {
        return {};
    }
    ++cursor;
    while (cursor < text.size() && std::isspace(static_cast<unsigned char>(text[cursor]))) {
        ++cursor;
    }
    if (cursor >= text.size() || text[cursor] != '"') {
        return {};
    }
    ++cursor;
    std::string value;
    bool escaping = false;
    while (cursor < text.size()) {
        const char c = text[cursor++];
        if (!escaping && c == '"') {
            break;
        }
        if (!escaping && c == '\\') {
            escaping = true;
            continue;
        }
        escaping = false;
        value.push_back(c);
    }
    return value;
}

std::size_t extract_json_size(std::string_view text, std::string_view key) {
    const std::string needle = "\"" + std::string(key) + "\"";
    const std::size_t start = text.find(needle);
    if (start == std::string::npos) {
        return 0;
    }
    std::size_t cursor = text.find(':', start + needle.size());
    if (cursor == std::string::npos) {
        return 0;
    }
    ++cursor;
    while (cursor < text.size() && std::isspace(static_cast<unsigned char>(text[cursor]))) {
        ++cursor;
    }
    std::size_t end = cursor;
    while (end < text.size() && std::isdigit(static_cast<unsigned char>(text[end]))) {
        ++end;
    }
    if (end == cursor) {
        return 0;
    }
    return static_cast<std::size_t>(std::stoull(std::string(text.substr(cursor, end - cursor))));
}

bool is_known_local_port(int port) {
    static const std::set<int> ports = {22, 53, 80, 443, 5000, 5432, 8000, 8080, 8443};
    return ports.find(port) != ports.end();
}

double clamp_probability(double value) {
    return std::max(0.0, std::min(1.0, value));
}

std::string format_double(double value) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(3) << value;
    return out.str();
}

MathAttribution attr(std::string name,
                     double raw_value,
                     double weight,
                     std::string source,
                     std::string rationale) {
    MathAttribution attribution;
    attribution.name = std::move(name);
    attribution.raw_value = raw_value;
    attribution.weight = weight;
    attribution.contribution = raw_value * weight;
    attribution.source = std::move(source);
    attribution.rationale = std::move(rationale);
    return attribution;
}

double sum_contribution(const std::vector<MathAttribution>& attributions) {
    double total = 0.0;
    for (const MathAttribution& attribution : attributions) {
        total += attribution.contribution;
    }
    return std::max(0.0, total);
}

NeuralRoutePrediction prediction(std::string label,
                                 std::string rationale,
                                 std::vector<MathAttribution> attributions) {
    NeuralRoutePrediction result;
    result.label = std::move(label);
    result.rationale = std::move(rationale);
    result.attributions = std::move(attributions);
    result.confidence = clamp_probability(sum_contribution(result.attributions));
    return result;
}

std::string render_prediction_summary(const NeuralRoutePrediction& prediction) {
    std::ostringstream out;
    out << prediction.label << " confidence=" << format_double(prediction.confidence);
    if (!prediction.attributions.empty()) {
        const auto top = std::max_element(
            prediction.attributions.begin(),
            prediction.attributions.end(),
            [](const MathAttribution& lhs, const MathAttribution& rhs) {
                return lhs.contribution < rhs.contribution;
            });
        out << " top_factor=" << top->name << "(" << format_double(top->contribution) << ")";
    }
    return out.str();
}

std::string render_feature_summary(const NeuralFeatureVector& features) {
    std::ostringstream out;
    out << "packets=" << features.packet_count
        << ", flows=" << features.flow_count
        << ", payload_packets=" << features.payload_packet_count
        << ", bytes=" << features.total_payload_bytes
        << ", http=" << features.http_plaintext_count
        << ", utf8=" << features.text_utf8_count
        << ", tls=" << features.tls_opaque_count
        << ", opaque=" << features.opaque_payload_count
        << ", parse_error=" << features.parse_error_count
        << ", unknown_ports=" << features.unknown_port_count;
    return out.str();
}

}  // namespace

NeuralRouteReport NeuralSignalRouter::route_tview_jsonl(std::string_view path) const {
    NeuralRouteReport report;
    report.input_path = std::string(path);
    report.features.input_path = report.input_path;

    std::ifstream input{std::string(path)};
    if (!input) {
        report.status = "neural_route_input_missing";
        report.summary = "Unable to open TView JSONL input `" + std::string(path) + "`.";
        report.warnings.push_back("Provide a JSONL file produced by `omnix tview ... --out file.jsonl`.");
        return report;
    }

    std::map<std::string, std::size_t> flows;
    for (std::string line; std::getline(input, line);) {
        if (line.find("omnix.tview.packet.v1") == std::string::npos) {
            continue;
        }
        ++report.features.packet_count;
        const std::string code = extract_json_string(line, "analysis_code");
        const std::string classification = extract_json_string(line, "classification");
        const std::string src = extract_json_string(line, "src_ip") + ":" + std::to_string(extract_json_size(line, "src_port"));
        const std::string dst = extract_json_string(line, "dst_ip") + ":" + std::to_string(extract_json_size(line, "dst_port"));
        flows[src + "->" + dst]++;
        const std::size_t payload = extract_json_size(line, "payload_length");
        report.features.total_payload_bytes += payload;
        if (payload > 0) {
            ++report.features.payload_packet_count;
        }
        const int src_port = static_cast<int>(extract_json_size(line, "src_port"));
        const int dst_port = static_cast<int>(extract_json_size(line, "dst_port"));
        if (!is_known_local_port(src_port) && !is_known_local_port(dst_port)) {
            ++report.features.unknown_port_count;
        }
        if (code == "NET.TCP.CONTROL" || classification == "tcp_control" ||
            code == "NET.GHOSTLINE.FRAME_DETECTED" ||
            code == "NET.GHOSTLINE.ORIGINAL_RELEASED") {
            ++report.features.control_count;
        } else if (code == "NET.TCP.HTTP_PLAINTEXT") {
            ++report.features.http_plaintext_count;
        } else if (code == "NET.TCP.TEXT_UTF8") {
            ++report.features.text_utf8_count;
        } else if (code == "NET.TCP.TLS_OPAQUE") {
            ++report.features.tls_opaque_count;
        } else if (code == "NET.TCP.OPAQUE_PAYLOAD" ||
                   code == "NET.GHOSTLINE.MODIFIED_RELEASED") {
            ++report.features.opaque_payload_count;
        } else if (code == "NET.TCP.PARSE_ERROR" ||
                   code == "NET.GHOSTLINE.FALLBACK_ORIGINAL" ||
                   code == "NET.GHOSTLINE.REVIEW_REQUIRED" ||
                   code == "NET.GHOSTLINE.PARSE_OR_VALIDATION_RISK") {
            ++report.features.parse_error_count;
        }
    }

    report.features.flow_count = flows.size();
    report.packet_count = report.features.packet_count;
    report.flow_count = report.features.flow_count;
    report.features.feature_summary.push_back(render_feature_summary(report.features));

    if (report.packet_count == 0) {
        report.status = "neural_route_empty";
        report.summary = "No `omnix.tview.packet.v1` events were found in `" + report.input_path + "`.";
        report.warnings.push_back("Router only consumes TView JSONL packet events in this phase.");
        return report;
    }

    const double packets = std::max<double>(1.0, static_cast<double>(report.features.packet_count));
    const double payload_ratio = static_cast<double>(report.features.payload_packet_count) / packets;
    const double control_ratio = static_cast<double>(report.features.control_count) / packets;
    const double control_only_ratio = report.features.payload_packet_count == 0 ? control_ratio : 0.0;
    const double payload_absent = report.features.payload_packet_count == 0 ? 1.0 : 0.0;
    const double unknown_ratio = static_cast<double>(report.features.unknown_port_count) / packets;
    const double unknown_payload_ratio = unknown_ratio * payload_ratio;
    const double byte_bucket = clamp_probability(std::log1p(static_cast<double>(report.features.total_payload_bytes)) / 8.0);

    report.predictions.push_back(prediction(
        "benign_control",
        "TCP control-only captures with no payload are usually handshake/teardown evidence.",
        {attr("control_only_ratio", control_only_ratio, 0.82, "tview.analysis_code", "Control-only captures increase benign-control confidence."),
         attr("payload_absent", payload_absent, 0.18, "tview.payload_length", "No observed payload keeps the capture in control-only territory.")}));
    report.predictions.push_back(prediction(
        "plaintext_http",
        "Plain HTTP or readable UTF-8 payload was visible in local packet evidence.",
        {attr("http_plaintext", static_cast<double>(report.features.http_plaintext_count) / packets, 0.78, "tview.analysis_code", "HTTP plaintext is directly decoded evidence."),
         attr("text_utf8", static_cast<double>(report.features.text_utf8_count) / packets, 0.22, "tview.payload_text_utf8", "Readable payload supports plaintext routing.")}));
    report.predictions.push_back(prediction(
        "tls_opaque",
        "TLS-like or encrypted payload is present and should stay opaque.",
        {attr("tls_opaque", static_cast<double>(report.features.tls_opaque_count) / packets, 0.74, "tview.analysis_code", "TLS Simplex code indicates encrypted payload."),
         attr("opaque_payload", static_cast<double>(report.features.opaque_payload_count) / packets, 0.18, "tview.analysis_code", "Opaque payload supports encrypted/unknown routing."),
         attr("byte_bucket", byte_bucket, 0.08, "tview.payload_length", "Payload bytes add weak support for opaque routing.")}));
    report.predictions.push_back(prediction(
        "unknown_service",
        "Payload on an unknown local port needs service identification before trust decisions.",
        {attr("unknown_port_ratio", unknown_ratio, 0.46, "tview.src_port|dst_port", "Uncatalogued ports raise unknown-service confidence."),
         attr("unknown_payload_ratio", unknown_payload_ratio, 0.34, "tview.payload_length", "Payload-bearing unknown traffic is more important than pure control traffic."),
         attr("opaque_payload_on_unknown_port",
              unknown_ratio > 0.0 ? static_cast<double>(report.features.opaque_payload_count) / packets : 0.0,
              0.20,
              "tview.analysis_code",
              "Opaque payload makes unknown service identity harder.")}));
    report.predictions.push_back(prediction(
        "suspicious_port",
        "Unknown ports with payload, parse errors, or opaque traffic should be reviewed before defensive action.",
        {attr("unknown_payload", unknown_payload_ratio, 0.44, "tview.port+payload", "Unknown payload-bearing ports are the strongest suspicious signal."),
         attr("parse_error", static_cast<double>(report.features.parse_error_count) / packets, 0.32, "tview.parser", "Parse errors increase suspicion."),
         attr("opaque_payload_on_unknown_port",
              unknown_ratio > 0.0 ? static_cast<double>(report.features.opaque_payload_count) / packets : 0.0,
              0.24,
              "tview.analysis_code",
              "Opaque payload adds uncertainty on unknown services.")}));
    report.predictions.push_back(prediction(
        "needs_human_review",
        "Human review is recommended when uncertainty, unknown ports, or parser errors dominate.",
        {attr("parse_error", static_cast<double>(report.features.parse_error_count) / packets, 0.42, "tview.parser", "Parser errors require manual review."),
         attr("unknown_port_ratio", unknown_ratio, 0.34, "tview.src_port|dst_port", "Unknown local ports require operator interpretation."),
         attr("opaque_or_tls", static_cast<double>(report.features.opaque_payload_count + report.features.tls_opaque_count) / packets, 0.24, "tview.analysis_code", "Opaque/encrypted traffic limits content-level certainty.")}));

    std::stable_sort(report.predictions.begin(), report.predictions.end(), [](const NeuralRoutePrediction& lhs,
                                                                              const NeuralRoutePrediction& rhs) {
        return lhs.confidence > rhs.confidence;
    });

    report.status = "neural_route_complete";
    report.summary = "Routed TView packet evidence: " + render_prediction_summary(report.predictions.front()) +
        " from " + render_feature_summary(report.features) + ".";
    return report;
}

bool NeuralSignalRouter::export_jsonl(const NeuralRouteReport& report,
                                      std::string_view path,
                                      std::string* error) const {
    if (path.empty()) {
        if (error != nullptr) {
            *error = "No neural route export path was provided.";
        }
        return false;
    }
    std::ofstream out{std::string(path)};
    if (!out) {
        if (error != nullptr) {
            *error = "Unable to open neural route export path `" + std::string(path) + "`.";
        }
        return false;
    }
    for (const NeuralRoutePrediction& prediction : report.predictions) {
        out << "{\"event_type\":\"omnix.nn.route.v1\""
            << ",\"input_path\":\"" << escape_json(report.input_path) << "\""
            << ",\"label\":\"" << escape_json(prediction.label) << "\""
            << ",\"confidence\":" << std::fixed << std::setprecision(4) << prediction.confidence
            << ",\"rationale\":\"" << escape_json(prediction.rationale) << "\""
            << ",\"packet_count\":" << report.packet_count
            << ",\"flow_count\":" << report.flow_count
            << ",\"feature_summary\":\"" << escape_json(render_feature_summary(report.features)) << "\""
            << "}\n";
    }
    return true;
}

}  // namespace tze
