#include "tze/memory_store.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
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

std::string lowercase(std::string_view value) {
    std::string lowered;
    lowered.reserve(value.size());
    for (char c : value) {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return lowered;
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

std::string timestamp_after_hours(int hours) {
    using clock = std::chrono::system_clock;
    const std::time_t raw = clock::to_time_t(clock::now() + std::chrono::hours(hours));
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

bool timestamp_expired(std::string_view expires_at) {
    const std::string expiry = trim(expires_at);
    return !expiry.empty() && expiry <= now_timestamp();
}

bool is_expired(const StoredDefinition& entry) {
    return entry.scope == "temporary" && timestamp_expired(entry.expires_at);
}

bool is_expired(const MemoryHistoryEntry& entry) {
    return entry.scope == "temporary" && timestamp_expired(entry.expires_at);
}

std::string make_scoped_id(std::string_view prefix, std::string_view seed) {
    return std::string(prefix) + "-" +
        std::to_string(std::hash<std::string>{}(std::string(seed)));
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

std::vector<double> extract_json_double_array(std::string_view text, std::string_view key) {
    std::vector<double> values;
    const std::size_t start = find_json_value_start(text, key, '[');
    if (start == std::string::npos) {
        return values;
    }

    std::size_t cursor = start + 1;
    while (cursor < text.size() && text[cursor] != ']') {
        while (cursor < text.size() &&
               (text[cursor] == ' ' || text[cursor] == ',' || text[cursor] == '\n')) {
            ++cursor;
        }
        std::size_t end = cursor;
        while (end < text.size() &&
               (std::isdigit(static_cast<unsigned char>(text[end])) || text[end] == '-' || text[end] == '+' ||
                text[end] == '.' || text[end] == 'e' || text[end] == 'E')) {
            ++end;
        }
        if (end == cursor) {
            break;
        }
        values.push_back(std::stod(std::string(text.substr(cursor, end - cursor))));
        cursor = end;
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
std::string render_double_array(const std::vector<double>& values);

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
    entry.scope = extract_json_string(line, "scope");
    entry.created_at = extract_json_string(line, "created_at");
    entry.expires_at = extract_json_string(line, "expires_at");
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

MathAttribution parse_math_attribution_entry(std::string_view object_text) {
    MathAttribution entry;
    entry.name = extract_json_string(object_text, "name");
    entry.raw_value = extract_json_double(object_text, "raw_value", 0.0);
    entry.weight = extract_json_double(object_text, "weight", 0.0);
    entry.contribution = extract_json_double(object_text, "contribution", 0.0);
    entry.source = extract_json_string(object_text, "source");
    entry.rationale = extract_json_string(object_text, "rationale");
    return entry;
}

ReasoningSlotRecord parse_reasoning_slot_entry(std::string_view object_text) {
    ReasoningSlotRecord entry;
    entry.slot_id = extract_json_string(object_text, "slot_id");
    entry.label = extract_json_string(object_text, "label");
    entry.facts = extract_json_string_array(object_text, "facts");
    entry.evidence_refs = extract_json_string_array(object_text, "evidence_refs");
    entry.confidence = extract_json_double(object_text, "confidence", 0.0);
    return entry;
}

RecursivePathRecord parse_recursive_path_entry(std::string_view object_text) {
    RecursivePathRecord entry;
    entry.path_type = extract_json_string(object_text, "path_type");
    entry.ordered_steps = extract_json_string_array(object_text, "ordered_steps");
    entry.assumptions = extract_json_string_array(object_text, "assumptions");
    entry.evidence_refs = extract_json_string_array(object_text, "evidence_refs");
    entry.score = extract_json_double(object_text, "score", 0.0);
    return entry;
}

RecursiveDiffReport parse_recursive_diff_report_entry(std::string_view object_text) {
    RecursiveDiffReport entry;
    entry.status = extract_json_string(object_text, "status");
    entry.route_learning_status = extract_json_string(object_text, "route_learning_status");
    entry.source_run_id = extract_json_string(object_text, "source_run_id");
    entry.current_state = extract_json_string(object_text, "current_state");
    entry.likely_goal = extract_json_string(object_text, "likely_goal");
    entry.logs_and_reasoning = extract_json_string(object_text, "logs_and_reasoning");
    entry.successful_path_pattern = extract_json_string(object_text, "successful_path_pattern");
    entry.difference_found = extract_json_string(object_text, "difference_found");
    entry.best_estimate_answer = extract_json_string(object_text, "best_estimate_answer");
    entry.why_this_matters = extract_json_string(object_text, "why_this_matters");
    entry.next_action = extract_json_string(object_text, "next_action");
    entry.diff_category = extract_json_string(object_text, "diff_category");
    entry.confidence = extract_json_double(object_text, "confidence", 0.0);
    for (const std::string& slot_text : extract_object_entries(object_text, "slots")) {
        entry.slots.push_back(parse_reasoning_slot_entry(slot_text));
    }
    const std::string success_text = extract_json_object(object_text, "success_path");
    if (!success_text.empty()) {
        entry.success_path = parse_recursive_path_entry(success_text);
    }
    const std::string failure_text = extract_json_object(object_text, "failure_path");
    if (!failure_text.empty()) {
        entry.failure_path = parse_recursive_path_entry(failure_text);
    }
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

AssistPlanMetadata parse_assist_plan_metadata_entry(std::string_view object_text) {
    AssistPlanMetadata entry;
    entry.provider = extract_json_string(object_text, "provider");
    entry.model = extract_json_string(object_text, "model");
    entry.status = extract_json_string(object_text, "status");
    entry.confidence = extract_json_double(object_text, "confidence", 0.0);
    entry.rationale = extract_json_string(object_text, "rationale");
    entry.warnings = extract_json_string_array(object_text, "warnings");
    entry.validation_notes = extract_json_string_array(object_text, "validation_notes");
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
    const std::string metadata_text = extract_json_object(object_text, "metadata");
    if (!metadata_text.empty()) {
        entry.metadata = parse_assist_plan_metadata_entry(metadata_text);
    }
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
    const std::string metadata_text = extract_json_object(object_text, "metadata");
    if (!metadata_text.empty()) {
        entry.metadata = parse_assist_plan_metadata_entry(metadata_text);
    }
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
    const std::string metadata_text = extract_json_object(object_text, "metadata");
    if (!metadata_text.empty()) {
        entry.metadata = parse_assist_plan_metadata_entry(metadata_text);
    }
    return entry;
}

NextStepAssistPlan parse_next_step_assist_plan_entry(std::string_view object_text) {
    NextStepAssistPlan entry;
    entry.task_id = extract_json_string(object_text, "task_id");
    entry.provider_id = extract_json_string(object_text, "provider_id");
    entry.model = extract_json_string(object_text, "model");
    entry.status = extract_json_string(object_text, "status");
    entry.suggested_next_step = extract_json_string(object_text, "suggested_next_step");
    entry.safer_alternative = extract_json_string(object_text, "safer_alternative");
    entry.rationale = extract_json_string(object_text, "rationale");
    entry.confidence = extract_json_double(object_text, "confidence", 0.0);
    entry.warnings = extract_json_string_array(object_text, "warnings");
    entry.validation_notes = extract_json_string_array(object_text, "validation_notes");
    entry.validated = extract_json_bool(object_text, "validated", false);
    const std::string metadata_text = extract_json_object(object_text, "metadata");
    if (!metadata_text.empty()) {
        entry.metadata = parse_assist_plan_metadata_entry(metadata_text);
    }
    return entry;
}

CaseSummaryAssistPlan parse_case_summary_assist_plan_entry(std::string_view object_text) {
    CaseSummaryAssistPlan entry;
    entry.task_id = extract_json_string(object_text, "task_id");
    entry.provider_id = extract_json_string(object_text, "provider_id");
    entry.model = extract_json_string(object_text, "model");
    entry.status = extract_json_string(object_text, "status");
    entry.summary_title = extract_json_string(object_text, "summary_title");
    entry.executive_summary = extract_json_string(object_text, "executive_summary");
    entry.highlights = extract_json_string_array(object_text, "highlights");
    entry.rationale = extract_json_string(object_text, "rationale");
    entry.confidence = extract_json_double(object_text, "confidence", 0.0);
    entry.warnings = extract_json_string_array(object_text, "warnings");
    entry.validation_notes = extract_json_string_array(object_text, "validation_notes");
    entry.validated = extract_json_bool(object_text, "validated", false);
    const std::string metadata_text = extract_json_object(object_text, "metadata");
    if (!metadata_text.empty()) {
        entry.metadata = parse_assist_plan_metadata_entry(metadata_text);
    }
    return entry;
}

FreeformAssistAnswer parse_freeform_assist_answer_entry(std::string_view object_text) {
    FreeformAssistAnswer entry;
    entry.task_id = extract_json_string(object_text, "task_id");
    entry.provider_id = extract_json_string(object_text, "provider_id");
    entry.model = extract_json_string(object_text, "model");
    entry.status = extract_json_string(object_text, "status");
    entry.answer = extract_json_string(object_text, "answer");
    entry.rationale = extract_json_string(object_text, "rationale");
    entry.confidence = extract_json_double(object_text, "confidence", 0.0);
    entry.suggested_commands = extract_json_string_array(object_text, "suggested_commands");
    entry.safety_warnings = extract_json_string_array(object_text, "safety_warnings");
    entry.used_context = extract_json_string_array(object_text, "used_context");
    entry.validated = extract_json_bool(object_text, "validated", false);
    const std::string metadata_text = extract_json_object(object_text, "metadata");
    if (!metadata_text.empty()) {
        entry.metadata = parse_assist_plan_metadata_entry(metadata_text);
    }
    return entry;
}

DefinitionAnswer parse_definition_answer_entry(std::string_view object_text) {
    DefinitionAnswer entry;
    entry.query = extract_json_string(object_text, "query");
    entry.found = extract_json_bool(object_text, "found", false);
    entry.summary = extract_json_string(object_text, "summary");
    entry.mapped_cpp_target = extract_json_string(object_text, "mapped_cpp_target");
    entry.semantic_family = extract_json_string(object_text, "semantic_family");
    entry.normalized_concept = extract_json_string(object_text, "normalized_concept");
    entry.domain_hint = extract_json_string(object_text, "domain_hint");
    entry.route_class = extract_json_string(object_text, "route_class");
    entry.selected_source_type = extract_json_string(object_text, "selected_source_type");
    entry.selected_source_label = extract_json_string(object_text, "selected_source_label");
    entry.selected_authority_tier = extract_json_string(object_text, "selected_authority_tier");
    entry.confidence = extract_json_double(object_text, "confidence", 0.0);
    entry.comparison_rationale = extract_json_string(object_text, "comparison_rationale");
    entry.sources = extract_json_string_array(object_text, "sources");
    entry.suggestions = extract_json_string_array(object_text, "suggestions");
    for (const std::string& attribution_text : extract_object_entries(object_text, "math_attributions")) {
        entry.math_attributions.push_back(parse_math_attribution_entry(attribution_text));
    }
    return entry;
}

PostProcessRecord parse_postprocess_record_entry(std::string_view object_text) {
    PostProcessRecord entry;
    entry.status = extract_json_string(object_text, "status");
    entry.final_artifact_summary = extract_json_string(object_text, "final_artifact_summary");
    entry.provenance = extract_json_string(object_text, "provenance");
    entry.retention_decision = extract_json_string(object_text, "retention_decision");
    entry.dropped_transient_chain = extract_json_bool(object_text, "dropped_transient_chain", false);
    entry.retained_fields = extract_json_string_array(object_text, "retained_fields");
    entry.discarded_fields = extract_json_string_array(object_text, "discarded_fields");
    entry.persisted_at = extract_json_string(object_text, "persisted_at");
    return entry;
}

ReviewFinding parse_review_finding_entry(std::string_view object_text) {
    ReviewFinding entry;
    entry.severity = extract_json_string(object_text, "severity");
    entry.category = extract_json_string(object_text, "category");
    entry.file_path = extract_json_string(object_text, "file_path");
    entry.line = static_cast<std::size_t>(extract_json_int(object_text, "line", 0));
    entry.summary = extract_json_string(object_text, "summary");
    entry.rationale = extract_json_string(object_text, "rationale");
    return entry;
}

ReviewArtifact parse_review_artifact_entry(std::string_view object_text) {
    ReviewArtifact entry;
    entry.id = extract_json_string(object_text, "id");
    entry.target = extract_json_string(object_text, "target");
    entry.status = extract_json_string(object_text, "status");
    entry.summary = extract_json_string(object_text, "summary");
    entry.nearby_symbols = extract_json_string_array(object_text, "nearby_symbols");
    entry.suggested_tests = extract_json_string_array(object_text, "suggested_tests");
    entry.persisted_at = extract_json_string(object_text, "persisted_at");
    entry.artifact_path = extract_json_string(object_text, "artifact_path");
    for (const std::string& finding_text : extract_object_entries(object_text, "findings")) {
        entry.findings.push_back(parse_review_finding_entry(finding_text));
    }
    const std::string assist_text = extract_json_object(object_text, "assist_annotation");
    if (!assist_text.empty()) {
        entry.assist_annotation = parse_assist_annotation_entry(assist_text);
    }
    return entry;
}

PatchProposalArtifact parse_patch_proposal_artifact_entry(std::string_view object_text) {
    PatchProposalArtifact entry;
    entry.id = extract_json_string(object_text, "id");
    entry.target = extract_json_string(object_text, "target");
    entry.status = extract_json_string(object_text, "status");
    entry.summary = extract_json_string(object_text, "summary");
    entry.target_files = extract_json_string_array(object_text, "target_files");
    entry.intended_behavior_changes = extract_json_string_array(object_text, "intended_behavior_changes");
    entry.acceptance_tests = extract_json_string_array(object_text, "acceptance_tests");
    entry.unified_diff_text = extract_json_string(object_text, "unified_diff_text");
    entry.persisted_at = extract_json_string(object_text, "persisted_at");
    entry.artifact_path = extract_json_string(object_text, "artifact_path");
    const std::string assist_text = extract_json_object(object_text, "assist_annotation");
    if (!assist_text.empty()) {
        entry.assist_annotation = parse_assist_annotation_entry(assist_text);
    }
    return entry;
}

AssistOutcomeRecord parse_assist_outcome_entry(std::string_view object_text) {
    AssistOutcomeRecord entry;
    entry.id = extract_json_string(object_text, "id");
    entry.task_type = extract_json_string(object_text, "task_type");
    entry.plan_type = extract_json_string(object_text, "plan_type");
    entry.provider_id = extract_json_string(object_text, "provider_id");
    entry.model = extract_json_string(object_text, "model");
    entry.status = extract_json_string(object_text, "status");
    entry.target_label = extract_json_string(object_text, "target_label");
    entry.canonical_value = extract_json_string(object_text, "canonical_value");
    entry.rejection_reason = extract_json_string(object_text, "rejection_reason");
    entry.host_platform = extract_json_string(object_text, "host_platform");
    entry.environment_signature = extract_json_string(object_text, "environment_signature");
    entry.persisted_at = extract_json_string(object_text, "persisted_at");
    return entry;
}

AssistCorrectionRecord parse_assist_correction_entry(std::string_view object_text) {
    AssistCorrectionRecord entry;
    entry.id = extract_json_string(object_text, "id");
    entry.original_prompt = extract_json_string(object_text, "original_prompt");
    entry.corrected_value = extract_json_string(object_text, "corrected_value");
    entry.category = extract_json_string(object_text, "category");
    entry.host_platform = extract_json_string(object_text, "host_platform");
    entry.persisted_at = extract_json_string(object_text, "persisted_at");
    return entry;
}

AssistLearningRecord parse_assist_learning_entry(std::string_view object_text) {
    AssistLearningRecord entry;
    entry.id = extract_json_string(object_text, "id");
    entry.category = extract_json_string(object_text, "category");
    entry.prompt_fragment = extract_json_string(object_text, "prompt_fragment");
    entry.learned_value = extract_json_string(object_text, "learned_value");
    entry.host_platform = extract_json_string(object_text, "host_platform");
    entry.success_count = extract_json_int(object_text, "success_count", 0);
    entry.rejection_count = extract_json_int(object_text, "rejection_count", 0);
    entry.last_status = extract_json_string(object_text, "last_status");
    entry.persisted_at = extract_json_string(object_text, "persisted_at");
    return entry;
}

HostAssistPreferenceRecord parse_host_assist_preference_entry(std::string_view object_text) {
    HostAssistPreferenceRecord entry;
    entry.id = extract_json_string(object_text, "id");
    entry.host_platform = extract_json_string(object_text, "host_platform");
    entry.provider_id = extract_json_string(object_text, "provider_id");
    entry.model = extract_json_string(object_text, "model");
    entry.preferred_shell_phrases = extract_json_string_array(object_text, "preferred_shell_phrases");
    entry.preferred_tool_routes = extract_json_string_array(object_text, "preferred_tool_routes");
    entry.preferred_recipe_routes = extract_json_string_array(object_text, "preferred_recipe_routes");
    entry.persisted_at = extract_json_string(object_text, "persisted_at");
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

DecompressionCandidate parse_decompression_candidate_entry(std::string_view object_text) {
    DecompressionCandidate entry;
    entry.label = extract_json_string(object_text, "label");
    entry.status = extract_json_string(object_text, "status");
    entry.confidence = extract_json_double(object_text, "confidence", 0.0);
    entry.notes = extract_json_string_array(object_text, "notes");
    return entry;
}

ShellLexiconEntry parse_shell_lexicon_entry(std::string_view object_text) {
    ShellLexiconEntry entry;
    entry.phrase = extract_json_string(object_text, "phrase");
    entry.canonical = extract_json_string(object_text, "canonical");
    entry.category = extract_json_string(object_text, "category");
    entry.confidence = extract_json_double(object_text, "confidence", 0.0);
    entry.correction_notes = extract_json_string_array(object_text, "correction_notes");
    entry.clarification_required = extract_json_bool(object_text, "clarification_required", false);
    return entry;
}

OperatorPersonaRecord parse_operator_persona_entry(std::string_view object_text) {
    OperatorPersonaRecord entry;
    entry.preferred_label = extract_json_string(object_text, "preferred_label");
    entry.role_label = extract_json_string(object_text, "role_label");
    entry.local_username = extract_json_string(object_text, "local_username");
    entry.host_identifier = extract_json_string(object_text, "host_identifier");
    entry.last_source_map = extract_json_string(object_text, "last_source_map");
    entry.last_memory_root = extract_json_string(object_text, "last_memory_root");
    entry.self_description = extract_json_string(object_text, "self_description");
    entry.persona_mode = extract_json_string(object_text, "persona_mode");
    entry.tone_profile = extract_json_string(object_text, "tone_profile");
    entry.interaction_style = extract_json_string(object_text, "interaction_style");
    entry.safety_posture = extract_json_string(object_text, "safety_posture");
    entry.preferred_next_action_style = extract_json_string(object_text, "preferred_next_action_style");
    entry.custom_phrases = extract_json_string_array(object_text, "custom_phrases");
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
    for (const std::string& candidate_text : extract_object_entries(object_text, "decompression_candidates")) {
        entry.decompression_candidates.push_back(parse_decompression_candidate_entry(candidate_text));
    }
    entry.research_notes = extract_json_string_array(object_text, "research_notes");
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
    entry.normalized_prompt = extract_json_string(object_text, "normalized_prompt");
    entry.query_tokens = extract_json_string_array(object_text, "query_tokens");
    entry.instruction_family_hint = extract_json_string(object_text, "instruction_family_hint");
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
    entry.chapter_series_label = extract_json_string(object_text, "chapter_series_label");
    entry.epoch_tier_label = extract_json_string(object_text, "epoch_tier_label");
    for (const std::string& trait_text : extract_object_entries(object_text, "indexed_traits")) {
        entry.indexed_traits.push_back(parse_uac_trait_entry(trait_text));
    }
    entry.recovery_hints = extract_json_string_array(object_text, "recovery_hints");
    entry.deletion_discrepancies = extract_json_string_array(object_text, "deletion_discrepancies");
    entry.search_context_habits = extract_json_string_array(object_text, "search_context_habits");
    entry.time_on_site_traits = extract_json_string_array(object_text, "time_on_site_traits");
    entry.reasoning_trace = extract_json_string_array(object_text, "reasoning_trace");
    entry.persisted_at = extract_json_string(object_text, "persisted_at");
    return entry;
}

LegacySourceRecord parse_legacy_source_entry(std::string_view object_text) {
    LegacySourceRecord entry;
    entry.id = extract_json_string(object_text, "id");
    entry.source_path = extract_json_string(object_text, "source_path");
    entry.source_label = extract_json_string(object_text, "source_label");
    entry.source_kind = extract_json_string(object_text, "source_kind");
    entry.source_hash = extract_json_string(object_text, "source_hash");
    entry.line_count = static_cast<std::size_t>(extract_json_int(object_text, "line_count", 0));
    entry.section_count = static_cast<std::size_t>(extract_json_int(object_text, "section_count", 0));
    entry.symbol_count = static_cast<std::size_t>(extract_json_int(object_text, "symbol_count", 0));
    entry.tracked_at = extract_json_string(object_text, "tracked_at");
    return entry;
}

LegacySymbolCoverage parse_legacy_symbol_coverage_entry(std::string_view object_text) {
    LegacySymbolCoverage entry;
    entry.symbol = extract_json_string(object_text, "symbol");
    entry.section_title = extract_json_string(object_text, "section_title");
    entry.recovery_status = extract_json_string(object_text, "recovery_status");
    entry.semantic_family = extract_json_string(object_text, "semantic_family");
    entry.mapped_cpp_target = extract_json_string(object_text, "mapped_cpp_target");
    entry.source_origin = extract_json_string(object_text, "source_origin");
    entry.occurrence_count = static_cast<std::size_t>(extract_json_int(object_text, "occurrence_count", 0));
    entry.notes = extract_json_string_array(object_text, "notes");
    return entry;
}

LegacyRecoveryStatus parse_legacy_recovery_status_entry(std::string_view object_text) {
    LegacyRecoveryStatus entry;
    entry.source_label = extract_json_string(object_text, "source_label");
    entry.implemented_count = static_cast<std::size_t>(extract_json_int(object_text, "implemented_count", 0));
    entry.partial_count = static_cast<std::size_t>(extract_json_int(object_text, "partial_count", 0));
    entry.missing_count = static_cast<std::size_t>(extract_json_int(object_text, "missing_count", 0));
    entry.research_only_count = static_cast<std::size_t>(extract_json_int(object_text, "research_only_count", 0));
    entry.blocked_count = static_cast<std::size_t>(extract_json_int(object_text, "blocked_count", 0));
    entry.summary_lines = extract_json_string_array(object_text, "summary_lines");
    return entry;
}

LegacyResearchArtifact parse_legacy_research_artifact_entry(std::string_view object_text) {
    LegacyResearchArtifact entry;
    entry.id = extract_json_string(object_text, "id");
    entry.label = extract_json_string(object_text, "label");
    entry.category = extract_json_string(object_text, "category");
    entry.status = extract_json_string(object_text, "status");
    entry.summary = extract_json_string(object_text, "summary");
    entry.source_origin = extract_json_string(object_text, "source_origin");
    entry.evidence = extract_json_string_array(object_text, "evidence");
    entry.blocked_paths = extract_json_string_array(object_text, "blocked_paths");
    return entry;
}

LegacyCorrelationRecord parse_legacy_correlation_entry(std::string_view object_text) {
    LegacyCorrelationRecord entry;
    entry.id = extract_json_string(object_text, "id");
    entry.left_label = extract_json_string(object_text, "left_label");
    entry.right_label = extract_json_string(object_text, "right_label");
    entry.correlation_type = extract_json_string(object_text, "correlation_type");
    entry.summary = extract_json_string(object_text, "summary");
    entry.strength = extract_json_int(object_text, "strength", 0);
    entry.evidence = extract_json_string_array(object_text, "evidence");
    return entry;
}

LegacyBridgeReport parse_legacy_bridge_report_entry(std::string_view object_text) {
    LegacyBridgeReport entry;
    entry.id = extract_json_string(object_text, "id");
    entry.query = extract_json_string(object_text, "query");
    entry.status = extract_json_string(object_text, "status");
    entry.bridge_mode = extract_json_string(object_text, "bridge_mode");
    entry.summary = extract_json_string(object_text, "summary");
    entry.bridge_steps = extract_json_string_array(object_text, "bridge_steps");
    entry.safe_actions = extract_json_string_array(object_text, "safe_actions");
    entry.research_actions = extract_json_string_array(object_text, "research_actions");
    entry.blocked_actions = extract_json_string_array(object_text, "blocked_actions");
    entry.correlation_signals = extract_json_string_array(object_text, "correlation_signals");
    entry.reasoning_trace = extract_json_string_array(object_text, "reasoning_trace");
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

RecipeAuthoringArtifact parse_recipe_authoring_artifact_entry(std::string_view object_text);

NeuralMathSample parse_neural_math_sample_entry(std::string_view object_text) {
    NeuralMathSample entry;
    entry.inputs = extract_json_double_array(object_text, "inputs");
    entry.expected = extract_json_int(object_text, "expected", 0);
    entry.predicted = extract_json_int(object_text, "predicted", 0);
    return entry;
}

NeuralMathReport parse_neural_math_report_entry(std::string_view object_text) {
    NeuralMathReport entry;
    entry.status = extract_json_string(object_text, "status");
    entry.summary = extract_json_string(object_text, "summary");
    entry.model_type = extract_json_string(object_text, "model_type");
    entry.dataset = extract_json_string(object_text, "dataset");
    entry.epochs_requested = static_cast<std::size_t>(extract_json_int(object_text, "epochs_requested", 0));
    entry.epochs_ran = static_cast<std::size_t>(extract_json_int(object_text, "epochs_ran", 0));
    entry.learning_rate = extract_json_double(object_text, "learning_rate", 0.0);
    entry.weights = extract_json_double_array(object_text, "weights");
    entry.bias = extract_json_double(object_text, "bias", 0.0);
    entry.accuracy = extract_json_double(object_text, "accuracy", 0.0);
    for (const std::string& sample_text : extract_object_entries(object_text, "predictions")) {
        entry.predictions.push_back(parse_neural_math_sample_entry(sample_text));
    }
    entry.math_trace = extract_json_string_array(object_text, "math_trace");
    entry.warnings = extract_json_string_array(object_text, "warnings");
    return entry;
}

NeuralFeatureVector parse_neural_feature_vector_entry(std::string_view object_text) {
    NeuralFeatureVector entry;
    entry.input_path = extract_json_string(object_text, "input_path");
    entry.packet_count = static_cast<std::size_t>(extract_json_int(object_text, "packet_count", 0));
    entry.flow_count = static_cast<std::size_t>(extract_json_int(object_text, "flow_count", 0));
    entry.control_count = static_cast<std::size_t>(extract_json_int(object_text, "control_count", 0));
    entry.http_plaintext_count = static_cast<std::size_t>(extract_json_int(object_text, "http_plaintext_count", 0));
    entry.text_utf8_count = static_cast<std::size_t>(extract_json_int(object_text, "text_utf8_count", 0));
    entry.tls_opaque_count = static_cast<std::size_t>(extract_json_int(object_text, "tls_opaque_count", 0));
    entry.opaque_payload_count = static_cast<std::size_t>(extract_json_int(object_text, "opaque_payload_count", 0));
    entry.parse_error_count = static_cast<std::size_t>(extract_json_int(object_text, "parse_error_count", 0));
    entry.payload_packet_count = static_cast<std::size_t>(extract_json_int(object_text, "payload_packet_count", 0));
    entry.unknown_port_count = static_cast<std::size_t>(extract_json_int(object_text, "unknown_port_count", 0));
    entry.total_payload_bytes = static_cast<std::size_t>(extract_json_int(object_text, "total_payload_bytes", 0));
    entry.feature_summary = extract_json_string_array(object_text, "feature_summary");
    return entry;
}

NeuralRoutePrediction parse_neural_route_prediction_entry(std::string_view object_text) {
    NeuralRoutePrediction entry;
    entry.label = extract_json_string(object_text, "label");
    entry.confidence = extract_json_double(object_text, "confidence", 0.0);
    entry.rationale = extract_json_string(object_text, "rationale");
    for (const std::string& attribution_text : extract_object_entries(object_text, "attributions")) {
        entry.attributions.push_back(parse_math_attribution_entry(attribution_text));
    }
    return entry;
}

NeuralRouteReport parse_neural_route_report_entry(std::string_view object_text) {
    NeuralRouteReport entry;
    entry.status = extract_json_string(object_text, "status");
    entry.summary = extract_json_string(object_text, "summary");
    entry.input_path = extract_json_string(object_text, "input_path");
    entry.packet_count = static_cast<std::size_t>(extract_json_int(object_text, "packet_count", 0));
    entry.flow_count = static_cast<std::size_t>(extract_json_int(object_text, "flow_count", 0));
    const std::string features_text = extract_json_object(object_text, "features");
    if (!features_text.empty()) {
        entry.features = parse_neural_feature_vector_entry(features_text);
    }
    for (const std::string& prediction_text : extract_object_entries(object_text, "predictions")) {
        entry.predictions.push_back(parse_neural_route_prediction_entry(prediction_text));
    }
    entry.warnings = extract_json_string_array(object_text, "warnings");
    entry.artifact_path = extract_json_string(object_text, "artifact_path");
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
    entry.source_map_path = extract_json_string(object_text, "source_map_path");
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
    const std::string next_step_assist_plan_text = extract_json_object(object_text, "next_step_assist_plan");
    if (!next_step_assist_plan_text.empty()) {
        entry.next_step_assist_plan = parse_next_step_assist_plan_entry(next_step_assist_plan_text);
    }
    const std::string case_summary_assist_plan_text = extract_json_object(object_text, "case_summary_assist_plan");
    if (!case_summary_assist_plan_text.empty()) {
        entry.case_summary_assist_plan = parse_case_summary_assist_plan_entry(case_summary_assist_plan_text);
    }
    const std::string freeform_assist_answer_text = extract_json_object(object_text, "freeform_assist_answer");
    if (!freeform_assist_answer_text.empty()) {
        entry.freeform_assist_answer = parse_freeform_assist_answer_entry(freeform_assist_answer_text);
    }
    const std::string recursive_diff_text = extract_json_object(object_text, "recursive_diff_report");
    if (!recursive_diff_text.empty()) {
        entry.recursive_diff_report = parse_recursive_diff_report_entry(recursive_diff_text);
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
    const std::string postprocess_text = extract_json_object(object_text, "postprocess_record");
    if (!postprocess_text.empty()) {
        entry.postprocess_record = parse_postprocess_record_entry(postprocess_text);
    }
    const std::string legacy_source_text = extract_json_object(object_text, "legacy_source");
    if (!legacy_source_text.empty()) {
        entry.legacy_source = parse_legacy_source_entry(legacy_source_text);
    }
    const std::string legacy_bridge_text = extract_json_object(object_text, "legacy_bridge_report");
    if (!legacy_bridge_text.empty()) {
        entry.legacy_bridge_report = parse_legacy_bridge_report_entry(legacy_bridge_text);
    }
    const std::string query_session_text = extract_json_object(object_text, "query_session");
    if (!query_session_text.empty()) {
        entry.query_session = parse_query_session_entry(query_session_text);
    }
    for (const std::string& artifact_text : extract_object_entries(object_text, "legacy_research_artifacts")) {
        entry.legacy_research_artifacts.push_back(parse_legacy_research_artifact_entry(artifact_text));
    }
    for (const std::string& correlation_text : extract_object_entries(object_text, "legacy_correlations")) {
        entry.legacy_correlations.push_back(parse_legacy_correlation_entry(correlation_text));
    }
    for (const std::string& coverage_text : extract_object_entries(object_text, "legacy_symbol_coverage")) {
        entry.legacy_symbol_coverage.push_back(parse_legacy_symbol_coverage_entry(coverage_text));
    }
    const std::string legacy_status_text = extract_json_object(object_text, "legacy_recovery_status");
    if (!legacy_status_text.empty()) {
        entry.legacy_recovery_status = parse_legacy_recovery_status_entry(legacy_status_text);
    }
    const std::string review_artifact_text = extract_json_object(object_text, "review_artifact");
    if (!review_artifact_text.empty()) {
        entry.review_artifact = parse_review_artifact_entry(review_artifact_text);
    }
    const std::string patch_proposal_text = extract_json_object(object_text, "patch_proposal_artifact");
    if (!patch_proposal_text.empty()) {
        entry.patch_proposal_artifact = parse_patch_proposal_artifact_entry(patch_proposal_text);
    }
    const std::string recipe_authoring_text = extract_json_object(object_text, "recipe_authoring_artifact");
    if (!recipe_authoring_text.empty()) {
        entry.recipe_authoring_artifact = parse_recipe_authoring_artifact_entry(recipe_authoring_text);
    }
    const std::string definition_answer_text = extract_json_object(object_text, "definition_answer");
    if (!definition_answer_text.empty()) {
        entry.definition_answer = parse_definition_answer_entry(definition_answer_text);
    }
    const std::string neural_math_text = extract_json_object(object_text, "neural_math_report");
    if (!neural_math_text.empty()) {
        entry.neural_math_report = parse_neural_math_report_entry(neural_math_text);
    }
    const std::string neural_route_text = extract_json_object(object_text, "neural_route_report");
    if (!neural_route_text.empty()) {
        entry.neural_route_report = parse_neural_route_report_entry(neural_route_text);
    }
    for (const std::string& stage_text : extract_object_entries(object_text, "stages")) {
        entry.stages.push_back(parse_tze_stage_entry(stage_text));
    }
    return entry;
}

StoredDefinition parse_definition_entry(std::string_view object_text) {
    StoredDefinition entry;
    entry.term = extract_json_string(object_text, "term");
    entry.normalized_concept = extract_json_string(object_text, "normalized_concept");
    entry.domain_hint = extract_json_string(object_text, "domain_hint");
    entry.summary = extract_json_string(object_text, "summary");
    entry.mapped_cpp_target = extract_json_string(object_text, "mapped_cpp_target");
    entry.semantic_family = extract_json_string(object_text, "semantic_family");
    entry.source = extract_json_string(object_text, "source");
    entry.source_type = extract_json_string(object_text, "source_type");
    entry.authority_tier = extract_json_string(object_text, "authority_tier");
    entry.confidence = extract_json_double(object_text, "confidence", 0.0);
    entry.scope = extract_json_string(object_text, "scope");
    entry.created_at = extract_json_string(object_text, "created_at");
    entry.expires_at = extract_json_string(object_text, "expires_at");
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

BuildRecipe parse_build_recipe_entry(std::string_view object_text) {
    BuildRecipe entry;
    entry.id = extract_json_string(object_text, "id");
    entry.acquisition_method = extract_json_string(object_text, "acquisition_method");
    entry.build_system = extract_json_string(object_text, "build_system");
    entry.supported_platforms = extract_json_string_array(object_text, "supported_platforms");
    entry.default_target = extract_json_string(object_text, "default_target");
    entry.install_target = extract_json_string(object_text, "install_target");
    entry.artifact_patterns = extract_json_string_array(object_text, "artifact_patterns");
    entry.install_output_patterns = extract_json_string_array(object_text, "install_output_patterns");
    entry.fallback_stage_patterns = extract_json_string_array(object_text, "fallback_stage_patterns");
    entry.dependency_hints = extract_json_string_array(object_text, "dependency_hints");
    entry.configure_arguments = extract_json_string_array(object_text, "configure_arguments");
    entry.supports_install = extract_json_bool(object_text, "supports_install", true);
    entry.copy_artifacts_on_install = extract_json_bool(object_text, "copy_artifacts_on_install", false);
    return entry;
}

AuthoredRecipeRecord parse_authored_recipe_entry(std::string_view object_text) {
    AuthoredRecipeRecord entry;
    const std::string recipe_text = extract_json_object(object_text, "recipe");
    if (!recipe_text.empty()) {
        entry.recipe = parse_build_recipe_entry(recipe_text);
    }
    entry.resolved_source_path = extract_json_string(object_text, "resolved_source_path");
    entry.canonical_name = extract_json_string(object_text, "canonical_name");
    entry.origin_provider = extract_json_string(object_text, "origin_provider");
    entry.origin_model = extract_json_string(object_text, "origin_model");
    entry.evidence_summary = extract_json_string_array(object_text, "evidence_summary");
    entry.validation_status = extract_json_string(object_text, "validation_status");
    entry.last_validation_summary = extract_json_string(object_text, "last_validation_summary");
    entry.last_validation_log = extract_json_string(object_text, "last_validation_log");
    entry.last_artifact = extract_json_string(object_text, "last_artifact");
    entry.authoring_run_id = extract_json_string(object_text, "authoring_run_id");
    entry.active_scope = extract_json_string(object_text, "active_scope");
    entry.active = extract_json_bool(object_text, "active", false);
    entry.repair_attempted = extract_json_bool(object_text, "repair_attempted", false);
    entry.persisted_at = extract_json_string(object_text, "persisted_at");
    return entry;
}

RecipeAuthoringArtifact parse_recipe_authoring_artifact_entry(std::string_view object_text) {
    RecipeAuthoringArtifact entry;
    entry.id = extract_json_string(object_text, "id");
    entry.source_path = extract_json_string(object_text, "source_path");
    entry.resolved_source_path = extract_json_string(object_text, "resolved_source_path");
    entry.canonical_project_name = extract_json_string(object_text, "canonical_project_name");
    entry.provider_id = extract_json_string(object_text, "provider_id");
    entry.model = extract_json_string(object_text, "model");
    entry.status = extract_json_string(object_text, "status");
    entry.generated_recipe_id = extract_json_string(object_text, "generated_recipe_id");
    entry.generated_build_system = extract_json_string(object_text, "generated_build_system");
    entry.rationale = extract_json_string(object_text, "rationale");
    entry.ranked_evidence = extract_json_string_array(object_text, "ranked_evidence");
    entry.validation_feedback = extract_json_string_array(object_text, "validation_feedback");
    entry.repair_attempted = extract_json_bool(object_text, "repair_attempted", false);
    entry.activated = extract_json_bool(object_text, "activated", false);
    entry.persisted_at = extract_json_string(object_text, "persisted_at");
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
    for (const std::string& attribution_text : extract_object_entries(object_text, "math_attributions")) {
        entry.math_attributions.push_back(parse_math_attribution_entry(attribution_text));
    }
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
        << "\",\"summary\":\"" << escape_json(entry.summary)
        << "\",\"scope\":\"" << escape_json(entry.scope)
        << "\",\"created_at\":\"" << escape_json(entry.created_at)
        << "\",\"expires_at\":\"" << escape_json(entry.expires_at) << "\"}";
    return out.str();
}

std::string render_history_jsonl(const MemorySnapshot& snapshot) {
    std::ostringstream out;
    for (const MemoryHistoryEntry& entry : snapshot.history) {
        out << render_history_entry(entry) << '\n';
    }
    return out.str();
}

std::string first_non_empty_line(std::string_view text) {
    std::istringstream input{std::string(text)};
    std::string line;
    while (std::getline(input, line)) {
        line = trim(line);
        if (!line.empty()) {
            return line;
        }
    }
    return {};
}

std::string collapse_whitespace(std::string_view text) {
    std::string collapsed;
    collapsed.reserve(text.size());
    bool previous_space = false;
    for (char c : text) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!previous_space) {
                collapsed.push_back(' ');
            }
            previous_space = true;
            continue;
        }
        previous_space = false;
        collapsed.push_back(c);
    }
    return trim(collapsed);
}

std::string truncate_summary(std::string text, std::size_t max_length = 220) {
    if (text.size() <= max_length) {
        return text;
    }
    if (max_length <= 3) {
        return text.substr(0, max_length);
    }
    return text.substr(0, max_length - 3) + "...";
}

std::string summarize_interaction_for_history(const ProcessingReport& report) {
    if (report.postprocess_record.has_value() && !report.postprocess_record->final_artifact_summary.empty()) {
        std::string summary = report.postprocess_record->final_artifact_summary;
        if (!report.postprocess_record->provenance.empty()) {
            summary += " [source=" + report.postprocess_record->provenance + "]";
        }
        if (!report.produced_artifact.empty()) {
            summary += " [artifact=" + report.produced_artifact + "]";
        }
        return truncate_summary(collapse_whitespace(summary));
    }

    if (report.freeform_assist_answer.has_value() && !report.freeform_assist_answer->answer.empty()) {
        std::string summary = report.freeform_assist_answer->answer + " [source=openai_freeform]";
        return truncate_summary(collapse_whitespace(summary));
    }

    if (report.definition_answer.has_value()) {
        const DefinitionAnswer& answer = *report.definition_answer;
        std::string summary = answer.summary.empty() ? first_non_empty_line(report.answer_explanation) : answer.summary;
        if (summary.empty()) {
            summary = "Definition lookup completed.";
        }
        if (!answer.selected_source_type.empty()) {
            summary += " [source=" + answer.selected_source_type + "]";
        }
        if (!answer.selected_authority_tier.empty()) {
            summary += " [authority=" + answer.selected_authority_tier + "]";
        }
        if (!report.produced_artifact.empty()) {
            summary += " [artifact=" + report.produced_artifact + "]";
        }
        return truncate_summary(collapse_whitespace(summary));
    }

    if (!report.produced_artifact.empty()) {
        std::string summary = first_non_empty_line(report.answer_explanation);
        if (summary.empty()) {
            summary = report.answer_status.empty() ? "Artifact stored." : "Artifact stored for " + report.answer_status + ".";
        }
        summary += " [artifact=" + report.produced_artifact + "]";
        return truncate_summary(collapse_whitespace(summary));
    }

    const std::string primary_line = first_non_empty_line(report.answer_explanation);
    if (!primary_line.empty()) {
        return truncate_summary(collapse_whitespace(primary_line));
    }
    if (!report.answer_status.empty()) {
        return truncate_summary(collapse_whitespace(report.answer_status));
    }
    return {};
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

std::string render_reasoning_slot_json(const ReasoningSlotRecord& entry) {
    std::ostringstream out;
    out << "{\"slot_id\":\"" << escape_json(entry.slot_id)
        << "\",\"label\":\"" << escape_json(entry.label)
        << "\",\"facts\":" << render_string_array(entry.facts)
        << ",\"evidence_refs\":" << render_string_array(entry.evidence_refs)
        << ",\"confidence\":" << entry.confidence << "}";
    return out.str();
}

std::string render_recursive_path_json(const RecursivePathRecord& entry) {
    std::ostringstream out;
    out << "{\"path_type\":\"" << escape_json(entry.path_type)
        << "\",\"ordered_steps\":" << render_string_array(entry.ordered_steps)
        << ",\"assumptions\":" << render_string_array(entry.assumptions)
        << ",\"evidence_refs\":" << render_string_array(entry.evidence_refs)
        << ",\"score\":" << entry.score << "}";
    return out.str();
}

std::string render_recursive_diff_json(const RecursiveDiffReport& entry) {
    std::ostringstream out;
    out << "{\"status\":\"" << escape_json(entry.status)
        << "\",\"route_learning_status\":\"" << escape_json(entry.route_learning_status)
        << "\",\"source_run_id\":\"" << escape_json(entry.source_run_id)
        << "\",\"current_state\":\"" << escape_json(entry.current_state)
        << "\",\"likely_goal\":\"" << escape_json(entry.likely_goal)
        << "\",\"logs_and_reasoning\":\"" << escape_json(entry.logs_and_reasoning)
        << "\",\"successful_path_pattern\":\"" << escape_json(entry.successful_path_pattern)
        << "\",\"difference_found\":\"" << escape_json(entry.difference_found)
        << "\",\"best_estimate_answer\":\"" << escape_json(entry.best_estimate_answer)
        << "\",\"why_this_matters\":\"" << escape_json(entry.why_this_matters)
        << "\",\"next_action\":\"" << escape_json(entry.next_action)
        << "\",\"diff_category\":\"" << escape_json(entry.diff_category)
        << "\",\"confidence\":" << entry.confidence
        << ",\"slots\":[";
    for (std::size_t index = 0; index < entry.slots.size(); ++index) {
        if (index != 0) {
            out << ",";
        }
        out << render_reasoning_slot_json(entry.slots[index]);
    }
    out << "],\"success_path\":" << render_recursive_path_json(entry.success_path)
        << ",\"failure_path\":" << render_recursive_path_json(entry.failure_path)
        << "}";
    return out.str();
}

std::string render_recursive_diff_summary(const RecursiveDiffReport& entry) {
    std::ostringstream out;
    out << "Recursive Why/Diff:\n";
    if (!entry.route_learning_status.empty()) {
        out << " - Recursive Route Learning: " << entry.route_learning_status << "\n";
    }
    out << " - Source run: " << entry.source_run_id << "\n";
    out << " - Status: " << entry.status << "\n";
    out << " - Confidence: " << entry.confidence << "\n";
    out << "Current State\n" << entry.current_state << "\n";
    out << "Likely Goal\n" << entry.likely_goal << "\n";
    out << "What the Logs/Reasoning Show\n" << entry.logs_and_reasoning << "\n";
    out << "Successful Path Pattern\n" << entry.successful_path_pattern << "\n";
    out << "Difference Found\n" << entry.difference_found << "\n";
    out << "Best Estimate Answer\n" << entry.best_estimate_answer << "\n";
    out << "Why This Matters\n" << entry.why_this_matters << "\n";
    out << "Next Action\n" << entry.next_action << "\n";
    if (!entry.diff_category.empty()) {
        out << " - Diff category: " << entry.diff_category << "\n";
    }
    if (!entry.slots.empty()) {
        out << "Reasoning slots:\n";
        for (const ReasoningSlotRecord& slot : entry.slots) {
            out << " - " << slot.slot_id << " " << slot.label << " confidence=" << slot.confidence << "\n";
            for (const std::string& fact : slot.facts) {
                out << "   - " << fact << "\n";
            }
        }
    }
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

std::string render_decompression_candidate_json(const DecompressionCandidate& entry) {
    std::ostringstream out;
    out << "{\"label\":\"" << escape_json(entry.label)
        << "\",\"status\":\"" << escape_json(entry.status)
        << "\",\"confidence\":" << std::fixed << std::setprecision(4) << entry.confidence
        << ",\"notes\":" << render_string_array(entry.notes) << "}";
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
    out << "],\"decompression_candidates\":[";
    for (std::size_t index = 0; index < entry.decompression_candidates.size(); ++index) {
        if (index != 0) {
            out << ",";
        }
        out << render_decompression_candidate_json(entry.decompression_candidates[index]);
    }
    out << "],\"research_notes\":" << render_string_array(entry.research_notes)
        << ",\"reasoning_trace\":" << render_string_array(entry.reasoning_trace)
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
        << "\",\"normalized_prompt\":\"" << escape_json(entry.normalized_prompt)
        << "\",\"query_tokens\":" << render_string_array(entry.query_tokens)
        << ",\"instruction_family_hint\":\"" << escape_json(entry.instruction_family_hint)
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
        << "\",\"chapter_series_label\":\"" << escape_json(entry.chapter_series_label)
        << "\",\"epoch_tier_label\":\"" << escape_json(entry.epoch_tier_label)
        << "\",\"indexed_traits\":[";
    for (std::size_t index = 0; index < entry.indexed_traits.size(); ++index) {
        if (index != 0) {
            out << ",";
        }
        out << render_uac_trait_json(entry.indexed_traits[index]);
    }
    out << "],\"recovery_hints\":" << render_string_array(entry.recovery_hints)
        << ",\"deletion_discrepancies\":" << render_string_array(entry.deletion_discrepancies)
        << ",\"search_context_habits\":" << render_string_array(entry.search_context_habits)
        << ",\"time_on_site_traits\":" << render_string_array(entry.time_on_site_traits)
        << ",\"reasoning_trace\":" << render_string_array(entry.reasoning_trace)
        << ",\"persisted_at\":\"" << escape_json(entry.persisted_at) << "\"}";
    return out.str();
}

std::string render_legacy_source_json(const LegacySourceRecord& entry) {
    std::ostringstream out;
    out << "{\"id\":\"" << escape_json(entry.id)
        << "\",\"source_path\":\"" << escape_json(entry.source_path)
        << "\",\"source_label\":\"" << escape_json(entry.source_label)
        << "\",\"source_kind\":\"" << escape_json(entry.source_kind)
        << "\",\"source_hash\":\"" << escape_json(entry.source_hash)
        << "\",\"line_count\":" << entry.line_count
        << ",\"section_count\":" << entry.section_count
        << ",\"symbol_count\":" << entry.symbol_count
        << ",\"tracked_at\":\"" << escape_json(entry.tracked_at) << "\"}";
    return out.str();
}

std::string render_legacy_symbol_coverage_json(const LegacySymbolCoverage& entry) {
    std::ostringstream out;
    out << "{\"symbol\":\"" << escape_json(entry.symbol)
        << "\",\"section_title\":\"" << escape_json(entry.section_title)
        << "\",\"recovery_status\":\"" << escape_json(entry.recovery_status)
        << "\",\"semantic_family\":\"" << escape_json(entry.semantic_family)
        << "\",\"mapped_cpp_target\":\"" << escape_json(entry.mapped_cpp_target)
        << "\",\"source_origin\":\"" << escape_json(entry.source_origin)
        << "\",\"occurrence_count\":" << entry.occurrence_count
        << ",\"notes\":" << render_string_array(entry.notes) << "}";
    return out.str();
}

std::string render_legacy_recovery_status_json(const LegacyRecoveryStatus& entry) {
    std::ostringstream out;
    out << "{\"source_label\":\"" << escape_json(entry.source_label)
        << "\",\"implemented_count\":" << entry.implemented_count
        << ",\"partial_count\":" << entry.partial_count
        << ",\"missing_count\":" << entry.missing_count
        << ",\"research_only_count\":" << entry.research_only_count
        << ",\"blocked_count\":" << entry.blocked_count
        << ",\"summary_lines\":" << render_string_array(entry.summary_lines) << "}";
    return out.str();
}

std::string render_legacy_research_artifact_json(const LegacyResearchArtifact& entry) {
    std::ostringstream out;
    out << "{\"id\":\"" << escape_json(entry.id)
        << "\",\"label\":\"" << escape_json(entry.label)
        << "\",\"category\":\"" << escape_json(entry.category)
        << "\",\"status\":\"" << escape_json(entry.status)
        << "\",\"summary\":\"" << escape_json(entry.summary)
        << "\",\"source_origin\":\"" << escape_json(entry.source_origin)
        << "\",\"evidence\":" << render_string_array(entry.evidence)
        << ",\"blocked_paths\":" << render_string_array(entry.blocked_paths) << "}";
    return out.str();
}

std::string render_legacy_correlation_json(const LegacyCorrelationRecord& entry) {
    std::ostringstream out;
    out << "{\"id\":\"" << escape_json(entry.id)
        << "\",\"left_label\":\"" << escape_json(entry.left_label)
        << "\",\"right_label\":\"" << escape_json(entry.right_label)
        << "\",\"correlation_type\":\"" << escape_json(entry.correlation_type)
        << "\",\"summary\":\"" << escape_json(entry.summary)
        << "\",\"strength\":" << entry.strength
        << ",\"evidence\":" << render_string_array(entry.evidence) << "}";
    return out.str();
}

std::string render_legacy_bridge_report_json(const LegacyBridgeReport& entry) {
    std::ostringstream out;
    out << "{\"id\":\"" << escape_json(entry.id)
        << "\",\"query\":\"" << escape_json(entry.query)
        << "\",\"status\":\"" << escape_json(entry.status)
        << "\",\"bridge_mode\":\"" << escape_json(entry.bridge_mode)
        << "\",\"summary\":\"" << escape_json(entry.summary)
        << "\",\"bridge_steps\":" << render_string_array(entry.bridge_steps)
        << ",\"safe_actions\":" << render_string_array(entry.safe_actions)
        << ",\"research_actions\":" << render_string_array(entry.research_actions)
        << ",\"blocked_actions\":" << render_string_array(entry.blocked_actions)
        << ",\"correlation_signals\":" << render_string_array(entry.correlation_signals)
        << ",\"reasoning_trace\":" << render_string_array(entry.reasoning_trace) << "}";
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

std::string render_assist_plan_metadata_json(const AssistPlanMetadata& entry) {
    std::ostringstream out;
    out << "{\"provider\":\"" << escape_json(entry.provider)
        << "\",\"model\":\"" << escape_json(entry.model)
        << "\",\"status\":\"" << escape_json(entry.status)
        << "\",\"confidence\":" << entry.confidence
        << ",\"rationale\":\"" << escape_json(entry.rationale)
        << "\",\"warnings\":" << render_string_array(entry.warnings)
        << ",\"validation_notes\":" << render_string_array(entry.validation_notes) << "}";
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
        << ",\"metadata\":" << render_assist_plan_metadata_json(entry.metadata)
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
        << ",\"metadata\":" << render_assist_plan_metadata_json(entry.metadata)
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
        << ",\"metadata\":" << render_assist_plan_metadata_json(entry.metadata)
        << ",\"validated\":" << (entry.validated ? "true" : "false") << "}";
    return out.str();
}

std::string render_next_step_assist_plan_json(const NextStepAssistPlan& entry) {
    std::ostringstream out;
    out << "{\"task_id\":\"" << escape_json(entry.task_id)
        << "\",\"provider_id\":\"" << escape_json(entry.provider_id)
        << "\",\"model\":\"" << escape_json(entry.model)
        << "\",\"status\":\"" << escape_json(entry.status)
        << "\",\"suggested_next_step\":\"" << escape_json(entry.suggested_next_step)
        << "\",\"safer_alternative\":\"" << escape_json(entry.safer_alternative)
        << "\",\"rationale\":\"" << escape_json(entry.rationale)
        << "\",\"confidence\":" << entry.confidence
        << ",\"warnings\":" << render_string_array(entry.warnings)
        << ",\"validation_notes\":" << render_string_array(entry.validation_notes)
        << ",\"metadata\":" << render_assist_plan_metadata_json(entry.metadata)
        << ",\"validated\":" << (entry.validated ? "true" : "false") << "}";
    return out.str();
}

std::string render_case_summary_assist_plan_json(const CaseSummaryAssistPlan& entry) {
    std::ostringstream out;
    out << "{\"task_id\":\"" << escape_json(entry.task_id)
        << "\",\"provider_id\":\"" << escape_json(entry.provider_id)
        << "\",\"model\":\"" << escape_json(entry.model)
        << "\",\"status\":\"" << escape_json(entry.status)
        << "\",\"summary_title\":\"" << escape_json(entry.summary_title)
        << "\",\"executive_summary\":\"" << escape_json(entry.executive_summary)
        << "\",\"highlights\":" << render_string_array(entry.highlights)
        << ",\"rationale\":\"" << escape_json(entry.rationale)
        << "\",\"confidence\":" << entry.confidence
        << ",\"warnings\":" << render_string_array(entry.warnings)
        << ",\"validation_notes\":" << render_string_array(entry.validation_notes)
        << ",\"metadata\":" << render_assist_plan_metadata_json(entry.metadata)
        << ",\"validated\":" << (entry.validated ? "true" : "false") << "}";
    return out.str();
}

std::string render_freeform_assist_answer_json(const FreeformAssistAnswer& entry) {
    std::ostringstream out;
    out << "{\"task_id\":\"" << escape_json(entry.task_id)
        << "\",\"provider_id\":\"" << escape_json(entry.provider_id)
        << "\",\"model\":\"" << escape_json(entry.model)
        << "\",\"status\":\"" << escape_json(entry.status)
        << "\",\"answer\":\"" << escape_json(entry.answer)
        << "\",\"rationale\":\"" << escape_json(entry.rationale)
        << "\",\"confidence\":" << entry.confidence
        << ",\"suggested_commands\":" << render_string_array(entry.suggested_commands)
        << ",\"safety_warnings\":" << render_string_array(entry.safety_warnings)
        << ",\"used_context\":" << render_string_array(entry.used_context)
        << ",\"metadata\":" << render_assist_plan_metadata_json(entry.metadata)
        << ",\"validated\":" << (entry.validated ? "true" : "false") << "}";
    return out.str();
}

std::string render_math_attribution_json(const MathAttribution& entry) {
    std::ostringstream out;
    out << "{\"name\":\"" << escape_json(entry.name)
        << "\",\"raw_value\":" << entry.raw_value
        << ",\"weight\":" << entry.weight
        << ",\"contribution\":" << entry.contribution
        << ",\"source\":\"" << escape_json(entry.source)
        << "\",\"rationale\":\"" << escape_json(entry.rationale) << "\"}";
    return out.str();
}

std::string render_math_attributions_json(const std::vector<MathAttribution>& entries) {
    std::ostringstream out;
    out << "[";
    for (std::size_t index = 0; index < entries.size(); ++index) {
        if (index != 0) {
            out << ",";
        }
        out << render_math_attribution_json(entries[index]);
    }
    out << "]";
    return out.str();
}

std::string render_definition_answer_json(const DefinitionAnswer& entry) {
    std::ostringstream out;
    out << "{\"query\":\"" << escape_json(entry.query)
        << "\",\"found\":" << (entry.found ? "true" : "false")
        << ",\"summary\":\"" << escape_json(entry.summary)
        << "\",\"mapped_cpp_target\":\"" << escape_json(entry.mapped_cpp_target)
        << "\",\"semantic_family\":\"" << escape_json(entry.semantic_family)
        << "\",\"normalized_concept\":\"" << escape_json(entry.normalized_concept)
        << "\",\"domain_hint\":\"" << escape_json(entry.domain_hint)
        << "\",\"route_class\":\"" << escape_json(entry.route_class)
        << "\",\"selected_source_type\":\"" << escape_json(entry.selected_source_type)
        << "\",\"selected_source_label\":\"" << escape_json(entry.selected_source_label)
        << "\",\"selected_authority_tier\":\"" << escape_json(entry.selected_authority_tier)
        << "\",\"confidence\":" << entry.confidence
        << ",\"comparison_rationale\":\"" << escape_json(entry.comparison_rationale)
        << "\",\"sources\":" << render_string_array(entry.sources)
        << ",\"suggestions\":" << render_string_array(entry.suggestions)
        << ",\"math_attributions\":" << render_math_attributions_json(entry.math_attributions) << "}";
    return out.str();
}

std::string render_postprocess_record_json(const PostProcessRecord& entry) {
    std::ostringstream out;
    out << "{\"status\":\"" << escape_json(entry.status)
        << "\",\"final_artifact_summary\":\"" << escape_json(entry.final_artifact_summary)
        << "\",\"provenance\":\"" << escape_json(entry.provenance)
        << "\",\"retention_decision\":\"" << escape_json(entry.retention_decision)
        << "\",\"dropped_transient_chain\":" << (entry.dropped_transient_chain ? "true" : "false")
        << ",\"retained_fields\":" << render_string_array(entry.retained_fields)
        << ",\"discarded_fields\":" << render_string_array(entry.discarded_fields)
        << ",\"persisted_at\":\"" << escape_json(entry.persisted_at) << "\"}";
    return out.str();
}

std::string render_neural_math_sample_json(const NeuralMathSample& entry) {
    std::ostringstream out;
    out << "{\"inputs\":" << render_double_array(entry.inputs)
        << ",\"expected\":" << entry.expected
        << ",\"predicted\":" << entry.predicted << "}";
    return out.str();
}

std::string render_neural_math_report_json(const NeuralMathReport& entry) {
    std::ostringstream out;
    out << "{\"status\":\"" << escape_json(entry.status)
        << "\",\"summary\":\"" << escape_json(entry.summary)
        << "\",\"model_type\":\"" << escape_json(entry.model_type)
        << "\",\"dataset\":\"" << escape_json(entry.dataset)
        << "\",\"epochs_requested\":" << entry.epochs_requested
        << ",\"epochs_ran\":" << entry.epochs_ran
        << ",\"learning_rate\":" << entry.learning_rate
        << ",\"weights\":" << render_double_array(entry.weights)
        << ",\"bias\":" << entry.bias
        << ",\"accuracy\":" << entry.accuracy
        << ",\"predictions\":[";
    for (std::size_t index = 0; index < entry.predictions.size(); ++index) {
        if (index != 0) {
            out << ",";
        }
        out << render_neural_math_sample_json(entry.predictions[index]);
    }
    out << "],\"math_trace\":" << render_string_array(entry.math_trace)
        << ",\"warnings\":" << render_string_array(entry.warnings) << "}";
    return out.str();
}

std::string render_neural_feature_vector_json(const NeuralFeatureVector& entry) {
    std::ostringstream out;
    out << "{\"input_path\":\"" << escape_json(entry.input_path)
        << "\",\"packet_count\":" << entry.packet_count
        << ",\"flow_count\":" << entry.flow_count
        << ",\"control_count\":" << entry.control_count
        << ",\"http_plaintext_count\":" << entry.http_plaintext_count
        << ",\"text_utf8_count\":" << entry.text_utf8_count
        << ",\"tls_opaque_count\":" << entry.tls_opaque_count
        << ",\"opaque_payload_count\":" << entry.opaque_payload_count
        << ",\"parse_error_count\":" << entry.parse_error_count
        << ",\"payload_packet_count\":" << entry.payload_packet_count
        << ",\"unknown_port_count\":" << entry.unknown_port_count
        << ",\"total_payload_bytes\":" << entry.total_payload_bytes
        << ",\"feature_summary\":" << render_string_array(entry.feature_summary) << "}";
    return out.str();
}

std::string render_neural_route_prediction_json(const NeuralRoutePrediction& entry) {
    std::ostringstream out;
    out << "{\"label\":\"" << escape_json(entry.label)
        << "\",\"confidence\":" << entry.confidence
        << ",\"rationale\":\"" << escape_json(entry.rationale)
        << "\",\"attributions\":" << render_math_attributions_json(entry.attributions) << "}";
    return out.str();
}

std::string render_neural_route_report_json(const NeuralRouteReport& entry) {
    std::ostringstream out;
    out << "{\"status\":\"" << escape_json(entry.status)
        << "\",\"summary\":\"" << escape_json(entry.summary)
        << "\",\"input_path\":\"" << escape_json(entry.input_path)
        << "\",\"packet_count\":" << entry.packet_count
        << ",\"flow_count\":" << entry.flow_count
        << ",\"features\":" << render_neural_feature_vector_json(entry.features)
        << ",\"predictions\":[";
    for (std::size_t index = 0; index < entry.predictions.size(); ++index) {
        if (index != 0) {
            out << ",";
        }
        out << render_neural_route_prediction_json(entry.predictions[index]);
    }
    out << "],\"warnings\":" << render_string_array(entry.warnings)
        << ",\"artifact_path\":\"" << escape_json(entry.artifact_path) << "\"}";
    return out.str();
}

std::string render_review_finding_json(const ReviewFinding& entry) {
    std::ostringstream out;
    out << "{\"severity\":\"" << escape_json(entry.severity)
        << "\",\"category\":\"" << escape_json(entry.category)
        << "\",\"file_path\":\"" << escape_json(entry.file_path)
        << "\",\"line\":" << entry.line
        << ",\"summary\":\"" << escape_json(entry.summary)
        << "\",\"rationale\":\"" << escape_json(entry.rationale) << "\"}";
    return out.str();
}

std::string render_review_artifact_json(const ReviewArtifact& entry) {
    std::ostringstream out;
    out << "{\"id\":\"" << escape_json(entry.id)
        << "\",\"target\":\"" << escape_json(entry.target)
        << "\",\"status\":\"" << escape_json(entry.status)
        << "\",\"summary\":\"" << escape_json(entry.summary)
        << "\",\"findings\":[";
    for (std::size_t index = 0; index < entry.findings.size(); ++index) {
        if (index != 0) {
            out << ",";
        }
        out << render_review_finding_json(entry.findings[index]);
    }
    out << "],\"nearby_symbols\":" << render_string_array(entry.nearby_symbols)
        << ",\"suggested_tests\":" << render_string_array(entry.suggested_tests)
        << ",\"persisted_at\":\"" << escape_json(entry.persisted_at)
        << "\",\"artifact_path\":\"" << escape_json(entry.artifact_path)
        << "\",\"assist_annotation\":"
        << (entry.assist_annotation.has_value() ? render_assist_annotation_json(*entry.assist_annotation) : "null")
        << "}";
    return out.str();
}

std::string render_patch_proposal_artifact_json(const PatchProposalArtifact& entry) {
    std::ostringstream out;
    out << "{\"id\":\"" << escape_json(entry.id)
        << "\",\"target\":\"" << escape_json(entry.target)
        << "\",\"status\":\"" << escape_json(entry.status)
        << "\",\"summary\":\"" << escape_json(entry.summary)
        << "\",\"target_files\":" << render_string_array(entry.target_files)
        << ",\"intended_behavior_changes\":" << render_string_array(entry.intended_behavior_changes)
        << ",\"acceptance_tests\":" << render_string_array(entry.acceptance_tests)
        << ",\"unified_diff_text\":\"" << escape_json(entry.unified_diff_text)
        << "\",\"persisted_at\":\"" << escape_json(entry.persisted_at)
        << "\",\"artifact_path\":\"" << escape_json(entry.artifact_path)
        << "\",\"assist_annotation\":"
        << (entry.assist_annotation.has_value() ? render_assist_annotation_json(*entry.assist_annotation) : "null")
        << "}";
    return out.str();
}

std::string render_recipe_authoring_artifact_json(const RecipeAuthoringArtifact& entry) {
    std::ostringstream out;
    out << "{\"id\":\"" << escape_json(entry.id)
        << "\",\"source_path\":\"" << escape_json(entry.source_path)
        << "\",\"resolved_source_path\":\"" << escape_json(entry.resolved_source_path)
        << "\",\"canonical_project_name\":\"" << escape_json(entry.canonical_project_name)
        << "\",\"provider_id\":\"" << escape_json(entry.provider_id)
        << "\",\"model\":\"" << escape_json(entry.model)
        << "\",\"status\":\"" << escape_json(entry.status)
        << "\",\"generated_recipe_id\":\"" << escape_json(entry.generated_recipe_id)
        << "\",\"generated_build_system\":\"" << escape_json(entry.generated_build_system)
        << "\",\"rationale\":\"" << escape_json(entry.rationale)
        << "\",\"ranked_evidence\":" << render_string_array(entry.ranked_evidence)
        << ",\"validation_feedback\":" << render_string_array(entry.validation_feedback)
        << ",\"repair_attempted\":" << (entry.repair_attempted ? "true" : "false")
        << ",\"activated\":" << (entry.activated ? "true" : "false")
        << ",\"persisted_at\":\"" << escape_json(entry.persisted_at) << "\"}";
    return out.str();
}

std::string render_assist_outcome_json(const AssistOutcomeRecord& entry) {
    std::ostringstream out;
    out << "{\"id\":\"" << escape_json(entry.id)
        << "\",\"task_type\":\"" << escape_json(entry.task_type)
        << "\",\"plan_type\":\"" << escape_json(entry.plan_type)
        << "\",\"provider_id\":\"" << escape_json(entry.provider_id)
        << "\",\"model\":\"" << escape_json(entry.model)
        << "\",\"status\":\"" << escape_json(entry.status)
        << "\",\"target_label\":\"" << escape_json(entry.target_label)
        << "\",\"canonical_value\":\"" << escape_json(entry.canonical_value)
        << "\",\"rejection_reason\":\"" << escape_json(entry.rejection_reason)
        << "\",\"host_platform\":\"" << escape_json(entry.host_platform)
        << "\",\"environment_signature\":\"" << escape_json(entry.environment_signature)
        << "\",\"persisted_at\":\"" << escape_json(entry.persisted_at) << "\"}";
    return out.str();
}

std::string render_assist_correction_json(const AssistCorrectionRecord& entry) {
    std::ostringstream out;
    out << "{\"id\":\"" << escape_json(entry.id)
        << "\",\"original_prompt\":\"" << escape_json(entry.original_prompt)
        << "\",\"corrected_value\":\"" << escape_json(entry.corrected_value)
        << "\",\"category\":\"" << escape_json(entry.category)
        << "\",\"host_platform\":\"" << escape_json(entry.host_platform)
        << "\",\"persisted_at\":\"" << escape_json(entry.persisted_at) << "\"}";
    return out.str();
}

std::string render_assist_learning_json(const AssistLearningRecord& entry) {
    std::ostringstream out;
    out << "{\"id\":\"" << escape_json(entry.id)
        << "\",\"category\":\"" << escape_json(entry.category)
        << "\",\"prompt_fragment\":\"" << escape_json(entry.prompt_fragment)
        << "\",\"learned_value\":\"" << escape_json(entry.learned_value)
        << "\",\"host_platform\":\"" << escape_json(entry.host_platform)
        << "\",\"success_count\":" << entry.success_count
        << ",\"rejection_count\":" << entry.rejection_count
        << ",\"last_status\":\"" << escape_json(entry.last_status)
        << "\",\"persisted_at\":\"" << escape_json(entry.persisted_at) << "\"}";
    return out.str();
}

std::string render_host_assist_preference_json(const HostAssistPreferenceRecord& entry) {
    std::ostringstream out;
    out << "{\"id\":\"" << escape_json(entry.id)
        << "\",\"host_platform\":\"" << escape_json(entry.host_platform)
        << "\",\"provider_id\":\"" << escape_json(entry.provider_id)
        << "\",\"model\":\"" << escape_json(entry.model)
        << "\",\"preferred_shell_phrases\":" << render_string_array(entry.preferred_shell_phrases)
        << ",\"preferred_tool_routes\":" << render_string_array(entry.preferred_tool_routes)
        << ",\"preferred_recipe_routes\":" << render_string_array(entry.preferred_recipe_routes)
        << ",\"persisted_at\":\"" << escape_json(entry.persisted_at) << "\"}";
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

std::string render_legacy_sources_json(const MemorySnapshot& snapshot) {
    std::ostringstream out;
    out << "{\n  \"entries\": [\n";
    for (std::size_t index = 0; index < snapshot.legacy_sources.size(); ++index) {
        out << "    " << render_legacy_source_json(snapshot.legacy_sources[index]);
        if (index + 1 < snapshot.legacy_sources.size()) {
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
            << "\",\"source_map_path\":\"" << escape_json(entry.source_map_path)
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
            << ",\"next_step_assist_plan\":"
            << (entry.next_step_assist_plan.has_value() ? render_next_step_assist_plan_json(*entry.next_step_assist_plan) : "null")
            << ",\"case_summary_assist_plan\":"
            << (entry.case_summary_assist_plan.has_value() ? render_case_summary_assist_plan_json(*entry.case_summary_assist_plan) : "null")
            << ",\"freeform_assist_answer\":"
            << (entry.freeform_assist_answer.has_value() ? render_freeform_assist_answer_json(*entry.freeform_assist_answer) : "null")
            << ",\"recursive_diff_report\":"
            << (entry.recursive_diff_report.has_value() ? render_recursive_diff_json(*entry.recursive_diff_report) : "null")
            << ",\"security_audit\":"
            << (entry.security_audit.has_value() ? render_security_audit_json(*entry.security_audit) : "null")
            << ",\"language_resolution\":"
            << (entry.language_resolution.has_value() ? render_language_resolution_json(*entry.language_resolution) : "null")
            << ",\"uac_state\":"
            << (entry.uac_state.has_value() ? render_uac_state_json(*entry.uac_state) : "null")
            << ",\"postprocess_record\":"
            << (entry.postprocess_record.has_value() ? render_postprocess_record_json(*entry.postprocess_record) : "null")
            << ",\"legacy_source\":"
            << (entry.legacy_source.has_value() ? render_legacy_source_json(*entry.legacy_source) : "null")
            << ",\"legacy_bridge_report\":"
            << (entry.legacy_bridge_report.has_value() ? render_legacy_bridge_report_json(*entry.legacy_bridge_report) : "null")
            << ",\"query_session\":"
            << (entry.query_session.has_value() ? render_query_session_json(*entry.query_session) : "null")
            << ",\"legacy_research_artifacts\":[";
        for (std::size_t artifact_index = 0; artifact_index < entry.legacy_research_artifacts.size(); ++artifact_index) {
            if (artifact_index != 0) {
                out << ",";
            }
            out << render_legacy_research_artifact_json(entry.legacy_research_artifacts[artifact_index]);
        }
        out << "],\"legacy_correlations\":[";
        for (std::size_t correlation_index = 0; correlation_index < entry.legacy_correlations.size(); ++correlation_index) {
            if (correlation_index != 0) {
                out << ",";
            }
            out << render_legacy_correlation_json(entry.legacy_correlations[correlation_index]);
        }
        out << "],\"legacy_symbol_coverage\":[";
        for (std::size_t coverage_index = 0; coverage_index < entry.legacy_symbol_coverage.size(); ++coverage_index) {
            if (coverage_index != 0) {
                out << ",";
            }
            out << render_legacy_symbol_coverage_json(entry.legacy_symbol_coverage[coverage_index]);
        }
        out << "]";
        if (entry.legacy_recovery_status.has_value()) {
            out << ",\"legacy_recovery_status\":"
                << render_legacy_recovery_status_json(*entry.legacy_recovery_status);
        } else {
            out << ",\"legacy_recovery_status\":null";
        }
        out << ",\"review_artifact\":"
            << (entry.review_artifact.has_value() ? render_review_artifact_json(*entry.review_artifact) : "null");
        out << ",\"patch_proposal_artifact\":"
            << (entry.patch_proposal_artifact.has_value() ? render_patch_proposal_artifact_json(*entry.patch_proposal_artifact) : "null");
        out << ",\"recipe_authoring_artifact\":"
            << (entry.recipe_authoring_artifact.has_value() ? render_recipe_authoring_artifact_json(*entry.recipe_authoring_artifact) : "null");
        out << ",\"definition_answer\":"
            << (entry.definition_answer.has_value() ? render_definition_answer_json(*entry.definition_answer) : "null");
        out << ",\"neural_math_report\":"
            << (entry.neural_math_report.has_value() ? render_neural_math_report_json(*entry.neural_math_report) : "null");
        out << ",\"neural_route_report\":"
            << (entry.neural_route_report.has_value() ? render_neural_route_report_json(*entry.neural_route_report) : "null");
        out << ",\"stages\":[";
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
            << "\",\"normalized_concept\":\"" << escape_json(entry.normalized_concept)
            << "\",\"domain_hint\":\"" << escape_json(entry.domain_hint)
            << "\",\"summary\":\"" << escape_json(entry.summary)
            << "\",\"mapped_cpp_target\":\"" << escape_json(entry.mapped_cpp_target)
            << "\",\"semantic_family\":\"" << escape_json(entry.semantic_family)
            << "\",\"source\":\"" << escape_json(entry.source)
            << "\",\"source_type\":\"" << escape_json(entry.source_type)
            << "\",\"authority_tier\":\"" << escape_json(entry.authority_tier)
            << "\",\"confidence\":" << entry.confidence
            << ",\"scope\":\"" << escape_json(entry.scope)
            << "\",\"created_at\":\"" << escape_json(entry.created_at)
            << "\",\"expires_at\":\"" << escape_json(entry.expires_at) << "\"}";
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
    out << "],\n  \"operator_persona\": ";
    if (snapshot.operator_persona.has_value()) {
        const OperatorPersonaRecord& persona = *snapshot.operator_persona;
        out << "{\"preferred_label\":\"" << escape_json(persona.preferred_label)
            << "\",\"role_label\":\"" << escape_json(persona.role_label)
            << "\",\"local_username\":\"" << escape_json(persona.local_username)
            << "\",\"host_identifier\":\"" << escape_json(persona.host_identifier)
            << "\",\"last_source_map\":\"" << escape_json(persona.last_source_map)
            << "\",\"last_memory_root\":\"" << escape_json(persona.last_memory_root)
            << "\",\"self_description\":\"" << escape_json(persona.self_description)
            << "\",\"persona_mode\":\"" << escape_json(persona.persona_mode)
            << "\",\"tone_profile\":\"" << escape_json(persona.tone_profile)
            << "\",\"interaction_style\":\"" << escape_json(persona.interaction_style)
            << "\",\"safety_posture\":\"" << escape_json(persona.safety_posture)
            << "\",\"preferred_next_action_style\":\"" << escape_json(persona.preferred_next_action_style)
            << "\",\"custom_phrases\":[";
        for (std::size_t index = 0; index < persona.custom_phrases.size(); ++index) {
            if (index != 0) {
                out << ", ";
            }
            out << "\"" << escape_json(persona.custom_phrases[index]) << "\"";
        }
        out << "]}";
    } else {
        out << "null";
    }
    out << ",\n  \"shell_lexicon_overlay\": [";
    for (std::size_t index = 0; index < snapshot.shell_lexicon_overlay.size(); ++index) {
        const ShellLexiconEntry& entry = snapshot.shell_lexicon_overlay[index];
        if (index != 0) {
            out << ", ";
        }
        out << "{\"phrase\":\"" << escape_json(entry.phrase)
            << "\",\"canonical\":\"" << escape_json(entry.canonical)
            << "\",\"category\":\"" << escape_json(entry.category)
            << "\",\"confidence\":" << entry.confidence
            << ",\"clarification_required\":" << (entry.clarification_required ? "true" : "false")
            << ",\"correction_notes\":[";
        for (std::size_t note_index = 0; note_index < entry.correction_notes.size(); ++note_index) {
            if (note_index != 0) {
                out << ", ";
            }
            out << "\"" << escape_json(entry.correction_notes[note_index]) << "\"";
        }
        out << "]}";
    }
    out << "],\n  \"assist_learning_status\": {\"outcomes\":" << snapshot.assist_outcomes.size()
        << ",\"corrections\":" << snapshot.assist_corrections.size()
        << ",\"learned_routes\":" << snapshot.assist_learning.size() << "}\n}\n";
    return out.str();
}

std::string render_assist_memory_json(const MemorySnapshot& snapshot) {
    std::ostringstream out;
    out << "{\n  \"assist_outcomes\": [\n";
    for (std::size_t index = 0; index < snapshot.assist_outcomes.size(); ++index) {
        out << "    " << render_assist_outcome_json(snapshot.assist_outcomes[index]);
        if (index + 1 < snapshot.assist_outcomes.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "  ],\n  \"assist_corrections\": [\n";
    for (std::size_t index = 0; index < snapshot.assist_corrections.size(); ++index) {
        out << "    " << render_assist_correction_json(snapshot.assist_corrections[index]);
        if (index + 1 < snapshot.assist_corrections.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "  ],\n  \"assist_learning\": [\n";
    for (std::size_t index = 0; index < snapshot.assist_learning.size(); ++index) {
        out << "    " << render_assist_learning_json(snapshot.assist_learning[index]);
        if (index + 1 < snapshot.assist_learning.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "  ],\n  \"host_assist_preferences\": [\n";
    for (std::size_t index = 0; index < snapshot.host_assist_preferences.size(); ++index) {
        out << "    " << render_host_assist_preference_json(snapshot.host_assist_preferences[index]);
        if (index + 1 < snapshot.host_assist_preferences.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "  ]\n}\n";
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

std::string render_build_recipe_json(const BuildRecipe& entry) {
    std::ostringstream out;
    out << "{\"id\":\"" << escape_json(entry.id)
        << "\",\"acquisition_method\":\"" << escape_json(entry.acquisition_method)
        << "\",\"build_system\":\"" << escape_json(entry.build_system)
        << "\",\"supported_platforms\":" << render_string_array(entry.supported_platforms)
        << ",\"default_target\":\"" << escape_json(entry.default_target)
        << "\",\"install_target\":\"" << escape_json(entry.install_target)
        << "\",\"artifact_patterns\":" << render_string_array(entry.artifact_patterns)
        << ",\"install_output_patterns\":" << render_string_array(entry.install_output_patterns)
        << ",\"fallback_stage_patterns\":" << render_string_array(entry.fallback_stage_patterns)
        << ",\"dependency_hints\":" << render_string_array(entry.dependency_hints)
        << ",\"configure_arguments\":" << render_string_array(entry.configure_arguments)
        << ",\"supports_install\":" << (entry.supports_install ? "true" : "false")
        << ",\"copy_artifacts_on_install\":" << (entry.copy_artifacts_on_install ? "true" : "false")
        << "}";
    return out.str();
}

std::string render_authored_recipes_json(const MemorySnapshot& snapshot) {
    std::ostringstream out;
    out << "{\n  \"entries\": [\n";
    for (std::size_t index = 0; index < snapshot.authored_recipes.size(); ++index) {
        const AuthoredRecipeRecord& entry = snapshot.authored_recipes[index];
        out << "    {\"recipe\":" << render_build_recipe_json(entry.recipe)
            << ",\"resolved_source_path\":\"" << escape_json(entry.resolved_source_path)
            << "\",\"canonical_name\":\"" << escape_json(entry.canonical_name)
            << "\",\"origin_provider\":\"" << escape_json(entry.origin_provider)
            << "\",\"origin_model\":\"" << escape_json(entry.origin_model)
            << "\",\"evidence_summary\":" << render_string_array(entry.evidence_summary)
            << ",\"validation_status\":\"" << escape_json(entry.validation_status)
            << "\",\"last_validation_summary\":\"" << escape_json(entry.last_validation_summary)
            << "\",\"last_validation_log\":\"" << escape_json(entry.last_validation_log)
            << "\",\"last_artifact\":\"" << escape_json(entry.last_artifact)
            << "\",\"authoring_run_id\":\"" << escape_json(entry.authoring_run_id)
            << "\",\"active_scope\":\"" << escape_json(entry.active_scope)
            << "\",\"active\":" << (entry.active ? "true" : "false")
            << ",\"repair_attempted\":" << (entry.repair_attempted ? "true" : "false")
            << ",\"persisted_at\":\"" << escape_json(entry.persisted_at) << "\"}";
        if (index + 1 < snapshot.authored_recipes.size()) {
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

std::string render_double_array(const std::vector<double>& values) {
    std::ostringstream out;
    out << "[";
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) {
            out << ",";
        }
        out << values[index];
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
            << ",\"math_attributions\":" << render_math_attributions_json(entry.math_attributions)
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

std::string replace_all_copy(std::string value, std::string_view needle, std::string_view replacement) {
    std::size_t offset = 0;
    while ((offset = value.find(needle, offset)) != std::string::npos) {
        value.replace(offset, needle.size(), replacement);
        offset += replacement.size();
    }
    return value;
}

std::string human_readable_stage_id(std::string_view stage_id) {
    if (stage_id == "xProcessingCache") {
        return "Cache.PrepareWorkspace";
    }
    if (stage_id == "x.Define.Low") {
        return "Intent.DecodeInstruction";
    }
    if (stage_id == "x.DisplayPriorityProcessingGate") {
        return "Knowledge.EvidenceRanking";
    }
    if (stage_id == "x.DisplayFeedBackLoop") {
        return "Memory.FeedbackReview";
    }
    if (stage_id == "x.Store") {
        return "Memory.StoreArtifact";
    }
    return std::string(stage_id);
}

std::string render_stage_label(const TzeStageRecord& stage) {
    const std::string readable = human_readable_stage_id(stage.stage_id);
    if (readable == stage.stage_id) {
        return stage.stage_id;
    }
    return readable + " (legacy=" + stage.stage_id + ")";
}

std::string translate_storage_names(std::string text) {
    text = replace_all_copy(std::move(text), "xMap_Temp", "Storage.Temporary");
    text = replace_all_copy(std::move(text), "xMap_Perm", "Storage.Permanent");
    text = replace_all_copy(std::move(text), "xMap_Core", "Storage.Core");
    return text;
}

std::string human_readable_storage_text(std::string_view value) {
    const std::string raw(value);
    if (raw.rfind("x.Store(", 0) == 0 && !raw.empty() && raw.back() == ')') {
        const std::string inner = raw.substr(8, raw.size() - 9);
        return "Memory.StoreArtifact(" + translate_storage_names(inner) + ") (legacy=" + raw + ")";
    }
    return translate_storage_names(raw);
}

std::string join_stage_fields_for_display(const std::vector<std::string>& values) {
    std::ostringstream out;
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) {
            out << ", ";
        }
        out << human_readable_storage_text(values[index]);
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

std::string render_legacy_source_summary(const LegacySourceRecord& entry) {
    std::ostringstream out;
    out << " - Source: " << entry.source_label << "\n";
    out << " - Path: " << entry.source_path << "\n";
    out << " - Kind: " << entry.source_kind << "\n";
    out << " - Lines: " << entry.line_count << "\n";
    out << " - Sections: " << entry.section_count << "\n";
    out << " - Symbols: " << entry.symbol_count << "\n";
    return out.str();
}

std::string render_legacy_bridge_summary(const LegacyBridgeReport& entry) {
    std::ostringstream out;
    out << " - Status: " << entry.status << "\n";
    out << " - Mode: " << entry.bridge_mode << "\n";
    out << " - Summary: " << entry.summary << "\n";
    if (!entry.bridge_steps.empty()) {
        out << " - Bridge steps:\n";
        for (const std::string& step : entry.bridge_steps) {
            out << "   - " << step << "\n";
        }
    }
    if (!entry.safe_actions.empty()) {
        out << " - Safe actions:\n";
        for (const std::string& action : entry.safe_actions) {
            out << "   - " << action << "\n";
        }
    }
    if (!entry.research_actions.empty()) {
        out << " - Research actions:\n";
        for (const std::string& action : entry.research_actions) {
            out << "   - " << action << "\n";
        }
    }
    if (!entry.blocked_actions.empty()) {
        out << " - Blocked actions:\n";
        for (const std::string& action : entry.blocked_actions) {
            out << "   - " << action << "\n";
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

std::string render_next_step_assist_plan_summary(const NextStepAssistPlan& plan) {
    std::ostringstream out;
    out << " - Task: " << plan.task_id << "\n";
    out << " - Provider: " << plan.provider_id;
    if (!plan.model.empty()) {
        out << " (" << plan.model << ")";
    }
    out << "\n";
    out << " - Status: " << plan.status << "\n";
    out << " - Suggested next step: " << plan.suggested_next_step << "\n";
    if (!plan.safer_alternative.empty()) {
        out << " - Safer alternative: " << plan.safer_alternative << "\n";
    }
    out << " - Confidence: " << plan.confidence << "\n";
    if (!plan.rationale.empty()) {
        out << " - Rationale: " << plan.rationale << "\n";
    }
    if (!plan.warnings.empty()) {
        out << " - Warnings: " << join_stage_fields(plan.warnings) << "\n";
    }
    out << " - Validated: " << (plan.validated ? "yes" : "no") << "\n";
    return out.str();
}

std::string render_case_summary_assist_plan_summary(const CaseSummaryAssistPlan& plan) {
    std::ostringstream out;
    out << " - Task: " << plan.task_id << "\n";
    out << " - Provider: " << plan.provider_id;
    if (!plan.model.empty()) {
        out << " (" << plan.model << ")";
    }
    out << "\n";
    out << " - Status: " << plan.status << "\n";
    if (!plan.summary_title.empty()) {
        out << " - Title: " << plan.summary_title << "\n";
    }
    out << " - Executive summary: " << plan.executive_summary << "\n";
    if (!plan.highlights.empty()) {
        out << " - Highlights: " << join_stage_fields(plan.highlights) << "\n";
    }
    out << " - Confidence: " << plan.confidence << "\n";
    out << " - Validated: " << (plan.validated ? "yes" : "no") << "\n";
    return out.str();
}

std::string render_review_artifact_summary(const ReviewArtifact& entry) {
    std::ostringstream out;
    out << " - Target: " << entry.target << "\n";
    out << " - Status: " << entry.status << "\n";
    out << " - Summary: " << entry.summary << "\n";
    out << " - Findings: " << entry.findings.size() << "\n";
    for (const ReviewFinding& finding : entry.findings) {
        out << "   - [" << finding.severity << "] " << finding.category << " :: " << finding.summary;
        if (!finding.file_path.empty()) {
            out << " (" << finding.file_path;
            if (finding.line != 0) {
                out << ":" << finding.line;
            }
            out << ")";
        }
        out << "\n";
    }
    if (!entry.suggested_tests.empty()) {
        out << " - Suggested tests: " << join_stage_fields(entry.suggested_tests) << "\n";
    }
    return out.str();
}

std::string render_patch_proposal_artifact_summary(const PatchProposalArtifact& entry) {
    std::ostringstream out;
    out << " - Target: " << entry.target << "\n";
    out << " - Status: " << entry.status << "\n";
    out << " - Summary: " << entry.summary << "\n";
    if (!entry.target_files.empty()) {
        out << " - Target files: " << join_stage_fields(entry.target_files) << "\n";
    }
    if (!entry.intended_behavior_changes.empty()) {
        out << " - Intended changes: " << join_stage_fields(entry.intended_behavior_changes) << "\n";
    }
    if (!entry.acceptance_tests.empty()) {
        out << " - Acceptance tests: " << join_stage_fields(entry.acceptance_tests) << "\n";
    }
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
    if (!entry.decompression_candidates.empty()) {
        out << " - Decompression ladder:\n";
        for (const DecompressionCandidate& candidate : entry.decompression_candidates) {
            out << "   - " << candidate.label << " [" << candidate.status << "] confidence="
                << std::fixed << std::setprecision(2) << candidate.confidence;
            if (!candidate.notes.empty()) {
                out << " notes=" << join_stage_fields(candidate.notes);
            }
            out << "\n";
        }
    }
    if (!entry.research_notes.empty()) {
        out << " - Research notes: " << join_stage_fields(entry.research_notes) << "\n";
    }
    if (!entry.reasoning_trace.empty()) {
        out << " - Trace: " << join_stage_fields(entry.reasoning_trace) << "\n";
    }
    return out.str();
}

std::string render_uac_state_summary(const UacStateRecord& entry) {
    std::ostringstream out;
    if (!entry.normalized_prompt.empty()) {
        out << " - Normalized prompt: " << entry.normalized_prompt << "\n";
    }
    if (!entry.query_tokens.empty()) {
        out << " - Tokens: " << join_stage_fields(entry.query_tokens) << "\n";
    }
    if (!entry.instruction_family_hint.empty()) {
        out << " - Instruction family: " << entry.instruction_family_hint << "\n";
    }
    out << " - Epoch: " << entry.epoch_marker << "\n";
    out << " - Machine: " << entry.machine_identifier << "\n";
    out << " - Namespace: " << human_readable_storage_text(entry.store_namespace) << "\n";
    out << " - Search namespace: " << human_readable_storage_text(entry.search_namespace) << "\n";
    out << " - GENx: " << entry.genx_token_value << "\n";
    out << " - Compression: " << entry.compression_label << "\n";
    if (!entry.chapter_series_label.empty()) {
        out << " - Chapter series: " << entry.chapter_series_label << "\n";
    }
    if (!entry.epoch_tier_label.empty()) {
        out << " - Epoch tier: " << entry.epoch_tier_label << "\n";
    }
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
    if (!entry.deletion_discrepancies.empty()) {
        out << " - Deletion discrepancies: " << join_stage_fields(entry.deletion_discrepancies) << "\n";
    }
    if (!entry.search_context_habits.empty()) {
        out << " - Search context habits: " << join_stage_fields(entry.search_context_habits) << "\n";
    }
    if (!entry.time_on_site_traits.empty()) {
        out << " - Time-on-site traits: " << join_stage_fields(entry.time_on_site_traits) << "\n";
    }
    if (!entry.reasoning_trace.empty()) {
        out << " - Trace: " << join_stage_fields(entry.reasoning_trace) << "\n";
    }
    return out.str();
}

std::string render_freeform_assist_answer_summary(const FreeformAssistAnswer& answer) {
    std::ostringstream out;
    out << " - Task: " << answer.task_id << "\n";
    out << " - Provider: " << answer.provider_id;
    if (!answer.model.empty()) {
        out << " (" << answer.model << ")";
    }
    out << "\n";
    out << " - Status: " << answer.status << "\n";
    out << " - Answer: " << answer.answer << "\n";
    out << " - Confidence: " << answer.confidence << "\n";
    if (!answer.rationale.empty()) {
        out << " - Rationale: " << answer.rationale << "\n";
    }
    if (!answer.suggested_commands.empty()) {
        out << " - Suggested commands: " << join_stage_fields(answer.suggested_commands) << "\n";
    }
    if (!answer.safety_warnings.empty()) {
        out << " - Safety warnings: " << join_stage_fields(answer.safety_warnings) << "\n";
    }
    if (!answer.used_context.empty()) {
        out << " - Used context: " << join_stage_fields(answer.used_context) << "\n";
    }
    return out.str();
}

std::string render_postprocess_record_summary(const PostProcessRecord& entry) {
    std::ostringstream out;
    out << " - Status: " << entry.status << "\n";
    if (!entry.final_artifact_summary.empty()) {
        out << " - Final artifact: " << entry.final_artifact_summary << "\n";
    }
    if (!entry.provenance.empty()) {
        out << " - Provenance: " << entry.provenance << "\n";
    }
    if (!entry.retention_decision.empty()) {
        out << " - Retention: " << entry.retention_decision << "\n";
    }
    out << " - Dropped transient chain: " << (entry.dropped_transient_chain ? "yes" : "no") << "\n";
    if (!entry.retained_fields.empty()) {
        out << " - Retained fields: " << join_stage_fields(entry.retained_fields) << "\n";
    }
    if (!entry.discarded_fields.empty()) {
        out << " - Discarded fields: " << join_stage_fields(entry.discarded_fields) << "\n";
    }
    return out.str();
}

std::string render_recipe_authoring_artifact_summary(const RecipeAuthoringArtifact& entry) {
    std::ostringstream out;
    out << " - Status: " << entry.status << "\n";
    if (!entry.generated_recipe_id.empty()) {
        out << " - Recipe id: " << entry.generated_recipe_id << "\n";
    }
    if (!entry.generated_build_system.empty()) {
        out << " - Build system: " << entry.generated_build_system << "\n";
    }
    out << " - Activated: " << (entry.activated ? "yes" : "no") << "\n";
    out << " - Repair attempted: " << (entry.repair_attempted ? "yes" : "no") << "\n";
    if (!entry.ranked_evidence.empty()) {
        out << " - Ranked evidence: " << join_stage_fields(entry.ranked_evidence) << "\n";
    }
    if (!entry.validation_feedback.empty()) {
        out << " - Validation feedback: " << join_stage_fields(entry.validation_feedback) << "\n";
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

std::string current_host_platform() {
#if defined(__APPLE__)
    return "macos";
#elif defined(__linux__)
    return "linux";
#else
    return "unknown";
#endif
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
    paths.authored_recipes_path = paths.root / "authored_recipes.json";
    paths.native_tools_path = paths.root / "native_tools.json";
    paths.language_contexts_path = paths.root / "language_contexts.json";
    paths.uac_states_path = paths.root / "uac_states.json";
    paths.security_audits_path = paths.root / "security_audits.json";
    paths.legacy_sources_path = paths.root / "legacy_sources.json";
    paths.assist_memory_path = paths.root / "assist_memory.json";
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
    ensure_file_exists(paths.authored_recipes_path, "{\n  \"entries\": []\n}\n");
    ensure_file_exists(paths.native_tools_path, "{\n  \"entries\": []\n}\n");
    ensure_file_exists(paths.language_contexts_path, "{\n  \"entries\": []\n}\n");
    ensure_file_exists(paths.uac_states_path, "{\n  \"entries\": []\n}\n");
    ensure_file_exists(paths.security_audits_path, "{\n  \"entries\": []\n}\n");
    ensure_file_exists(paths.legacy_sources_path, "{\n  \"entries\": []\n}\n");
    ensure_file_exists(paths.assist_memory_path,
                       "{\n  \"assist_outcomes\": [],\n  \"assist_corrections\": [],\n  \"assist_learning\": [],\n  \"host_assist_preferences\": []\n}\n");
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
    const std::string operator_persona_text = extract_json_object(preferences_json, "operator_persona");
    if (!operator_persona_text.empty()) {
        snapshot.operator_persona = parse_operator_persona_entry(operator_persona_text);
    }
    for (const std::string& object_text : extract_object_entries(preferences_json, "shell_lexicon_overlay")) {
        snapshot.shell_lexicon_overlay.push_back(parse_shell_lexicon_entry(object_text));
    }

    const std::string projects_json = read_text(snapshot.paths.projects_path);
    for (const std::string& object_text : extract_object_entries(projects_json, "entries")) {
        snapshot.projects.push_back(parse_project_entry(object_text));
    }
    for (const std::string& object_text : extract_object_entries(projects_json, "learned_recipes")) {
        snapshot.learned_recipes.push_back(parse_learned_recipe_entry(object_text));
    }

    const std::string authored_recipes_json = read_text(snapshot.paths.authored_recipes_path);
    for (const std::string& object_text : extract_object_entries(authored_recipes_json, "entries")) {
        snapshot.authored_recipes.push_back(parse_authored_recipe_entry(object_text));
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

    const std::string legacy_sources_json = read_text(snapshot.paths.legacy_sources_path);
    for (const std::string& object_text : extract_object_entries(legacy_sources_json, "entries")) {
        snapshot.legacy_sources.push_back(parse_legacy_source_entry(object_text));
    }

    const std::string assist_memory_json = read_text(snapshot.paths.assist_memory_path);
    for (const std::string& object_text : extract_object_entries(assist_memory_json, "assist_outcomes")) {
        snapshot.assist_outcomes.push_back(parse_assist_outcome_entry(object_text));
    }
    for (const std::string& object_text : extract_object_entries(assist_memory_json, "assist_corrections")) {
        snapshot.assist_corrections.push_back(parse_assist_correction_entry(object_text));
    }
    for (const std::string& object_text : extract_object_entries(assist_memory_json, "assist_learning")) {
        snapshot.assist_learning.push_back(parse_assist_learning_entry(object_text));
    }
    for (const std::string& object_text : extract_object_entries(assist_memory_json, "host_assist_preferences")) {
        snapshot.host_assist_preferences.push_back(parse_host_assist_preference_entry(object_text));
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
    write_text(snapshot.paths.authored_recipes_path, render_authored_recipes_json(snapshot));
    write_text(snapshot.paths.native_tools_path, render_native_tools_json(snapshot));
    write_text(snapshot.paths.language_contexts_path, render_language_contexts_json(snapshot));
    write_text(snapshot.paths.uac_states_path, render_uac_states_json(snapshot));
    write_text(snapshot.paths.security_audits_path, render_security_audits_json(snapshot));
    write_text(snapshot.paths.legacy_sources_path, render_legacy_sources_json(snapshot));
    write_text(snapshot.paths.assist_memory_path, render_assist_memory_json(snapshot));
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
    if (!entry->source_map_path.empty()) {
        out << " - Source map: " << entry->source_map_path << "\n";
    }
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
    if (entry->next_step_assist_plan.has_value()) {
        out << "Next-step assist plan:\n";
        out << render_next_step_assist_plan_summary(*entry->next_step_assist_plan);
    }
    if (entry->case_summary_assist_plan.has_value()) {
        out << "Case summary assist plan:\n";
        out << render_case_summary_assist_plan_summary(*entry->case_summary_assist_plan);
    }
    if (entry->freeform_assist_answer.has_value()) {
        out << "Freeform assist answer:\n";
        out << render_freeform_assist_answer_summary(*entry->freeform_assist_answer);
    }
    if (entry->recursive_diff_report.has_value()) {
        out << render_recursive_diff_summary(*entry->recursive_diff_report);
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
    if (entry->postprocess_record.has_value()) {
        out << "Postprocess:\n";
        out << render_postprocess_record_summary(*entry->postprocess_record);
    }
    if (entry->recipe_authoring_artifact.has_value()) {
        out << "Recipe authoring:\n";
        out << render_recipe_authoring_artifact_summary(*entry->recipe_authoring_artifact);
    }
    if (entry->legacy_source.has_value()) {
        out << "Legacy source:\n";
        out << render_legacy_source_summary(*entry->legacy_source);
    }
    if (entry->legacy_bridge_report.has_value()) {
        out << "Legacy bridge:\n";
        out << render_legacy_bridge_summary(*entry->legacy_bridge_report);
    }
    if (entry->legacy_recovery_status.has_value()) {
        out << "Legacy recovery:\n";
        out << " - Implemented: " << entry->legacy_recovery_status->implemented_count << "\n";
        out << " - Partial: " << entry->legacy_recovery_status->partial_count << "\n";
        out << " - Missing: " << entry->legacy_recovery_status->missing_count << "\n";
        out << " - Research-only: " << entry->legacy_recovery_status->research_only_count << "\n";
        out << " - Blocked: " << entry->legacy_recovery_status->blocked_count << "\n";
    }
    if (entry->query_session.has_value()) {
        out << "Query session:\n";
        out << render_query_session_summary(*entry->query_session);
    }
    if (entry->definition_answer.has_value()) {
        out << "Definition answer:\n";
        out << " - Query: " << entry->definition_answer->query << "\n";
        if (!entry->definition_answer->summary.empty()) {
            out << " - Summary: " << entry->definition_answer->summary << "\n";
        }
        if (!entry->definition_answer->domain_hint.empty()) {
            out << " - Domain: " << entry->definition_answer->domain_hint << "\n";
        }
        if (!entry->definition_answer->selected_source_type.empty()) {
            out << " - Source: " << entry->definition_answer->selected_source_type;
            if (!entry->definition_answer->selected_source_label.empty()) {
                out << " (" << entry->definition_answer->selected_source_label << ")";
            }
            out << "\n";
        }
        if (!entry->definition_answer->selected_authority_tier.empty()) {
            out << " - Authority tier: " << entry->definition_answer->selected_authority_tier << "\n";
        }
        if (!entry->definition_answer->comparison_rationale.empty()) {
            out << " - Comparison rationale: " << entry->definition_answer->comparison_rationale << "\n";
        }
        if (!entry->definition_answer->math_attributions.empty()) {
            out << " - Math attributions:\n";
            for (const MathAttribution& attribution : entry->definition_answer->math_attributions) {
                out << "   - " << attribution.name
                    << " raw=" << attribution.raw_value
                    << " weight=" << attribution.weight
                    << " contribution=" << attribution.contribution;
                if (!attribution.source.empty()) {
                    out << " source=" << attribution.source;
                }
                if (!attribution.rationale.empty()) {
                    out << " | " << attribution.rationale;
                }
                out << "\n";
            }
        }
    }
    if (entry->neural_math_report.has_value()) {
        const NeuralMathReport& neural = *entry->neural_math_report;
        out << "Neural math:\n";
        out << " - Status: " << neural.status << "\n";
        out << " - Model: " << neural.model_type << "\n";
        out << " - Dataset: " << neural.dataset << "\n";
        if (!neural.summary.empty()) {
            out << " - Summary: " << neural.summary << "\n";
        }
        out << " - Accuracy: " << neural.accuracy << "\n";
        if (!neural.math_trace.empty()) {
            out << " - Trace: " << join_stage_fields_for_display(neural.math_trace) << "\n";
        }
    }
    if (entry->neural_route_report.has_value()) {
        const NeuralRouteReport& neural = *entry->neural_route_report;
        out << "Neural route:\n";
        out << " - Status: " << neural.status << "\n";
        out << " - Input: " << neural.input_path << "\n";
        out << " - Packets: " << neural.packet_count << "\n";
        out << " - Flows: " << neural.flow_count << "\n";
        if (!neural.summary.empty()) {
            out << " - Summary: " << neural.summary << "\n";
        }
        if (!neural.predictions.empty()) {
            const NeuralRoutePrediction& top = neural.predictions.front();
            out << " - Top label: " << top.label << " confidence=" << top.confidence << "\n";
            if (!top.attributions.empty()) {
                out << " - Math attributions:\n";
                for (const MathAttribution& attribution : top.attributions) {
                    out << "   - " << attribution.name
                        << " raw=" << attribution.raw_value
                        << " weight=" << attribution.weight
                        << " contribution=" << attribution.contribution;
                    if (!attribution.source.empty()) {
                        out << " source=" << attribution.source;
                    }
                    if (!attribution.rationale.empty()) {
                        out << " | " << attribution.rationale;
                    }
                    out << "\n";
                }
            }
        }
        if (!neural.artifact_path.empty()) {
            out << " - Artifact: " << neural.artifact_path << "\n";
        }
    }
    if (entry->review_artifact.has_value()) {
        out << "Review artifact:\n";
        out << render_review_artifact_summary(*entry->review_artifact);
    }
    if (entry->patch_proposal_artifact.has_value()) {
        out << "Patch proposal artifact:\n";
        out << render_patch_proposal_artifact_summary(*entry->patch_proposal_artifact);
    }
    out << "Stages:\n";
    for (const TzeStageRecord& stage : entry->stages) {
        out << " - " << render_stage_label(stage) << " [" << stage.status << "] " << stage.module;
        if (!stage.detail.empty()) {
            out << " | " << human_readable_storage_text(stage.detail);
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
            out << "   inputs: " << join_stage_fields_for_display(stage.inputs) << "\n";
        }
        if (!stage.outputs.empty()) {
            out << "   outputs: " << join_stage_fields_for_display(stage.outputs) << "\n";
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
            stage_changes.push_back(render_stage_label(left_stage) + ": removed from right-hand run");
            continue;
        }
        const TzeStageRecord& right_stage = right_it->second;
        if (left_stage.status != right_stage.status || left_stage.detail != right_stage.detail ||
            left_stage.module != right_stage.module || left_stage.inputs != right_stage.inputs ||
            left_stage.outputs != right_stage.outputs || !same_stage_provenance(left_stage, right_stage)) {
            std::ostringstream line;
            line << render_stage_label(left_stage) << ": [" << left_stage.status << "] -> [" << right_stage.status << "]";
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
        if (left_stages.find(stage_id) == left_stages.end()) {
            stage_changes.push_back(render_stage_label(right_stage) + ": added in right-hand run");
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
        const std::string label = human_readable_stage_id(stage_id);
        const std::string display = label == stage_id
            ? std::string(stage_id)
            : label + "` (legacy=`" + std::string(stage_id) + ")";
        if (lhs.status == rhs.status && lhs.detail == rhs.detail && lhs.inputs == rhs.inputs &&
            lhs.outputs == rhs.outputs && same_stage_provenance(lhs, rhs)) {
            return;
        }
        if (!same_stage_provenance(lhs, rhs) &&
            lhs.status == rhs.status && lhs.detail == rhs.detail && lhs.inputs == rhs.inputs && lhs.outputs == rhs.outputs) {
            stage_lines.push_back("`" + display + "` kept the same runtime behavior, but its source-backed origin changed.");
            return;
        }
        if (stage_id == "x.Define.Low") {
            stage_lines.push_back("`Intent.DecodeInstruction` (legacy=`x.Define.Low`) changed, so intent decoding or target selection differed between the two runs.");
            return;
        }
        if (stage_id == "x.DisplayPriorityProcessingGate") {
            stage_lines.push_back("`Knowledge.EvidenceRanking` (legacy=`x.DisplayPriorityProcessingGate`) changed, so Omni reordered knowledge sources or module preferences.");
            return;
        }
        if (stage_id == "x.DisplayFeedBackLoop") {
            stage_lines.push_back("`Memory.FeedbackReview` (legacy=`x.DisplayFeedBackLoop`) changed, so the later run used different prior outcomes or learned context.");
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
            stage_lines.push_back("`Memory.StoreArtifact` (legacy=`x.Store`) changed, so persistence targets or saved results differed between the runs.");
            return;
        }
        if (stage_id == "xProcessingCache") {
            stage_lines.push_back("`Cache.PrepareWorkspace` (legacy=`xProcessingCache`) changed, so the run opened or reused a different cache/work-buffer state.");
            return;
        }
        stage_lines.push_back("`" + display + "` changed between runs.");
    };

    for (const auto& [stage_id, left_stage] : left_stages) {
        const auto it = right_stages.find(stage_id);
        if (it == right_stages.end()) {
            stage_lines.push_back("`" + render_stage_label(left_stage) + "` was present in the left run but missing on the right.");
            continue;
        }
        explain_stage(stage_id, left_stage, it->second);
    }
    for (const auto& [stage_id, right_stage] : right_stages) {
        if (left_stages.find(stage_id) == left_stages.end()) {
            stage_lines.push_back("`" + render_stage_label(right_stage) + "` was added in the right-hand run.");
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
    if (!entry->source_map_path.empty()) {
        report << "- Source map: " << entry->source_map_path << "\n";
    }
    report << "- Reasoning provider: " << entry->reasoning_provider << "\n";
    if (entry->provider_probe_report.has_value()) {
        report << "\n## Provider Probe\n\n";
        report << render_provider_probe_summary(*entry->provider_probe_report) << "\n";
    }
    if (!entry->assist_status.empty() || entry->assist_annotation.has_value() || entry->command_assist_plan.has_value() ||
        entry->tool_assist_plan.has_value() ||
        entry->build_assist_plan.has_value() ||
        entry->freeform_assist_answer.has_value()) {
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
        if (entry->freeform_assist_answer.has_value()) {
            report << render_freeform_assist_answer_summary(*entry->freeform_assist_answer) << "\n";
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
    if (entry->postprocess_record.has_value()) {
        report << "\n## Postprocess\n\n";
        report << render_postprocess_record_summary(*entry->postprocess_record) << "\n";
    }
    if (entry->recipe_authoring_artifact.has_value()) {
        report << "\n## Recipe Authoring\n\n";
        report << render_recipe_authoring_artifact_summary(*entry->recipe_authoring_artifact) << "\n";
    }
    if (entry->legacy_source.has_value()) {
        report << "\n## Legacy Source\n\n";
        report << render_legacy_source_summary(*entry->legacy_source) << "\n";
    }
    if (entry->legacy_bridge_report.has_value()) {
        report << "\n## Legacy Bridge\n\n";
        report << render_legacy_bridge_summary(*entry->legacy_bridge_report) << "\n";
    }
    if (entry->legacy_recovery_status.has_value()) {
        report << "\n## Legacy Recovery\n\n";
        report << "- Implemented: " << entry->legacy_recovery_status->implemented_count << "\n";
        report << "- Partial: " << entry->legacy_recovery_status->partial_count << "\n";
        report << "- Missing: " << entry->legacy_recovery_status->missing_count << "\n";
        report << "- Research-only: " << entry->legacy_recovery_status->research_only_count << "\n";
        report << "- Blocked: " << entry->legacy_recovery_status->blocked_count << "\n";
        for (const std::string& line : entry->legacy_recovery_status->summary_lines) {
            report << "- " << line << "\n";
        }
    }
    if (entry->query_session.has_value()) {
        report << "\n## Query Session\n\n";
        report << render_query_session_summary(*entry->query_session) << "\n";
    }
    if (entry->definition_answer.has_value()) {
        report << "\n## Definition Answer\n\n";
        report << "- Query: " << entry->definition_answer->query << "\n";
        if (!entry->definition_answer->summary.empty()) {
            report << "- Summary: " << entry->definition_answer->summary << "\n";
        }
        if (!entry->definition_answer->domain_hint.empty()) {
            report << "- Domain: " << entry->definition_answer->domain_hint << "\n";
        }
        if (!entry->definition_answer->selected_source_type.empty()) {
            report << "- Source: " << entry->definition_answer->selected_source_type << "\n";
        }
        if (!entry->definition_answer->selected_source_label.empty()) {
            report << "- Source label: " << entry->definition_answer->selected_source_label << "\n";
        }
        if (!entry->definition_answer->selected_authority_tier.empty()) {
            report << "- Authority tier: " << entry->definition_answer->selected_authority_tier << "\n";
        }
        if (!entry->definition_answer->comparison_rationale.empty()) {
            report << "- Comparison rationale: " << entry->definition_answer->comparison_rationale << "\n";
        }
        for (const MathAttribution& attribution : entry->definition_answer->math_attributions) {
            report << "- Math attribution: " << attribution.name
                   << " raw=" << attribution.raw_value
                   << " weight=" << attribution.weight
                   << " contribution=" << attribution.contribution;
            if (!attribution.source.empty()) {
                report << " source=" << attribution.source;
            }
            if (!attribution.rationale.empty()) {
                report << " | " << attribution.rationale;
            }
            report << "\n";
        }
    }
    if (entry->neural_math_report.has_value()) {
        const NeuralMathReport& neural = *entry->neural_math_report;
        report << "\n## Neural Math\n\n";
        report << "- Status: " << neural.status << "\n";
        report << "- Model: " << neural.model_type << "\n";
        report << "- Dataset: " << neural.dataset << "\n";
        report << "- Epochs: " << neural.epochs_ran << " / " << neural.epochs_requested << "\n";
        report << "- Learning rate: " << neural.learning_rate << "\n";
        report << "- Accuracy: " << neural.accuracy << "\n";
        if (!neural.summary.empty()) {
            report << "- Summary: " << neural.summary << "\n";
        }
        for (const std::string& line : neural.math_trace) {
            report << "- Trace: " << line << "\n";
        }
    }
    if (entry->neural_route_report.has_value()) {
        const NeuralRouteReport& neural = *entry->neural_route_report;
        report << "\n## Neural Signal Router\n\n";
        report << "- Status: " << neural.status << "\n";
        report << "- Input: " << neural.input_path << "\n";
        report << "- Packets: " << neural.packet_count << "\n";
        report << "- Flows: " << neural.flow_count << "\n";
        if (!neural.summary.empty()) {
            report << "- Summary: " << neural.summary << "\n";
        }
        if (!neural.artifact_path.empty()) {
            report << "- Artifact: " << neural.artifact_path << "\n";
        }
        for (const NeuralRoutePrediction& prediction : neural.predictions) {
            report << "- Prediction: " << prediction.label
                   << " confidence=" << prediction.confidence
                   << " | " << prediction.rationale << "\n";
            for (const MathAttribution& attribution : prediction.attributions) {
                report << "- Math attribution: " << attribution.name
                       << " raw=" << attribution.raw_value
                       << " weight=" << attribution.weight
                       << " contribution=" << attribution.contribution;
                if (!attribution.source.empty()) {
                    report << " source=" << attribution.source;
                }
                if (!attribution.rationale.empty()) {
                    report << " | " << attribution.rationale;
                }
                report << "\n";
            }
        }
    }
    if (entry->review_artifact.has_value()) {
        report << "\n## Review Artifact\n\n";
        report << render_review_artifact_summary(*entry->review_artifact) << "\n";
    }
    if (entry->patch_proposal_artifact.has_value()) {
        report << "\n## Patch Proposal Artifact\n\n";
        report << render_patch_proposal_artifact_summary(*entry->patch_proposal_artifact) << "\n";
    }

    report << "\n## Stage Trace\n\n";
    for (const TzeStageRecord& stage : entry->stages) {
        report << "- " << render_stage_label(stage) << " [" << stage.status << "] " << stage.module << "\n";
        if (!stage.detail.empty()) {
            report << "  - Detail: " << human_readable_storage_text(stage.detail) << "\n";
        }
        const std::string source_ref = render_stage_source(stage);
        if (!source_ref.empty()) {
            report << "  - Source: " << source_ref << "\n";
        }
        if (!stage.source_excerpt.empty()) {
            report << "  - Source excerpt: `" << stage.source_excerpt << "`\n";
        }
        if (!stage.inputs.empty()) {
            report << "  - Inputs: " << join_stage_fields_for_display(stage.inputs) << "\n";
        }
        if (!stage.outputs.empty()) {
            report << "  - Outputs: " << join_stage_fields_for_display(stage.outputs) << "\n";
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

std::string MemoryStore::prune_expired(MemorySnapshot& snapshot) const {
    const std::size_t definitions_before = snapshot.definitions.size();
    const std::size_t history_before = snapshot.history.size();
    snapshot.definitions.erase(
        std::remove_if(snapshot.definitions.begin(),
                       snapshot.definitions.end(),
                       [](const StoredDefinition& entry) {
                           return is_expired(entry);
                       }),
        snapshot.definitions.end());
    snapshot.history.erase(
        std::remove_if(snapshot.history.begin(),
                       snapshot.history.end(),
                       [](const MemoryHistoryEntry& entry) {
                           return is_expired(entry);
                       }),
        snapshot.history.end());

    std::ostringstream out;
    out << "Pruned expired temporary memory: definitions_removed="
        << (definitions_before - snapshot.definitions.size())
        << ", history_removed=" << (history_before - snapshot.history.size()) << ".";
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
        if (entry->uac_state.has_value() && !entry->uac_state->instruction_family_hint.empty()) {
            out << " | pre=" << entry->uac_state->instruction_family_hint;
        }
        if (entry->postprocess_record.has_value() && !entry->postprocess_record->status.empty()) {
            out << " | post=" << entry->postprocess_record->status;
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
    entry.summary = summarize_interaction_for_history(report);
    entry.scope = "temporary";
    entry.created_at = entry.timestamp;
    entry.expires_at = timestamp_after_hours(24);
    snapshot.history.push_back(entry);

    std::ofstream output(snapshot.paths.history_path, std::ios::app);
    output << render_history_entry(entry) << '\n';

    auto record_plan_outcome = [&](std::string_view plan_type,
                                   std::string_view canonical_value,
                                   std::string_view provider_id,
                                   std::string_view model,
                                   std::string_view status,
                                   std::string_view rejection_reason) {
        if (provider_id.empty() && canonical_value.empty()) {
            return;
        }
        AssistOutcomeRecord outcome;
        outcome.id = make_scoped_id("assist-outcome",
                                    report.tze_run_id.empty() ? report.raw_prompt
                                                              : report.tze_run_id + std::string(plan_type));
        outcome.task_type = report.resolved_intent;
        outcome.plan_type = std::string(plan_type);
        outcome.provider_id = std::string(provider_id);
        outcome.model = std::string(model);
        outcome.status = std::string(status);
        outcome.target_label = report.resolved_project.empty() ? report.raw_prompt : report.resolved_project;
        outcome.canonical_value = std::string(canonical_value);
        outcome.rejection_reason = std::string(rejection_reason);
        outcome.host_platform = current_host_platform();
        if (report.preflight_report.has_value()) {
            outcome.environment_signature = report.preflight_report->environment_signature;
        } else if (report.build_execution.has_value()) {
            outcome.environment_signature = report.build_execution->environment_signature;
        } else if (report.tool_resolution.has_value()) {
            outcome.environment_signature = report.tool_resolution->environment_signature;
        }
        outcome.persisted_at = now_timestamp();
        remember_assist_outcome(snapshot, outcome);
    };

    if (report.command_assist_plan.has_value()) {
        record_plan_outcome("command",
                            report.command_assist_plan->canonical_command,
                            report.command_assist_plan->provider_id,
                            report.command_assist_plan->model,
                            report.assist_status.empty() ? report.command_assist_plan->status : report.assist_status,
                            report.assist_status == "assist_bypassed" ? report.answer_explanation : "");
    }
    if (report.tool_assist_plan.has_value()) {
        record_plan_outcome("tool",
                            report.tool_assist_plan->tool_name,
                            report.tool_assist_plan->provider_id,
                            report.tool_assist_plan->model,
                            report.assist_status.empty() ? report.tool_assist_plan->status : report.assist_status,
                            report.assist_status == "assist_bypassed" ? report.answer_explanation : "");
    }
    if (report.build_assist_plan.has_value()) {
        record_plan_outcome("build",
                            report.build_assist_plan->selected_recipe_id,
                            report.build_assist_plan->provider_id,
                            report.build_assist_plan->model,
                            report.assist_status.empty() ? report.build_assist_plan->status : report.assist_status,
                            report.assist_status == "assist_bypassed" ? report.answer_explanation : "");
    }
    if (report.next_step_assist_plan.has_value()) {
        record_plan_outcome("next_step",
                            report.next_step_assist_plan->suggested_next_step,
                            report.next_step_assist_plan->provider_id,
                            report.next_step_assist_plan->model,
                            report.next_step_assist_plan->status,
                            {});
    }
    if (report.case_summary_assist_plan.has_value()) {
        record_plan_outcome("case_summary",
                            report.case_summary_assist_plan->summary_title,
                            report.case_summary_assist_plan->provider_id,
                            report.case_summary_assist_plan->model,
                            report.case_summary_assist_plan->status,
                            {});
    }
    if (report.freeform_assist_answer.has_value()) {
        record_plan_outcome("freeform",
                            report.freeform_assist_answer->answer,
                            report.freeform_assist_answer->provider_id,
                            report.freeform_assist_answer->model,
                            report.freeform_assist_answer->status,
                            {});
    }
    if (report.shell_normalization.has_value() && !report.shell_normalization->correction_notes.empty()) {
        AssistCorrectionRecord correction;
        correction.id = make_scoped_id("assist-correction", report.raw_prompt);
        correction.original_prompt = report.raw_prompt;
        correction.corrected_value = report.shell_normalization->canonical;
        correction.category = report.shell_normalization->category;
        correction.host_platform = current_host_platform();
        correction.persisted_at = now_timestamp();
        remember_assist_correction(snapshot, correction);
    }
    if (report.shell_normalization.has_value()) {
        AssistLearningRecord learning;
        learning.id = make_scoped_id("assist-learning",
                                     report.shell_normalization->canonical + report.shell_normalization->category);
        learning.category = report.shell_normalization->category;
        learning.prompt_fragment = report.shell_normalization->phrase;
        learning.learned_value = report.shell_normalization->canonical;
        learning.host_platform = current_host_platform();
        learning.success_count = report.answer_status == "unknown_intent" ? 0 : 1;
        learning.rejection_count = report.answer_status == "unknown_intent" ? 1 : 0;
        learning.last_status = report.answer_status;
        learning.persisted_at = now_timestamp();
        remember_assist_learning(snapshot, learning);
    }
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

    const auto infer_authority_tier = [&answer]() {
        if (!answer.selected_authority_tier.empty()) {
            return answer.selected_authority_tier;
        }
        if (answer.selected_source_type == "local_glossary") {
            return std::string("operator_override");
        }
        if (answer.selected_source_type == "system_dictionary" ||
            answer.selected_source_type == "webster_fallback") {
            return std::string("reference_cache");
        }
        return std::string("memory_artifact");
    };
    const auto authority_rank = [](std::string_view authority) {
        if (authority == "operator_override") {
            return 3;
        }
        if (authority == "memory_artifact") {
            return 2;
        }
        if (authority == "reference_cache") {
            return 1;
        }
        return 0;
    };
    const std::string authority_tier = infer_authority_tier();
    const std::string created_at = now_timestamp();
    const bool durable_source = answer.selected_source_type == "local_glossary" ||
        answer.selected_source_type == "source_map";
    const std::string scope = durable_source ? "durable" : "temporary";
    const std::string expires_at = scope == "temporary" ? timestamp_after_hours(24) : std::string{};

    const auto existing = std::find_if(snapshot.definitions.begin(), snapshot.definitions.end(), [&answer](const StoredDefinition& entry) {
        const std::string entry_normalized =
            !entry.normalized_concept.empty() ? entry.normalized_concept : lowercase(entry.term);
        const bool concept_match = entry.term == answer.query ||
            entry_normalized == answer.normalized_concept;
        const bool domain_match = lowercase(entry.domain_hint) == lowercase(answer.domain_hint);
        return concept_match && domain_match;
    });
    if (existing != snapshot.definitions.end()) {
        if (authority_rank(existing->authority_tier) > authority_rank(authority_tier)) {
            return;
        }
        existing->term = answer.query;
        existing->normalized_concept = answer.normalized_concept;
        existing->domain_hint = answer.domain_hint;
        existing->summary = answer.summary;
        existing->mapped_cpp_target = answer.mapped_cpp_target;
        existing->semantic_family = answer.semantic_family;
        existing->source = answer.selected_source_label.empty()
            ? (answer.sources.empty() ? "memory" : answer.sources.front())
            : answer.selected_source_label;
        existing->source_type = answer.selected_source_type.empty() ? "memory" : answer.selected_source_type;
        existing->authority_tier = authority_tier;
        existing->confidence = answer.confidence;
        existing->scope = scope;
        existing->created_at = existing->created_at.empty() ? created_at : existing->created_at;
        existing->expires_at = expires_at;
        return;
    }

    snapshot.definitions.push_back({
        answer.query,
        answer.normalized_concept,
        answer.domain_hint,
        answer.summary,
        answer.mapped_cpp_target,
        answer.semantic_family,
        answer.selected_source_label.empty()
            ? (answer.sources.empty() ? "memory" : answer.sources.front())
            : answer.selected_source_label,
        answer.selected_source_type.empty() ? "memory" : answer.selected_source_type,
        authority_tier,
        answer.confidence,
        scope,
        created_at,
        expires_at,
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

void MemoryStore::remember_legacy_source(MemorySnapshot& snapshot, const LegacySourceRecord& record) const {
    if (record.id.empty()) {
        return;
    }

    LegacySourceRecord stored = record;
    if (stored.tracked_at.empty()) {
        stored.tracked_at = now_timestamp();
    }

    const auto existing = std::find_if(snapshot.legacy_sources.begin(),
                                       snapshot.legacy_sources.end(),
                                       [&stored](const LegacySourceRecord& entry) {
                                           return entry.id == stored.id || entry.source_path == stored.source_path;
                                       });
    if (existing != snapshot.legacy_sources.end()) {
        *existing = stored;
        return;
    }

    snapshot.legacy_sources.push_back(std::move(stored));
}

void MemoryStore::remember_operator_persona(MemorySnapshot& snapshot, const OperatorPersonaRecord& record) const {
    snapshot.operator_persona = record;
}

void MemoryStore::remember_assist_outcome(MemorySnapshot& snapshot, const AssistOutcomeRecord& record) const {
    if (record.id.empty()) {
        return;
    }
    const auto existing = std::find_if(snapshot.assist_outcomes.begin(),
                                       snapshot.assist_outcomes.end(),
                                       [&record](const AssistOutcomeRecord& entry) { return entry.id == record.id; });
    if (existing != snapshot.assist_outcomes.end()) {
        *existing = record;
        return;
    }
    snapshot.assist_outcomes.push_back(record);
}

void MemoryStore::remember_assist_correction(MemorySnapshot& snapshot, const AssistCorrectionRecord& record) const {
    if (record.id.empty()) {
        return;
    }
    const auto existing = std::find_if(snapshot.assist_corrections.begin(),
                                       snapshot.assist_corrections.end(),
                                       [&record](const AssistCorrectionRecord& entry) { return entry.id == record.id; });
    if (existing != snapshot.assist_corrections.end()) {
        *existing = record;
        return;
    }
    snapshot.assist_corrections.push_back(record);
}

void MemoryStore::remember_assist_learning(MemorySnapshot& snapshot, const AssistLearningRecord& record) const {
    if (record.id.empty()) {
        return;
    }
    const auto existing = std::find_if(snapshot.assist_learning.begin(),
                                       snapshot.assist_learning.end(),
                                       [&record](const AssistLearningRecord& entry) { return entry.id == record.id; });
    if (existing != snapshot.assist_learning.end()) {
        *existing = record;
        return;
    }
    snapshot.assist_learning.push_back(record);
}

void MemoryStore::remember_host_assist_preference(MemorySnapshot& snapshot, const HostAssistPreferenceRecord& record) const {
    if (record.id.empty()) {
        return;
    }
    const auto existing = std::find_if(snapshot.host_assist_preferences.begin(),
                                       snapshot.host_assist_preferences.end(),
                                       [&record](const HostAssistPreferenceRecord& entry) { return entry.id == record.id; });
    if (existing != snapshot.host_assist_preferences.end()) {
        *existing = record;
        return;
    }
    snapshot.host_assist_preferences.push_back(record);
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

void MemoryStore::remember_authored_recipe(MemorySnapshot& snapshot, const AuthoredRecipeRecord& record) const {
    if (record.recipe.id.empty() || record.resolved_source_path.empty()) {
        return;
    }

    AuthoredRecipeRecord normalized = record;
    std::error_code ec;
    const std::filesystem::path resolved = std::filesystem::weakly_canonical(record.resolved_source_path, ec);
    if (!ec) {
        normalized.resolved_source_path = resolved.string();
    }

    const auto existing = std::find_if(snapshot.authored_recipes.begin(),
                                       snapshot.authored_recipes.end(),
                                       [&normalized](const AuthoredRecipeRecord& entry) {
                                           return entry.recipe.id == normalized.recipe.id &&
                                               entry.resolved_source_path == normalized.resolved_source_path;
                                       });
    if (existing != snapshot.authored_recipes.end()) {
        *existing = normalized;
        return;
    }
    snapshot.authored_recipes.push_back(normalized);
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

        if (snapshot.operator_persona.has_value()) {
            const OperatorPersonaRecord& persona = *snapshot.operator_persona;
            out << "Operator Persona:\n";
            if (!persona.preferred_label.empty()) {
                out << " - Label: " << persona.preferred_label << "\n";
            }
            if (!persona.role_label.empty()) {
                out << " - Role: " << persona.role_label << "\n";
            }
            if (!persona.local_username.empty()) {
                out << " - Local user: " << persona.local_username << "\n";
            }
            if (!persona.host_identifier.empty()) {
                out << " - Host: " << persona.host_identifier << "\n";
            }
            if (!persona.persona_mode.empty()) {
                out << " - Mode: " << persona.persona_mode << "\n";
            }
            if (!persona.tone_profile.empty()) {
                out << " - Tone: " << persona.tone_profile << "\n";
            }
            if (!persona.interaction_style.empty()) {
                out << " - Style: " << persona.interaction_style << "\n";
            }
            if (!persona.safety_posture.empty()) {
                out << " - Safety posture: " << persona.safety_posture << "\n";
            }
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

        if (!snapshot.authored_recipes.empty()) {
            out << "Authored Recipes:\n";
            for (const AuthoredRecipeRecord& entry : snapshot.authored_recipes) {
                out << " - " << entry.canonical_name << " :: " << entry.recipe.id
                    << " path=" << entry.resolved_source_path
                    << " status=" << entry.validation_status
                    << " active=" << (entry.active ? "yes" : "no") << "\n";
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

        if (!snapshot.legacy_sources.empty()) {
            out << "Legacy Sources:\n";
            for (const LegacySourceRecord& entry : snapshot.legacy_sources) {
                out << " - " << entry.source_label
                    << " path=" << entry.source_path
                    << " sections=" << entry.section_count
                    << " symbols=" << entry.symbol_count << "\n";
            }
        }
        if (!snapshot.assist_outcomes.empty() || !snapshot.assist_corrections.empty() || !snapshot.assist_learning.empty()) {
            out << "Assist Learning:\n";
            out << " - Outcomes: " << snapshot.assist_outcomes.size() << "\n";
            out << " - Corrections: " << snapshot.assist_corrections.size() << "\n";
            out << " - Learned routes: " << snapshot.assist_learning.size() << "\n";
        }
        return out.str();
    }

    if (view == "assist") {
        out << "Assist Memory:\n";
        out << " - Outcomes: " << snapshot.assist_outcomes.size() << "\n";
        const std::size_t outcome_start = snapshot.assist_outcomes.size() > 8 ? snapshot.assist_outcomes.size() - 8 : 0;
        for (std::size_t index = outcome_start; index < snapshot.assist_outcomes.size(); ++index) {
            const AssistOutcomeRecord& entry = snapshot.assist_outcomes[index];
            out << "   * [" << entry.status << "] " << entry.plan_type << " :: " << entry.canonical_value;
            if (!entry.provider_id.empty()) {
                out << " via " << entry.provider_id;
            }
            if (!entry.model.empty()) {
                out << " (" << entry.model << ")";
            }
            if (!entry.rejection_reason.empty()) {
                out << " reject=" << entry.rejection_reason;
            }
            out << "\n";
        }
        out << " - Corrections: " << snapshot.assist_corrections.size() << "\n";
        for (const AssistCorrectionRecord& entry : snapshot.assist_corrections) {
            out << "   * " << entry.original_prompt << " -> " << entry.corrected_value << " [" << entry.category << "]\n";
        }
        out << " - Learned routes: " << snapshot.assist_learning.size() << "\n";
        for (const AssistLearningRecord& entry : snapshot.assist_learning) {
            out << "   * " << entry.category << " :: " << entry.prompt_fragment << " => " << entry.learned_value
                << " success=" << entry.success_count << " reject=" << entry.rejection_count << "\n";
        }
        if (!snapshot.host_assist_preferences.empty()) {
            out << " - Host preferences:\n";
            for (const HostAssistPreferenceRecord& entry : snapshot.host_assist_preferences) {
                out << "   * " << entry.host_platform << " :: " << entry.provider_id;
                if (!entry.model.empty()) {
                    out << " (" << entry.model << ")";
                }
                out << "\n";
            }
        }
        return out.str();
    }

    if (view == "persona" || view == "operator") {
        out << "Operator Persona:\n";
        if (!snapshot.operator_persona.has_value()) {
            out << " - No richer operator persona is stored yet.\n";
            return out.str();
        }
        const OperatorPersonaRecord& persona = *snapshot.operator_persona;
        out << " - Label: " << (persona.preferred_label.empty() ? "(unset)" : persona.preferred_label) << "\n";
        if (!persona.role_label.empty()) {
            out << " - Role: " << persona.role_label << "\n";
        }
        if (!persona.local_username.empty()) {
            out << " - Local user: " << persona.local_username << "\n";
        }
        if (!persona.host_identifier.empty()) {
            out << " - Host: " << persona.host_identifier << "\n";
        }
        if (!persona.persona_mode.empty()) {
            out << " - Mode: " << persona.persona_mode << "\n";
        }
        if (!persona.tone_profile.empty()) {
            out << " - Tone: " << persona.tone_profile << "\n";
        }
        if (!persona.interaction_style.empty()) {
            out << " - Style: " << persona.interaction_style << "\n";
        }
        if (!persona.safety_posture.empty()) {
            out << " - Safety posture: " << persona.safety_posture << "\n";
        }
        if (!persona.preferred_next_action_style.empty()) {
            out << " - Next-action style: " << persona.preferred_next_action_style << "\n";
        }
        if (!persona.last_source_map.empty()) {
            out << " - Last source map: " << persona.last_source_map << "\n";
        }
        if (!persona.last_memory_root.empty()) {
            out << " - Last memory root: " << persona.last_memory_root << "\n";
        }
        if (!persona.self_description.empty()) {
            out << " - About: " << persona.self_description << "\n";
        }
        if (!persona.custom_phrases.empty()) {
            out << " - Custom phrases:\n";
            for (const std::string& phrase : persona.custom_phrases) {
                out << "   * " << phrase << "\n";
            }
        }
        return out.str();
    }

    if (view == "definitions") {
        out << "Stored Definitions:\n";
        for (const StoredDefinition& entry : snapshot.definitions) {
            out << " - " << entry.term << " => " << entry.summary;
            if (!entry.domain_hint.empty()) {
                out << " (domain=" << entry.domain_hint << ")";
            }
            if (!entry.authority_tier.empty()) {
                out << " [authority=" << entry.authority_tier << "]";
            }
            if (!entry.mapped_cpp_target.empty()) {
                out << " [" << entry.mapped_cpp_target << "]";
            }
            if (!entry.source_type.empty()) {
                out << " <" << entry.source_type;
                if (!entry.source.empty()) {
                    out << ":" << entry.source;
                }
                out << ">";
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
            if (entry.uac_state.has_value() && !entry.uac_state->instruction_family_hint.empty()) {
                out << " | pre=" << entry.uac_state->instruction_family_hint;
            }
            if (entry.postprocess_record.has_value() && !entry.postprocess_record->status.empty()) {
                out << " | post=" << entry.postprocess_record->status;
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
            if (entry.neural_math_report.has_value() && !entry.neural_math_report->dataset.empty()) {
                out << " | neural=" << entry.neural_math_report->dataset
                    << " acc=" << entry.neural_math_report->accuracy;
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
                out << "   - " << render_stage_label(stage) << " [" << stage.status << "] " << stage.module;
                if (!stage.detail.empty()) {
                    out << " | " << human_readable_storage_text(stage.detail);
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

    if (view == "legacy") {
        out << "Legacy Sources:\n";
        if (snapshot.legacy_sources.empty()) {
            out << " - No stored legacy source maps.\n";
            return out.str();
        }
        for (const LegacySourceRecord& entry : snapshot.legacy_sources) {
            out << " - " << entry.id << " | " << entry.source_label
                << " | " << entry.source_path
                << " | lines=" << entry.line_count
                << " | sections=" << entry.section_count
                << " | symbols=" << entry.symbol_count << "\n";
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
