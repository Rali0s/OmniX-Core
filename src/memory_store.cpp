#include "tze/memory_store.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>

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

std::string now_timestamp() {
    using clock = std::chrono::system_clock;
    const std::time_t raw = clock::to_time_t(clock::now());
    std::tm local{};
#if defined(__APPLE__) || defined(__unix__)
    localtime_r(&raw, &local);
#else
    local = *std::localtime(&raw);
#endif
    std::ostringstream out;
    out << std::put_time(&local, "%Y-%m-%dT%H:%M:%S");
    return out.str();
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) {
        return {};
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

void write_text(const std::filesystem::path& path, const std::string& content) {
    std::ofstream output(path);
    output << content;
}

std::string escape_json(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size() + 8);
    for (char c : value) {
        switch (c) {
            case '\\':
                escaped += "\\\\";
                break;
            case '"':
                escaped += "\\\"";
                break;
            case '\n':
                escaped += "\\n";
                break;
            case '\r':
                escaped += "\\r";
                break;
            case '\t':
                escaped += "\\t";
                break;
            default:
                escaped.push_back(c);
                break;
        }
    }
    return escaped;
}

std::string unescape_json(std::string_view value) {
    std::string unescaped;
    unescaped.reserve(value.size());
    bool escaping = false;
    for (char c : value) {
        if (!escaping) {
            if (c == '\\') {
                escaping = true;
                continue;
            }
            unescaped.push_back(c);
            continue;
        }

        escaping = false;
        switch (c) {
            case 'n':
                unescaped.push_back('\n');
                break;
            case 'r':
                unescaped.push_back('\r');
                break;
            case 't':
                unescaped.push_back('\t');
                break;
            default:
                unescaped.push_back(c);
                break;
        }
    }
    return unescaped;
}

std::size_t find_json_value_start(std::string_view text, std::string_view key, char opener) {
    const std::string needle = "\"" + std::string(key) + "\"";
    const std::size_t key_start = text.find(needle);
    if (key_start == std::string::npos) {
        return std::string::npos;
    }

    std::size_t cursor = key_start + needle.size();
    while (cursor < text.size() && std::isspace(static_cast<unsigned char>(text[cursor]))) {
        ++cursor;
    }
    if (cursor >= text.size() || text[cursor] != ':') {
        return std::string::npos;
    }
    ++cursor;
    while (cursor < text.size() && std::isspace(static_cast<unsigned char>(text[cursor]))) {
        ++cursor;
    }
    if (cursor >= text.size() || text[cursor] != opener) {
        return std::string::npos;
    }
    return cursor;
}

std::string extract_json_string(std::string_view text, std::string_view key) {
    const std::size_t start = find_json_value_start(text, key, '"');
    if (start == std::string::npos) {
        return {};
    }
    std::size_t cursor = start + 1;
    std::string value;
    bool escaping = false;
    while (cursor < text.size()) {
        const char c = text[cursor++];
        if (!escaping && c == '"') {
            break;
        }
        if (!escaping && c == '\\') {
            escaping = true;
            value.push_back(c);
            continue;
        }
        escaping = false;
        value.push_back(c);
    }
    return unescape_json(value);
}

int extract_json_int(std::string_view text, std::string_view key, int default_value = 0) {
    const std::string needle = "\"" + std::string(key) + "\"";
    const std::size_t key_start = text.find(needle);
    if (key_start == std::string::npos) {
        return default_value;
    }
    std::size_t cursor = key_start + needle.size();
    while (cursor < text.size() && std::isspace(static_cast<unsigned char>(text[cursor]))) {
        ++cursor;
    }
    if (cursor >= text.size() || text[cursor] != ':') {
        return default_value;
    }
    ++cursor;
    while (cursor < text.size() && std::isspace(static_cast<unsigned char>(text[cursor]))) {
        ++cursor;
    }
    std::size_t end = cursor;
    while (end < text.size() && (std::isdigit(static_cast<unsigned char>(text[end])) || text[end] == '-')) {
        ++end;
    }
    if (end == cursor) {
        return default_value;
    }
    return std::stoi(std::string(text.substr(cursor, end - cursor)));
}

double extract_json_double(std::string_view text, std::string_view key, double default_value = 0.0) {
    const std::string needle = "\"" + std::string(key) + "\"";
    const std::size_t key_start = text.find(needle);
    if (key_start == std::string::npos) {
        return default_value;
    }
    std::size_t cursor = key_start + needle.size();
    while (cursor < text.size() && std::isspace(static_cast<unsigned char>(text[cursor]))) {
        ++cursor;
    }
    if (cursor >= text.size() || text[cursor] != ':') {
        return default_value;
    }
    ++cursor;
    while (cursor < text.size() && std::isspace(static_cast<unsigned char>(text[cursor]))) {
        ++cursor;
    }
    std::size_t end = cursor;
    while (end < text.size() &&
           (std::isdigit(static_cast<unsigned char>(text[end])) || text[end] == '-' || text[end] == '+' ||
            text[end] == '.' || text[end] == 'e' || text[end] == 'E')) {
        ++end;
    }
    if (end == cursor) {
        return default_value;
    }
    return std::stod(std::string(text.substr(cursor, end - cursor)));
}

bool extract_json_bool(std::string_view text, std::string_view key, bool default_value = false) {
    const std::string needle = "\"" + std::string(key) + "\"";
    const std::size_t key_start = text.find(needle);
    if (key_start == std::string::npos) {
        return default_value;
    }
    std::size_t cursor = key_start + needle.size();
    while (cursor < text.size() && std::isspace(static_cast<unsigned char>(text[cursor]))) {
        ++cursor;
    }
    if (cursor >= text.size() || text[cursor] != ':') {
        return default_value;
    }
    ++cursor;
    while (cursor < text.size() && std::isspace(static_cast<unsigned char>(text[cursor]))) {
        ++cursor;
    }
    if (text.substr(cursor, 4) == "true") {
        return true;
    }
    if (text.substr(cursor, 5) == "false") {
        return false;
    }
    return default_value;
}

std::vector<std::string> extract_json_string_array(std::string_view text, std::string_view key) {
    std::vector<std::string> values;
    const std::size_t start = find_json_value_start(text, key, '[');
    if (start == std::string::npos) {
        return values;
    }

    std::size_t cursor = start + 1;
    while (cursor < text.size() && text[cursor] != ']') {
        while (cursor < text.size() && (text[cursor] == ' ' || text[cursor] == ',' || text[cursor] == '\n')) {
            ++cursor;
        }
        if (cursor >= text.size() || text[cursor] != '"') {
            break;
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
                value.push_back(c);
                continue;
            }
            escaping = false;
            value.push_back(c);
        }
        values.push_back(unescape_json(value));
        while (cursor < text.size() && text[cursor] != ',' && text[cursor] != ']') {
            ++cursor;
        }
    }
    return values;
}

std::vector<std::string> extract_object_entries(std::string_view text, std::string_view key) {
    std::vector<std::string> entries;
    const std::size_t start = find_json_value_start(text, key, '[');
    if (start == std::string::npos) {
        return entries;
    }

    std::size_t cursor = start + 1;
    int depth = 0;
    std::size_t object_start = std::string::npos;
    bool in_string = false;
    bool escaping = false;
    while (cursor < text.size()) {
        const char c = text[cursor];
        if (in_string) {
            if (escaping) {
                escaping = false;
            } else if (c == '\\') {
                escaping = true;
            } else if (c == '"') {
                in_string = false;
            }
            ++cursor;
            continue;
        }

        if (c == '"') {
            in_string = true;
        } else if (c == '{') {
            if (depth == 0) {
                object_start = cursor;
            }
            ++depth;
        } else if (c == '}') {
            --depth;
            if (depth == 0 && object_start != std::string::npos) {
                entries.emplace_back(text.substr(object_start, cursor - object_start + 1));
                object_start = std::string::npos;
            }
        } else if (c == ']' && depth == 0) {
            break;
        }
        ++cursor;
    }
    return entries;
}

std::string render_string_array(const std::vector<std::string>& values);
std::string join_stage_fields(const std::vector<std::string>& values);

std::string extract_json_object(std::string_view text, std::string_view key) {
    const std::size_t start = find_json_value_start(text, key, '{');
    if (start == std::string::npos) {
        return {};
    }

    std::size_t cursor = start;
    int depth = 0;
    bool in_string = false;
    bool escaping = false;
    while (cursor < text.size()) {
        const char c = text[cursor];
        if (in_string) {
            if (escaping) {
                escaping = false;
            } else if (c == '\\') {
                escaping = true;
            } else if (c == '"') {
                in_string = false;
            }
            ++cursor;
            continue;
        }

        if (c == '"') {
            in_string = true;
        } else if (c == '{') {
            ++depth;
        } else if (c == '}') {
            --depth;
            if (depth == 0) {
                return std::string(text.substr(start, cursor - start + 1));
            }
        }
        ++cursor;
    }
    return {};
}

MemoryHistoryEntry parse_history_entry(std::string_view line) {
    MemoryHistoryEntry entry;
    entry.timestamp = extract_json_string(line, "timestamp");
    entry.prompt = extract_json_string(line, "prompt");
    entry.intent = extract_json_string(line, "intent");
    entry.project = extract_json_string(line, "project");
    entry.status = extract_json_string(line, "status");
    entry.summary = extract_json_string(line, "summary");
    return entry;
}

QueryCandidate parse_query_candidate_entry(std::string_view object_text) {
    QueryCandidate entry;
    entry.label = extract_json_string(object_text, "label");
    entry.detail = extract_json_string(object_text, "detail");
    entry.score = extract_json_int(object_text, "score", 0);
    entry.matched_context = extract_json_string_array(object_text, "matched_context");
    entry.reasons = extract_json_string_array(object_text, "reasons");
    return entry;
}

QueryOperation parse_query_operation_entry(std::string_view object_text) {
    QueryOperation entry;
    entry.operator_name = extract_json_string(object_text, "operator_name");
    entry.label = extract_json_string(object_text, "label");
    entry.inputs = extract_json_string_array(object_text, "inputs");
    for (const std::string& candidate_text : extract_object_entries(object_text, "candidates")) {
        entry.candidates.push_back(parse_query_candidate_entry(candidate_text));
    }
    entry.outputs = extract_json_string_array(object_text, "outputs");
    entry.trace = extract_json_string_array(object_text, "trace");
    return entry;
}

QuerySessionRecord parse_query_session_entry(std::string_view object_text) {
    QuerySessionRecord entry;
    entry.id = extract_json_string(object_text, "id");
    entry.command_label = extract_json_string(object_text, "command_label");
    entry.query_seed = extract_json_string(object_text, "query_seed");
    entry.active_context = extract_json_string_array(object_text, "active_context");
    entry.indexed_values = extract_json_string_array(object_text, "indexed_values");
    for (const std::string& operation_text : extract_object_entries(object_text, "operations")) {
        entry.operations.push_back(parse_query_operation_entry(operation_text));
    }
    entry.final_results = extract_json_string_array(object_text, "final_results");
    return entry;
}

ProviderProbeReport parse_provider_probe_entry(std::string_view object_text) {
    ProviderProbeReport entry;
    entry.provider_id = extract_json_string(object_text, "provider_id");
    entry.status = extract_json_string(object_text, "status");
    entry.summary = extract_json_string(object_text, "summary");
    entry.configured = extract_json_bool(object_text, "configured", false);
    entry.available = extract_json_bool(object_text, "available", false);
    entry.base_url = extract_json_string(object_text, "base_url");
    entry.model = extract_json_string(object_text, "model");
    entry.checks = extract_json_string_array(object_text, "checks");
    entry.warnings = extract_json_string_array(object_text, "warnings");
    return entry;
}

AssistAnnotation parse_assist_annotation_entry(std::string_view object_text) {
    AssistAnnotation entry;
    entry.task_id = extract_json_string(object_text, "task_id");
    entry.provider_id = extract_json_string(object_text, "provider_id");
    entry.model = extract_json_string(object_text, "model");
    entry.status = extract_json_string(object_text, "status");
    entry.summary = extract_json_string(object_text, "summary");
    entry.highlights = extract_json_string_array(object_text, "highlights");
    entry.operator_takeaway = extract_json_string(object_text, "operator_takeaway");
    entry.warnings = extract_json_string_array(object_text, "warnings");
    return entry;
}

CommandAssistPlan parse_command_assist_plan_entry(std::string_view object_text) {
    CommandAssistPlan entry;
    entry.task_id = extract_json_string(object_text, "task_id");
    entry.provider_id = extract_json_string(object_text, "provider_id");
    entry.model = extract_json_string(object_text, "model");
    entry.status = extract_json_string(object_text, "status");
    entry.canonical_command = extract_json_string(object_text, "canonical_command");
    entry.command_family = extract_json_string(object_text, "command_family");
    entry.rationale = extract_json_string(object_text, "rationale");
    entry.confidence = extract_json_double(object_text, "confidence", 0.0);
    entry.requires_confirmation = extract_json_bool(object_text, "requires_confirmation", false);
    entry.safety_notes = extract_json_string_array(object_text, "safety_notes");
    entry.validated = extract_json_bool(object_text, "validated", false);
    return entry;
}

ToolAssistPlan parse_tool_assist_plan_entry(std::string_view object_text) {
    ToolAssistPlan entry;
    entry.task_id = extract_json_string(object_text, "task_id");
    entry.provider_id = extract_json_string(object_text, "provider_id");
    entry.model = extract_json_string(object_text, "model");
    entry.status = extract_json_string(object_text, "status");
    entry.tool_name = extract_json_string(object_text, "tool_name");
    entry.arguments = extract_json_string_array(object_text, "arguments");
    entry.rationale = extract_json_string(object_text, "rationale");
    entry.safety_notes = extract_json_string_array(object_text, "safety_notes");
    entry.validated = extract_json_bool(object_text, "validated", false);
    return entry;
}

BuildAssistPlan parse_build_assist_plan_entry(std::string_view object_text) {
    BuildAssistPlan entry;
    entry.task_id = extract_json_string(object_text, "task_id");
    entry.provider_id = extract_json_string(object_text, "provider_id");
    entry.model = extract_json_string(object_text, "model");
    entry.status = extract_json_string(object_text, "status");
    entry.selected_recipe_id = extract_json_string(object_text, "selected_recipe_id");
    entry.fallback_recipe_id = extract_json_string(object_text, "fallback_recipe_id");
    entry.rationale = extract_json_string(object_text, "rationale");
    entry.confidence = extract_json_double(object_text, "confidence", 0.0);
    entry.safety_notes = extract_json_string_array(object_text, "safety_notes");
    entry.validated = extract_json_bool(object_text, "validated", false);
    return entry;
}

SecurityAudit parse_security_audit_entry(std::string_view object_text) {
    SecurityAudit entry;
    entry.id = extract_json_string(object_text, "id");
    entry.query = extract_json_string(object_text, "query");
    entry.status = extract_json_string(object_text, "status");
    entry.behavior_mode = extract_json_string(object_text, "behavior_mode");
    entry.threat_label = extract_json_string(object_text, "threat_label");
    entry.threat_bracket = extract_json_string(object_text, "threat_bracket");
    entry.admin_verified = extract_json_bool(object_text, "admin_verified", false);
    entry.phases = extract_json_string_array(object_text, "phases");
    entry.communications = extract_json_string_array(object_text, "communications");
    entry.mitigations = extract_json_string_array(object_text, "mitigations");
    entry.trace_paths = extract_json_string_array(object_text, "trace_paths");
    entry.simulated_actions = extract_json_string_array(object_text, "simulated_actions");
    entry.blocked_paths = extract_json_string_array(object_text, "blocked_paths");
    entry.evidence = extract_json_string_array(object_text, "evidence");
    entry.reasoning_trace = extract_json_string_array(object_text, "reasoning_trace");
    entry.persisted_at = extract_json_string(object_text, "persisted_at");
    return entry;
}

LanguageCandidate parse_language_candidate_entry(std::string_view object_text) {
    LanguageCandidate entry;
    entry.candidate_type = extract_json_string(object_text, "candidate_type");
    entry.label = extract_json_string(object_text, "label");
    entry.probability = extract_json_double(object_text, "probability", 0.0);
    entry.status = extract_json_string(object_text, "status");
    entry.evidence = extract_json_string_array(object_text, "evidence");
    return entry;
}

LanguageResolutionRecord parse_language_resolution_entry(std::string_view object_text) {
    LanguageResolutionRecord entry;
    entry.id = extract_json_string(object_text, "id");
    entry.query = extract_json_string(object_text, "query");
    entry.native_os = extract_json_string(object_text, "native_os");
    entry.observed_locale = extract_json_string(object_text, "observed_locale");
    entry.selected_os = extract_json_string(object_text, "selected_os");
    entry.selected_language = extract_json_string(object_text, "selected_language");
    entry.combined_context = extract_json_string(object_text, "combined_context");
    entry.passes = extract_json_int(object_text, "passes", 0);
    entry.confidence = extract_json_double(object_text, "confidence", 0.0);
    entry.manual_confirmation_required = extract_json_bool(object_text, "manual_confirmation_required", false);
    entry.manual_confirmation_used = extract_json_bool(object_text, "manual_confirmation_used", false);
    entry.manual_confirmation_prompt = extract_json_string(object_text, "manual_confirmation_prompt");
    entry.manual_confirmation_response = extract_json_string(object_text, "manual_confirmation_response");
    for (const std::string& candidate_text : extract_object_entries(object_text, "os_candidates")) {
        entry.os_candidates.push_back(parse_language_candidate_entry(candidate_text));
    }
    for (const std::string& candidate_text : extract_object_entries(object_text, "language_candidates")) {
        entry.language_candidates.push_back(parse_language_candidate_entry(candidate_text));
    }
    entry.reasoning_trace = extract_json_string_array(object_text, "reasoning_trace");
    entry.persisted_at = extract_json_string(object_text, "persisted_at");
    return entry;
}

UacTraitRecord parse_uac_trait_entry(std::string_view object_text) {
    UacTraitRecord entry;
    entry.trait_name = extract_json_string(object_text, "trait_name");
    entry.trait_value = extract_json_string(object_text, "trait_value");
    entry.source = extract_json_string(object_text, "source");
    entry.weight = extract_json_int(object_text, "weight", 0);
    entry.recovery_relevant = extract_json_bool(object_text, "recovery_relevant", false);
    return entry;
}

UacStateRecord parse_uac_state_entry(std::string_view object_text) {
    UacStateRecord entry;
    entry.id = extract_json_string(object_text, "id");
    entry.query = extract_json_string(object_text, "query");
    entry.epoch_marker = extract_json_string(object_text, "epoch_marker");
    entry.machine_identifier = extract_json_string(object_text, "machine_identifier");
    entry.chapter_reference = extract_json_string(object_text, "chapter_reference");
    entry.store_namespace = extract_json_string(object_text, "store_namespace");
    entry.search_namespace = extract_json_string(object_text, "search_namespace");
    entry.genx_token_value = extract_json_string(object_text, "genx_token_value");
    entry.compression_label = extract_json_string(object_text, "compression_label");
    entry.encoded_value = extract_json_string(object_text, "encoded_value");
    entry.encrypted_value = extract_json_string(object_text, "encrypted_value");
    entry.key_store_address_value = extract_json_string(object_text, "key_store_address_value");
    entry.key_budget_value = extract_json_string(object_text, "key_budget_value");
    entry.operational_usage_habit = extract_json_string(object_text, "operational_usage_habit");
    for (const std::string& trait_text : extract_object_entries(object_text, "indexed_traits")) {
        entry.indexed_traits.push_back(parse_uac_trait_entry(trait_text));
    }
    entry.recovery_hints = extract_json_string_array(object_text, "recovery_hints");
    entry.reasoning_trace = extract_json_string_array(object_text, "reasoning_trace");
    entry.persisted_at = extract_json_string(object_text, "persisted_at");
    return entry;
}

TzeStageRecord parse_tze_stage_entry(std::string_view object_text) {
    TzeStageRecord entry;
    entry.stage_id = extract_json_string(object_text, "stage_id");
    entry.stage_name = extract_json_string(object_text, "stage_name");
    entry.module = extract_json_string(object_text, "module");
    entry.status = extract_json_string(object_text, "status");
    entry.detail = extract_json_string(object_text, "detail");
    entry.inputs = extract_json_string_array(object_text, "inputs");
    entry.outputs = extract_json_string_array(object_text, "outputs");
    entry.graph_origin = extract_json_string(object_text, "graph_origin");
    entry.source_section = extract_json_string(object_text, "source_section");
    entry.source_line = static_cast<std::size_t>(extract_json_int(object_text, "source_line", 0));
    entry.source_excerpt = extract_json_string(object_text, "source_excerpt");
    return entry;
}

TzeRunRecord parse_tze_run_entry(std::string_view object_text) {
    TzeRunRecord entry;
    entry.id = extract_json_string(object_text, "id");
    entry.timestamp = extract_json_string(object_text, "timestamp");
    entry.intent = extract_json_string(object_text, "intent");
    entry.prompt = extract_json_string(object_text, "prompt");
    entry.target = extract_json_string(object_text, "target");
    entry.linked_case_id = extract_json_string(object_text, "linked_case_id");
    entry.status = extract_json_string(object_text, "status");
    entry.reasoning_provider = extract_json_string(object_text, "reasoning_provider");
    entry.provider_probe_status = extract_json_string(object_text, "provider_probe_status");
    entry.assist_status = extract_json_string(object_text, "assist_status");
    entry.next_action = extract_json_string(object_text, "next_action");
    entry.produced_artifact = extract_json_string(object_text, "produced_artifact");
    entry.feedback_status = extract_json_string(object_text, "feedback_status");
    entry.feedback_note = extract_json_string(object_text, "feedback_note");
    entry.feedback_timestamp = extract_json_string(object_text, "feedback_timestamp");
    const std::string provider_probe_text = extract_json_object(object_text, "provider_probe_report");
    if (!provider_probe_text.empty()) {
        entry.provider_probe_report = parse_provider_probe_entry(provider_probe_text);
    }
    const std::string assist_annotation_text = extract_json_object(object_text, "assist_annotation");
    if (!assist_annotation_text.empty()) {
        entry.assist_annotation = parse_assist_annotation_entry(assist_annotation_text);
    }
    const std::string command_assist_plan_text = extract_json_object(object_text, "command_assist_plan");
    if (!command_assist_plan_text.empty()) {
        entry.command_assist_plan = parse_command_assist_plan_entry(command_assist_plan_text);
    }
    const std::string tool_assist_plan_text = extract_json_object(object_text, "tool_assist_plan");
    if (!tool_assist_plan_text.empty()) {
        entry.tool_assist_plan = parse_tool_assist_plan_entry(tool_assist_plan_text);
    }
    const std::string build_assist_plan_text = extract_json_object(object_text, "build_assist_plan");
    if (!build_assist_plan_text.empty()) {
        entry.build_assist_plan = parse_build_assist_plan_entry(build_assist_plan_text);
    }
    const std::string security_audit_text = extract_json_object(object_text, "security_audit");
    if (!security_audit_text.empty()) {
        entry.security_audit = parse_security_audit_entry(security_audit_text);
    }
    const std::string language_resolution_text = extract_json_object(object_text, "language_resolution");
    if (!language_resolution_text.empty()) {
        entry.language_resolution = parse_language_resolution_entry(language_resolution_text);
    }
    const std::string uac_state_text = extract_json_object(object_text, "uac_state");
    if (!uac_state_text.empty()) {
        entry.uac_state = parse_uac_state_entry(uac_state_text);
    }
    const std::string query_session_text = extract_json_object(object_text, "query_session");
    if (!query_session_text.empty()) {
        entry.query_session = parse_query_session_entry(query_session_text);
    }
    for (const std::string& stage_text : extract_object_entries(object_text, "stages")) {
        entry.stages.push_back(parse_tze_stage_entry(stage_text));
    }
    return entry;
}

StoredDefinition parse_definition_entry(std::string_view object_text) {
    StoredDefinition entry;
    entry.term = extract_json_string(object_text, "term");
    entry.summary = extract_json_string(object_text, "summary");
    entry.mapped_cpp_target = extract_json_string(object_text, "mapped_cpp_target");
    entry.semantic_family = extract_json_string(object_text, "semantic_family");
    entry.source = extract_json_string(object_text, "source");
    return entry;
}

ProjectRecord parse_project_entry(std::string_view object_text) {
    ProjectRecord entry;
    entry.canonical_name = extract_json_string(object_text, "canonical_name");
    entry.resolved_source_path = extract_json_string(object_text, "resolved_source_path");
    entry.build_system = extract_json_string(object_text, "build_system");
    entry.status = extract_json_string(object_text, "status");
    entry.upstream_url = extract_json_string(object_text, "upstream_url");
    return entry;
}

LearnedRecipeRecord parse_learned_recipe_entry(std::string_view object_text) {
    LearnedRecipeRecord entry;
    entry.canonical_name = extract_json_string(object_text, "canonical_name");
    entry.recipe_id = extract_json_string(object_text, "recipe_id");
    entry.environment_key = extract_json_string(object_text, "environment_key");
    entry.build_system = extract_json_string(object_text, "build_system");
    entry.success_count = extract_json_int(object_text, "success_count", 0);
    entry.failure_count = extract_json_int(object_text, "failure_count", 0);
    entry.last_success_at = extract_json_string(object_text, "last_success_at");
    entry.last_failure_at = extract_json_string(object_text, "last_failure_at");
    entry.last_status = extract_json_string(object_text, "last_status");
    entry.last_artifact = extract_json_string(object_text, "last_artifact");
    entry.last_install_prefix = extract_json_string(object_text, "last_install_prefix");
    entry.confidence_score = extract_json_int(object_text, "confidence_score", 50);
    return entry;
}

NativeToolRecord parse_native_tool_entry(std::string_view object_text) {
    NativeToolRecord entry;
    entry.logical_name = extract_json_string(object_text, "logical_name");
    entry.provider_type = extract_json_string(object_text, "provider_type");
    entry.executable_path = extract_json_string(object_text, "executable_path");
    entry.applet_name = extract_json_string(object_text, "applet_name");
    entry.version_fingerprint = extract_json_string(object_text, "version_fingerprint");
    entry.capability_flags = extract_json_string_array(object_text, "capability_flags");
    entry.environment_signature = extract_json_string(object_text, "environment_signature");
    entry.size_bytes = static_cast<std::uintmax_t>(extract_json_int(object_text, "size_bytes", 0));
    entry.modified_timestamp = extract_json_int(object_text, "modified_timestamp", 0);
    entry.discovery_origin = extract_json_string(object_text, "discovery_origin");
    entry.last_verified = extract_json_string(object_text, "last_verified");
    return entry;
}

PermissionContext parse_permission_context(std::string_view object_text) {
    PermissionContext context;
    context.role = extract_json_string(object_text, "role");
    if (context.role.empty()) {
        context.role = "analyst";
    }
    context.can_view_raw = extract_json_bool(object_text, "can_view_raw", true);
    context.can_run_actions = extract_json_bool(object_text, "can_run_actions", true);
    context.can_store_feedback = extract_json_bool(object_text, "can_store_feedback", true);
    return context;
}

ObservationRecord parse_observation_entry(std::string_view object_text) {
    ObservationRecord entry;
    entry.id = extract_json_string(object_text, "id");
    entry.case_id = extract_json_string(object_text, "case_id");
    entry.source_kind = extract_json_string(object_text, "source_kind");
    entry.source_ref = extract_json_string(object_text, "source_ref");
    entry.collected_at = extract_json_string(object_text, "collected_at");
    entry.summary = extract_json_string(object_text, "summary");
    entry.raw_content = extract_json_string(object_text, "raw_content");
    entry.content_hash = extract_json_string(object_text, "content_hash");
    return entry;
}

NormalizedObject parse_normalized_object_entry(std::string_view object_text) {
    NormalizedObject entry;
    entry.id = extract_json_string(object_text, "id");
    entry.case_id = extract_json_string(object_text, "case_id");
    entry.observation_id = extract_json_string(object_text, "observation_id");
    entry.object_type = extract_json_string(object_text, "object_type");
    entry.title = extract_json_string(object_text, "title");
    entry.summary = extract_json_string(object_text, "summary");
    entry.attributes = extract_json_string_array(object_text, "attributes");
    return entry;
}

EvidenceLink parse_evidence_link_entry(std::string_view object_text) {
    EvidenceLink entry;
    entry.id = extract_json_string(object_text, "id");
    entry.case_id = extract_json_string(object_text, "case_id");
    entry.source_observation_id = extract_json_string(object_text, "source_observation_id");
    entry.target_object_id = extract_json_string(object_text, "target_object_id");
    entry.relation = extract_json_string(object_text, "relation");
    entry.rationale = extract_json_string(object_text, "rationale");
    return entry;
}

AnalystComment parse_analyst_comment_entry(std::string_view object_text) {
    AnalystComment entry;
    entry.id = extract_json_string(object_text, "id");
    entry.case_id = extract_json_string(object_text, "case_id");
    entry.author = extract_json_string(object_text, "author");
    entry.text = extract_json_string(object_text, "text");
    entry.created_at = extract_json_string(object_text, "created_at");
    return entry;
}

DecisionCandidate parse_decision_candidate_entry(std::string_view object_text) {
    DecisionCandidate entry;
    entry.id = extract_json_string(object_text, "id");
    entry.case_id = extract_json_string(object_text, "case_id");
    entry.title = extract_json_string(object_text, "title");
    entry.rationale = extract_json_string(object_text, "rationale");
    entry.recommended_command = extract_json_string(object_text, "recommended_command");
    entry.status = extract_json_string(object_text, "status");
    entry.score = extract_json_int(object_text, "score", 0);
    entry.valid = extract_json_bool(object_text, "valid", true);
    entry.validity_score = extract_json_int(object_text, "validity_score", 100);
    entry.evidence_coverage = extract_json_int(object_text, "evidence_coverage", 0);
    entry.prior_success_score = extract_json_int(object_text, "prior_success_score", 50);
    entry.confidence = extract_json_double(object_text, "confidence", 0.0);
    entry.probability_likelihood = extract_json_double(object_text, "probability_likelihood", 0.0);
    entry.supporting_signals = extract_json_string_array(object_text, "supporting_signals");
    entry.validation_checks = extract_json_string_array(object_text, "validation_checks");
    entry.score_trace = extract_json_string_array(object_text, "score_trace");
    entry.operator_feedback = extract_json_string(object_text, "operator_feedback");
    entry.feedback_note = extract_json_string(object_text, "feedback_note");
    entry.feedback_timestamp = extract_json_string(object_text, "feedback_timestamp");
    entry.outcome_status = extract_json_string(object_text, "outcome_status");
    entry.outcome_note = extract_json_string(object_text, "outcome_note");
    entry.outcome_timestamp = extract_json_string(object_text, "outcome_timestamp");
    return entry;
}

CaseLink parse_case_link_entry(std::string_view object_text) {
    CaseLink entry;
    entry.id = extract_json_string(object_text, "id");
    entry.left_case_id = extract_json_string(object_text, "left_case_id");
    entry.right_case_id = extract_json_string(object_text, "right_case_id");
    entry.link_type = extract_json_string(object_text, "link_type");
    entry.link_value = extract_json_string(object_text, "link_value");
    entry.rationale = extract_json_string(object_text, "rationale");
    entry.strength = extract_json_int(object_text, "strength", 0);
    return entry;
}

CaseRecord parse_case_record_entry(std::string_view object_text) {
    CaseRecord entry;
    entry.id = extract_json_string(object_text, "id");
    entry.title = extract_json_string(object_text, "title");
    entry.primary_source = extract_json_string(object_text, "primary_source");
    entry.status = extract_json_string(object_text, "status");
    entry.created_at = extract_json_string(object_text, "created_at");
    entry.updated_at = extract_json_string(object_text, "updated_at");
    entry.created_by_run_id = extract_json_string(object_text, "created_by_run_id");
    entry.analyzed_by_run_id = extract_json_string(object_text, "analyzed_by_run_id");
    entry.decided_by_run_id = extract_json_string(object_text, "decided_by_run_id");
    entry.reported_by_run_id = extract_json_string(object_text, "reported_by_run_id");
    entry.observation_ids = extract_json_string_array(object_text, "observation_ids");
    entry.object_ids = extract_json_string_array(object_text, "object_ids");
    entry.evidence_link_ids = extract_json_string_array(object_text, "evidence_link_ids");
    entry.comment_ids = extract_json_string_array(object_text, "comment_ids");
    entry.decision_ids = extract_json_string_array(object_text, "decision_ids");
    entry.latest_summary = extract_json_string(object_text, "latest_summary");
    const std::string permission_text = extract_json_object(object_text, "permission");
    if (!permission_text.empty()) {
        entry.permission = parse_permission_context(permission_text);
    }
    return entry;
}

std::string render_history_entry(const MemoryHistoryEntry& entry) {
    std::ostringstream out;
    out << "{\"timestamp\":\"" << escape_json(entry.timestamp)
        << "\",\"prompt\":\"" << escape_json(entry.prompt)
        << "\",\"intent\":\"" << escape_json(entry.intent)
        << "\",\"project\":\"" << escape_json(entry.project)
        << "\",\"status\":\"" << escape_json(entry.status)
        << "\",\"summary\":\"" << escape_json(entry.summary) << "\"}";
    return out.str();
}

std::string render_history_jsonl(const MemorySnapshot& snapshot) {
    std::ostringstream out;
    for (const MemoryHistoryEntry& entry : snapshot.history) {
        out << render_history_entry(entry) << '\n';
    }
    return out.str();
}

std::string render_query_candidate_json(const QueryCandidate& entry) {
    std::ostringstream out;
    out << "{\"label\":\"" << escape_json(entry.label)
        << "\",\"detail\":\"" << escape_json(entry.detail)
        << "\",\"score\":" << entry.score
        << ",\"matched_context\":" << render_string_array(entry.matched_context)
        << ",\"reasons\":" << render_string_array(entry.reasons) << "}";
    return out.str();
}

std::string render_query_operation_json(const QueryOperation& entry) {
    std::ostringstream out;
    out << "{\"operator_name\":\"" << escape_json(entry.operator_name)
        << "\",\"label\":\"" << escape_json(entry.label)
        << "\",\"inputs\":" << render_string_array(entry.inputs)
        << ",\"candidates\":[";
    for (std::size_t index = 0; index < entry.candidates.size(); ++index) {
        if (index != 0) {
            out << ",";
        }
        out << render_query_candidate_json(entry.candidates[index]);
    }
    out << "],\"outputs\":" << render_string_array(entry.outputs)
        << ",\"trace\":" << render_string_array(entry.trace) << "}";
    return out.str();
}

std::string render_query_session_json(const QuerySessionRecord& entry) {
    std::ostringstream out;
    out << "{\"id\":\"" << escape_json(entry.id)
        << "\",\"command_label\":\"" << escape_json(entry.command_label)
        << "\",\"query_seed\":\"" << escape_json(entry.query_seed)
        << "\",\"active_context\":" << render_string_array(entry.active_context)
        << ",\"indexed_values\":" << render_string_array(entry.indexed_values)
        << ",\"operations\":[";
    for (std::size_t index = 0; index < entry.operations.size(); ++index) {
        if (index != 0) {
            out << ",";
        }
        out << render_query_operation_json(entry.operations[index]);
    }
    out << "],\"final_results\":" << render_string_array(entry.final_results) << "}";
    return out.str();
}

std::string render_security_audit_json(const SecurityAudit& entry) {
    std::ostringstream out;
    out << "{\"id\":\"" << escape_json(entry.id)
        << "\",\"query\":\"" << escape_json(entry.query)
        << "\",\"status\":\"" << escape_json(entry.status)
        << "\",\"behavior_mode\":\"" << escape_json(entry.behavior_mode)
        << "\",\"threat_label\":\"" << escape_json(entry.threat_label)
        << "\",\"threat_bracket\":\"" << escape_json(entry.threat_bracket)
        << "\",\"admin_verified\":" << (entry.admin_verified ? "true" : "false")
        << ",\"phases\":" << render_string_array(entry.phases)
        << ",\"communications\":" << render_string_array(entry.communications)
        << ",\"mitigations\":" << render_string_array(entry.mitigations)
        << ",\"trace_paths\":" << render_string_array(entry.trace_paths)
        << ",\"simulated_actions\":" << render_string_array(entry.simulated_actions)
        << ",\"blocked_paths\":" << render_string_array(entry.blocked_paths)
        << ",\"evidence\":" << render_string_array(entry.evidence)
        << ",\"reasoning_trace\":" << render_string_array(entry.reasoning_trace)
        << ",\"persisted_at\":\"" << escape_json(entry.persisted_at) << "\"}";
    return out.str();
}

std::string render_language_candidate_json(const LanguageCandidate& entry) {
    std::ostringstream out;
    out << "{\"candidate_type\":\"" << escape_json(entry.candidate_type)
        << "\",\"label\":\"" << escape_json(entry.label)
        << "\",\"probability\":" << std::fixed << std::setprecision(4) << entry.probability
        << ",\"status\":\"" << escape_json(entry.status)
        << "\",\"evidence\":" << render_string_array(entry.evidence) << "}";
    return out.str();
}

std::string render_language_resolution_json(const LanguageResolutionRecord& entry) {
    std::ostringstream out;
    out << "{\"id\":\"" << escape_json(entry.id)
        << "\",\"query\":\"" << escape_json(entry.query)
        << "\",\"native_os\":\"" << escape_json(entry.native_os)
        << "\",\"observed_locale\":\"" << escape_json(entry.observed_locale)
        << "\",\"selected_os\":\"" << escape_json(entry.selected_os)
        << "\",\"selected_language\":\"" << escape_json(entry.selected_language)
        << "\",\"combined_context\":\"" << escape_json(entry.combined_context)
        << "\",\"passes\":" << entry.passes
        << ",\"confidence\":" << std::fixed << std::setprecision(4) << entry.confidence
        << ",\"manual_confirmation_required\":" << (entry.manual_confirmation_required ? "true" : "false")
        << ",\"manual_confirmation_used\":" << (entry.manual_confirmation_used ? "true" : "false")
        << ",\"manual_confirmation_prompt\":\"" << escape_json(entry.manual_confirmation_prompt)
        << "\",\"manual_confirmation_response\":\"" << escape_json(entry.manual_confirmation_response)
        << "\",\"os_candidates\":[";
    for (std::size_t index = 0; index < entry.os_candidates.size(); ++index) {
        if (index != 0) {
            out << ",";
        }
        out << render_language_candidate_json(entry.os_candidates[index]);
    }
    out << "],\"language_candidates\":[";
    for (std::size_t index = 0; index < entry.language_candidates.size(); ++index) {
        if (index != 0) {
            out << ",";
        }
        out << render_language_candidate_json(entry.language_candidates[index]);
    }
    out << "],\"reasoning_trace\":" << render_string_array(entry.reasoning_trace)
        << ",\"persisted_at\":\"" << escape_json(entry.persisted_at) << "\"}";
    return out.str();
}

std::string render_uac_trait_json(const UacTraitRecord& entry) {
    std::ostringstream out;
    out << "{\"trait_name\":\"" << escape_json(entry.trait_name)
        << "\",\"trait_value\":\"" << escape_json(entry.trait_value)
        << "\",\"source\":\"" << escape_json(entry.source)
        << "\",\"weight\":" << entry.weight
        << ",\"recovery_relevant\":" << (entry.recovery_relevant ? "true" : "false") << "}";
    return out.str();
}

std::string render_uac_state_json(const UacStateRecord& entry) {
    std::ostringstream out;
    out << "{\"id\":\"" << escape_json(entry.id)
        << "\",\"query\":\"" << escape_json(entry.query)
        << "\",\"epoch_marker\":\"" << escape_json(entry.epoch_marker)
        << "\",\"machine_identifier\":\"" << escape_json(entry.machine_identifier)
        << "\",\"chapter_reference\":\"" << escape_json(entry.chapter_reference)
        << "\",\"store_namespace\":\"" << escape_json(entry.store_namespace)
        << "\",\"search_namespace\":\"" << escape_json(entry.search_namespace)
        << "\",\"genx_token_value\":\"" << escape_json(entry.genx_token_value)
        << "\",\"compression_label\":\"" << escape_json(entry.compression_label)
        << "\",\"encoded_value\":\"" << escape_json(entry.encoded_value)
        << "\",\"encrypted_value\":\"" << escape_json(entry.encrypted_value)
        << "\",\"key_store_address_value\":\"" << escape_json(entry.key_store_address_value)
        << "\",\"key_budget_value\":\"" << escape_json(entry.key_budget_value)
        << "\",\"operational_usage_habit\":\"" << escape_json(entry.operational_usage_habit)
        << "\",\"indexed_traits\":[";
    for (std::size_t index = 0; index < entry.indexed_traits.size(); ++index) {
        if (index != 0) {
            out << ",";
        }
        out << render_uac_trait_json(entry.indexed_traits[index]);
    }
    out << "],\"recovery_hints\":" << render_string_array(entry.recovery_hints)
        << ",\"reasoning_trace\":" << render_string_array(entry.reasoning_trace)
        << ",\"persisted_at\":\"" << escape_json(entry.persisted_at) << "\"}";
    return out.str();
}

std::string render_provider_probe_json(const ProviderProbeReport& entry) {
    std::ostringstream out;
    out << "{\"provider_id\":\"" << escape_json(entry.provider_id)
        << "\",\"status\":\"" << escape_json(entry.status)
        << "\",\"summary\":\"" << escape_json(entry.summary)
        << "\",\"configured\":" << (entry.configured ? "true" : "false")
        << ",\"available\":" << (entry.available ? "true" : "false")
        << ",\"base_url\":\"" << escape_json(entry.base_url)
        << "\",\"model\":\"" << escape_json(entry.model)
        << "\",\"checks\":" << render_string_array(entry.checks)
        << ",\"warnings\":" << render_string_array(entry.warnings) << "}";
    return out.str();
}

std::string render_assist_annotation_json(const AssistAnnotation& entry) {
    std::ostringstream out;
    out << "{\"task_id\":\"" << escape_json(entry.task_id)
        << "\",\"provider_id\":\"" << escape_json(entry.provider_id)
        << "\",\"model\":\"" << escape_json(entry.model)
        << "\",\"status\":\"" << escape_json(entry.status)
        << "\",\"summary\":\"" << escape_json(entry.summary)
        << "\",\"highlights\":" << render_string_array(entry.highlights)
        << ",\"operator_takeaway\":\"" << escape_json(entry.operator_takeaway)
        << "\",\"warnings\":" << render_string_array(entry.warnings) << "}";
    return out.str();
}

std::string render_command_assist_plan_json(const CommandAssistPlan& entry) {
    std::ostringstream out;
    out << "{\"task_id\":\"" << escape_json(entry.task_id)
        << "\",\"provider_id\":\"" << escape_json(entry.provider_id)
        << "\",\"model\":\"" << escape_json(entry.model)
        << "\",\"status\":\"" << escape_json(entry.status)
        << "\",\"canonical_command\":\"" << escape_json(entry.canonical_command)
        << "\",\"command_family\":\"" << escape_json(entry.command_family)
        << "\",\"rationale\":\"" << escape_json(entry.rationale)
        << "\",\"confidence\":" << entry.confidence
        << ",\"requires_confirmation\":" << (entry.requires_confirmation ? "true" : "false")
        << ",\"safety_notes\":" << render_string_array(entry.safety_notes)
        << ",\"validated\":" << (entry.validated ? "true" : "false") << "}";
    return out.str();
}

std::string render_tool_assist_plan_json(const ToolAssistPlan& entry) {
    std::ostringstream out;
    out << "{\"task_id\":\"" << escape_json(entry.task_id)
        << "\",\"provider_id\":\"" << escape_json(entry.provider_id)
        << "\",\"model\":\"" << escape_json(entry.model)
        << "\",\"status\":\"" << escape_json(entry.status)
        << "\",\"tool_name\":\"" << escape_json(entry.tool_name)
        << "\",\"arguments\":" << render_string_array(entry.arguments)
        << ",\"rationale\":\"" << escape_json(entry.rationale)
        << "\",\"safety_notes\":" << render_string_array(entry.safety_notes)
        << ",\"validated\":" << (entry.validated ? "true" : "false") << "}";
    return out.str();
}

std::string render_build_assist_plan_json(const BuildAssistPlan& entry) {
    std::ostringstream out;
    out << "{\"task_id\":\"" << escape_json(entry.task_id)
        << "\",\"provider_id\":\"" << escape_json(entry.provider_id)
        << "\",\"model\":\"" << escape_json(entry.model)
        << "\",\"status\":\"" << escape_json(entry.status)
        << "\",\"selected_recipe_id\":\"" << escape_json(entry.selected_recipe_id)
        << "\",\"fallback_recipe_id\":\"" << escape_json(entry.fallback_recipe_id)
        << "\",\"rationale\":\"" << escape_json(entry.rationale)
        << "\",\"confidence\":" << entry.confidence
        << ",\"safety_notes\":" << render_string_array(entry.safety_notes)
        << ",\"validated\":" << (entry.validated ? "true" : "false") << "}";
    return out.str();
}

std::string render_language_contexts_json(const MemorySnapshot& snapshot) {
    std::ostringstream out;
    out << "{\n  \"entries\": [\n";
    for (std::size_t index = 0; index < snapshot.language_contexts.size(); ++index) {
        out << "    " << render_language_resolution_json(snapshot.language_contexts[index]);
        if (index + 1 < snapshot.language_contexts.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "  ]\n}\n";
    return out.str();
}

std::string render_uac_states_json(const MemorySnapshot& snapshot) {
    std::ostringstream out;
    out << "{\n  \"entries\": [\n";
    for (std::size_t index = 0; index < snapshot.uac_states.size(); ++index) {
        out << "    " << render_uac_state_json(snapshot.uac_states[index]);
        if (index + 1 < snapshot.uac_states.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "  ]\n}\n";
    return out.str();
}

std::string render_security_audits_json(const MemorySnapshot& snapshot) {
    std::ostringstream out;
    out << "{\n  \"entries\": [\n";
    for (std::size_t index = 0; index < snapshot.security_audits.size(); ++index) {
        out << "    " << render_security_audit_json(snapshot.security_audits[index]);
        if (index + 1 < snapshot.security_audits.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "  ]\n}\n";
    return out.str();
}

bool is_important_tze_intent(std::string_view intent) {
    return intent == "build_project" || intent == "decide_action" || intent == "inspect_case" ||
        intent == "analyze_case" || intent == "ingest_data" || intent == "tool_action";
}

std::vector<std::size_t> recent_tze_indexes(const MemorySnapshot& snapshot,
                                            std::size_t count,
                                            bool important_only) {
    std::vector<std::size_t> indexes;
    if (count == 0) {
        return indexes;
    }

    for (std::size_t reverse = snapshot.tze_runs.size(); reverse > 0; --reverse) {
        const std::size_t index = reverse - 1;
        if (important_only && !is_important_tze_intent(snapshot.tze_runs[index].intent)) {
            continue;
        }
        indexes.push_back(index);
        if (indexes.size() >= count) {
            break;
        }
    }
    return indexes;
}

std::filesystem::path tze_report_path(const MemorySnapshot& snapshot,
                                      std::string_view stem,
                                      const std::filesystem::path& explicit_output) {
    if (!explicit_output.empty()) {
        return explicit_output;
    }
    return snapshot.paths.root / "tze-reports" / (std::string(stem) + ".txt");
}

std::string render_tze_stage_json(const TzeStageRecord& entry) {
    std::ostringstream out;
    out << "{\"stage_id\":\"" << escape_json(entry.stage_id)
        << "\",\"stage_name\":\"" << escape_json(entry.stage_name)
        << "\",\"module\":\"" << escape_json(entry.module)
        << "\",\"status\":\"" << escape_json(entry.status)
        << "\",\"detail\":\"" << escape_json(entry.detail)
        << "\",\"inputs\":" << render_string_array(entry.inputs)
        << ",\"outputs\":" << render_string_array(entry.outputs)
        << ",\"graph_origin\":\"" << escape_json(entry.graph_origin)
        << "\",\"source_section\":\"" << escape_json(entry.source_section)
        << "\",\"source_line\":" << entry.source_line
        << ",\"source_excerpt\":\"" << escape_json(entry.source_excerpt) << "\"}";
    return out.str();
}

std::string render_tze_runs_json(const MemorySnapshot& snapshot) {
    std::ostringstream out;
    out << "{\n  \"entries\": [\n";
    for (std::size_t index = 0; index < snapshot.tze_runs.size(); ++index) {
        const TzeRunRecord& entry = snapshot.tze_runs[index];
        out << "    {\"id\":\"" << escape_json(entry.id)
            << "\",\"timestamp\":\"" << escape_json(entry.timestamp)
            << "\",\"intent\":\"" << escape_json(entry.intent)
            << "\",\"prompt\":\"" << escape_json(entry.prompt)
            << "\",\"target\":\"" << escape_json(entry.target)
            << "\",\"linked_case_id\":\"" << escape_json(entry.linked_case_id)
            << "\",\"status\":\"" << escape_json(entry.status)
            << "\",\"reasoning_provider\":\"" << escape_json(entry.reasoning_provider)
            << "\",\"provider_probe_status\":\"" << escape_json(entry.provider_probe_status)
            << "\",\"assist_status\":\"" << escape_json(entry.assist_status)
            << "\",\"next_action\":\"" << escape_json(entry.next_action)
            << "\",\"produced_artifact\":\"" << escape_json(entry.produced_artifact)
            << "\",\"feedback_status\":\"" << escape_json(entry.feedback_status)
            << "\",\"feedback_note\":\"" << escape_json(entry.feedback_note)
            << "\",\"feedback_timestamp\":\"" << escape_json(entry.feedback_timestamp) << "\""
            << ",\"provider_probe_report\":"
            << (entry.provider_probe_report.has_value() ? render_provider_probe_json(*entry.provider_probe_report) : "null")
            << ",\"assist_annotation\":"
            << (entry.assist_annotation.has_value() ? render_assist_annotation_json(*entry.assist_annotation) : "null")
            << ",\"command_assist_plan\":"
            << (entry.command_assist_plan.has_value() ? render_command_assist_plan_json(*entry.command_assist_plan) : "null")
            << ",\"tool_assist_plan\":"
            << (entry.tool_assist_plan.has_value() ? render_tool_assist_plan_json(*entry.tool_assist_plan) : "null")
            << ",\"build_assist_plan\":"
            << (entry.build_assist_plan.has_value() ? render_build_assist_plan_json(*entry.build_assist_plan) : "null")
            << ",\"security_audit\":"
            << (entry.security_audit.has_value() ? render_security_audit_json(*entry.security_audit) : "null")
            << ",\"language_resolution\":"
            << (entry.language_resolution.has_value() ? render_language_resolution_json(*entry.language_resolution) : "null")
            << ",\"uac_state\":"
            << (entry.uac_state.has_value() ? render_uac_state_json(*entry.uac_state) : "null")
            << ",\"query_session\":"
            << (entry.query_session.has_value() ? render_query_session_json(*entry.query_session) : "null")
            << ",\"stages\":[";
        for (std::size_t stage_index = 0; stage_index < entry.stages.size(); ++stage_index) {
            if (stage_index != 0) {
                out << ",";
            }
            out << render_tze_stage_json(entry.stages[stage_index]);
        }
        out << "]}";
        if (index + 1 < snapshot.tze_runs.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "  ]\n}\n";
    return out.str();
}

std::string render_definitions_json(const std::vector<StoredDefinition>& entries) {
    std::ostringstream out;
    out << "{\n  \"entries\": [\n";
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const StoredDefinition& entry = entries[index];
        out << "    {\"term\":\"" << escape_json(entry.term)
            << "\",\"summary\":\"" << escape_json(entry.summary)
            << "\",\"mapped_cpp_target\":\"" << escape_json(entry.mapped_cpp_target)
            << "\",\"semantic_family\":\"" << escape_json(entry.semantic_family)
            << "\",\"source\":\"" << escape_json(entry.source) << "\"}";
        if (index + 1 < entries.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "  ]\n}\n";
    return out.str();
}

std::string render_preferences_json(const MemorySnapshot& snapshot) {
    std::ostringstream out;
    out << "{\n  \"source_preference_order\": [";
    for (std::size_t index = 0; index < snapshot.source_preference_order.size(); ++index) {
        if (index != 0) {
            out << ", ";
        }
        out << "\"" << escape_json(snapshot.source_preference_order[index]) << "\"";
    }
    out << "]\n}\n";
    return out.str();
}

std::string render_projects_json(const MemorySnapshot& snapshot) {
    std::ostringstream out;
    out << "{\n  \"entries\": [\n";
    for (std::size_t index = 0; index < snapshot.projects.size(); ++index) {
        const ProjectRecord& entry = snapshot.projects[index];
        out << "    {\"canonical_name\":\"" << escape_json(entry.canonical_name)
            << "\",\"resolved_source_path\":\"" << escape_json(entry.resolved_source_path)
            << "\",\"build_system\":\"" << escape_json(entry.build_system)
            << "\",\"status\":\"" << escape_json(entry.status)
            << "\",\"upstream_url\":\"" << escape_json(entry.upstream_url) << "\"}";
        if (index + 1 < snapshot.projects.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "  ],\n  \"learned_recipes\": [\n";
    for (std::size_t index = 0; index < snapshot.learned_recipes.size(); ++index) {
        const LearnedRecipeRecord& entry = snapshot.learned_recipes[index];
        out << "    {\"canonical_name\":\"" << escape_json(entry.canonical_name)
            << "\",\"recipe_id\":\"" << escape_json(entry.recipe_id)
            << "\",\"environment_key\":\"" << escape_json(entry.environment_key)
            << "\",\"build_system\":\"" << escape_json(entry.build_system)
            << "\",\"success_count\":" << entry.success_count
            << ",\"failure_count\":" << entry.failure_count
            << ",\"last_success_at\":\"" << escape_json(entry.last_success_at)
            << "\",\"last_failure_at\":\"" << escape_json(entry.last_failure_at)
            << "\",\"last_status\":\"" << escape_json(entry.last_status)
            << "\",\"last_artifact\":\"" << escape_json(entry.last_artifact)
            << "\",\"last_install_prefix\":\"" << escape_json(entry.last_install_prefix)
            << "\",\"confidence_score\":" << entry.confidence_score << "}";
        if (index + 1 < snapshot.learned_recipes.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "  ]\n}\n";
    return out.str();
}

std::string render_native_tools_json(const MemorySnapshot& snapshot) {
    std::ostringstream out;
    out << "{\n  \"entries\": [\n";
    for (std::size_t index = 0; index < snapshot.native_tools.size(); ++index) {
        const NativeToolRecord& entry = snapshot.native_tools[index];
        out << "    {\"logical_name\":\"" << escape_json(entry.logical_name)
            << "\",\"provider_type\":\"" << escape_json(entry.provider_type)
            << "\",\"executable_path\":\"" << escape_json(entry.executable_path)
            << "\",\"applet_name\":\"" << escape_json(entry.applet_name)
            << "\",\"version_fingerprint\":\"" << escape_json(entry.version_fingerprint)
            << "\",\"capability_flags\":[";
        for (std::size_t capability_index = 0; capability_index < entry.capability_flags.size(); ++capability_index) {
            if (capability_index != 0) {
                out << ",";
            }
            out << "\"" << escape_json(entry.capability_flags[capability_index]) << "\"";
        }
        out << "],\"environment_signature\":\"" << escape_json(entry.environment_signature)
            << "\",\"size_bytes\":" << entry.size_bytes
            << ",\"modified_timestamp\":" << entry.modified_timestamp
            << ",\"discovery_origin\":\"" << escape_json(entry.discovery_origin)
            << "\",\"last_verified\":\"" << escape_json(entry.last_verified) << "\"}";
        if (index + 1 < snapshot.native_tools.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "  ]\n}\n";
    return out.str();
}

std::string render_string_array(const std::vector<std::string>& values) {
    std::ostringstream out;
    out << "[";
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) {
            out << ",";
        }
        out << "\"" << escape_json(values[index]) << "\"";
    }
    out << "]";
    return out.str();
}

std::string render_permission_context_json(const PermissionContext& context) {
    std::ostringstream out;
    out << "{\"role\":\"" << escape_json(context.role)
        << "\",\"can_view_raw\":" << (context.can_view_raw ? "true" : "false")
        << ",\"can_run_actions\":" << (context.can_run_actions ? "true" : "false")
        << ",\"can_store_feedback\":" << (context.can_store_feedback ? "true" : "false") << "}";
    return out.str();
}

std::string render_cases_json(const MemorySnapshot& snapshot) {
    std::ostringstream out;
    out << "{\n  \"observations\": [\n";
    for (std::size_t index = 0; index < snapshot.observations.size(); ++index) {
        const ObservationRecord& entry = snapshot.observations[index];
        out << "    {\"id\":\"" << escape_json(entry.id)
            << "\",\"case_id\":\"" << escape_json(entry.case_id)
            << "\",\"source_kind\":\"" << escape_json(entry.source_kind)
            << "\",\"source_ref\":\"" << escape_json(entry.source_ref)
            << "\",\"collected_at\":\"" << escape_json(entry.collected_at)
            << "\",\"summary\":\"" << escape_json(entry.summary)
            << "\",\"raw_content\":\"" << escape_json(entry.raw_content)
            << "\",\"content_hash\":\"" << escape_json(entry.content_hash) << "\"}";
        if (index + 1 < snapshot.observations.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "  ],\n  \"normalized_objects\": [\n";
    for (std::size_t index = 0; index < snapshot.normalized_objects.size(); ++index) {
        const NormalizedObject& entry = snapshot.normalized_objects[index];
        out << "    {\"id\":\"" << escape_json(entry.id)
            << "\",\"case_id\":\"" << escape_json(entry.case_id)
            << "\",\"observation_id\":\"" << escape_json(entry.observation_id)
            << "\",\"object_type\":\"" << escape_json(entry.object_type)
            << "\",\"title\":\"" << escape_json(entry.title)
            << "\",\"summary\":\"" << escape_json(entry.summary)
            << "\",\"attributes\":" << render_string_array(entry.attributes) << "}";
        if (index + 1 < snapshot.normalized_objects.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "  ],\n  \"evidence_links\": [\n";
    for (std::size_t index = 0; index < snapshot.evidence_links.size(); ++index) {
        const EvidenceLink& entry = snapshot.evidence_links[index];
        out << "    {\"id\":\"" << escape_json(entry.id)
            << "\",\"case_id\":\"" << escape_json(entry.case_id)
            << "\",\"source_observation_id\":\"" << escape_json(entry.source_observation_id)
            << "\",\"target_object_id\":\"" << escape_json(entry.target_object_id)
            << "\",\"relation\":\"" << escape_json(entry.relation)
            << "\",\"rationale\":\"" << escape_json(entry.rationale) << "\"}";
        if (index + 1 < snapshot.evidence_links.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "  ],\n  \"analyst_comments\": [\n";
    for (std::size_t index = 0; index < snapshot.analyst_comments.size(); ++index) {
        const AnalystComment& entry = snapshot.analyst_comments[index];
        out << "    {\"id\":\"" << escape_json(entry.id)
            << "\",\"case_id\":\"" << escape_json(entry.case_id)
            << "\",\"author\":\"" << escape_json(entry.author)
            << "\",\"text\":\"" << escape_json(entry.text)
            << "\",\"created_at\":\"" << escape_json(entry.created_at) << "\"}";
        if (index + 1 < snapshot.analyst_comments.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "  ],\n  \"decision_candidates\": [\n";
    for (std::size_t index = 0; index < snapshot.decision_candidates.size(); ++index) {
        const DecisionCandidate& entry = snapshot.decision_candidates[index];
        out << "    {\"id\":\"" << escape_json(entry.id)
            << "\",\"case_id\":\"" << escape_json(entry.case_id)
            << "\",\"title\":\"" << escape_json(entry.title)
            << "\",\"rationale\":\"" << escape_json(entry.rationale)
            << "\",\"recommended_command\":\"" << escape_json(entry.recommended_command)
            << "\",\"status\":\"" << escape_json(entry.status)
            << "\",\"score\":" << entry.score
            << ",\"valid\":" << (entry.valid ? "true" : "false")
            << ",\"validity_score\":" << entry.validity_score
            << ",\"evidence_coverage\":" << entry.evidence_coverage
            << ",\"prior_success_score\":" << entry.prior_success_score
            << ",\"confidence\":" << std::fixed << std::setprecision(4) << entry.confidence
            << ",\"probability_likelihood\":" << std::fixed << std::setprecision(4) << entry.probability_likelihood
            << ",\"supporting_signals\":" << render_string_array(entry.supporting_signals)
            << ",\"validation_checks\":" << render_string_array(entry.validation_checks)
            << ",\"score_trace\":" << render_string_array(entry.score_trace)
            << ",\"operator_feedback\":\"" << escape_json(entry.operator_feedback)
            << "\",\"feedback_note\":\"" << escape_json(entry.feedback_note)
            << "\",\"feedback_timestamp\":\"" << escape_json(entry.feedback_timestamp)
            << "\",\"outcome_status\":\"" << escape_json(entry.outcome_status)
            << "\",\"outcome_note\":\"" << escape_json(entry.outcome_note)
            << "\",\"outcome_timestamp\":\"" << escape_json(entry.outcome_timestamp) << "\"}";
        if (index + 1 < snapshot.decision_candidates.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "  ],\n  \"case_records\": [\n";
    for (std::size_t index = 0; index < snapshot.case_records.size(); ++index) {
        const CaseRecord& entry = snapshot.case_records[index];
        out << "    {\"id\":\"" << escape_json(entry.id)
            << "\",\"title\":\"" << escape_json(entry.title)
            << "\",\"primary_source\":\"" << escape_json(entry.primary_source)
            << "\",\"status\":\"" << escape_json(entry.status)
            << "\",\"created_at\":\"" << escape_json(entry.created_at)
            << "\",\"updated_at\":\"" << escape_json(entry.updated_at)
            << "\",\"created_by_run_id\":\"" << escape_json(entry.created_by_run_id)
            << "\",\"analyzed_by_run_id\":\"" << escape_json(entry.analyzed_by_run_id)
            << "\",\"decided_by_run_id\":\"" << escape_json(entry.decided_by_run_id)
            << "\",\"reported_by_run_id\":\"" << escape_json(entry.reported_by_run_id)
            << "\",\"permission\":" << render_permission_context_json(entry.permission)
            << ",\"observation_ids\":" << render_string_array(entry.observation_ids)
            << ",\"object_ids\":" << render_string_array(entry.object_ids)
            << ",\"evidence_link_ids\":" << render_string_array(entry.evidence_link_ids)
            << ",\"comment_ids\":" << render_string_array(entry.comment_ids)
            << ",\"decision_ids\":" << render_string_array(entry.decision_ids)
            << ",\"latest_summary\":\"" << escape_json(entry.latest_summary) << "\"}";
        if (index + 1 < snapshot.case_records.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "  ],\n  \"case_links\": [\n";
    for (std::size_t index = 0; index < snapshot.case_links.size(); ++index) {
        const CaseLink& entry = snapshot.case_links[index];
        out << "    {\"id\":\"" << escape_json(entry.id)
            << "\",\"left_case_id\":\"" << escape_json(entry.left_case_id)
            << "\",\"right_case_id\":\"" << escape_json(entry.right_case_id)
            << "\",\"link_type\":\"" << escape_json(entry.link_type)
            << "\",\"link_value\":\"" << escape_json(entry.link_value)
            << "\",\"rationale\":\"" << escape_json(entry.rationale)
            << "\",\"strength\":" << entry.strength << "}";
        if (index + 1 < snapshot.case_links.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "  ]\n}\n";
    return out.str();
}

std::string join_stage_fields(const std::vector<std::string>& values) {
    std::ostringstream out;
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) {
            out << ", ";
        }
        out << values[index];
    }
    return out.str();
}

std::string render_stage_source(const TzeStageRecord& stage) {
    if (stage.source_line == 0 && stage.source_section.empty() && stage.graph_origin.empty()) {
        return {};
    }

    std::ostringstream out;
    bool has_value = false;
    if (!stage.source_section.empty()) {
        out << stage.source_section;
        has_value = true;
    }
    if (stage.source_line != 0) {
        if (has_value) {
            out << ":";
        }
        out << stage.source_line;
        has_value = true;
    }
    if (!stage.graph_origin.empty()) {
        if (has_value) {
            out << " | ";
        }
        out << stage.graph_origin;
    }
    return out.str();
}

bool same_stage_provenance(const TzeStageRecord& left, const TzeStageRecord& right) {
    return left.graph_origin == right.graph_origin &&
        left.source_section == right.source_section &&
        left.source_line == right.source_line &&
        left.source_excerpt == right.source_excerpt;
}

std::string render_query_session_summary(const QuerySessionRecord& session) {
    std::ostringstream out;
    out << " - Session id: " << session.id << "\n";
    if (!session.command_label.empty()) {
        out << " - Command: " << session.command_label << "\n";
    }
    if (!session.query_seed.empty()) {
        out << " - Seed: " << session.query_seed << "\n";
    }
    if (!session.active_context.empty()) {
        out << " - Context: " << join_stage_fields(session.active_context) << "\n";
    }
    if (!session.indexed_values.empty()) {
        out << " - Indexed values: " << join_stage_fields(session.indexed_values) << "\n";
    }
    if (!session.final_results.empty()) {
        out << " - Final results: " << join_stage_fields(session.final_results) << "\n";
    }
    out << "Operations:\n";
    for (const QueryOperation& operation : session.operations) {
        out << " - " << operation.operator_name;
        if (!operation.label.empty()) {
            out << " [" << operation.label << "]";
        }
        if (!operation.outputs.empty()) {
            out << " => " << join_stage_fields(operation.outputs);
        }
        out << "\n";
        if (!operation.trace.empty()) {
            out << "   trace: " << join_stage_fields(operation.trace) << "\n";
        }
        for (const QueryCandidate& candidate : operation.candidates) {
            out << "   candidate: " << candidate.label << " [" << candidate.score << "]";
            if (!candidate.matched_context.empty()) {
                out << " context=" << join_stage_fields(candidate.matched_context);
            }
            out << "\n";
        }
    }
    return out.str();
}

std::string render_provider_probe_summary(const ProviderProbeReport& probe) {
    std::ostringstream out;
    out << " - Provider: " << probe.provider_id << "\n";
    out << " - Status: " << probe.status << "\n";
    out << " - Summary: " << probe.summary << "\n";
    if (!probe.base_url.empty()) {
        out << " - Base URL: " << probe.base_url << "\n";
    }
    if (!probe.model.empty()) {
        out << " - Model: " << probe.model << "\n";
    }
    if (!probe.checks.empty()) {
        out << " - Checks:\n";
        for (const std::string& check : probe.checks) {
            out << "   - " << check << "\n";
        }
    }
    if (!probe.warnings.empty()) {
        out << " - Warnings:\n";
        for (const std::string& warning : probe.warnings) {
            out << "   - " << warning << "\n";
        }
    }
    return out.str();
}

std::string render_assist_annotation_summary(const AssistAnnotation& assist) {
    std::ostringstream out;
    out << " - Task: " << assist.task_id << "\n";
    out << " - Provider: " << assist.provider_id;
    if (!assist.model.empty()) {
        out << " (" << assist.model << ")";
    }
    out << "\n";
    out << " - Status: " << assist.status << "\n";
    out << " - Summary: " << assist.summary << "\n";
    if (!assist.highlights.empty()) {
        out << " - Highlights:\n";
        for (const std::string& highlight : assist.highlights) {
            out << "   - " << highlight << "\n";
        }
    }
    if (!assist.operator_takeaway.empty()) {
        out << " - Operator takeaway: " << assist.operator_takeaway << "\n";
    }
    if (!assist.warnings.empty()) {
        out << " - Warnings:\n";
        for (const std::string& warning : assist.warnings) {
            out << "   - " << warning << "\n";
        }
    }
    return out.str();
}

std::string render_tool_assist_plan_summary(const ToolAssistPlan& plan) {
    std::ostringstream out;
    out << " - Task: " << plan.task_id << "\n";
    out << " - Provider: " << plan.provider_id;
    if (!plan.model.empty()) {
        out << " (" << plan.model << ")";
    }
    out << "\n";
    out << " - Status: " << plan.status << "\n";
    out << " - Tool: " << plan.tool_name << "\n";
    if (!plan.arguments.empty()) {
        out << " - Arguments: " << join_stage_fields(plan.arguments) << "\n";
    }
    if (!plan.rationale.empty()) {
        out << " - Rationale: " << plan.rationale << "\n";
    }
    if (!plan.safety_notes.empty()) {
        out << " - Safety notes:\n";
        for (const std::string& note : plan.safety_notes) {
            out << "   - " << note << "\n";
        }
    }
    out << " - Validated: " << (plan.validated ? "yes" : "no") << "\n";
    return out.str();
}

std::string render_command_assist_plan_summary(const CommandAssistPlan& plan) {
    std::ostringstream out;
    out << " - Task: " << plan.task_id << "\n";
    out << " - Provider: " << plan.provider_id;
    if (!plan.model.empty()) {
        out << " (" << plan.model << ")";
    }
    out << "\n";
    out << " - Status: " << plan.status << "\n";
    out << " - Canonical command: " << plan.canonical_command << "\n";
    out << " - Command family: " << plan.command_family << "\n";
    out << " - Confidence: " << plan.confidence << "\n";
    out << " - Requires confirmation: " << (plan.requires_confirmation ? "yes" : "no") << "\n";
    if (!plan.rationale.empty()) {
        out << " - Rationale: " << plan.rationale << "\n";
    }
    if (!plan.safety_notes.empty()) {
        out << " - Safety notes:\n";
        for (const std::string& note : plan.safety_notes) {
            out << "   - " << note << "\n";
        }
    }
    out << " - Validated: " << (plan.validated ? "yes" : "no") << "\n";
    return out.str();
}

std::string render_build_assist_plan_summary(const BuildAssistPlan& plan) {
    std::ostringstream out;
    out << " - Task: " << plan.task_id << "\n";
    out << " - Provider: " << plan.provider_id;
    if (!plan.model.empty()) {
        out << " (" << plan.model << ")";
    }
    out << "\n";
    out << " - Status: " << plan.status << "\n";
    out << " - Selected recipe: " << plan.selected_recipe_id << "\n";
    if (!plan.fallback_recipe_id.empty()) {
        out << " - Fallback recipe: " << plan.fallback_recipe_id << "\n";
    }
    out << " - Confidence: " << plan.confidence << "\n";
    if (!plan.rationale.empty()) {
        out << " - Rationale: " << plan.rationale << "\n";
    }
    if (!plan.safety_notes.empty()) {
        out << " - Safety notes:\n";
        for (const std::string& note : plan.safety_notes) {
            out << "   - " << note << "\n";
        }
    }
    out << " - Validated: " << (plan.validated ? "yes" : "no") << "\n";
    return out.str();
}

std::string render_security_audit_summary(const SecurityAudit& entry) {
    std::ostringstream out;
    out << " - Status: " << entry.status << "\n";
    out << " - Behavior: " << entry.behavior_mode << "\n";
    out << " - Threat: " << entry.threat_label << "\n";
    out << " - Bracket: " << entry.threat_bracket << "\n";
    out << " - Admin verified: " << (entry.admin_verified ? "yes" : "no") << "\n";
    if (!entry.trace_paths.empty()) {
        out << " - Trace paths: " << join_stage_fields(entry.trace_paths) << "\n";
    }
    if (!entry.simulated_actions.empty()) {
        out << " - Simulated actions: " << join_stage_fields(entry.simulated_actions) << "\n";
    }
    if (!entry.blocked_paths.empty()) {
        out << " - Blocked paths: " << join_stage_fields(entry.blocked_paths) << "\n";
    }
    if (!entry.mitigations.empty()) {
        out << " - Mitigations: " << join_stage_fields(entry.mitigations) << "\n";
    }
    if (!entry.evidence.empty()) {
        out << " - Evidence: " << join_stage_fields(entry.evidence) << "\n";
    }
    if (!entry.reasoning_trace.empty()) {
        out << " - Trace: " << join_stage_fields(entry.reasoning_trace) << "\n";
    }
    return out.str();
}

std::string render_language_resolution_summary(const LanguageResolutionRecord& entry) {
    std::ostringstream out;
    out << " - Context: " << entry.combined_context << "\n";
    out << " - Native OS: " << entry.native_os << "\n";
    out << " - Observed locale: " << entry.observed_locale << "\n";
    out << " - Selected OS: " << entry.selected_os << "\n";
    out << " - Selected language: " << entry.selected_language << "\n";
    out << " - Passes: " << entry.passes << "\n";
    out << " - Confidence: " << std::fixed << std::setprecision(2) << entry.confidence << "\n";
    if (entry.manual_confirmation_required) {
        out << " - Manual confirmation: required";
        if (!entry.manual_confirmation_response.empty()) {
            out << " (" << entry.manual_confirmation_response << ")";
        }
        out << "\n";
    } else if (entry.manual_confirmation_used) {
        out << " - Manual confirmation: " << entry.manual_confirmation_response << "\n";
    }
    if (!entry.reasoning_trace.empty()) {
        out << " - Trace: " << join_stage_fields(entry.reasoning_trace) << "\n";
    }
    return out.str();
}

std::string render_uac_state_summary(const UacStateRecord& entry) {
    std::ostringstream out;
    out << " - Epoch: " << entry.epoch_marker << "\n";
    out << " - Machine: " << entry.machine_identifier << "\n";
    out << " - Namespace: " << entry.store_namespace << "\n";
    out << " - Search namespace: " << entry.search_namespace << "\n";
    out << " - GENx: " << entry.genx_token_value << "\n";
    out << " - Compression: " << entry.compression_label << "\n";
    out << " - Key store: " << entry.key_store_address_value << "\n";
    out << " - Key budget: " << entry.key_budget_value << "\n";
    if (!entry.indexed_traits.empty()) {
        out << " - Traits: ";
        for (std::size_t index = 0; index < entry.indexed_traits.size(); ++index) {
            if (index != 0) {
                out << ", ";
            }
            out << entry.indexed_traits[index].trait_name;
        }
        out << "\n";
    }
    if (!entry.recovery_hints.empty()) {
        out << " - Recovery hints: " << join_stage_fields(entry.recovery_hints) << "\n";
    }
    if (!entry.reasoning_trace.empty()) {
        out << " - Trace: " << join_stage_fields(entry.reasoning_trace) << "\n";
    }
    return out.str();
}

const CaseRecord* find_case_record(const MemorySnapshot& snapshot, std::string_view case_reference) {
    for (const CaseRecord& entry : snapshot.case_records) {
        if (entry.id == case_reference || entry.primary_source == case_reference) {
            return &entry;
        }
    }
    return nullptr;
}

CaseRecord* find_case_record(MemorySnapshot& snapshot, std::string_view case_reference) {
    for (CaseRecord& entry : snapshot.case_records) {
        if (entry.id == case_reference || entry.primary_source == case_reference) {
            return &entry;
        }
    }
    return nullptr;
}

DecisionCandidate* find_decision_candidate(MemorySnapshot& snapshot,
                                           std::string_view case_id,
                                           std::string_view decision_reference) {
    for (DecisionCandidate& entry : snapshot.decision_candidates) {
        if (entry.case_id != case_id) {
            continue;
        }
        if (entry.id == decision_reference || entry.title == decision_reference) {
            return &entry;
        }
    }
    return nullptr;
}

const DecisionCandidate* find_decision_candidate(const MemorySnapshot& snapshot,
                                                 std::string_view case_id,
                                                 std::string_view decision_reference) {
    for (const DecisionCandidate& entry : snapshot.decision_candidates) {
        if (entry.case_id != case_id) {
            continue;
        }
        if (entry.id == decision_reference || entry.title == decision_reference) {
            return &entry;
        }
    }
    return nullptr;
}

std::vector<const ObservationRecord*> observations_for_case(const MemorySnapshot& snapshot, std::string_view case_id) {
    std::vector<const ObservationRecord*> entries;
    for (const ObservationRecord& entry : snapshot.observations) {
        if (entry.case_id == case_id) {
            entries.push_back(&entry);
        }
    }
    return entries;
}

std::vector<const NormalizedObject*> objects_for_case(const MemorySnapshot& snapshot, std::string_view case_id) {
    std::vector<const NormalizedObject*> entries;
    for (const NormalizedObject& entry : snapshot.normalized_objects) {
        if (entry.case_id == case_id) {
            entries.push_back(&entry);
        }
    }
    return entries;
}

std::vector<const EvidenceLink*> links_for_case(const MemorySnapshot& snapshot, std::string_view case_id) {
    std::vector<const EvidenceLink*> entries;
    for (const EvidenceLink& entry : snapshot.evidence_links) {
        if (entry.case_id == case_id) {
            entries.push_back(&entry);
        }
    }
    return entries;
}

std::vector<const AnalystComment*> comments_for_case(const MemorySnapshot& snapshot, std::string_view case_id) {
    std::vector<const AnalystComment*> entries;
    for (const AnalystComment& entry : snapshot.analyst_comments) {
        if (entry.case_id == case_id) {
            entries.push_back(&entry);
        }
    }
    return entries;
}

std::vector<const DecisionCandidate*> decisions_for_case(const MemorySnapshot& snapshot, std::string_view case_id) {
    std::vector<const DecisionCandidate*> entries;
    for (const DecisionCandidate& entry : snapshot.decision_candidates) {
        if (entry.case_id == case_id) {
            entries.push_back(&entry);
        }
    }
    std::stable_sort(entries.begin(), entries.end(), [](const DecisionCandidate* lhs, const DecisionCandidate* rhs) {
        if (lhs->score != rhs->score) {
            return lhs->score > rhs->score;
        }
        if (lhs->probability_likelihood != rhs->probability_likelihood) {
            return lhs->probability_likelihood > rhs->probability_likelihood;
        }
        return lhs->confidence > rhs->confidence;
    });
    return entries;
}

std::vector<const TzeRunRecord*> runs_for_case(const MemorySnapshot& snapshot, std::string_view case_id) {
    std::vector<const TzeRunRecord*> entries;
    for (const TzeRunRecord& entry : snapshot.tze_runs) {
        if (entry.linked_case_id == case_id) {
            entries.push_back(&entry);
        }
    }
    std::stable_sort(entries.begin(), entries.end(), [](const TzeRunRecord* lhs, const TzeRunRecord* rhs) {
        if (lhs->timestamp != rhs->timestamp) {
            return lhs->timestamp < rhs->timestamp;
        }
        return lhs->id < rhs->id;
    });
    return entries;
}

std::vector<const TzeRunRecord*> chain_runs_for(const MemorySnapshot& snapshot, const TzeRunRecord& anchor) {
    std::vector<const TzeRunRecord*> entries;
    for (const TzeRunRecord& entry : snapshot.tze_runs) {
        if (!anchor.linked_case_id.empty() && entry.linked_case_id == anchor.linked_case_id) {
            entries.push_back(&entry);
            continue;
        }
        if (anchor.linked_case_id.empty() && !anchor.target.empty() && entry.target == anchor.target) {
            entries.push_back(&entry);
        }
    }
    if (entries.empty()) {
        entries.push_back(&anchor);
    }
    std::stable_sort(entries.begin(), entries.end(), [](const TzeRunRecord* lhs, const TzeRunRecord* rhs) {
        if (lhs->timestamp != rhs->timestamp) {
            return lhs->timestamp < rhs->timestamp;
        }
        return lhs->id < rhs->id;
    });
    entries.erase(std::unique(entries.begin(), entries.end()), entries.end());
    return entries;
}

std::vector<CaseCluster> derive_incident_clusters(const MemorySnapshot& snapshot) {
    std::vector<CaseCluster> clusters;
    if (snapshot.case_records.size() < 2 || snapshot.case_links.empty()) {
        return clusters;
    }

    std::map<std::string, std::vector<const CaseLink*>> adjacency;
    for (const CaseLink& link : snapshot.case_links) {
        adjacency[link.left_case_id].push_back(&link);
        adjacency[link.right_case_id].push_back(&link);
    }

    std::set<std::string> visited;
    for (const CaseRecord& case_record : snapshot.case_records) {
        if (visited.find(case_record.id) != visited.end() || adjacency.find(case_record.id) == adjacency.end()) {
            continue;
        }

        std::vector<std::string> queue = {case_record.id};
        std::vector<std::string> case_ids;
        std::vector<const CaseLink*> component_links;
        visited.insert(case_record.id);
        for (std::size_t index = 0; index < queue.size(); ++index) {
            const std::string current = queue[index];
            case_ids.push_back(current);
            for (const CaseLink* link : adjacency[current]) {
                component_links.push_back(link);
                const std::string& neighbor = link->left_case_id == current ? link->right_case_id : link->left_case_id;
                if (visited.insert(neighbor).second) {
                    queue.push_back(neighbor);
                }
            }
        }

        std::sort(case_ids.begin(), case_ids.end());
        case_ids.erase(std::unique(case_ids.begin(), case_ids.end()), case_ids.end());
        if (case_ids.size() < 2) {
            continue;
        }

        std::set<std::string> case_set(case_ids.begin(), case_ids.end());
        std::set<std::string> link_ids;
        std::vector<std::string> link_types;
        std::vector<std::string> indicators;
        int total_strength = 0;
        int strongest_link = 0;
        std::vector<const CaseLink*> internal_links;
        for (const CaseLink* link : component_links) {
            if (case_set.find(link->left_case_id) == case_set.end() || case_set.find(link->right_case_id) == case_set.end()) {
                continue;
            }
            if (!link_ids.insert(link->id).second) {
                continue;
            }
            internal_links.push_back(link);
            total_strength += link->strength;
            strongest_link = std::max(strongest_link, link->strength);
            if (std::find(link_types.begin(), link_types.end(), link->link_type) == link_types.end()) {
                link_types.push_back(link->link_type);
            }
            if (!link->link_value.empty() &&
                std::find(indicators.begin(), indicators.end(), link->link_value) == indicators.end()) {
                indicators.push_back(link->link_value);
            }
        }
        if (internal_links.empty()) {
            continue;
        }

        const double average_strength = static_cast<double>(total_strength) / static_cast<double>(internal_links.size());
        const double max_edges = std::max(1.0,
            static_cast<double>(case_ids.size() * (case_ids.size() - 1)) / 2.0);
        const double density = static_cast<double>(internal_links.size()) / max_edges;
        const double diversity = static_cast<double>(link_types.size()) / 4.0;
        const double indicator_coverage = std::min(
            1.0,
            static_cast<double>(indicators.size()) / static_cast<double>(case_ids.size()));
        const double likelihood = std::max(
            0.0,
            std::min(1.0,
                (0.48 * (average_strength / 100.0)) + (0.24 * density) + (0.14 * diversity) + (0.14 * indicator_coverage)));

        CaseCluster cluster;
        cluster.id = "incident-" + std::to_string(std::hash<std::string>{}(join_stage_fields(case_ids)));
        cluster.case_ids = case_ids;
        cluster.case_count = static_cast<int>(case_ids.size());
        cluster.link_types = link_types;
        cluster.shared_indicators = indicators;
        cluster.likelihood = likelihood;
        cluster.correlation_score = std::max(0, std::min(100, static_cast<int>(std::round(likelihood * 100.0))));
        cluster.cluster_type = case_ids.size() >= 3 ? "campaign_cluster" : "incident_cluster";
        cluster.title = cluster.cluster_type == "campaign_cluster"
            ? "Campaign cluster"
            : "Incident cluster";
        std::ostringstream summary;
        summary << "Derived from " << internal_links.size() << " correlated link(s) across "
                << case_ids.size() << " case(s); strongest link=" << strongest_link;
        if (!link_types.empty()) {
            summary << "; link types: " << join_stage_fields(link_types);
        }
        if (!indicators.empty()) {
            summary << "; indicators: " << join_stage_fields(indicators);
        }
        cluster.summary = summary.str();
        for (const CaseLink* link : internal_links) {
            cluster.link_ids.push_back(link->id);
        }
        clusters.push_back(std::move(cluster));
    }

    std::stable_sort(clusters.begin(), clusters.end(), [](const CaseCluster& lhs, const CaseCluster& rhs) {
        if (lhs.correlation_score != rhs.correlation_score) {
            return lhs.correlation_score > rhs.correlation_score;
        }
        if (lhs.case_count != rhs.case_count) {
            return lhs.case_count > rhs.case_count;
        }
        return lhs.id < rhs.id;
    });
    return clusters;
}

const CaseCluster* find_incident_cluster(const std::vector<CaseCluster>& clusters, std::string_view reference) {
    for (const CaseCluster& cluster : clusters) {
        if (cluster.id == reference || cluster.title == reference) {
            return &cluster;
        }
    }
    return nullptr;
}

std::filesystem::path export_path(const MemorySnapshot& snapshot,
                                  std::string_view group,
                                  std::string_view stem,
                                  const std::filesystem::path& explicit_output) {
    if (!explicit_output.empty()) {
        return explicit_output;
    }
    return snapshot.paths.root / "exports" / std::string(group) / (std::string(stem) + ".json");
}

std::filesystem::path incident_report_path(const MemorySnapshot& snapshot,
                                           std::string_view stem,
                                           const std::filesystem::path& explicit_output) {
    if (!explicit_output.empty()) {
        return explicit_output;
    }
    return snapshot.paths.root / "incident-reports" / (std::string(stem) + ".txt");
}

void ensure_file_exists(const std::filesystem::path& path, const std::string& default_content) {
    if (std::filesystem::exists(path)) {
        return;
    }
    write_text(path, default_content);
}

}  // namespace

MemoryPaths MemoryStore::resolve_paths(const std::filesystem::path& requested_root) const {
    MemoryPaths paths;
    if (!requested_root.empty()) {
        paths.root = requested_root;
    } else if (const char* env_home = std::getenv("OMNIX_HOME"); env_home != nullptr && *env_home != '\0') {
        paths.root = env_home;
    } else if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        paths.root = std::filesystem::path(home) / ".omnix";
    } else {
    paths.root = std::filesystem::current_path() / ".omnix";
    }

    paths.history_path = paths.root / "history.jsonl";
    paths.tze_runs_path = paths.root / "tze_runs.json";
    paths.definitions_path = paths.root / "definitions.json";
    paths.preferences_path = paths.root / "preferences.json";
    paths.projects_path = paths.root / "projects.json";
    paths.native_tools_path = paths.root / "native_tools.json";
    paths.language_contexts_path = paths.root / "language_contexts.json";
    paths.uac_states_path = paths.root / "uac_states.json";
    paths.security_audits_path = paths.root / "security_audits.json";
    paths.cases_path = paths.root / "cases.json";
    paths.workspaces_root = paths.root / "workspaces";
    paths.installs_root = paths.root / "installs";
    paths.logs_root = paths.root / "logs";

    std::filesystem::create_directories(paths.root);
    std::filesystem::create_directories(paths.workspaces_root);
    std::filesystem::create_directories(paths.installs_root);
    std::filesystem::create_directories(paths.logs_root);
    ensure_file_exists(paths.history_path, "");
    ensure_file_exists(paths.tze_runs_path, "{\n  \"entries\": []\n}\n");
    ensure_file_exists(paths.definitions_path, "{\n  \"entries\": []\n}\n");
    ensure_file_exists(paths.preferences_path, "{\n  \"source_preference_order\": [\"Wikipedia\", \"Oxford\", \"Webster\"]\n}\n");
    ensure_file_exists(paths.projects_path, "{\n  \"entries\": [],\n  \"learned_recipes\": []\n}\n");
    ensure_file_exists(paths.native_tools_path, "{\n  \"entries\": []\n}\n");
    ensure_file_exists(paths.language_contexts_path, "{\n  \"entries\": []\n}\n");
    ensure_file_exists(paths.uac_states_path, "{\n  \"entries\": []\n}\n");
    ensure_file_exists(paths.security_audits_path, "{\n  \"entries\": []\n}\n");
    ensure_file_exists(paths.cases_path, "{\n  \"observations\": [],\n  \"normalized_objects\": [],\n  \"evidence_links\": [],\n  \"analyst_comments\": [],\n  \"decision_candidates\": [],\n  \"case_records\": [],\n  \"case_links\": []\n}\n");
    return paths;
}

MemorySnapshot MemoryStore::load(const std::filesystem::path& requested_root) const {
    MemorySnapshot snapshot;
    snapshot.paths = resolve_paths(requested_root);

    {
        std::ifstream history_input(snapshot.paths.history_path);
        for (std::string line; std::getline(history_input, line);) {
            line = trim(line);
            if (line.empty()) {
                continue;
            }
            snapshot.history.push_back(parse_history_entry(line));
        }
    }

    const std::string tze_runs_json = read_text(snapshot.paths.tze_runs_path);
    for (const std::string& object_text : extract_object_entries(tze_runs_json, "entries")) {
        snapshot.tze_runs.push_back(parse_tze_run_entry(object_text));
    }

    const std::string definitions_json = read_text(snapshot.paths.definitions_path);
    for (const std::string& object_text : extract_object_entries(definitions_json, "entries")) {
        snapshot.definitions.push_back(parse_definition_entry(object_text));
    }

    const std::string preferences_json = read_text(snapshot.paths.preferences_path);
    snapshot.source_preference_order = extract_json_string_array(preferences_json, "source_preference_order");
    if (snapshot.source_preference_order.empty()) {
        snapshot.source_preference_order = {"Wikipedia", "Oxford", "Webster"};
    }

    const std::string projects_json = read_text(snapshot.paths.projects_path);
    for (const std::string& object_text : extract_object_entries(projects_json, "entries")) {
        snapshot.projects.push_back(parse_project_entry(object_text));
    }
    for (const std::string& object_text : extract_object_entries(projects_json, "learned_recipes")) {
        snapshot.learned_recipes.push_back(parse_learned_recipe_entry(object_text));
    }

    const std::string native_tools_json = read_text(snapshot.paths.native_tools_path);
    for (const std::string& object_text : extract_object_entries(native_tools_json, "entries")) {
        snapshot.native_tools.push_back(parse_native_tool_entry(object_text));
    }

    const std::string language_contexts_json = read_text(snapshot.paths.language_contexts_path);
    for (const std::string& object_text : extract_object_entries(language_contexts_json, "entries")) {
        snapshot.language_contexts.push_back(parse_language_resolution_entry(object_text));
    }

    const std::string uac_states_json = read_text(snapshot.paths.uac_states_path);
    for (const std::string& object_text : extract_object_entries(uac_states_json, "entries")) {
        snapshot.uac_states.push_back(parse_uac_state_entry(object_text));
    }

    const std::string security_audits_json = read_text(snapshot.paths.security_audits_path);
    for (const std::string& object_text : extract_object_entries(security_audits_json, "entries")) {
        snapshot.security_audits.push_back(parse_security_audit_entry(object_text));
    }

    const std::string cases_json = read_text(snapshot.paths.cases_path);
    for (const std::string& object_text : extract_object_entries(cases_json, "observations")) {
        snapshot.observations.push_back(parse_observation_entry(object_text));
    }
    for (const std::string& object_text : extract_object_entries(cases_json, "normalized_objects")) {
        snapshot.normalized_objects.push_back(parse_normalized_object_entry(object_text));
    }
    for (const std::string& object_text : extract_object_entries(cases_json, "evidence_links")) {
        snapshot.evidence_links.push_back(parse_evidence_link_entry(object_text));
    }
    for (const std::string& object_text : extract_object_entries(cases_json, "analyst_comments")) {
        snapshot.analyst_comments.push_back(parse_analyst_comment_entry(object_text));
    }
    for (const std::string& object_text : extract_object_entries(cases_json, "decision_candidates")) {
        snapshot.decision_candidates.push_back(parse_decision_candidate_entry(object_text));
    }
    for (const std::string& object_text : extract_object_entries(cases_json, "case_records")) {
        snapshot.case_records.push_back(parse_case_record_entry(object_text));
    }
    for (const std::string& object_text : extract_object_entries(cases_json, "case_links")) {
        snapshot.case_links.push_back(parse_case_link_entry(object_text));
    }

    return snapshot;
}

void MemoryStore::persist_snapshot(const MemorySnapshot& snapshot) const {
    write_text(snapshot.paths.history_path, render_history_jsonl(snapshot));
    write_text(snapshot.paths.tze_runs_path, render_tze_runs_json(snapshot));
    write_text(snapshot.paths.definitions_path, render_definitions_json(snapshot.definitions));
    write_text(snapshot.paths.preferences_path, render_preferences_json(snapshot));
    write_text(snapshot.paths.projects_path, render_projects_json(snapshot));
    write_text(snapshot.paths.native_tools_path, render_native_tools_json(snapshot));
    write_text(snapshot.paths.language_contexts_path, render_language_contexts_json(snapshot));
    write_text(snapshot.paths.uac_states_path, render_uac_states_json(snapshot));
    write_text(snapshot.paths.security_audits_path, render_security_audits_json(snapshot));
    write_text(snapshot.paths.cases_path, render_cases_json(snapshot));
}

std::string MemoryStore::resolve_tze_run_id(const MemorySnapshot& snapshot,
                                            std::string_view reference,
                                            bool important_only) const {
    const std::string trimmed = trim(reference);
    if (trimmed.empty()) {
        return {};
    }
    if (trimmed == "latest") {
        const std::vector<std::size_t> indexes = recent_tze_indexes(snapshot, 1, important_only);
        return indexes.empty() ? std::string{} : snapshot.tze_runs[indexes.front()].id;
    }
    if (trimmed == "previous") {
        const std::vector<std::size_t> indexes = recent_tze_indexes(snapshot, 2, important_only);
        return indexes.size() < 2 ? std::string{} : snapshot.tze_runs[indexes[1]].id;
    }
    if (trimmed == "latest-important") {
        return resolve_tze_run_id(snapshot, "latest", true);
    }
    if (trimmed == "previous-important") {
        return resolve_tze_run_id(snapshot, "previous", true);
    }
    return trimmed;
}

const TzeRunRecord* MemoryStore::find_tze_run(const MemorySnapshot& snapshot, std::string_view id) const {
    const std::string resolved_id = resolve_tze_run_id(snapshot, id, false);
    if (resolved_id.empty()) {
        return nullptr;
    }
    const auto match = std::find_if(snapshot.tze_runs.begin(), snapshot.tze_runs.end(), [&resolved_id](const TzeRunRecord& entry) {
        return entry.id == resolved_id;
    });
    if (match == snapshot.tze_runs.end()) {
        return nullptr;
    }
    return &(*match);
}

std::string MemoryStore::render_tze_run(const MemorySnapshot& snapshot, std::string_view id) const {
    const TzeRunRecord* entry = find_tze_run(snapshot, id);
    if (entry == nullptr) {
        return "No TZE run matched `" + std::string(id) + "`.";
    }

    std::ostringstream out;
    out << "TZE Run Replay\n";
    out << " - Run id: " << entry->id << "\n";
    out << " - Timestamp: " << entry->timestamp << "\n";
    out << " - Intent: " << entry->intent << "\n";
    if (!entry->prompt.empty()) {
        out << " - Prompt: " << entry->prompt << "\n";
    }
    if (!entry->target.empty()) {
    out << " - Target: " << entry->target << "\n";
    }
    if (!entry->linked_case_id.empty()) {
        out << " - Linked case: " << entry->linked_case_id << "\n";
    }
    out << " - Status: " << entry->status << "\n";
    if (!entry->reasoning_provider.empty()) {
        out << " - Reasoning provider: " << entry->reasoning_provider << "\n";
    }
    if (entry->provider_probe_report.has_value()) {
        out << "Provider probe:\n";
        out << render_provider_probe_summary(*entry->provider_probe_report);
    }
    if (!entry->assist_status.empty()) {
        out << " - Assist status: " << entry->assist_status << "\n";
    }
    if (entry->assist_annotation.has_value()) {
        out << "Guarded assist:\n";
        out << render_assist_annotation_summary(*entry->assist_annotation);
    }
    if (entry->command_assist_plan.has_value()) {
        out << "Command assist plan:\n";
        out << render_command_assist_plan_summary(*entry->command_assist_plan);
    }
    if (entry->tool_assist_plan.has_value()) {
        out << "Tool assist plan:\n";
        out << render_tool_assist_plan_summary(*entry->tool_assist_plan);
    }
    if (entry->build_assist_plan.has_value()) {
        out << "Build assist plan:\n";
        out << render_build_assist_plan_summary(*entry->build_assist_plan);
    }
    if (!entry->next_action.empty()) {
        out << " - Next action: " << entry->next_action << "\n";
    }
    if (!entry->produced_artifact.empty()) {
        out << " - Produced artifact: " << entry->produced_artifact << "\n";
    }
    if (!entry->feedback_status.empty()) {
        out << " - Operator feedback: " << entry->feedback_status;
        if (!entry->feedback_timestamp.empty()) {
            out << " @ " << entry->feedback_timestamp;
        }
        out << "\n";
        if (!entry->feedback_note.empty()) {
            out << " - Feedback note: " << entry->feedback_note << "\n";
        }
    }
    if (entry->security_audit.has_value()) {
        out << "Security audit:\n";
        out << render_security_audit_summary(*entry->security_audit);
    }
    if (entry->language_resolution.has_value()) {
        out << "Language resolution:\n";
        out << render_language_resolution_summary(*entry->language_resolution);
    }
    if (entry->uac_state.has_value()) {
        out << "uAC state:\n";
        out << render_uac_state_summary(*entry->uac_state);
    }
    if (entry->query_session.has_value()) {
        out << "Query session:\n";
        out << render_query_session_summary(*entry->query_session);
    }
    out << "Stages:\n";
    for (const TzeStageRecord& stage : entry->stages) {
        out << " - " << stage.stage_id << " [" << stage.status << "] " << stage.module;
        if (!stage.detail.empty()) {
            out << " | " << stage.detail;
        }
        out << "\n";
        const std::string source_ref = render_stage_source(stage);
        if (!source_ref.empty()) {
            out << "   source: " << source_ref << "\n";
        }
        if (!stage.source_excerpt.empty()) {
            out << "   excerpt: " << stage.source_excerpt << "\n";
        }
        if (!stage.inputs.empty()) {
            out << "   inputs: " << join_stage_fields(stage.inputs) << "\n";
        }
        if (!stage.outputs.empty()) {
            out << "   outputs: " << join_stage_fields(stage.outputs) << "\n";
        }
    }
    return out.str();
}

std::string MemoryStore::render_tze_diff(const MemorySnapshot& snapshot,
                                         std::string_view left_id,
                                         std::string_view right_id) const {
    const TzeRunRecord* left = find_tze_run(snapshot, left_id);
    const TzeRunRecord* right = find_tze_run(snapshot, right_id);
    if (left == nullptr || right == nullptr) {
        std::ostringstream out;
        out << "TZE Run Diff\n";
        if (left == nullptr) {
            out << " - Missing run: " << left_id << "\n";
        }
        if (right == nullptr) {
            out << " - Missing run: " << right_id << "\n";
        }
        return out.str();
    }

    std::ostringstream out;
    out << "TZE Run Diff\n";
    out << " - Left: " << left->id << " (" << left->intent << ", " << left->status << ")\n";
    out << " - Right: " << right->id << " (" << right->intent << ", " << right->status << ")\n";

    std::vector<std::string> changed_fields;
    const auto note_if_changed = [&changed_fields](std::string_view label,
                                                   const std::string& lhs,
                                                   const std::string& rhs) {
        if (lhs != rhs) {
            changed_fields.push_back(std::string(label) + ": `" + lhs + "` -> `" + rhs + "`");
        }
    };

    note_if_changed("intent", left->intent, right->intent);
    note_if_changed("prompt", left->prompt, right->prompt);
    note_if_changed("target", left->target, right->target);
    note_if_changed("status", left->status, right->status);
    note_if_changed("reasoning_provider", left->reasoning_provider, right->reasoning_provider);
    note_if_changed("provider_probe_status", left->provider_probe_status, right->provider_probe_status);
    note_if_changed("assist_status", left->assist_status, right->assist_status);
    note_if_changed("assist_command",
                    left->command_assist_plan.has_value() ? left->command_assist_plan->canonical_command : std::string{},
                    right->command_assist_plan.has_value() ? right->command_assist_plan->canonical_command : std::string{});
    note_if_changed("assist_tool",
                    left->tool_assist_plan.has_value() ? left->tool_assist_plan->tool_name : std::string{},
                    right->tool_assist_plan.has_value() ? right->tool_assist_plan->tool_name : std::string{});
    note_if_changed("assist_recipe",
                    left->build_assist_plan.has_value() ? left->build_assist_plan->selected_recipe_id : std::string{},
                    right->build_assist_plan.has_value() ? right->build_assist_plan->selected_recipe_id : std::string{});
    note_if_changed("next_action", left->next_action, right->next_action);
    note_if_changed("security_status",
                    left->security_audit.has_value() ? left->security_audit->status : std::string{},
                    right->security_audit.has_value() ? right->security_audit->status : std::string{});
    note_if_changed("security_mode",
                    left->security_audit.has_value() ? left->security_audit->behavior_mode : std::string{},
                    right->security_audit.has_value() ? right->security_audit->behavior_mode : std::string{});
    note_if_changed("language_context",
                    left->language_resolution.has_value() ? left->language_resolution->combined_context : std::string{},
                    right->language_resolution.has_value() ? right->language_resolution->combined_context : std::string{});
    note_if_changed("language_confidence",
                    left->language_resolution.has_value() ? std::to_string(left->language_resolution->confidence) : std::string{},
                    right->language_resolution.has_value() ? std::to_string(right->language_resolution->confidence) : std::string{});
    note_if_changed("uac_epoch",
                    left->uac_state.has_value() ? left->uac_state->epoch_marker : std::string{},
                    right->uac_state.has_value() ? right->uac_state->epoch_marker : std::string{});
    note_if_changed("uac_machine",
                    left->uac_state.has_value() ? left->uac_state->machine_identifier : std::string{},
                    right->uac_state.has_value() ? right->uac_state->machine_identifier : std::string{});
    note_if_changed("query_final_results",
                    left->query_session.has_value() ? join_stage_fields(left->query_session->final_results) : std::string{},
                    right->query_session.has_value() ? join_stage_fields(right->query_session->final_results) : std::string{});

    if (changed_fields.empty()) {
        out << "Changed fields:\n - None\n";
    } else {
        out << "Changed fields:\n";
        for (const std::string& field : changed_fields) {
            out << " - " << field << "\n";
        }
    }

    std::map<std::string, TzeStageRecord> left_stages;
    std::map<std::string, TzeStageRecord> right_stages;
    for (const TzeStageRecord& stage : left->stages) {
        left_stages[stage.stage_id] = stage;
    }
    for (const TzeStageRecord& stage : right->stages) {
        right_stages[stage.stage_id] = stage;
    }

    std::vector<std::string> stage_changes;
    for (const auto& [stage_id, left_stage] : left_stages) {
        const auto right_it = right_stages.find(stage_id);
        if (right_it == right_stages.end()) {
            stage_changes.push_back(stage_id + ": removed from right-hand run");
            continue;
        }
        const TzeStageRecord& right_stage = right_it->second;
        if (left_stage.status != right_stage.status || left_stage.detail != right_stage.detail ||
            left_stage.module != right_stage.module || left_stage.inputs != right_stage.inputs ||
            left_stage.outputs != right_stage.outputs || !same_stage_provenance(left_stage, right_stage)) {
            std::ostringstream line;
            line << stage_id << ": [" << left_stage.status << "] -> [" << right_stage.status << "]";
            if (left_stage.inputs != right_stage.inputs) {
                line << " | inputs changed";
            }
            if (left_stage.detail != right_stage.detail) {
                line << " | detail changed";
            }
            if (left_stage.outputs != right_stage.outputs) {
                line << " | outputs changed";
            }
            if (!same_stage_provenance(left_stage, right_stage)) {
                line << " | source changed";
            }
            stage_changes.push_back(line.str());
        }
    }
    for (const auto& [stage_id, right_stage] : right_stages) {
        (void)right_stage;
        if (left_stages.find(stage_id) == left_stages.end()) {
            stage_changes.push_back(stage_id + ": added in right-hand run");
        }
    }

    out << "Stage changes:\n";
    if (stage_changes.empty()) {
        out << " - No stage-level differences.\n";
    } else {
        for (const std::string& line : stage_changes) {
            out << " - " << line << "\n";
        }
    }

    if (left->target == right->target && !left->target.empty()) {
        out << "Summary:\n - Both runs targeted `" << left->target << "`, so the differences above reflect how Omni's orchestration changed for the same path.\n";
    } else {
        out << "Summary:\n - The runs targeted different prompts or paths, so compare them as separate TZE traces rather than direct retries.\n";
    }

    return out.str();
}

std::string MemoryStore::render_tze_change_explanation(const MemorySnapshot& snapshot,
                                                       std::string_view left_id,
                                                       std::string_view right_id) const {
    const TzeRunRecord* left = find_tze_run(snapshot, left_id);
    const TzeRunRecord* right = find_tze_run(snapshot, right_id);
    if (left == nullptr || right == nullptr) {
        return render_tze_diff(snapshot, left_id, right_id);
    }

    std::ostringstream out;
    out << "TZE Change Explanation\n";
    out << " - Left run: " << left->id << " [" << left->intent << " => " << left->status << "]\n";
    out << " - Right run: " << right->id << " [" << right->intent << " => " << right->status << "]\n";

    if (left->target == right->target && !left->target.empty()) {
        out << "Overview:\n";
        out << " - Omni revisited the same target `" << left->target << "` and changed how it orchestrated the flow.\n";
    } else {
        out << "Overview:\n";
        out << " - Omni compared two different targets or prompts, so the change reflects a different work path rather than a retry.\n";
    }

    std::vector<std::string> meaning_lines;
    if (left->intent != right->intent) {
        meaning_lines.push_back("Intent changed from `" + left->intent + "` to `" + right->intent +
                                "`, so Omni selected a different top-level runtime path.");
    }
    if (left->status != right->status) {
        meaning_lines.push_back("Final outcome changed from `" + left->status + "` to `" + right->status +
                                "`, which means the later run ended in a different verdict.");
    }
    if (left->target != right->target && !left->target.empty() && !right->target.empty()) {
        meaning_lines.push_back("Target changed from `" + left->target + "` to `" + right->target +
                                "`, so Omni was no longer evaluating the same subject.");
    }
    if (left->linked_case_id != right->linked_case_id) {
        if (!left->linked_case_id.empty() || !right->linked_case_id.empty()) {
            meaning_lines.push_back("Case linkage changed from `" +
                                    (left->linked_case_id.empty() ? std::string("-") : left->linked_case_id) + "` to `" +
                                    (right->linked_case_id.empty() ? std::string("-") : right->linked_case_id) +
                                    "`, connecting the run to a different analyst case context.");
        }
    }
    if (left->produced_artifact != right->produced_artifact) {
        meaning_lines.push_back("Produced artifact changed, indicating the later run wrote a different saved output.");
    }
    if (left->feedback_status != right->feedback_status) {
        meaning_lines.push_back("Operator feedback changed from `" +
                                (left->feedback_status.empty() ? std::string("none") : left->feedback_status) + "` to `" +
                                (right->feedback_status.empty() ? std::string("none") : right->feedback_status) +
                                "`, which can affect future deterministic planning.");
    }
    if (left->provider_probe_status != right->provider_probe_status) {
        meaning_lines.push_back("Provider probe status changed from `" +
                                (left->provider_probe_status.empty() ? std::string("none") : left->provider_probe_status) + "` to `" +
                                (right->provider_probe_status.empty() ? std::string("none") : right->provider_probe_status) +
                                "`, so the local assistive-provider readiness changed between runs.");
    }
    if (left->assist_status != right->assist_status) {
        meaning_lines.push_back("Guarded assist status changed from `" +
                                (left->assist_status.empty() ? std::string("none") : left->assist_status) + "` to `" +
                                (right->assist_status.empty() ? std::string("none") : right->assist_status) +
                                "`, so the later run either used or bypassed local assistive annotation differently.");
    }
    const std::string left_command_assist =
        left->command_assist_plan.has_value() ? left->command_assist_plan->canonical_command : std::string{};
    const std::string right_command_assist =
        right->command_assist_plan.has_value() ? right->command_assist_plan->canonical_command : std::string{};
    if (left_command_assist != right_command_assist) {
        meaning_lines.push_back("Guarded command routing changed from `" +
                                (left_command_assist.empty() ? std::string("none") : left_command_assist) + "` to `" +
                                (right_command_assist.empty() ? std::string("none") : right_command_assist) +
                                "`, so the later run mapped the prompt into a different canonical Omni command.");
    }
    const std::string left_tool_assist =
        left->tool_assist_plan.has_value() ? left->tool_assist_plan->tool_name : std::string{};
    const std::string right_tool_assist =
        right->tool_assist_plan.has_value() ? right->tool_assist_plan->tool_name : std::string{};
    if (left_tool_assist != right_tool_assist) {
        meaning_lines.push_back("Guarded tool planning changed from `" +
                                (left_tool_assist.empty() ? std::string("none") : left_tool_assist) + "` to `" +
                                (right_tool_assist.empty() ? std::string("none") : right_tool_assist) +
                                "`, so the later run proposed a different allowlisted tool action.");
    }
    const std::string left_build_assist =
        left->build_assist_plan.has_value() ? left->build_assist_plan->selected_recipe_id : std::string{};
    const std::string right_build_assist =
        right->build_assist_plan.has_value() ? right->build_assist_plan->selected_recipe_id : std::string{};
    if (left_build_assist != right_build_assist) {
        meaning_lines.push_back("Guarded build planning changed from `" +
                                (left_build_assist.empty() ? std::string("none") : left_build_assist) + "` to `" +
                                (right_build_assist.empty() ? std::string("none") : right_build_assist) +
                                "`, so the later run selected a different allowlisted build recipe.");
    }
    const std::string left_security_mode =
        left->security_audit.has_value() ? left->security_audit->behavior_mode : std::string{};
    const std::string right_security_mode =
        right->security_audit.has_value() ? right->security_audit->behavior_mode : std::string{};
    if (left_security_mode != right_security_mode) {
        meaning_lines.push_back("Security handling changed from `" +
                                (left_security_mode.empty() ? std::string("none") : left_security_mode) + "` to `" +
                                (right_security_mode.empty() ? std::string("none") : right_security_mode) +
                                "`, so the later run used a different defensive audit posture.");
    }
    const std::string left_language_context =
        left->language_resolution.has_value() ? left->language_resolution->combined_context : std::string{};
    const std::string right_language_context =
        right->language_resolution.has_value() ? right->language_resolution->combined_context : std::string{};
    if (left_language_context != right_language_context) {
        meaning_lines.push_back("Language context changed from `" +
                                (left_language_context.empty() ? std::string("none") : left_language_context) + "` to `" +
                                (right_language_context.empty() ? std::string("none") : right_language_context) +
                                "`, so the later run resolved a different OS/language operating context.");
    }
    const std::string left_uac_epoch = left->uac_state.has_value() ? left->uac_state->epoch_marker : std::string{};
    const std::string right_uac_epoch = right->uac_state.has_value() ? right->uac_state->epoch_marker : std::string{};
    if (left_uac_epoch != right_uac_epoch) {
        meaning_lines.push_back("uAC epoch changed from `" +
                                (left_uac_epoch.empty() ? std::string("none") : left_uac_epoch) + "` to `" +
                                (right_uac_epoch.empty() ? std::string("none") : right_uac_epoch) +
                                "`, so the later run rebuilt or reused a different preprocessor recovery window.");
    }
    const std::string left_query_results = left->query_session.has_value() ? join_stage_fields(left->query_session->final_results) : std::string{};
    const std::string right_query_results = right->query_session.has_value() ? join_stage_fields(right->query_session->final_results) : std::string{};
    if (left_query_results != right_query_results) {
        meaning_lines.push_back("Query-state results changed from `" +
                                (left_query_results.empty() ? std::string("none") : left_query_results) + "` to `" +
                                (right_query_results.empty() ? std::string("none") : right_query_results) +
                                "`, so the later run narrowed or ranked candidates differently.");
    }

    std::map<std::string, TzeStageRecord> left_stages;
    std::map<std::string, TzeStageRecord> right_stages;
    for (const TzeStageRecord& stage : left->stages) {
        left_stages[stage.stage_id] = stage;
    }
    for (const TzeStageRecord& stage : right->stages) {
        right_stages[stage.stage_id] = stage;
    }

    std::vector<std::string> stage_lines;
    const auto explain_stage = [&stage_lines](std::string_view stage_id,
                                              const TzeStageRecord& lhs,
                                              const TzeStageRecord& rhs) {
        if (lhs.status == rhs.status && lhs.detail == rhs.detail && lhs.inputs == rhs.inputs &&
            lhs.outputs == rhs.outputs && same_stage_provenance(lhs, rhs)) {
            return;
        }
        if (!same_stage_provenance(lhs, rhs) &&
            lhs.status == rhs.status && lhs.detail == rhs.detail && lhs.inputs == rhs.inputs && lhs.outputs == rhs.outputs) {
            stage_lines.push_back("`" + std::string(stage_id) + "` kept the same runtime behavior, but its source-backed origin changed.");
            return;
        }
        if (stage_id == "x.Define.Low") {
            stage_lines.push_back("`x.Define.Low` changed, so intent decoding or target selection differed between the two runs.");
            return;
        }
        if (stage_id == "x.DisplayPriorityProcessingGate") {
            stage_lines.push_back("`x.DisplayPriorityProcessingGate` changed, so Omni reordered knowledge sources or module preferences.");
            return;
        }
        if (stage_id == "x.DisplayFeedBackLoop") {
            stage_lines.push_back("`x.DisplayFeedBackLoop` changed, so the later run used different prior outcomes or learned context.");
            return;
        }
        if (stage_id == "x.Assist.Plan") {
            stage_lines.push_back("`x.Assist.Plan` changed, so the provider proposed a different assistive interpretation for the same prompt.");
            return;
        }
        if (stage_id == "x.Assist.CommandPlan") {
            stage_lines.push_back("`x.Assist.CommandPlan` changed, so the provider proposed a different canonical Omni command.");
            return;
        }
        if (stage_id == "x.Assist.CommandValidate") {
            stage_lines.push_back("`x.Assist.CommandValidate` changed, so Omni either accepted or rejected a different provider-proposed canonical command.");
            return;
        }
        if (stage_id == "x.Assist.Validate") {
            stage_lines.push_back("`x.Assist.Validate` changed, so Omni either accepted or rejected a different provider-proposed action plan.");
            return;
        }
        if (stage_id == "x.Assist.BuildPlan") {
            stage_lines.push_back("`x.Assist.BuildPlan` changed, so the provider proposed a different allowlisted build recipe.");
            return;
        }
        if (stage_id == "x.Assist.BuildValidate") {
            stage_lines.push_back("`x.Assist.BuildValidate` changed, so Omni either accepted or rejected a different provider-proposed build recipe.");
            return;
        }
        if (stage_id == "x.Dispatch") {
            stage_lines.push_back("`x.Dispatch` changed, which means Omni executed a different runtime module path or returned a different dispatch result.");
            return;
        }
        if (stage_id == "x.Assist.Guarded") {
            stage_lines.push_back("`x.Assist.Guarded` changed, so the later run used a different validated assist annotation outcome on top of the deterministic result.");
            return;
        }
        if (stage_id == "x.Store") {
            stage_lines.push_back("`x.Store` changed, so persistence targets or saved results differed between the runs.");
            return;
        }
        if (stage_id == "xProcessingCache") {
            stage_lines.push_back("`xProcessingCache` changed, so the run opened or reused a different cache/work-buffer state.");
            return;
        }
        stage_lines.push_back("`" + std::string(stage_id) + "` changed between runs.");
    };

    for (const auto& [stage_id, left_stage] : left_stages) {
        const auto it = right_stages.find(stage_id);
        if (it == right_stages.end()) {
            stage_lines.push_back("`" + stage_id + "` was present in the left run but missing on the right.");
            continue;
        }
        explain_stage(stage_id, left_stage, it->second);
    }
    for (const auto& [stage_id, right_stage] : right_stages) {
        (void)right_stage;
        if (left_stages.find(stage_id) == left_stages.end()) {
            stage_lines.push_back("`" + stage_id + "` was added in the right-hand run.");
        }
    }

    out << "Meaning:\n";
    if (meaning_lines.empty()) {
        out << " - The high-level run metadata stayed consistent; most differences are at the stage-detail level.\n";
    } else {
        for (const std::string& line : meaning_lines) {
            out << " - " << line << "\n";
        }
    }

    out << "Stage interpretation:\n";
    if (stage_lines.empty()) {
        out << " - The stage graph was stable; Omni followed the same orchestration path.\n";
    } else {
        for (const std::string& line : stage_lines) {
            out << " - " << line << "\n";
        }
    }

    out << "Raw diff reference:\n";
    out << render_tze_diff(snapshot, left->id, right->id);
    return out.str();
}

std::string MemoryStore::write_tze_run_report(const MemorySnapshot& snapshot,
                                              std::string_view id,
                                              const std::filesystem::path& explicit_output) const {
    const TzeRunRecord* entry = find_tze_run(snapshot, id);
    if (entry == nullptr) {
        return {};
    }

    const std::filesystem::path output_path = tze_report_path(snapshot, entry->id, explicit_output);
    if (!output_path.parent_path().empty()) {
        std::filesystem::create_directories(output_path.parent_path());
    }

    std::ostringstream report;
    report << "# OmniX TZE Run Report\n\n";
    report << "- Run id: " << entry->id << "\n";
    report << "- Timestamp: " << entry->timestamp << "\n";
    report << "- Intent: " << entry->intent << "\n";
    if (!entry->prompt.empty()) {
        report << "- Prompt: " << entry->prompt << "\n";
    }
    if (!entry->target.empty()) {
        report << "- Target: " << entry->target << "\n";
    }
    if (!entry->linked_case_id.empty()) {
        report << "- Linked case: " << entry->linked_case_id << "\n";
    }
    report << "- Status: " << entry->status << "\n";
    report << "- Reasoning provider: " << entry->reasoning_provider << "\n";
    if (entry->provider_probe_report.has_value()) {
        report << "\n## Provider Probe\n\n";
        report << render_provider_probe_summary(*entry->provider_probe_report) << "\n";
    }
    if (!entry->assist_status.empty() || entry->assist_annotation.has_value() || entry->command_assist_plan.has_value() ||
        entry->tool_assist_plan.has_value() ||
        entry->build_assist_plan.has_value()) {
        report << "\n## Guarded Assist\n\n";
        report << "- Status: " << (entry->assist_status.empty() ? "assist_bypassed" : entry->assist_status) << "\n";
        if (entry->assist_annotation.has_value()) {
            report << render_assist_annotation_summary(*entry->assist_annotation) << "\n";
        }
        if (entry->command_assist_plan.has_value()) {
            report << render_command_assist_plan_summary(*entry->command_assist_plan) << "\n";
        }
        if (entry->tool_assist_plan.has_value()) {
            report << render_tool_assist_plan_summary(*entry->tool_assist_plan) << "\n";
        }
        if (entry->build_assist_plan.has_value()) {
            report << render_build_assist_plan_summary(*entry->build_assist_plan) << "\n";
        }
    }
    if (!entry->produced_artifact.empty()) {
        report << "- Produced artifact: " << entry->produced_artifact << "\n";
    }
    if (!entry->next_action.empty()) {
        report << "- Next action: " << entry->next_action << "\n";
    }
    if (entry->security_audit.has_value()) {
        report << "\n## Security Audit\n\n";
        report << render_security_audit_summary(*entry->security_audit) << "\n";
    }
    if (entry->language_resolution.has_value()) {
        report << "\n## Language Resolution\n\n";
        report << render_language_resolution_summary(*entry->language_resolution) << "\n";
    }
    if (entry->uac_state.has_value()) {
        report << "\n## uAC State\n\n";
        report << render_uac_state_summary(*entry->uac_state) << "\n";
    }
    if (entry->query_session.has_value()) {
        report << "\n## Query Session\n\n";
        report << render_query_session_summary(*entry->query_session) << "\n";
    }

    report << "\n## Stage Trace\n\n";
    for (const TzeStageRecord& stage : entry->stages) {
        report << "- " << stage.stage_id << " [" << stage.status << "] " << stage.module << "\n";
        if (!stage.detail.empty()) {
            report << "  - Detail: " << stage.detail << "\n";
        }
        const std::string source_ref = render_stage_source(stage);
        if (!source_ref.empty()) {
            report << "  - Source: " << source_ref << "\n";
        }
        if (!stage.source_excerpt.empty()) {
            report << "  - Source excerpt: `" << stage.source_excerpt << "`\n";
        }
        if (!stage.inputs.empty()) {
            report << "  - Inputs: " << join_stage_fields(stage.inputs) << "\n";
        }
        if (!stage.outputs.empty()) {
            report << "  - Outputs: " << join_stage_fields(stage.outputs) << "\n";
        }
    }

    report << "\n## Replay Summary\n\n";
    report << render_tze_run(snapshot, entry->id) << "\n";

    std::ofstream output(output_path);
    output << report.str();
    return output_path.string();
}

std::string MemoryStore::write_tze_diff_report(const MemorySnapshot& snapshot,
                                               std::string_view left_id,
                                               std::string_view right_id,
                                               const std::filesystem::path& explicit_output) const {
    const std::string resolved_left = resolve_tze_run_id(snapshot, left_id, false);
    const std::string resolved_right = resolve_tze_run_id(snapshot, right_id, false);
    const TzeRunRecord* left = find_tze_run(snapshot, resolved_left);
    const TzeRunRecord* right = find_tze_run(snapshot, resolved_right);
    if (left == nullptr || right == nullptr) {
        return {};
    }

    const std::filesystem::path output_path =
        tze_report_path(snapshot, left->id + "-vs-" + right->id, explicit_output);
    if (!output_path.parent_path().empty()) {
        std::filesystem::create_directories(output_path.parent_path());
    }

    std::ostringstream report;
    report << "# OmniX TZE Diff Report\n\n";
    report << "- Left run: " << left->id << "\n";
    report << "- Right run: " << right->id << "\n";
    report << "- Left intent/status: " << left->intent << " / " << left->status << "\n";
    report << "- Right intent/status: " << right->intent << " / " << right->status << "\n";
    if (!left->linked_case_id.empty() || !right->linked_case_id.empty()) {
        report << "- Linked cases: " << (left->linked_case_id.empty() ? "-" : left->linked_case_id)
               << " vs " << (right->linked_case_id.empty() ? "-" : right->linked_case_id) << "\n";
    }
    report << "\n## Diff Summary\n\n";
    report << render_tze_diff(snapshot, left->id, right->id) << "\n";

    std::ofstream output(output_path);
    output << report.str();
    return output_path.string();
}

std::string MemoryStore::prune_tze_runs(MemorySnapshot& snapshot,
                                        std::size_t keep_count,
                                        bool important_only) const {
    keep_count = std::max<std::size_t>(1, keep_count);
    const std::size_t before = snapshot.tze_runs.size();
    if (before <= keep_count && !important_only) {
        return "No TZE runs were pruned; the ledger already fits the requested limit.";
    }

    std::set<std::string> keep_ids;
    std::vector<std::size_t> chosen = recent_tze_indexes(snapshot, keep_count, important_only);
    if (chosen.empty()) {
        chosen = recent_tze_indexes(snapshot, keep_count, false);
    }
    for (std::size_t index : chosen) {
        keep_ids.insert(snapshot.tze_runs[index].id);
    }

    snapshot.tze_runs.erase(
        std::remove_if(snapshot.tze_runs.begin(),
                       snapshot.tze_runs.end(),
                       [&keep_ids](const TzeRunRecord& entry) {
                           return keep_ids.find(entry.id) == keep_ids.end();
                       }),
        snapshot.tze_runs.end());

    const std::size_t removed = before - snapshot.tze_runs.size();
    std::ostringstream out;
    out << "Pruned " << removed << " TZE run(s); kept " << snapshot.tze_runs.size()
        << " run(s) with keep=" << keep_count;
    if (important_only) {
        out << " and important-only retention.";
    } else {
        out << ".";
    }
    return out.str();
}

std::string MemoryStore::prune_memory(MemorySnapshot& snapshot,
                                      std::size_t keep_count,
                                      bool important_only) const {
    keep_count = std::max<std::size_t>(1, keep_count);
    const std::size_t history_before = snapshot.history.size();
    const std::size_t security_before = snapshot.security_audits.size();
    const std::size_t language_before = snapshot.language_contexts.size();
    const std::size_t uac_before = snapshot.uac_states.size();
    const std::size_t history_keep = std::max<std::size_t>(keep_count * 2, 12);
    if (snapshot.history.size() > history_keep) {
        snapshot.history.erase(snapshot.history.begin(),
                               snapshot.history.begin() + static_cast<long long>(snapshot.history.size() - history_keep));
    }
    if (snapshot.language_contexts.size() > history_keep) {
        snapshot.language_contexts.erase(snapshot.language_contexts.begin(),
                                         snapshot.language_contexts.begin() +
                                             static_cast<long long>(snapshot.language_contexts.size() - history_keep));
    }
    if (snapshot.security_audits.size() > history_keep) {
        snapshot.security_audits.erase(snapshot.security_audits.begin(),
                                       snapshot.security_audits.begin() +
                                           static_cast<long long>(snapshot.security_audits.size() - history_keep));
    }
    if (snapshot.uac_states.size() > history_keep) {
        snapshot.uac_states.erase(snapshot.uac_states.begin(),
                                  snapshot.uac_states.begin() +
                                      static_cast<long long>(snapshot.uac_states.size() - history_keep));
    }
    const std::string run_summary = prune_tze_runs(snapshot, keep_count, important_only);
    std::ostringstream out;
    out << run_summary << " History entries kept: " << snapshot.history.size()
        << " (removed " << (history_before - snapshot.history.size()) << ")."
        << " Security audits kept: " << snapshot.security_audits.size()
        << " (removed " << (security_before - snapshot.security_audits.size()) << ")."
        << " Language contexts kept: " << snapshot.language_contexts.size()
        << " (removed " << (language_before - snapshot.language_contexts.size()) << ")."
        << " uAC states kept: " << snapshot.uac_states.size()
        << " (removed " << (uac_before - snapshot.uac_states.size()) << ").";
    return out.str();
}

bool MemoryStore::mark_tze_run_feedback(MemorySnapshot& snapshot,
                                        std::string_view reference,
                                        std::string_view feedback_value,
                                        std::string_view feedback_note,
                                        std::string* resolved_id) const {
    const std::string run_id = resolve_tze_run_id(snapshot, reference, false);
    if (resolved_id != nullptr) {
        *resolved_id = run_id;
    }
    if (run_id.empty()) {
        return false;
    }
    const auto existing = std::find_if(snapshot.tze_runs.begin(), snapshot.tze_runs.end(), [&run_id](const TzeRunRecord& entry) {
        return entry.id == run_id;
    });
    if (existing == snapshot.tze_runs.end()) {
        return false;
    }
    existing->feedback_status = std::string(feedback_value);
    existing->feedback_note = std::string(feedback_note);
    existing->feedback_timestamp = now_timestamp();
    return true;
}

std::string MemoryStore::render_case_timeline(const MemorySnapshot& snapshot, std::string_view case_reference) const {
    const CaseRecord* case_record = find_case_record(snapshot, case_reference);
    if (case_record == nullptr) {
        return "No case matched `" + std::string(case_reference) + "`.";
    }

    struct TimelineEntry {
        std::string timestamp;
        std::string label;
        std::string detail;
    };

    std::vector<TimelineEntry> entries;
    entries.push_back({case_record->created_at, "case_created",
        "Case `" + case_record->id + "` created from `" + case_record->primary_source + "`."});

    const auto add_run_entry = [&](std::string_view run_id, std::string_view label) {
        if (run_id.empty()) {
            return;
        }
        const TzeRunRecord* run = find_tze_run(snapshot, run_id);
        if (run == nullptr) {
            entries.push_back({{}, std::string(label), "Linked run `" + std::string(run_id) + "` is no longer in the local ledger."});
            return;
        }
        std::ostringstream detail;
        detail << "Run `" << run->id << "` [" << run->intent << " => " << run->status << "]";
        if (!run->target.empty()) {
            detail << " target=`" << run->target << "`";
        }
        if (!run->produced_artifact.empty()) {
            detail << " artifact=`" << run->produced_artifact << "`";
        }
        entries.push_back({run->timestamp, std::string(label), detail.str()});
    };

    add_run_entry(case_record->created_by_run_id, "created_by_run");
    add_run_entry(case_record->analyzed_by_run_id, "analyzed_by_run");
    add_run_entry(case_record->decided_by_run_id, "decided_by_run");
    add_run_entry(case_record->reported_by_run_id, "reported_by_run");

    for (const DecisionCandidate* decision : decisions_for_case(snapshot, case_record->id)) {
        if (!decision->feedback_timestamp.empty()) {
            entries.push_back({decision->feedback_timestamp,
                               "decision_feedback",
                               "Decision `" + decision->id + "` marked `" + decision->operator_feedback + "`." +
                                   (decision->feedback_note.empty() ? std::string{} : " Note: " + decision->feedback_note)});
        }
        if (!decision->outcome_timestamp.empty()) {
            entries.push_back({decision->outcome_timestamp,
                               "decision_outcome",
                               "Decision `" + decision->id + "` outcome=`" + decision->outcome_status + "`." +
                                   (decision->outcome_note.empty() ? std::string{} : " Note: " + decision->outcome_note)});
        }
    }

    std::stable_sort(entries.begin(), entries.end(), [](const TimelineEntry& lhs, const TimelineEntry& rhs) {
        if (lhs.timestamp != rhs.timestamp) {
            return lhs.timestamp < rhs.timestamp;
        }
        return lhs.label < rhs.label;
    });

    std::ostringstream out;
    out << "Case Timeline\n";
    out << " - Case id: " << case_record->id << "\n";
    out << " - Title: " << case_record->title << "\n";
    out << " - Status: " << case_record->status << "\n";
    out << " - Source: " << case_record->primary_source << "\n";
    out << "Entries:\n";
    for (const TimelineEntry& entry : entries) {
        out << " - ";
        if (!entry.timestamp.empty()) {
            out << "[" << entry.timestamp << "] ";
        }
        out << entry.label;
        if (!entry.detail.empty()) {
            out << " | " << entry.detail;
        }
        out << "\n";
    }
    return out.str();
}

bool MemoryStore::mark_decision_feedback(MemorySnapshot& snapshot,
                                         std::string_view case_reference,
                                         std::string_view decision_reference,
                                         std::string_view feedback_value,
                                         std::string_view feedback_note,
                                         std::string* resolved_case_id,
                                         std::string* resolved_decision_id) const {
    CaseRecord* case_record = find_case_record(snapshot, case_reference);
    if (case_record == nullptr) {
        return false;
    }
    DecisionCandidate* decision = find_decision_candidate(snapshot, case_record->id, decision_reference);
    if (decision == nullptr) {
        return false;
    }
    decision->operator_feedback = std::string(feedback_value);
    decision->feedback_note = std::string(feedback_note);
    decision->feedback_timestamp = now_timestamp();
    if (resolved_case_id != nullptr) {
        *resolved_case_id = case_record->id;
    }
    if (resolved_decision_id != nullptr) {
        *resolved_decision_id = decision->id;
    }
    case_record->updated_at = decision->feedback_timestamp;
    return true;
}

bool MemoryStore::mark_decision_outcome(MemorySnapshot& snapshot,
                                        std::string_view case_reference,
                                        std::string_view decision_reference,
                                        std::string_view outcome_status,
                                        std::string_view outcome_note,
                                        std::string* resolved_case_id,
                                        std::string* resolved_decision_id) const {
    CaseRecord* case_record = find_case_record(snapshot, case_reference);
    if (case_record == nullptr) {
        return false;
    }
    DecisionCandidate* decision = find_decision_candidate(snapshot, case_record->id, decision_reference);
    if (decision == nullptr) {
        return false;
    }
    decision->outcome_status = std::string(outcome_status);
    decision->outcome_note = std::string(outcome_note);
    decision->outcome_timestamp = now_timestamp();
    if (resolved_case_id != nullptr) {
        *resolved_case_id = case_record->id;
    }
    if (resolved_decision_id != nullptr) {
        *resolved_decision_id = decision->id;
    }
    case_record->updated_at = decision->outcome_timestamp;
    return true;
}

std::string MemoryStore::render_tze_chain(const MemorySnapshot& snapshot, std::string_view id) const {
    const TzeRunRecord* anchor = find_tze_run(snapshot, id);
    if (anchor == nullptr) {
        return "No TZE run matched `" + std::string(id) + "`.";
    }

    const std::vector<const TzeRunRecord*> chain = chain_runs_for(snapshot, *anchor);
    std::ostringstream out;
    out << "TZE Chain\n";
    out << " - Anchor run: " << anchor->id << "\n";
    if (!anchor->linked_case_id.empty()) {
        out << " - Linked case: " << anchor->linked_case_id << "\n";
    }
    if (!anchor->target.empty()) {
        out << " - Target: " << anchor->target << "\n";
    }
    out << "Chain entries:\n";
    for (const TzeRunRecord* entry : chain) {
        out << " - [" << entry->timestamp << "] " << entry->id << " | " << entry->intent << " => " << entry->status;
        if (!entry->linked_case_id.empty()) {
            out << " | case=" << entry->linked_case_id;
        }
        if (!entry->target.empty()) {
            out << " | target=" << entry->target;
        }
        if (!entry->produced_artifact.empty()) {
            out << " | artifact=" << entry->produced_artifact;
        }
        if (!entry->feedback_status.empty()) {
            out << " | feedback=" << entry->feedback_status;
        }
        out << "\n";
    }
    return out.str();
}

std::string MemoryStore::write_case_bundle(const MemorySnapshot& snapshot,
                                           std::string_view case_reference,
                                           const std::filesystem::path& explicit_output) const {
    const CaseRecord* case_record = find_case_record(snapshot, case_reference);
    if (case_record == nullptr) {
        return {};
    }

    MemorySnapshot bundle = snapshot;
    bundle.observations.clear();
    bundle.normalized_objects.clear();
    bundle.evidence_links.clear();
    bundle.analyst_comments.clear();
    bundle.decision_candidates.clear();
    bundle.case_records.clear();
    bundle.case_links.clear();
    bundle.tze_runs.clear();

    bundle.case_records.push_back(*case_record);
    for (const ObservationRecord* entry : observations_for_case(snapshot, case_record->id)) {
        bundle.observations.push_back(*entry);
    }
    for (const NormalizedObject* entry : objects_for_case(snapshot, case_record->id)) {
        bundle.normalized_objects.push_back(*entry);
    }
    for (const EvidenceLink* entry : links_for_case(snapshot, case_record->id)) {
        bundle.evidence_links.push_back(*entry);
    }
    for (const AnalystComment* entry : comments_for_case(snapshot, case_record->id)) {
        bundle.analyst_comments.push_back(*entry);
    }
    for (const DecisionCandidate* entry : decisions_for_case(snapshot, case_record->id)) {
        bundle.decision_candidates.push_back(*entry);
    }
    for (const CaseLink& entry : snapshot.case_links) {
        if (entry.left_case_id == case_record->id || entry.right_case_id == case_record->id) {
            bundle.case_links.push_back(entry);
        }
    }
    for (const TzeRunRecord* entry : runs_for_case(snapshot, case_record->id)) {
        bundle.tze_runs.push_back(*entry);
    }
    const auto add_run_id = [&](std::string_view run_id) {
        if (run_id.empty()) {
            return;
        }
        const TzeRunRecord* run = find_tze_run(snapshot, run_id);
        if (run == nullptr) {
            return;
        }
        const auto existing = std::find_if(bundle.tze_runs.begin(), bundle.tze_runs.end(), [&run](const TzeRunRecord& candidate) {
            return candidate.id == run->id;
        });
        if (existing == bundle.tze_runs.end()) {
            bundle.tze_runs.push_back(*run);
        }
    };
    add_run_id(case_record->created_by_run_id);
    add_run_id(case_record->analyzed_by_run_id);
    add_run_id(case_record->decided_by_run_id);
    add_run_id(case_record->reported_by_run_id);

    const std::filesystem::path output_path =
        export_path(snapshot, "cases", case_record->id + ".omnix-case", explicit_output);
    if (!output_path.parent_path().empty()) {
        std::filesystem::create_directories(output_path.parent_path());
    }

    std::ostringstream out;
    out << "{\n"
        << "  \"format\": \"omnix-case-bundle-v2\",\n"
        << "  \"exported_at\": \"" << escape_json(now_timestamp()) << "\",\n"
        << "  \"cases\": " << render_cases_json(bundle) << ",\n"
        << "  \"tze_runs\": " << render_tze_runs_json(bundle) << "\n"
        << "}\n";
    write_text(output_path, out.str());
    return output_path.string();
}

std::string MemoryStore::import_case_bundle(MemorySnapshot& snapshot,
                                            const std::filesystem::path& bundle_path) const {
    const std::string text = read_text(bundle_path);
    if (text.empty()) {
        return {};
    }

    for (const std::string& object_text : extract_object_entries(text, "observations")) {
        remember_observation(snapshot, parse_observation_entry(object_text));
    }
    for (const std::string& object_text : extract_object_entries(text, "normalized_objects")) {
        remember_normalized_object(snapshot, parse_normalized_object_entry(object_text));
    }
    for (const std::string& object_text : extract_object_entries(text, "evidence_links")) {
        remember_evidence_link(snapshot, parse_evidence_link_entry(object_text));
    }
    for (const std::string& object_text : extract_object_entries(text, "analyst_comments")) {
        remember_analyst_comment(snapshot, parse_analyst_comment_entry(object_text));
    }
    for (const std::string& object_text : extract_object_entries(text, "decision_candidates")) {
        remember_decision_candidate(snapshot, parse_decision_candidate_entry(object_text));
    }
    for (const std::string& object_text : extract_object_entries(text, "case_records")) {
        remember_case_record(snapshot, parse_case_record_entry(object_text));
    }
    for (const std::string& object_text : extract_object_entries(text, "case_links")) {
        remember_case_link(snapshot, parse_case_link_entry(object_text));
    }
    for (const std::string& object_text : extract_object_entries(text, "entries")) {
        remember_tze_run(snapshot, parse_tze_run_entry(object_text));
    }

    std::ostringstream summary;
    summary << "Imported case bundle `" << bundle_path.string() << "` with "
            << extract_object_entries(text, "case_records").size() << " case record(s).";
    return summary.str();
}

std::string MemoryStore::write_tze_bundle(const MemorySnapshot& snapshot,
                                          std::string_view id,
                                          const std::filesystem::path& explicit_output) const {
    const TzeRunRecord* anchor = find_tze_run(snapshot, id);
    if (anchor == nullptr) {
        return {};
    }

    MemorySnapshot bundle = snapshot;
    bundle.tze_runs.clear();
    bundle.case_records.clear();
    bundle.observations.clear();
    bundle.normalized_objects.clear();
    bundle.evidence_links.clear();
    bundle.analyst_comments.clear();
    bundle.decision_candidates.clear();
    bundle.case_links.clear();

    for (const TzeRunRecord* entry : chain_runs_for(snapshot, *anchor)) {
        bundle.tze_runs.push_back(*entry);
    }
    if (!anchor->linked_case_id.empty()) {
        const CaseRecord* case_record = find_case_record(snapshot, anchor->linked_case_id);
        if (case_record != nullptr) {
            bundle.case_records.push_back(*case_record);
            for (const ObservationRecord* entry : observations_for_case(snapshot, case_record->id)) {
                bundle.observations.push_back(*entry);
            }
            for (const NormalizedObject* entry : objects_for_case(snapshot, case_record->id)) {
                bundle.normalized_objects.push_back(*entry);
            }
            for (const EvidenceLink* entry : links_for_case(snapshot, case_record->id)) {
                bundle.evidence_links.push_back(*entry);
            }
            for (const AnalystComment* entry : comments_for_case(snapshot, case_record->id)) {
                bundle.analyst_comments.push_back(*entry);
            }
            for (const DecisionCandidate* entry : decisions_for_case(snapshot, case_record->id)) {
                bundle.decision_candidates.push_back(*entry);
            }
            for (const CaseLink& entry : snapshot.case_links) {
                if (entry.left_case_id == case_record->id || entry.right_case_id == case_record->id) {
                    bundle.case_links.push_back(entry);
                }
            }
        }
    }

    const std::filesystem::path output_path =
        export_path(snapshot, "tze", anchor->id + ".omnix-tze", explicit_output);
    if (!output_path.parent_path().empty()) {
        std::filesystem::create_directories(output_path.parent_path());
    }

    std::ostringstream out;
    out << "{\n"
        << "  \"format\": \"omnix-tze-bundle-v2\",\n"
        << "  \"exported_at\": \"" << escape_json(now_timestamp()) << "\",\n"
        << "  \"tze_runs\": " << render_tze_runs_json(bundle) << ",\n"
        << "  \"cases\": " << render_cases_json(bundle) << "\n"
        << "}\n";
    write_text(output_path, out.str());
    return output_path.string();
}

std::string MemoryStore::import_tze_bundle(MemorySnapshot& snapshot,
                                           const std::filesystem::path& bundle_path) const {
    const std::string text = read_text(bundle_path);
    if (text.empty()) {
        return {};
    }

    for (const std::string& object_text : extract_object_entries(text, "entries")) {
        remember_tze_run(snapshot, parse_tze_run_entry(object_text));
    }
    for (const std::string& object_text : extract_object_entries(text, "case_records")) {
        remember_case_record(snapshot, parse_case_record_entry(object_text));
    }
    for (const std::string& object_text : extract_object_entries(text, "observations")) {
        remember_observation(snapshot, parse_observation_entry(object_text));
    }
    for (const std::string& object_text : extract_object_entries(text, "normalized_objects")) {
        remember_normalized_object(snapshot, parse_normalized_object_entry(object_text));
    }
    for (const std::string& object_text : extract_object_entries(text, "evidence_links")) {
        remember_evidence_link(snapshot, parse_evidence_link_entry(object_text));
    }
    for (const std::string& object_text : extract_object_entries(text, "analyst_comments")) {
        remember_analyst_comment(snapshot, parse_analyst_comment_entry(object_text));
    }
    for (const std::string& object_text : extract_object_entries(text, "decision_candidates")) {
        remember_decision_candidate(snapshot, parse_decision_candidate_entry(object_text));
    }
    for (const std::string& object_text : extract_object_entries(text, "case_links")) {
        remember_case_link(snapshot, parse_case_link_entry(object_text));
    }

    std::ostringstream summary;
    summary << "Imported TZE bundle `" << bundle_path.string() << "` with "
            << extract_object_entries(text, "entries").size() << " run(s).";
    return summary.str();
}

std::string MemoryStore::render_incident_list(const MemorySnapshot& snapshot) const {
    const std::vector<CaseCluster> clusters = derive_incident_clusters(snapshot);
    std::ostringstream out;
    out << "Incidents:\n";
    if (clusters.empty()) {
        out << " - No incident or campaign clusters are currently derivable from the case graph.\n";
        return out.str();
    }
    for (const CaseCluster& cluster : clusters) {
        out << " - " << cluster.id << " | " << cluster.cluster_type
            << " | score=" << cluster.correlation_score
            << " | cases=" << cluster.case_count
            << " | " << cluster.title << "\n";
    }
    return out.str();
}

std::string MemoryStore::render_incident(const MemorySnapshot& snapshot, std::string_view incident_reference) const {
    const std::vector<CaseCluster> clusters = derive_incident_clusters(snapshot);
    const CaseCluster* cluster = find_incident_cluster(clusters, incident_reference);
    if (cluster == nullptr) {
        return "No incident matched `" + std::string(incident_reference) + "`.";
    }

    std::ostringstream out;
    out << "Incident\n";
    out << " - Id: " << cluster->id << "\n";
    out << " - Type: " << cluster->cluster_type << "\n";
    out << " - Title: " << cluster->title << "\n";
    out << " - Correlation score: " << cluster->correlation_score << "\n";
    out << " - Likelihood: " << cluster->likelihood << "\n";
    out << " - Summary: " << cluster->summary << "\n";
    out << "Cases:\n";
    for (const std::string& case_id : cluster->case_ids) {
        const CaseRecord* case_record = find_case_record(snapshot, case_id);
        out << " - " << case_id;
        if (case_record != nullptr) {
            out << " | " << case_record->status << " | " << case_record->title;
        }
        out << "\n";
    }
    if (!cluster->shared_indicators.empty()) {
        out << "Indicators:\n";
        for (const std::string& indicator : cluster->shared_indicators) {
            out << " - " << indicator << "\n";
        }
    }
    return out.str();
}

std::string MemoryStore::write_incident_report(const MemorySnapshot& snapshot,
                                               std::string_view incident_reference,
                                               const std::filesystem::path& explicit_output) const {
    const std::vector<CaseCluster> clusters = derive_incident_clusters(snapshot);
    const CaseCluster* cluster = find_incident_cluster(clusters, incident_reference);
    if (cluster == nullptr) {
        return {};
    }

    const std::filesystem::path output_path = incident_report_path(snapshot, cluster->id, explicit_output);
    if (!output_path.parent_path().empty()) {
        std::filesystem::create_directories(output_path.parent_path());
    }

    std::ostringstream report;
    report << "# OmniX Incident Report\n\n";
    report << "- Incident id: " << cluster->id << "\n";
    report << "- Type: " << cluster->cluster_type << "\n";
    report << "- Correlation score: " << cluster->correlation_score << "\n";
    report << "- Likelihood: " << cluster->likelihood << "\n";
    report << "- Title: " << cluster->title << "\n\n";

    report << "## Executive Summary\n\n";
    report << cluster->summary << "\n\n";

    report << "## Linked Cases\n\n";
    for (const std::string& case_id : cluster->case_ids) {
        const CaseRecord* case_record = find_case_record(snapshot, case_id);
        report << "- " << case_id;
        if (case_record != nullptr) {
            report << " | " << case_record->status << " | " << case_record->title
                   << " | source=" << case_record->primary_source << "\n";
            if (!case_record->latest_summary.empty()) {
                report << "  - " << case_record->latest_summary << "\n";
            }
        } else {
            report << "\n";
        }
    }

    report << "\n## Strongest TZE Runs\n\n";
    std::set<std::string> emitted_runs;
    for (const std::string& case_id : cluster->case_ids) {
        for (const TzeRunRecord* run : runs_for_case(snapshot, case_id)) {
            if (!emitted_runs.insert(run->id).second) {
                continue;
            }
            report << "- " << run->id << " | " << run->intent << " => " << run->status;
            if (!run->produced_artifact.empty()) {
                report << " | artifact=" << run->produced_artifact;
            }
            report << "\n";
        }
    }
    if (emitted_runs.empty()) {
        report << "- No linked TZE runs were found for this incident.\n";
    }

    report << "\n## Recommended Next Actions\n\n";
    std::set<std::string> emitted_decisions;
    for (const std::string& case_id : cluster->case_ids) {
        for (const DecisionCandidate* decision : decisions_for_case(snapshot, case_id)) {
            if (!emitted_decisions.insert(decision->id).second) {
                continue;
            }
            report << "- " << decision->title << " | score=" << decision->score
                   << " | likelihood=" << decision->probability_likelihood
                   << " | confidence=" << decision->confidence << "\n";
            if (!decision->recommended_command.empty()) {
                report << "  - Command: " << decision->recommended_command << "\n";
            }
            if (!decision->operator_feedback.empty()) {
                report << "  - Feedback: " << decision->operator_feedback << "\n";
            }
            if (!decision->outcome_status.empty()) {
                report << "  - Outcome: " << decision->outcome_status << "\n";
            }
        }
    }
    if (emitted_decisions.empty()) {
        report << "- No scored decision candidates are currently attached to the linked cases.\n";
    }

    report << "\n## Provenance Trail\n\n";
    for (const std::string& link_id : cluster->link_ids) {
        const auto link_it = std::find_if(snapshot.case_links.begin(), snapshot.case_links.end(), [&link_id](const CaseLink& entry) {
            return entry.id == link_id;
        });
        if (link_it == snapshot.case_links.end()) {
            continue;
        }
        report << "- [" << link_it->strength << "] " << link_it->left_case_id << " <-> " << link_it->right_case_id
               << " | " << link_it->link_type << "=" << link_it->link_value << "\n";
        if (!link_it->rationale.empty()) {
            report << "  - " << link_it->rationale << "\n";
        }
    }

    write_text(output_path, report.str());
    return output_path.string();
}

void MemoryStore::record_interaction(MemorySnapshot& snapshot, const ProcessingReport& report) const {
    MemoryHistoryEntry entry;
    entry.timestamp = now_timestamp();
    entry.prompt = report.raw_prompt.empty() ? report.resolved_project : report.raw_prompt;
    entry.intent = report.resolved_intent;
    entry.project = report.resolved_project;
    entry.status = report.answer_status;
    entry.summary = report.answer_explanation;
    snapshot.history.push_back(entry);

    std::ofstream output(snapshot.paths.history_path, std::ios::app);
    output << render_history_entry(entry) << '\n';
}

void MemoryStore::remember_tze_run(MemorySnapshot& snapshot, const TzeRunRecord& record) const {
    if (record.id.empty()) {
        return;
    }

    if (record.security_audit.has_value()) {
        remember_security_audit(snapshot, *record.security_audit);
    }
    if (record.language_resolution.has_value()) {
        remember_language_context(snapshot, *record.language_resolution);
    }
    if (record.uac_state.has_value()) {
        remember_uac_state(snapshot, *record.uac_state);
    }

    const auto existing = std::find_if(snapshot.tze_runs.begin(), snapshot.tze_runs.end(), [&record](const TzeRunRecord& entry) {
        return entry.id == record.id;
    });
    if (existing != snapshot.tze_runs.end()) {
        *existing = record;
        return;
    }

    snapshot.tze_runs.push_back(record);
}

void MemoryStore::remember_definition(MemorySnapshot& snapshot, const DefinitionAnswer& answer) const {
    if (!answer.found || answer.query.empty()) {
        return;
    }

    const auto existing = std::find_if(snapshot.definitions.begin(), snapshot.definitions.end(), [&answer](const StoredDefinition& entry) {
        return entry.term == answer.query;
    });
    if (existing != snapshot.definitions.end()) {
        existing->summary = answer.summary;
        existing->mapped_cpp_target = answer.mapped_cpp_target;
        existing->semantic_family = answer.semantic_family;
        existing->source = answer.sources.empty() ? "memory" : answer.sources.front();
        return;
    }

    snapshot.definitions.push_back({
        answer.query,
        answer.summary,
        answer.mapped_cpp_target,
        answer.semantic_family,
        answer.sources.empty() ? "memory" : answer.sources.front(),
    });
}

void MemoryStore::remember_security_audit(MemorySnapshot& snapshot, const SecurityAudit& record) const {
    if (record.id.empty()) {
        return;
    }

    SecurityAudit stored = record;
    if (stored.persisted_at.empty()) {
        stored.persisted_at = now_timestamp();
    }

    const auto existing = std::find_if(snapshot.security_audits.begin(),
                                       snapshot.security_audits.end(),
                                       [&stored](const SecurityAudit& entry) {
                                           return entry.id == stored.id;
                                       });
    if (existing != snapshot.security_audits.end()) {
        *existing = stored;
        return;
    }

    snapshot.security_audits.push_back(std::move(stored));
}

void MemoryStore::remember_language_context(MemorySnapshot& snapshot, const LanguageResolutionRecord& record) const {
    if (record.id.empty()) {
        return;
    }

    LanguageResolutionRecord stored = record;
    if (stored.persisted_at.empty()) {
        stored.persisted_at = now_timestamp();
    }

    const auto existing = std::find_if(snapshot.language_contexts.begin(),
                                       snapshot.language_contexts.end(),
                                       [&stored](const LanguageResolutionRecord& entry) {
                                           return entry.id == stored.id;
                                       });
    if (existing != snapshot.language_contexts.end()) {
        *existing = stored;
        return;
    }

    snapshot.language_contexts.push_back(std::move(stored));
}

void MemoryStore::remember_uac_state(MemorySnapshot& snapshot, const UacStateRecord& record) const {
    if (record.id.empty()) {
        return;
    }

    UacStateRecord stored = record;
    if (stored.persisted_at.empty()) {
        stored.persisted_at = now_timestamp();
    }

    const auto existing = std::find_if(snapshot.uac_states.begin(),
                                       snapshot.uac_states.end(),
                                       [&stored](const UacStateRecord& entry) {
                                           return entry.id == stored.id;
                                       });
    if (existing != snapshot.uac_states.end()) {
        *existing = stored;
        return;
    }

    snapshot.uac_states.push_back(std::move(stored));
}

void MemoryStore::remember_project(MemorySnapshot& snapshot,
                                   std::string_view canonical_name,
                                   std::string_view resolved_source_path,
                                   std::string_view build_system,
                                   std::string_view status,
                                   std::string_view upstream_url) const {
    if (canonical_name.empty()) {
        return;
    }

    const auto existing = std::find_if(snapshot.projects.begin(), snapshot.projects.end(), [canonical_name](const ProjectRecord& entry) {
        return entry.canonical_name == canonical_name;
    });
    if (existing != snapshot.projects.end()) {
        existing->resolved_source_path = std::string(resolved_source_path);
        existing->build_system = std::string(build_system);
        existing->status = std::string(status);
        existing->upstream_url = std::string(upstream_url);
        return;
    }

    snapshot.projects.push_back({
        std::string(canonical_name),
        std::string(resolved_source_path),
        std::string(build_system),
        std::string(status),
        std::string(upstream_url),
    });
}

void MemoryStore::remember_recipe_result(MemorySnapshot& snapshot, const LearnedRecipeRecord& record) const {
    if (record.canonical_name.empty() || record.recipe_id.empty() || record.environment_key.empty()) {
        return;
    }

    const auto existing = std::find_if(snapshot.learned_recipes.begin(),
                                       snapshot.learned_recipes.end(),
                                       [&record](const LearnedRecipeRecord& entry) {
                                           return entry.canonical_name == record.canonical_name &&
                                               entry.recipe_id == record.recipe_id &&
                                               entry.environment_key == record.environment_key;
                                       });
    if (existing != snapshot.learned_recipes.end()) {
        *existing = record;
        return;
    }
    snapshot.learned_recipes.push_back(record);
}

void MemoryStore::remember_native_tool(MemorySnapshot& snapshot, const NativeToolRecord& record) const {
    if (record.logical_name.empty() || record.environment_signature.empty()) {
        return;
    }

    const auto existing = std::find_if(snapshot.native_tools.begin(),
                                       snapshot.native_tools.end(),
                                       [&record](const NativeToolRecord& entry) {
                                           return entry.logical_name == record.logical_name &&
                                               entry.provider_type == record.provider_type &&
                                               entry.executable_path == record.executable_path &&
                                               entry.applet_name == record.applet_name &&
                                               entry.environment_signature == record.environment_signature;
                                       });
    if (existing != snapshot.native_tools.end()) {
        *existing = record;
        return;
    }
    snapshot.native_tools.push_back(record);
}

void MemoryStore::remember_observation(MemorySnapshot& snapshot, const ObservationRecord& record) const {
    if (record.id.empty()) {
        return;
    }
    const auto existing = std::find_if(snapshot.observations.begin(),
                                       snapshot.observations.end(),
                                       [&record](const ObservationRecord& entry) { return entry.id == record.id; });
    if (existing != snapshot.observations.end()) {
        *existing = record;
        return;
    }
    snapshot.observations.push_back(record);
}

void MemoryStore::remember_normalized_object(MemorySnapshot& snapshot, const NormalizedObject& record) const {
    if (record.id.empty()) {
        return;
    }
    const auto existing = std::find_if(snapshot.normalized_objects.begin(),
                                       snapshot.normalized_objects.end(),
                                       [&record](const NormalizedObject& entry) { return entry.id == record.id; });
    if (existing != snapshot.normalized_objects.end()) {
        *existing = record;
        return;
    }
    snapshot.normalized_objects.push_back(record);
}

void MemoryStore::remember_evidence_link(MemorySnapshot& snapshot, const EvidenceLink& record) const {
    if (record.id.empty()) {
        return;
    }
    const auto existing = std::find_if(snapshot.evidence_links.begin(),
                                       snapshot.evidence_links.end(),
                                       [&record](const EvidenceLink& entry) { return entry.id == record.id; });
    if (existing != snapshot.evidence_links.end()) {
        *existing = record;
        return;
    }
    snapshot.evidence_links.push_back(record);
}

void MemoryStore::remember_analyst_comment(MemorySnapshot& snapshot, const AnalystComment& record) const {
    if (record.id.empty()) {
        return;
    }
    const auto existing = std::find_if(snapshot.analyst_comments.begin(),
                                       snapshot.analyst_comments.end(),
                                       [&record](const AnalystComment& entry) { return entry.id == record.id; });
    if (existing != snapshot.analyst_comments.end()) {
        *existing = record;
        return;
    }
    snapshot.analyst_comments.push_back(record);
}

void MemoryStore::remember_decision_candidate(MemorySnapshot& snapshot, const DecisionCandidate& record) const {
    if (record.id.empty()) {
        return;
    }
    const auto existing = std::find_if(snapshot.decision_candidates.begin(),
                                       snapshot.decision_candidates.end(),
                                       [&record](const DecisionCandidate& entry) { return entry.id == record.id; });
    if (existing != snapshot.decision_candidates.end()) {
        DecisionCandidate merged = record;
        if (merged.operator_feedback.empty()) {
            merged.operator_feedback = existing->operator_feedback;
        }
        if (merged.feedback_note.empty()) {
            merged.feedback_note = existing->feedback_note;
        }
        if (merged.feedback_timestamp.empty()) {
            merged.feedback_timestamp = existing->feedback_timestamp;
        }
        if (merged.outcome_status.empty()) {
            merged.outcome_status = existing->outcome_status;
        }
        if (merged.outcome_note.empty()) {
            merged.outcome_note = existing->outcome_note;
        }
        if (merged.outcome_timestamp.empty()) {
            merged.outcome_timestamp = existing->outcome_timestamp;
        }
        *existing = std::move(merged);
        return;
    }
    snapshot.decision_candidates.push_back(record);
}

void MemoryStore::remember_case_record(MemorySnapshot& snapshot, const CaseRecord& record) const {
    if (record.id.empty()) {
        return;
    }
    const auto existing = std::find_if(snapshot.case_records.begin(),
                                       snapshot.case_records.end(),
                                       [&record](const CaseRecord& entry) { return entry.id == record.id; });
    if (existing != snapshot.case_records.end()) {
        *existing = record;
        return;
    }
    snapshot.case_records.push_back(record);
}

void MemoryStore::remember_case_link(MemorySnapshot& snapshot, const CaseLink& record) const {
    if (record.id.empty()) {
        return;
    }
    const auto existing = std::find_if(snapshot.case_links.begin(),
                                       snapshot.case_links.end(),
                                       [&record](const CaseLink& entry) { return entry.id == record.id; });
    if (existing != snapshot.case_links.end()) {
        *existing = record;
        return;
    }
    snapshot.case_links.push_back(record);
}

std::string MemoryStore::render_view(const MemorySnapshot& snapshot, std::string_view view) const {
    std::ostringstream out;
    if (view == "prefs" || view == "preferences") {
        out << "Source Preferences:\n";
        for (const std::string& source : snapshot.source_preference_order) {
            out << " - " << source << "\n";
        }

        if (!snapshot.learned_recipes.empty()) {
            std::map<std::string, LearnedRecipeRecord> preferred;
            for (const LearnedRecipeRecord& entry : snapshot.learned_recipes) {
                const std::string key = entry.canonical_name + "|" + entry.environment_key;
                const auto existing = preferred.find(key);
                if (existing == preferred.end() || entry.confidence_score > existing->second.confidence_score) {
                    preferred[key] = entry;
                }
            }

            out << "Preferred Recipes:\n";
            for (const auto& [key, entry] : preferred) {
                (void)key;
                out << " - " << entry.canonical_name << " :: " << entry.recipe_id
                    << " [" << entry.confidence_score << "]"
                    << " (" << entry.environment_key << ")"
                    << " status=" << entry.last_status;
                if (!entry.last_artifact.empty()) {
                    out << " artifact=" << entry.last_artifact;
                }
                if (!entry.last_install_prefix.empty()) {
                    out << " install=" << entry.last_install_prefix;
                }
                out << "\n";
            }
        }

        if (!snapshot.native_tools.empty()) {
            std::map<std::string, NativeToolRecord> preferred_native;
            for (const NativeToolRecord& entry : snapshot.native_tools) {
                const auto existing = preferred_native.find(entry.logical_name);
                if (existing == preferred_native.end() || entry.last_verified > existing->second.last_verified) {
                    preferred_native[entry.logical_name] = entry;
                }
            }

            out << "Native Tool Providers:\n";
            for (const auto& [tool_name, entry] : preferred_native) {
                out << " - " << tool_name << " :: " << entry.provider_type
                    << " path=" << entry.executable_path
                    << " origin=" << entry.discovery_origin;
                if (!entry.applet_name.empty()) {
                    out << " applet=" << entry.applet_name;
                }
                if (!entry.version_fingerprint.empty()) {
                    out << " version=" << entry.version_fingerprint;
                }
                out << "\n";
            }
        }

        if (!snapshot.security_audits.empty()) {
            out << "Security Audits:\n";
            const std::size_t start = snapshot.security_audits.size() > 6 ? snapshot.security_audits.size() - 6 : 0;
            for (std::size_t index = start; index < snapshot.security_audits.size(); ++index) {
                const SecurityAudit& entry = snapshot.security_audits[index];
                out << " - " << entry.status << " :: " << entry.behavior_mode
                    << " threat=" << entry.threat_label
                    << " bracket=" << entry.threat_bracket;
                if (!entry.query.empty()) {
                    out << " query=" << entry.query;
                }
                out << "\n";
            }
        }

        if (!snapshot.language_contexts.empty()) {
            out << "Language Contexts:\n";
            const std::size_t start = snapshot.language_contexts.size() > 6 ? snapshot.language_contexts.size() - 6 : 0;
            for (std::size_t index = start; index < snapshot.language_contexts.size(); ++index) {
                const LanguageResolutionRecord& entry = snapshot.language_contexts[index];
                out << " - " << entry.combined_context
                    << " [" << std::fixed << std::setprecision(2) << entry.confidence << "]"
                    << " passes=" << entry.passes;
                if (!entry.query.empty()) {
                    out << " query=" << entry.query;
                }
                if (entry.manual_confirmation_required) {
                    out << " manual=required";
                } else if (entry.manual_confirmation_used) {
                    out << " manual=" << entry.manual_confirmation_response;
                }
                out << "\n";
            }
        }

        if (!snapshot.uac_states.empty()) {
            out << "uAC States:\n";
            const std::size_t start = snapshot.uac_states.size() > 6 ? snapshot.uac_states.size() - 6 : 0;
            for (std::size_t index = start; index < snapshot.uac_states.size(); ++index) {
                const UacStateRecord& entry = snapshot.uac_states[index];
                out << " - " << entry.epoch_marker
                    << " machine=" << entry.machine_identifier;
                if (!entry.query.empty()) {
                    out << " query=" << entry.query;
                }
                out << " traits=" << entry.indexed_traits.size() << "\n";
            }
        }
        return out.str();
    }

    if (view == "definitions") {
        out << "Stored Definitions:\n";
        for (const StoredDefinition& entry : snapshot.definitions) {
            out << " - " << entry.term << " => " << entry.summary;
            if (!entry.mapped_cpp_target.empty()) {
                out << " [" << entry.mapped_cpp_target << "]";
            }
            out << "\n";
        }
        return out.str();
    }

    if (view == "runs" || view == "tze") {
        out << "TZE Runs:\n";
        const std::size_t start = snapshot.tze_runs.size() > 12 ? snapshot.tze_runs.size() - 12 : 0;
        for (std::size_t index = start; index < snapshot.tze_runs.size(); ++index) {
            const TzeRunRecord& entry = snapshot.tze_runs[index];
            out << " - " << entry.id << " | " << entry.intent;
            if (!entry.status.empty()) {
                out << " => " << entry.status;
            }
            if (!entry.target.empty()) {
                out << " | target=" << entry.target;
            }
            if (!entry.reasoning_provider.empty()) {
                out << " | provider=" << entry.reasoning_provider;
            }
            if (!entry.provider_probe_status.empty()) {
                out << " | probe=" << entry.provider_probe_status;
            }
            if (!entry.assist_status.empty()) {
                out << " | assist=" << entry.assist_status;
            }
            if (entry.command_assist_plan.has_value() && !entry.command_assist_plan->canonical_command.empty()) {
                out << " | assist-command=" << entry.command_assist_plan->canonical_command;
            }
            if (entry.tool_assist_plan.has_value() && !entry.tool_assist_plan->tool_name.empty()) {
                out << " | assist-tool=" << entry.tool_assist_plan->tool_name;
            }
            if (entry.build_assist_plan.has_value() && !entry.build_assist_plan->selected_recipe_id.empty()) {
                out << " | assist-recipe=" << entry.build_assist_plan->selected_recipe_id;
            }
            if (!entry.feedback_status.empty()) {
                out << " | feedback=" << entry.feedback_status;
            }
            if (entry.query_session.has_value() && !entry.query_session->final_results.empty()) {
                out << " | query=" << join_stage_fields(entry.query_session->final_results);
            }
            if (entry.security_audit.has_value() && !entry.security_audit->behavior_mode.empty()) {
                out << " | security=" << entry.security_audit->behavior_mode;
            }
            if (entry.language_resolution.has_value() && !entry.language_resolution->combined_context.empty()) {
                out << " | lang=" << entry.language_resolution->combined_context;
            }
            if (entry.uac_state.has_value() && !entry.uac_state->epoch_marker.empty()) {
                out << " | uac=" << entry.uac_state->epoch_marker;
            }
            out << "\n";
            for (const TzeStageRecord& stage : entry.stages) {
                out << "   - " << stage.stage_id << " [" << stage.status << "] " << stage.module;
                if (!stage.detail.empty()) {
                    out << " | " << stage.detail;
                }
                out << "\n";
            }
        }
        return out.str();
    }

    if (view == "language" || view == "languages") {
        out << "Language Contexts:\n";
        if (snapshot.language_contexts.empty()) {
            out << " - No stored language contexts.\n";
            return out.str();
        }
        for (const LanguageResolutionRecord& entry : snapshot.language_contexts) {
            out << " - " << entry.id << " | " << entry.combined_context
                << " [" << std::fixed << std::setprecision(2) << entry.confidence << "]";
            if (!entry.query.empty()) {
                out << " | query=" << entry.query;
            }
            out << "\n";
            if (!entry.reasoning_trace.empty()) {
                out << "   - trace: " << join_stage_fields(entry.reasoning_trace) << "\n";
            }
        }
        return out.str();
    }

    if (view == "security" || view == "audits") {
        out << "Security Audits:\n";
        if (snapshot.security_audits.empty()) {
            out << " - No stored security audits.\n";
            return out.str();
        }
        for (const SecurityAudit& entry : snapshot.security_audits) {
            out << " - " << entry.id << " | " << entry.status
                << " | mode=" << entry.behavior_mode
                << " | threat=" << entry.threat_label
                << " | bracket=" << entry.threat_bracket;
            if (!entry.query.empty()) {
                out << " | query=" << entry.query;
            }
            out << "\n";
            if (!entry.blocked_paths.empty()) {
                out << "   - blocked: " << join_stage_fields(entry.blocked_paths) << "\n";
            }
            if (!entry.simulated_actions.empty()) {
                out << "   - simulated: " << join_stage_fields(entry.simulated_actions) << "\n";
            }
        }
        return out.str();
    }

    if (view == "uac" || view == "preprocessor") {
        out << "uAC States:\n";
        if (snapshot.uac_states.empty()) {
            out << " - No stored uAC states.\n";
            return out.str();
        }
        for (const UacStateRecord& entry : snapshot.uac_states) {
            out << " - " << entry.id << " | epoch=" << entry.epoch_marker
                << " | machine=" << entry.machine_identifier;
            if (!entry.query.empty()) {
                out << " | query=" << entry.query;
            }
            out << "\n";
            if (!entry.recovery_hints.empty()) {
                out << "   - recovery: " << join_stage_fields(entry.recovery_hints) << "\n";
            }
        }
        return out.str();
    }

    if (view == "cases") {
        out << "Cases:\n";
        for (const CaseRecord& entry : snapshot.case_records) {
            const std::size_t link_count = static_cast<std::size_t>(std::count_if(
                snapshot.case_links.begin(),
                snapshot.case_links.end(),
                [&entry](const CaseLink& link) {
                    return link.left_case_id == entry.id || link.right_case_id == entry.id;
                }));
            out << " - " << entry.id << " | " << entry.status << " | " << entry.title;
            if (!entry.primary_source.empty()) {
                out << " | source=" << entry.primary_source;
            }
            out << " | links=" << link_count;
            if (!entry.created_by_run_id.empty()) {
                out << " | created_by=" << entry.created_by_run_id;
            }
            if (!entry.analyzed_by_run_id.empty()) {
                out << " | analyzed_by=" << entry.analyzed_by_run_id;
            }
            if (!entry.decided_by_run_id.empty()) {
                out << " | decided_by=" << entry.decided_by_run_id;
            }
            if (!entry.reported_by_run_id.empty()) {
                out << " | reported_by=" << entry.reported_by_run_id;
            }
            if (!entry.latest_summary.empty()) {
                out << " | " << entry.latest_summary;
            }
            out << "\n";
        }
        return out.str();
    }

    if (view == "incidents") {
        return render_incident_list(snapshot);
    }

    out << "History:\n";
    const std::size_t start = snapshot.history.size() > 12 ? snapshot.history.size() - 12 : 0;
    for (std::size_t index = start; index < snapshot.history.size(); ++index) {
        const MemoryHistoryEntry& entry = snapshot.history[index];
        out << " - " << entry.timestamp << " | " << entry.intent << " | " << entry.prompt;
        if (!entry.status.empty()) {
            out << " => " << entry.status;
        }
        out << "\n";
    }
    return out.str();
}

}  // namespace tze
