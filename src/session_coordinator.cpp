#include "tze/session_coordinator.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "tze/neural_math_engine.hpp"
#include "tze/neural_signal_router.hpp"
#include "tze/query_runtime.hpp"
#include "tze/preprocessor_runtime.hpp"
#include "xpp/index.hpp"
#include "xpp/parser.hpp"

#ifndef OMNIX_VERSION
#define OMNIX_VERSION "0.1.0-dev"
#endif

namespace tze {
namespace {

struct SourceStageMetadata {
    std::string graph_origin;
    std::string source_section;
    std::size_t source_line = 0;
    std::string source_excerpt;
};

using SourceStageIndex = std::map<std::string, SourceStageMetadata>;

std::optional<xpp::MappingUnit> try_parse_source_map(std::string_view source_map_path) {
    if (source_map_path.empty()) {
        return std::nullopt;
    }
    try {
        const std::string source = xpp::read_text_file(std::string(source_map_path));
        return xpp::parse_xpp(source, std::string(source_map_path));
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

const xpp::StageGraph* find_stage_graph(const xpp::MappingUnit& unit, std::string_view graph_id) {
    for (const xpp::StageGraph& graph : unit.stage_graphs) {
        if (graph.graph_id == graph_id) {
            return &graph;
        }
    }
    return nullptr;
}

SourceStageIndex build_source_stage_index(const xpp::MappingUnit& unit) {
    SourceStageIndex index;
    const xpp::StageGraph* graph = find_stage_graph(unit, "build_cmake_stage_graph");
    if (graph == nullptr) {
        return index;
    }

    for (const xpp::StageNode& stage : graph->stages) {
        index[stage.stage_id] = {
            unit.source_name + "#" + graph->graph_id,
            stage.section_title,
            stage.line,
            stage.source_excerpt,
        };
    }
    return index;
}

void apply_source_stage_metadata(TzeStageRecord& record, const SourceStageIndex& stage_index) {
    const auto it = stage_index.find(record.stage_id);
    if (it == stage_index.end()) {
        return;
    }

    record.graph_origin = it->second.graph_origin;
    record.source_section = it->second.source_section;
    record.source_line = it->second.source_line;
    record.source_excerpt = it->second.source_excerpt;
}

TzeStageRecord make_stage_record(std::string stage_id,
                                 std::string stage_name,
                                 std::string module,
                                 std::string status,
                                 std::string detail,
                                 std::vector<std::string> inputs,
                                 std::vector<std::string> outputs,
                                 const SourceStageIndex& stage_index) {
    TzeStageRecord record;
    record.stage_id = std::move(stage_id);
    record.stage_name = std::move(stage_name);
    record.module = std::move(module);
    record.status = std::move(status);
    record.detail = std::move(detail);
    record.inputs = std::move(inputs);
    record.outputs = std::move(outputs);
    apply_source_stage_metadata(record, stage_index);
    return record;
}

void attach_source_backed_mappings(const xpp::MappingUnit& unit, ProcessingReport& report) {
    const xpp::SymbolIndex index = xpp::build_symbol_index(unit);

    static const std::vector<std::string> kRuntimeSymbols = {
        "xProcessingCache",
        "xProcessingDefine",
        "x.Destroy",
        "x.Define.Low",
        "x.DisplayPriorityProcessingGate",
        "x.DisplayFeedBackLoop",
        "x.Security",
        "x.index",
        "x.seek",
        "x.read",
        "xout",
        "xin",
        "GENx",
        "x.DNLIO",
        "xMap_Perm",
    };

    for (const std::string& symbol : kRuntimeSymbols) {
        const xpp::SymbolMapping* mapping = xpp::find_mapping(index, symbol);
        if (mapping == nullptr) {
            continue;
        }

        report.source_backed_mappings.push_back({
            mapping->raw_symbol,
            mapping->inferred_meaning,
            mapping->mapped_cpp_target,
            std::string(xpp::to_string(mapping->status)),
            std::string(xpp::to_string(mapping->family)),
            mapping->occurrences.size(),
        });
    }
}

std::string command_label_for(RequestIntent intent, std::string_view decoded_instruction) {
    if (!decoded_instruction.empty() && decoded_instruction != "Unknown") {
        return std::string(decoded_instruction);
    }
    switch (intent) {
        case RequestIntent::Conversation:
            return "Conversation";
        case RequestIntent::GeneralDefinitionQuery:
            return "Investigate";
        case RequestIntent::PacketCapture:
            return "OmniXTView";
        case RequestIntent::DefenseDiagnostic:
            return "DefenseDiagnostic";
        case RequestIntent::DefenseDetection:
            return "DefenseDetect";
        case RequestIntent::VuplusGate:
            return "VuplusGate";
        case RequestIntent::NeuralMath:
            return "NeuralMath";
        case RequestIntent::NeuralRoute:
            return "NeuralSignalRouter";
        case RequestIntent::TensorAction:
            return "TensorFramework";
        case RequestIntent::SetPersonaMode:
            return "PersonaEngine";
        case RequestIntent::AuthorBuildRecipe:
            return "RecipeAuthoring";
        case RequestIntent::ReviewModule:
            return "Review";
        case RequestIntent::PatchProposal:
            return "PatchProposal";
        case RequestIntent::IngestData:
            return "Ingest";
        case RequestIntent::AnalyzeCase:
            return "Analyze";
        case RequestIntent::DecideAction:
            return "Decide";
        case RequestIntent::InspectCase:
            return "Case";
        case RequestIntent::CaseTimeline:
            return "CaseTimeline";
        case RequestIntent::ReplayTzeRun:
            return "TZEReplay";
        case RequestIntent::ChainTzeRun:
            return "TZEChain";
        case RequestIntent::RecursiveWhyDiff:
            return "RecursiveWhyDiff";
        case RequestIntent::DiffTzeRuns:
            return "TZEDiff";
        case RequestIntent::ExplainTzeChange:
            return "TZEExplainChange";
        case RequestIntent::ReportTzeRun:
            return "TZEReport";
        case RequestIntent::DiffReportTzeRuns:
            return "TZEDiffReport";
        case RequestIntent::ExportTzeBundle:
            return "TZEExport";
        case RequestIntent::ImportTzeBundle:
            return "TZEImport";
        case RequestIntent::PruneTzeRuns:
            return "TZEPrune";
        case RequestIntent::PruneMemory:
            return "MemoryPrune";
        case RequestIntent::MarkTzeRunOutcome:
            return "TZEMark";
        case RequestIntent::MarkDecisionFeedback:
            return "DecisionFeedback";
        case RequestIntent::MarkDecisionOutcome:
            return "DecisionOutcome";
        case RequestIntent::ExportCaseBundle:
            return "CaseExport";
        case RequestIntent::ImportCaseBundle:
            return "CaseImport";
        case RequestIntent::ListIncidents:
            return "IncidentList";
        case RequestIntent::InspectIncident:
            return "Incident";
        case RequestIntent::ReportIncident:
            return "IncidentReport";
        case RequestIntent::BuildProject:
            return "Build";
        case RequestIntent::DoctorProject:
            return "Doctor";
        case RequestIntent::ToolAction:
            return "Tool";
        case RequestIntent::ProbeProvider:
            return "ProviderProbe";
        case RequestIntent::DefineSymbol:
        case RequestIntent::ExplainCommand:
            return "Investigate";
        case RequestIntent::InspectToolchain:
            return "InspectToolchain";
        case RequestIntent::ShowMemory:
            return "Memory";
        case RequestIntent::Unknown:
            return "Unknown";
    }
    return "Unknown";
}

void apply_source_preferences(std::vector<KnowledgeReference>& references, const MemorySnapshot& memory) {
    if (memory.source_preference_order.empty()) {
        return;
    }

    std::stable_sort(references.begin(), references.end(), [&memory](const KnowledgeReference& lhs, const KnowledgeReference& rhs) {
        const auto rank = [&memory](std::string_view source) {
            const auto it = std::find(memory.source_preference_order.begin(), memory.source_preference_order.end(), source);
            if (it == memory.source_preference_order.end()) {
                return static_cast<int>(memory.source_preference_order.size() + 10);
            }
            return static_cast<int>(std::distance(memory.source_preference_order.begin(), it));
        };
        return rank(lhs.source) < rank(rhs.source);
    });
}

std::vector<std::string> relevant_history(const MemorySnapshot& memory, std::string_view project_or_prompt) {
    std::vector<std::string> matches;
    if (project_or_prompt.empty()) {
        return matches;
    }

    for (auto it = memory.history.rbegin(); it != memory.history.rend(); ++it) {
        if (it->prompt.find(project_or_prompt) == std::string::npos &&
            it->project.find(project_or_prompt) == std::string::npos) {
            continue;
        }
        matches.push_back(it->timestamp + " | " + it->summary);
        if (matches.size() >= 3) {
            break;
        }
    }
    return matches;
}

std::string join_lines(const std::vector<std::string>& lines) {
    std::ostringstream out;
    for (std::size_t index = 0; index < lines.size(); ++index) {
        if (index != 0) {
            out << '\n';
        }
        out << lines[index];
    }
    return out.str();
}

std::string collapse_inline_whitespace(std::string_view value) {
    std::string collapsed;
    collapsed.reserve(value.size());
    bool previous_space = false;
    for (char c : value) {
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
    std::size_t start = 0;
    while (start < collapsed.size() && std::isspace(static_cast<unsigned char>(collapsed[start]))) {
        ++start;
    }
    std::size_t end = collapsed.size();
    while (end > start && std::isspace(static_cast<unsigned char>(collapsed[end - 1]))) {
        --end;
    }
    return collapsed.substr(start, end - start);
}

std::string first_non_empty_line(std::string_view text) {
    std::istringstream input{std::string(text)};
    std::string line;
    while (std::getline(input, line)) {
        std::size_t start = 0;
        while (start < line.size() && std::isspace(static_cast<unsigned char>(line[start]))) {
            ++start;
        }
        std::size_t end = line.size();
        while (end > start && std::isspace(static_cast<unsigned char>(line[end - 1]))) {
            --end;
        }
        line = line.substr(start, end - start);
        if (!line.empty()) {
            return line;
        }
    }
    return {};
}

std::string trim_local(std::string_view value) {
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

std::string instruction_family_hint_for(RequestIntent intent) {
    switch (intent) {
        case RequestIntent::BuildProject:
        case RequestIntent::DoctorProject:
        case RequestIntent::AuthorBuildRecipe:
            return "build";
        case RequestIntent::PacketCapture:
            return "packet-capture";
        case RequestIntent::DefenseDiagnostic:
        case RequestIntent::DefenseDetection:
            return "defense";
        case RequestIntent::VuplusGate:
            return "vuplus-gate";
        case RequestIntent::NeuralMath:
            return "neural-math";
        case RequestIntent::NeuralRoute:
            return "neural-route";
        case RequestIntent::TensorAction:
            return "tensor";
        case RequestIntent::RecursiveWhyDiff:
            return "recursive-why-diff";
        case RequestIntent::SetPersonaMode:
            return "persona";
        case RequestIntent::GeneralDefinitionQuery:
        case RequestIntent::DefineSymbol:
        case RequestIntent::ExplainCommand:
            return "definition";
        case RequestIntent::ToolAction:
            return "tool";
        case RequestIntent::ProbeProvider:
            return "provider";
        case RequestIntent::IngestData:
        case RequestIntent::AnalyzeCase:
        case RequestIntent::DecideAction:
        case RequestIntent::InspectCase:
        case RequestIntent::CaseTimeline:
        case RequestIntent::ListIncidents:
        case RequestIntent::InspectIncident:
        case RequestIntent::ReportIncident:
            return "analyst";
        case RequestIntent::ReplayTzeRun:
        case RequestIntent::ChainTzeRun:
        case RequestIntent::DiffTzeRuns:
        case RequestIntent::ExplainTzeChange:
        case RequestIntent::ReportTzeRun:
        case RequestIntent::DiffReportTzeRuns:
        case RequestIntent::ExportTzeBundle:
        case RequestIntent::ImportTzeBundle:
        case RequestIntent::PruneTzeRuns:
        case RequestIntent::PruneMemory:
        case RequestIntent::MarkTzeRunOutcome:
        case RequestIntent::MarkDecisionFeedback:
        case RequestIntent::MarkDecisionOutcome:
            return "tze";
        case RequestIntent::ExportCaseBundle:
        case RequestIntent::ImportCaseBundle:
            return "case";
        case RequestIntent::Conversation:
            return "conversation";
        case RequestIntent::ReviewModule:
        case RequestIntent::PatchProposal:
            return "self-review";
        case RequestIntent::InspectToolchain:
            return "toolchain";
        case RequestIntent::ShowMemory:
            return "memory";
        case RequestIntent::Unknown:
            return "general";
    }
    return "general";
}

PostProcessRecord build_postprocess_record(const ProcessingReport& report) {
    PostProcessRecord record;
    if (report.answer_status == "clarify_needed") {
        record.status = "PostClarify";
    } else if (report.answer_status.find("blocked") != std::string::npos ||
               report.answer_status.find("rejected") != std::string::npos) {
        record.status = "PostBlocked";
    } else if (report.answer_status == "unknown_intent" ||
               report.answer_status == "unknown_query" ||
               report.answer_status == "provider_probe_failed") {
        record.status = "PostFail";
    } else {
        record.status = "PostSuccess";
    }

    if (report.definition_answer.has_value() && report.definition_answer->found && !report.definition_answer->summary.empty()) {
        record.final_artifact_summary = report.definition_answer->summary;
        record.provenance = report.definition_answer->selected_source_type.empty()
            ? "definition_answer"
            : report.definition_answer->selected_source_type;
    } else if (report.freeform_assist_answer.has_value() && !report.freeform_assist_answer->answer.empty()) {
        record.final_artifact_summary = report.freeform_assist_answer->answer;
        record.provenance = "openai_freeform";
    } else if (!report.produced_artifact.empty()) {
        record.final_artifact_summary = first_non_empty_line(report.answer_explanation);
        if (record.final_artifact_summary.empty()) {
            record.final_artifact_summary = "Artifact stored.";
        }
        record.provenance = "produced_artifact";
    } else {
        record.final_artifact_summary = first_non_empty_line(report.answer_explanation);
        if (record.final_artifact_summary.empty()) {
            record.final_artifact_summary = report.answer_status.empty() ? "Processing complete." : report.answer_status;
        }
        record.provenance = report.answer_status.empty() ? "runtime" : report.answer_status;
    }
    record.final_artifact_summary = collapse_inline_whitespace(record.final_artifact_summary);
    record.dropped_transient_chain =
        report.answer_explanation.find("Recent feedback:") != std::string::npos ||
        report.answer_explanation.find("Suggestions:") != std::string::npos ||
        report.answer_explanation.find('\n') != std::string::npos;
    record.retention_decision = record.dropped_transient_chain
        ? "retain.final_artifact_only"
        : "retain.final_artifact";
    record.retained_fields = {"answer_status", "final_artifact_summary", "provenance"};
    if (!report.produced_artifact.empty()) {
        record.retained_fields.push_back("produced_artifact");
    }
    if (record.dropped_transient_chain) {
        record.discarded_fields = {"recent_feedback", "suggestions", "transient_chain_text"};
    }
    using clock = std::chrono::system_clock;
    const std::time_t raw = clock::to_time_t(clock::now());
    std::tm local{};
#if defined(__APPLE__) || defined(__unix__)
    localtime_r(&raw, &local);
#else
    local = *std::localtime(&raw);
#endif
    std::ostringstream timestamp;
    timestamp << std::put_time(&local, "%Y-%m-%dT%H:%M:%S");
    record.persisted_at = timestamp.str();
    return record;
}

std::string run_timestamp() {
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

bool is_expired(const StoredDefinition& entry) {
    return entry.scope == "temporary" && !entry.expires_at.empty() &&
        entry.expires_at <= run_timestamp();
}

bool is_expired(const MemoryHistoryEntry& entry) {
    return entry.scope == "temporary" && !entry.expires_at.empty() &&
        entry.expires_at <= run_timestamp();
}

std::string make_tze_run_id(const MemorySnapshot& memory,
                            std::string_view prompt,
                            RequestIntent intent,
                            std::string_view primary_target) {
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::string seed = std::string(prompt) + "|" + std::string(to_string(intent)) + "|" +
        std::string(primary_target) + "|" + std::to_string(memory.history.size()) + "|" + std::to_string(tick);
    return "tze-run-" + std::to_string(std::hash<std::string>{}(seed));
}

void push_unique(std::vector<std::string>& values, const std::string& value) {
    if (value.empty()) {
        return;
    }
    if (std::find(values.begin(), values.end(), value) == values.end()) {
        values.push_back(value);
    }
}

std::optional<int> parse_port_text(std::string_view value) {
    std::string digits;
    for (char c : value) {
        if (std::isdigit(static_cast<unsigned char>(c))) {
            digits.push_back(c);
        } else if (!digits.empty()) {
            break;
        }
    }
    if (digits.empty()) {
        return std::nullopt;
    }
    const int port = std::stoi(digits);
    if (port <= 0 || port > 65535) {
        return std::nullopt;
    }
    return port;
}

std::string lowercase_basic(std::string_view value) {
    std::string lowered;
    lowered.reserve(value.size());
    for (char c : value) {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return lowered;
}

std::optional<int> first_open_tcp_port(const ToolInvocationReport& invocation) {
    for (const std::string& line : invocation.output_excerpt) {
        const std::string lowered = lowercase_basic(line);
        const std::size_t tcp_at = lowered.find("/tcp");
        if (tcp_at == std::string::npos || lowered.find("open") == std::string::npos) {
            continue;
        }
        std::size_t start = tcp_at;
        while (start > 0 && std::isdigit(static_cast<unsigned char>(lowered[start - 1]))) {
            --start;
        }
        if (start < tcp_at) {
            return parse_port_text(lowered.substr(start, tcp_at - start));
        }
    }
    return std::nullopt;
}

PacketCaptureRequest packet_request_from_profile(const RequestProfile& profile, const IntentResolution& resolution) {
    PacketCaptureRequest request;
    request.mode = !profile.packet_capture_mode.empty()
        ? profile.packet_capture_mode
        : (!resolution.memory_view.empty() ? resolution.memory_view : "live");
    request.interface_name = profile.packet_interface;
    request.pcap_path = profile.packet_pcap_path;
    request.export_path = profile.packet_export_path;
    request.host_filter = profile.packet_host_filter;
    request.port = profile.packet_port;
    if (request.port == 0 && !resolution.primary_target.empty()) {
        if (const std::optional<int> parsed = parse_port_text(resolution.primary_target); parsed.has_value()) {
            request.port = *parsed;
        }
    }
    request.packet_count = profile.packet_count;
    request.seconds = profile.packet_seconds;
    request.payload_bytes = profile.packet_payload_bytes;
    return request;
}

std::vector<std::string> command_lines(std::string_view command, std::size_t max_lines = 12) {
    std::vector<std::string> lines;
#if defined(__APPLE__) || defined(__linux__) || defined(__unix__)
    FILE* pipe = popen(std::string(command).c_str(), "r");
    if (pipe == nullptr) {
        lines.push_back("Unable to run diagnostic command.");
        return lines;
    }
    std::array<char, 512> buffer{};
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr && lines.size() < max_lines) {
        std::string line(buffer.data());
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
            line.pop_back();
        }
        if (!line.empty()) {
            lines.push_back(line);
        }
    }
    pclose(pipe);
#else
    lines.push_back("Local diagnostics are not specialized for this platform yet.");
#endif
    return lines;
}

DefenseDiagnosticRequest defense_request_from_profile(const RequestProfile& profile,
                                                      const IntentResolution& resolution) {
    DefenseDiagnosticRequest request;
    request.mode = !profile.defense_mode.empty() ? profile.defense_mode : "summary";
    request.target = !profile.defense_target.empty() ? profile.defense_target : resolution.primary_target;
    request.port = profile.defense_port;
    request.pid = profile.defense_pid;
    if (request.port == 0 && (request.mode == "port" || request.target.find("port") != std::string::npos)) {
        if (const std::optional<int> parsed = parse_port_text(request.target); parsed.has_value()) {
            request.port = *parsed;
        }
    }
    if (request.pid == 0 && request.mode == "pid") {
        if (const std::optional<int> parsed = parse_port_text(request.target); parsed.has_value()) {
            request.pid = *parsed;
        }
    }
    return request;
}

DefenseDiagnosticRequest defense_detection_request_from_profile(const RequestProfile& profile,
                                                                const IntentResolution& resolution) {
    DefenseDiagnosticRequest request;
    request.mode = !profile.defense_mode.empty() ? profile.defense_mode : "all";
    request.target = !profile.defense_target.empty() ? profile.defense_target : resolution.primary_target;
    return request;
}

DefenseDiagnosticReport run_defense_diagnostic(const DefenseDiagnosticRequest& request) {
    DefenseDiagnosticReport report;
    report.mode = request.mode.empty() ? "summary" : request.mode;
    report.target = request.target;
    report.status = "defense_diagnostic_complete";
    report.summary = "Collected local defensive diagnostic evidence without taking destructive action.";
    report.warnings.push_back("Diagnostic-only mode: OmniX did not kill processes, close ports, or change firewall state.");

    if (report.mode == "cpu") {
        report.evidence_lines = command_lines("ps -axo pid,pcpu,pmem,comm -r | head -n 10");
        report.proposed_actions.push_back("Review the top CPU consumers before deciding whether to stop a process.");
    } else if (report.mode == "memory") {
        report.evidence_lines = command_lines("ps -axo pid,rss,pmem,comm -m | head -n 10");
        report.proposed_actions.push_back("Review high-RSS processes and correlate with recent logs before action.");
    } else if (report.mode == "pid" && request.pid > 0) {
        report.evidence_lines = command_lines("ps -p " + std::to_string(request.pid) + " -o pid,ppid,pcpu,pmem,etime,comm");
        report.proposed_actions.push_back("If confirmed malicious or runaway, manually run `kill " + std::to_string(request.pid) + "`.");
        report.proposed_actions.push_back("Use `kill -TERM` before `kill -KILL` unless immediate containment is required.");
    } else if (report.mode == "port" && request.port > 0) {
        report.evidence_lines = command_lines("lsof -nP -iTCP:" + std::to_string(request.port) + " -sTCP:LISTEN");
        report.proposed_actions.push_back("Investigate traffic with `omnix tview port " + std::to_string(request.port) + "`.");
        report.proposed_actions.push_back("Identify the owning PID before manually stopping a service or closing a port.");
    } else if (report.mode == "logs") {
#if defined(__APPLE__)
        report.evidence_lines.push_back("Suggested log command: log show --last 5m --style compact --predicate 'eventMessage CONTAINS[c] \"error\" OR eventMessage CONTAINS[c] \"fail\"'");
#else
        report.evidence_lines = command_lines("dmesg | tail -n 20");
#endif
        report.proposed_actions.push_back("Preserve relevant log excerpts into a case before changing system state.");
    } else {
        report.evidence_lines = command_lines("ps -axo pid,pcpu,pmem,comm -r | head -n 8");
        report.proposed_actions.push_back("Run `omnix defend diag cpu`, `memory`, `logs`, `pid <pid>`, or `port <port>` for a focused diagnostic.");
    }
    if (report.evidence_lines.empty()) {
        report.status = "defense_diagnostic_empty";
        report.summary = "No local diagnostic evidence was returned for the requested target.";
    }
    return report;
}

std::string local_json_escape(std::string_view value) {
    std::ostringstream out;
    for (char c : value) {
        switch (c) {
            case '"':
                out << "\\\"";
                break;
            case '\\':
                out << "\\\\";
                break;
            case '\n':
                out << "\\n";
                break;
            case '\r':
                out << "\\r";
                break;
            case '\t':
                out << "\\t";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                        << static_cast<int>(static_cast<unsigned char>(c)) << std::dec;
                } else {
                    out << c;
                }
                break;
        }
    }
    return out.str();
}

std::string env_value_or_empty(const char* key) {
    const char* value = std::getenv(key);
    return value == nullptr ? std::string{} : std::string(value);
}

bool path_has_write_bit(const std::filesystem::path& path, bool* group_or_other = nullptr) {
    std::error_code ec;
    const auto status = std::filesystem::status(path, ec);
    if (ec) {
        if (group_or_other != nullptr) {
            *group_or_other = false;
        }
        return false;
    }
    const auto perms = status.permissions();
    const bool owner = (perms & std::filesystem::perms::owner_write) != std::filesystem::perms::none;
    const bool group = (perms & std::filesystem::perms::group_write) != std::filesystem::perms::none;
    const bool other = (perms & std::filesystem::perms::others_write) != std::filesystem::perms::none;
    if (group_or_other != nullptr) {
        *group_or_other = group || other;
    }
    return owner || group || other;
}

std::string severity_rank(const std::string& severity) {
    if (severity == "critical") {
        return "4";
    }
    if (severity == "high") {
        return "3";
    }
    if (severity == "medium") {
        return "2";
    }
    if (severity == "low") {
        return "1";
    }
    return "0";
}

void add_detection_signal(DefenseDetectionReport& report,
                          std::string category,
                          std::string id,
                          std::string severity,
                          std::string source,
                          std::string rationale,
                          std::vector<std::string> evidence,
                          std::string next_action,
                          double confidence) {
    DefenseDetectionSignal signal;
    signal.category = std::move(category);
    signal.id = std::move(id);
    signal.severity = std::move(severity);
    signal.source = std::move(source);
    signal.rationale = std::move(rationale);
    signal.evidence_lines = std::move(evidence);
    signal.recommended_next_action = std::move(next_action);
    signal.confidence = confidence;
    report.signals.push_back(std::move(signal));
}

void scan_vg_json_key_pairs(VuplusGateReport& report, const std::string& text, const std::string& source);
std::vector<EventViewerRetention> event_viewer_retention_from_pairs(const std::vector<VuplusGateReport::KeyPair>& pairs);
std::vector<SessionCorrelation> session_correlations_from_text(const std::string& lowered,
                                                               const std::vector<VuplusGateReport::KeyPair>& pairs);
bool has_detection_signal(const DefenseDetectionReport& report, const std::string& id);

void scan_profile_aliases(const std::filesystem::path& path,
                          std::vector<std::string>& evidence,
                          std::size_t max_lines) {
    std::ifstream input(path);
    if (!input) {
        return;
    }
    std::string line;
    std::size_t found = 0;
    while (std::getline(input, line) && found < max_lines) {
        const std::string trimmed = trim_local(line);
        if (trimmed.rfind("alias ", 0) == 0) {
            const std::size_t eq = trimmed.find('=');
            const std::string name = eq == std::string::npos ? trimmed.substr(0, std::min<std::size_t>(trimmed.size(), 48))
                                                             : trimmed.substr(0, eq);
            evidence.push_back("profile_alias=" + path.string() + ":" + name);
            ++found;
        } else if (trimmed.rfind("function ", 0) == 0 || trimmed.find("()") != std::string::npos) {
            std::string name = trimmed.substr(0, std::min<std::size_t>(trimmed.size(), 64));
            if (const std::size_t open = name.find('{'); open != std::string::npos) {
                name = name.substr(0, open);
            }
            evidence.push_back("profile_function=" + path.string() + ":" + trim_local(name));
            ++found;
        }
    }
}

std::vector<std::filesystem::path> shell_profile_paths() {
    std::vector<std::filesystem::path> paths;
    const std::filesystem::path home = env_value_or_empty("HOME");
    if (!home.empty()) {
        for (std::string_view file : {".zshrc", ".zprofile", ".zshenv", ".bashrc", ".bash_profile", ".profile"}) {
            paths.push_back(home / file);
        }
        paths.push_back(home / ".config/fish/config.fish");
    }
    for (std::string_view file : {"/etc/profile", "/etc/zshrc", "/etc/bashrc"}) {
        paths.emplace_back(file);
    }
    return paths;
}

void collect_env_detection(DefenseDetectionReport& report) {
    std::vector<std::string> path_evidence;
    std::istringstream paths(env_value_or_empty("PATH"));
    std::string entry;
    while (std::getline(paths, entry, ':') && path_evidence.size() < report.max_lines) {
        if (entry.empty()) {
            path_evidence.push_back("path_entry=<empty>");
            continue;
        }
        const std::filesystem::path path(entry);
        bool group_or_other = false;
        const bool writable = path_has_write_bit(path, &group_or_other);
        std::string line = "path_entry=" + entry + " writable=" + (writable ? "true" : "false");
        if (group_or_other) {
            line += " group_or_other_writable=true";
        }
        path_evidence.push_back(line);
    }
    add_detection_signal(report,
                         "env",
                         "path.surface",
                         "low",
                         "PATH",
                         "PATH entries were enumerated locally; writable or empty entries are important because command lookup can be redirected.",
                         path_evidence,
                         "Review writable PATH entries before trusting command execution.",
                         0.72);

    std::vector<std::string> profile_evidence;
    for (const std::filesystem::path& path : shell_profile_paths()) {
        if (profile_evidence.size() >= report.max_lines) {
            break;
        }
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) {
            continue;
        }
        std::uintmax_t size = std::filesystem::is_regular_file(path, ec) ? std::filesystem::file_size(path, ec) : 0;
        profile_evidence.push_back("profile_file=" + path.string() + " size=" + std::to_string(ec ? 0 : size));
        scan_profile_aliases(path, profile_evidence, 3);
    }
    if (!profile_evidence.empty()) {
        add_detection_signal(report,
                             "env",
                             "shell.startup.surface",
                             "low",
                             "shell_profiles",
                             "Shell startup files and alias/function names were inspected without dumping command bodies or secrets.",
                             profile_evidence,
                             "If a profile was recently changed unexpectedly, compare it against source control or a known-good backup.",
                             0.68);
    }
}

void collect_session_detection(DefenseDetectionReport& report) {
    std::vector<std::string> evidence = command_lines("who -a 2>/dev/null", report.max_lines / 2);
    std::vector<std::string> recent = command_lines("last -20 2>/dev/null", report.max_lines / 2);
    evidence.insert(evidence.end(), recent.begin(), recent.end());
    std::vector<std::string> persisted = command_lines("ps -axo pid,ppid,etime,tty,comm 2>/dev/null | grep -E 'sshd|ssh|tmux|screen' | grep -v grep", 10);
    evidence.insert(evidence.end(), persisted.begin(), persisted.end());
#if defined(__linux__)
    std::vector<std::string> lastlog = command_lines("lastlog 2>/dev/null | head -n 12", 12);
    evidence.insert(evidence.end(), lastlog.begin(), lastlog.end());
#endif
    std::string severity = "low";
    std::string rationale = "Local login/session evidence was collected from bounded who/last/process views.";
    if (!report.quiet_hours.empty() && !report.admin_user.empty()) {
        severity = "medium";
        rationale += " Quiet-hours context is active; admin sessions during this window should be verified.";
    }
    add_detection_signal(report,
                         "sessions",
                         "interactive.sessions",
                         severity,
                         "who/last/ps",
                         rationale,
                         evidence,
                         "If an unexpected SSH, TTY, tmux, or screen session appears, verify the operator before changing system state.",
                         0.76);
}

void collect_persistence_detection(DefenseDetectionReport& report) {
    std::vector<std::string> evidence;
    for (std::string_view path_text : {"/etc/crontab", "/etc/cron.d", "/etc/cron.daily", "/etc/cron.hourly",
                                      "/var/spool/cron", "/var/spool/cron/crontabs", "/etc/init.d", "/etc/rc.local",
                                      "/Library/LaunchAgents", "/Library/LaunchDaemons"}) {
        if (evidence.size() >= report.max_lines) {
            break;
        }
        std::error_code ec;
        const std::filesystem::path path(path_text);
        if (std::filesystem::exists(path, ec)) {
            evidence.push_back("persistence_path=" + path.string());
        }
    }
    std::vector<std::string> cron = command_lines("crontab -l 2>/dev/null | head -n 12", 12);
    evidence.insert(evidence.end(), cron.begin(), cron.end());
#if defined(__APPLE__)
    std::vector<std::string> launchd = command_lines("launchctl list 2>/dev/null | head -n 20", 20);
    evidence.insert(evidence.end(), launchd.begin(), launchd.end());
#else
    std::vector<std::string> timers = command_lines("systemctl list-timers --all --no-pager 2>/dev/null | head -n 20", 20);
    evidence.insert(evidence.end(), timers.begin(), timers.end());
#endif
    add_detection_signal(report,
                         "persistence",
                         "scheduled.persistence",
                         "medium",
                         "cron/launchd/systemd",
                         "Persistence surfaces were enumerated read-only to identify scheduled or startup execution points.",
                         evidence,
                         "Investigate new scheduled entries before containment; preserve evidence first.",
                         0.78);
}

void collect_package_detection(DefenseDetectionReport& report) {
    std::vector<std::string> evidence;
    for (std::string_view tool : {"npm", "pip", "pip3", "curl", "wget", "brew", "apt-get", "yum", "dnf"}) {
        std::vector<std::string> found = command_lines("command -v " + std::string(tool) + " 2>/dev/null", 1);
        for (const std::string& line : found) {
            evidence.push_back(std::string("tool_present=") + std::string(tool) + " path=" + line);
        }
    }
#if defined(__APPLE__)
    std::vector<std::string> brew = command_lines("brew list --versions 2>/dev/null | head -n 12", 12);
    evidence.insert(evidence.end(), brew.begin(), brew.end());
#else
    for (std::string_view log : {"/var/log/apt/history.log", "/var/log/dpkg.log", "/var/log/yum.log", "/var/log/dnf.log"}) {
        std::error_code ec;
        if (std::filesystem::exists(std::filesystem::path(log), ec)) {
            evidence.push_back("package_log=" + std::string(log));
        }
    }
#endif
    std::string severity = report.quiet_hours.empty() ? "low" : "medium";
    std::string rationale = "Package/script tooling and local package logs were inspected without installing or updating anything.";
    if (!report.quiet_hours.empty()) {
        rationale += " Quiet-hours context is active; package activity during this window should be treated as anomalous until verified.";
    }
    add_detection_signal(report,
                         "packages",
                         "package.script.surface",
                         severity,
                         "package_tools",
                         rationale,
                         evidence,
                         "If package activity is unexpected, preserve logs and compare against approved maintenance windows.",
                         0.7);
}

void collect_service_detection(DefenseDetectionReport& report) {
    std::vector<std::string> evidence;
#if defined(__APPLE__)
    evidence = command_lines("launchctl list 2>/dev/null | head -n 30", std::min<std::size_t>(report.max_lines, 30));
#else
    evidence = command_lines("systemctl --type=service --state=running --no-pager 2>/dev/null | head -n 30", std::min<std::size_t>(report.max_lines, 30));
    std::vector<std::string> enabled = command_lines("systemctl list-unit-files --state=enabled --no-pager 2>/dev/null | head -n 20", 20);
    evidence.insert(evidence.end(), enabled.begin(), enabled.end());
#endif
    add_detection_signal(report,
                         "services",
                         "service.state.surface",
                         "low",
                         "launchd/systemd",
                         "Service state was sampled read-only to reveal enabled/running service surfaces and possible restart-loop investigation targets.",
                         evidence,
                         "For unknown listening services, run `omnix gg search --pid <term>` or `omnix gg search --port <port>` before packet inspection.",
                         0.69);
}

void collect_log_detection(DefenseDetectionReport& report) {
    std::vector<std::string> evidence;
#if defined(__APPLE__)
    evidence = command_lines("log show --last 10m --style compact --predicate 'eventMessage CONTAINS[c] \"ssh\" OR eventMessage CONTAINS[c] \"sudo\" OR eventMessage CONTAINS[c] \"install\" OR eventMessage CONTAINS[c] \"launch\"' 2>/dev/null | head -n 30", std::min<std::size_t>(report.max_lines, 30));
#else
    evidence = command_lines("journalctl --since '1 hour ago' -n 60 --no-pager 2>/dev/null | grep -Ei 'ssh|sudo|cron|apt|dnf|yum|npm|systemd|service' | head -n 30", std::min<std::size_t>(report.max_lines, 30));
    if (evidence.empty()) {
        evidence = command_lines("dmesg 2>/dev/null | tail -n 20", 20);
    }
#endif
    add_detection_signal(report,
                         "logs",
                         "auth.system.logs",
                         evidence.empty() ? "low" : "medium",
                         "system_logs",
                         "Bounded system/auth log excerpts were inspected for SSH, sudo, package, cron, and service-change indicators.",
                         evidence,
                         "Preserve matching log excerpts into a case before making containment changes.",
                         0.74);
    std::string joined;
    for (const std::string& line : evidence) {
        joined += line + "\n";
    }
    VuplusGateReport scratch;
    report.session_correlations = session_correlations_from_text(lowercase_basic(joined), scratch.key_pairs);
    if (!report.session_correlations.empty() && !has_detection_signal(report, "session.syslog_lastlog.correlation")) {
        add_detection_signal(report,
                             "logs",
                             "session.syslog_lastlog.correlation",
                             report.quiet_hours.empty() ? "low" : "medium",
                             "syslog/lastlog/who",
                             "Auth/syslog evidence was cross-referenced against session-style evidence where available.",
                             evidence,
                             "If session evidence is unmatched or odd-hour, verify the operator before containment.",
                             0.76);
    }
}

void collect_eventviewer_detection(DefenseDetectionReport& report) {
    constexpr std::size_t kMinEventLogBytes = 1073741824ULL;
    std::vector<std::string> evidence;
    if (!report.source_path.empty()) {
        std::ifstream input(report.source_path);
        if (input) {
            std::ostringstream buffer;
            buffer << input.rdbuf();
            const std::string content = buffer.str();
            VuplusGateReport scratch;
            scan_vg_json_key_pairs(scratch, content, "eventviewer_fixture");
            report.event_viewer_retention = event_viewer_retention_from_pairs(scratch.key_pairs);
            for (const EventViewerRetention& record : report.event_viewer_retention) {
                evidence.push_back("channel=" + record.channel +
                                   " maxSizeBytes=" + std::to_string(record.max_size_bytes) +
                                   " belowMinimum=" + (record.below_minimum ? "true" : "false"));
            }
        } else {
            report.warnings.push_back("eventviewer_source_missing:" + report.source_path);
        }
    } else {
#if defined(_WIN32)
        const std::string channels = report.eventviewer_channels.empty() ? "Security,System,Application"
                                                                         : report.eventviewer_channels;
        std::istringstream split(channels);
        std::string channel;
        while (std::getline(split, channel, ',') && evidence.size() < report.max_lines) {
            channel = trim_local(channel);
            if (channel.empty()) {
                continue;
            }
            std::vector<std::string> lines = command_lines("wevtutil gl " + channel, 12);
            evidence.insert(evidence.end(), lines.begin(), lines.end());
        }
#else
        report.warnings.push_back("eventviewer_unsupported_platform: Windows Event Viewer metadata is only available on Windows or via --source fixture.");
#endif
    }

    bool below_minimum = false;
    for (const EventViewerRetention& record : report.event_viewer_retention) {
        below_minimum = below_minimum || record.max_size_bytes < kMinEventLogBytes;
    }
    if (!report.event_viewer_retention.empty()) {
        add_detection_signal(report,
                             "eventviewer",
                             below_minimum ? "eventviewer.retention.below_1gb" : "eventviewer.retention.healthy",
                             below_minimum ? "medium" : "low",
                             report.source_path.empty() ? "wevtutil" : report.source_path,
                             below_minimum
                                 ? "One or more Windows Event Viewer channels are below the 1GB evidence-retention floor; OmniX recommends CAB review only."
                                 : "Windows Event Viewer retention metadata is at or above the 1GB evidence-retention floor for inspected channels.",
                             evidence,
                             below_minimum
                                 ? "Prepare an Alarm CAB recommendation; changing retention requires elevated Admin/SYSTEM approval."
                                 : "Keep current retention and continue local evidence correlation.",
                             below_minimum ? 0.82 : 0.7);
    } else {
        add_detection_signal(report,
                             "eventviewer",
                             "eventviewer.unsupported_or_missing",
                             "low",
                             report.source_path.empty() ? "platform" : report.source_path,
                             "Event Viewer metadata could not be collected from this platform or fixture.",
                             evidence,
                             "Run this detector on Windows or provide a local Event Viewer metadata fixture with --source.",
                             0.45);
    }
}

void render_event_viewer_retention_json(std::ostringstream& out,
                                        const std::vector<EventViewerRetention>& records);
void render_session_correlations_json(std::ostringstream& out,
                                      const std::vector<SessionCorrelation>& correlations);
void render_heuristic_signals_json(std::ostringstream& out,
                                   const std::vector<HeuristicSignal>& signals);
void render_alarm_cab_json(std::ostringstream& out, const std::optional<AlarmCabRecommendation>& cab);
void render_shaped_fields_json(std::ostringstream& out, const std::vector<ShapedField>& fields);
void render_shaping_rules_json(std::ostringstream& out, const std::vector<ShapingRule>& rules);
void render_key_custody_json(std::ostringstream& out, const std::optional<KeyCustodyMap>& custody);

std::string render_defense_detection_json(const DefenseDetectionReport& report) {
    std::ostringstream out;
    out << "{\"event_type\":\"omnix.defense.detection.v1\""
        << ",\"status\":\"" << local_json_escape(report.status)
        << "\",\"summary\":\"" << local_json_escape(report.summary)
        << "\",\"mode\":\"" << local_json_escape(report.mode)
        << "\",\"since\":\"" << local_json_escape(report.since_window)
        << "\",\"quietHours\":\"" << local_json_escape(report.quiet_hours)
        << "\",\"adminUser\":\"" << local_json_escape(report.admin_user)
        << "\",\"maxLines\":" << report.max_lines
        << ",\"signals\":[";
    for (std::size_t index = 0; index < report.signals.size(); ++index) {
        const DefenseDetectionSignal& signal = report.signals[index];
        if (index != 0) {
            out << ",";
        }
        out << "{\"category\":\"" << local_json_escape(signal.category)
            << "\",\"id\":\"" << local_json_escape(signal.id)
            << "\",\"severity\":\"" << local_json_escape(signal.severity)
            << "\",\"source\":\"" << local_json_escape(signal.source)
            << "\",\"confidence\":" << signal.confidence
            << ",\"rationale\":\"" << local_json_escape(signal.rationale)
            << "\",\"recommendedNextAction\":\"" << local_json_escape(signal.recommended_next_action)
            << "\",\"evidence\":[";
        for (std::size_t evidence_index = 0; evidence_index < signal.evidence_lines.size(); ++evidence_index) {
            if (evidence_index != 0) {
                out << ",";
            }
            out << "\"" << local_json_escape(signal.evidence_lines[evidence_index]) << "\"";
        }
        out << "]}";
    }
    out << "],";
    render_event_viewer_retention_json(out, report.event_viewer_retention);
    out << ",";
    render_session_correlations_json(out, report.session_correlations);
    out << ",";
    render_heuristic_signals_json(out, report.heuristic_signals);
    out << ",";
    render_shaped_fields_json(out, report.shaped_fields);
    out << ",";
    render_shaping_rules_json(out, report.shaping_rules);
    out << ",";
    render_key_custody_json(out, report.key_custody);
    out << ",\"executionTopology\":\"" << local_json_escape(report.execution_topology) << "\",";
    render_alarm_cab_json(out, report.alarm_cab);
    out << ",\"warnings\":[";
    for (std::size_t index = 0; index < report.warnings.size(); ++index) {
        if (index != 0) {
            out << ",";
        }
        out << "\"" << local_json_escape(report.warnings[index]) << "\"";
    }
    out << "],\"proposedActions\":[";
    for (std::size_t index = 0; index < report.proposed_actions.size(); ++index) {
        if (index != 0) {
            out << ",";
        }
        out << "\"" << local_json_escape(report.proposed_actions[index]) << "\"";
    }
    out << "]}";
    return out.str();
}

bool export_defense_detection_json(DefenseDetectionReport& report, const std::string& path) {
    if (path.empty()) {
        return true;
    }
    std::ofstream output(path);
    if (!output) {
        report.warnings.push_back("Unable to write detection evidence JSON to `" + path + "`.");
        return false;
    }
    report.artifact_path = path;
    output << render_defense_detection_json(report) << "\n";
    return true;
}

std::string read_small_text_file(const std::string& path, bool* ok = nullptr) {
    std::ifstream input(path);
    if (!input) {
        if (ok != nullptr) {
            *ok = false;
        }
        return {};
    }
    std::ostringstream out;
    out << input.rdbuf();
    if (ok != nullptr) {
        *ok = true;
    }
    return out.str();
}

void append_vg_signal(VuplusGateReport& report, const std::string& signal) {
    if (std::find(report.signals.begin(), report.signals.end(), signal) == report.signals.end()) {
        report.signals.push_back(signal);
    }
}

bool has_vg_signal(const VuplusGateReport& report, const std::string& signal) {
    return std::find(report.signals.begin(), report.signals.end(), signal) != report.signals.end();
}

bool has_detection_signal(const DefenseDetectionReport& report, const std::string& id) {
    return std::any_of(report.signals.begin(), report.signals.end(), [&](const DefenseDetectionSignal& signal) {
        return signal.id == id;
    });
}

std::string trim_vg_value(std::string value) {
    value = trim_local(value);
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value = value.substr(1, value.size() - 2);
    }
    if (value.size() > 240) {
        value = value.substr(0, 237) + "...";
    }
    return value;
}

std::optional<std::size_t> parse_size_value(std::string value) {
    value = trim_vg_value(std::move(value));
    std::string digits;
    for (char c : value) {
        if (std::isdigit(static_cast<unsigned char>(c))) {
            digits.push_back(c);
        } else if (!digits.empty()) {
            break;
        }
    }
    if (digits.empty()) {
        return std::nullopt;
    }
    try {
        return static_cast<std::size_t>(std::stoull(digits));
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::string key_pair_value(const std::vector<VuplusGateReport::KeyPair>& pairs, std::string_view wanted_key) {
    const std::string wanted = lowercase_basic(std::string(wanted_key));
    for (const VuplusGateReport::KeyPair& pair : pairs) {
        if (lowercase_basic(pair.key) == wanted) {
            return pair.value;
        }
    }
    return {};
}

bool is_integer_text(const std::string& value) {
    if (value.empty()) {
        return false;
    }
    std::size_t index = (value.front() == '-' || value.front() == '+') ? 1 : 0;
    if (index >= value.size()) {
        return false;
    }
    for (; index < value.size(); ++index) {
        if (!std::isdigit(static_cast<unsigned char>(value[index]))) {
            return false;
        }
    }
    return true;
}

bool is_number_text(const std::string& value) {
    bool seen_digit = false;
    bool seen_dot = false;
    std::size_t index = (value.front() == '-' || value.front() == '+') ? 1 : 0;
    for (; index < value.size(); ++index) {
        const char c = value[index];
        if (std::isdigit(static_cast<unsigned char>(c))) {
            seen_digit = true;
        } else if (c == '.' && !seen_dot) {
            seen_dot = true;
        } else {
            return false;
        }
    }
    return seen_digit && seen_dot;
}

std::string infer_shape_type(const std::string& value) {
    const std::string lowered = lowercase_basic(value);
    if (lowered == "true" || lowered == "false") {
        return "boolean";
    }
    if (is_integer_text(value)) {
        return "integer";
    }
    if (is_number_text(value)) {
        return "number";
    }
    if (value.find('/') != std::string::npos || value.find('.') != std::string::npos ||
        value.find('-') != std::string::npos || value.find('_') != std::string::npos) {
        return "identifier";
    }
    return "string";
}

std::string semantic_for_field(const std::string& key) {
    const std::string lowered = lowercase_basic(key);
    if (lowered == "consumer_count" || lowered == "consumercount") {
        return "queue consumer count";
    }
    if (lowered == "queue" || lowered == "queue_name") {
        return "message queue identifier";
    }
    if (lowered == "error" || lowered == "errorcode" || lowered == "error_code") {
        return "application or log error signature";
    }
    if (lowered == "service" || lowered == "servicename") {
        return "application service name";
    }
    if (lowered == "rabbitmq_node" || lowered == "broker") {
        return "message broker node";
    }
    if (lowered == "ram" || lowered == "memory" || lowered == "memory_usage") {
        return "memory pressure metric";
    }
    if (lowered == "oldest_age" || lowered == "oldestmessageageseconds") {
        return "oldest queued message age";
    }
    if (lowered == "dependency" || lowered == "database") {
        return "dependency target";
    }
    if (lowered == "mount") {
        return "storage mount dependency";
    }
    if (lowered == "publish_rate") {
        return "queue publish rate";
    }
    if (lowered == "ack_rate") {
        return "queue acknowledgement rate";
    }
    if (lowered == "channel" || lowered == "maxsizebytes") {
        return "Windows Event Viewer retention metadata";
    }
    if (lowered == "server" || lowered == "host" || lowered == "host.name") {
        return "server identity anchor";
    }
    if (lowered == "cluster" || lowered == "kubernetes_cluster" || lowered == "namespace" ||
        lowered == "pod" || lowered == "node") {
        return "Kubernetes runtime identity";
    }
    if (lowered == "container" || lowered == "image") {
        return "container runtime identity";
    }
    if (lowered == "load_balancer" || lowered == "frontend" || lowered == "backend" ||
        lowered == "target_group" || lowered == "vip" || lowered == "drain_state" ||
        lowered == "health_status") {
        return "load balancer routing surface";
    }
    if (lowered == "terraform_action" || lowered == "resource_type" ||
        lowered == "resource_name" || lowered == "provider" || lowered == "gcp_project" ||
        lowered == "machine_type" || lowered == "network" || lowered == "subnet" ||
        lowered == "firewall_rule") {
        return "Terraform cloud resource declaration";
    }
    if (lowered == "ansible_group" || lowered == "ansible_controller" ||
        lowered == "playbook" || lowered == "role" || lowered == "inventory_host" ||
        lowered == "package_manager") {
        return "Ansible inventory/control mapping";
    }
    if (lowered == "omnix_master" || lowered == "omnix_minion" || lowered == "neighbor" ||
        lowered == "link" || lowered == "fingerprint" || lowered == "redundancy_group") {
        return "OmniX master/minion neighbor mapping";
    }
    if (lowered == "storage_mount" || lowered == "disk" || lowered == "volume" ||
        lowered == "persistent_volume" || lowered == "bucket" ||
        lowered == "data_loss_risk" || lowered == "shutdown_order") {
        return "storage dependency mapping";
    }
    if (lowered == "consumer") {
        return "queue consumer service";
    }
    if (lowered == "pkcs12_ref" || lowered == "key_ref" || lowered == "keyref") {
        return "certificate/key reference metadata";
    }
    if (lowered.find("encrypted") != std::string::npos || lowered.find("cipher") != std::string::npos) {
        return "encrypted evidence marker";
    }
    if (lowered.find("cert") != std::string::npos || lowered.find("fingerprint") != std::string::npos) {
        return "certificate custody metadata";
    }
    return "unclassified operational field";
}

std::string mapped_signal_for_field(const std::string& key, const std::string& value) {
    const std::string lowered_key = lowercase_basic(key);
    const std::string lowered_value = lowercase_basic(value);
    if ((lowered_key == "consumer_count" || lowered_key == "consumercount") && value == "0") {
        return "consumer.count.zero";
    }
    if (lowered_key == "error" && lowered_value == "eyz-47281") {
        return "log.signature.EYZ-47281";
    }
    if (lowered_key == "queue" && lowered_value == "xxb") {
        return "queue.XXB";
    }
    if (lowered_key == "rabbitmq_node" || lowered_value.find("rabbitmq") != std::string::npos) {
        return "broker.rabbitmq";
    }
    if (lowered_key == "dependency" && !value.empty()) {
        return "dependency.database";
    }
    if (lowered_key == "mount" && !value.empty()) {
        return "dependency.mount";
    }
    if (lowered_key == "maxsizebytes") {
        const std::size_t parsed = parse_size_value(value).value_or(0);
        return parsed > 0 && parsed < 1073741824ULL ? "eventviewer.retention.below_1gb"
                                                    : "eventviewer.retention.inspected";
    }
    if (lowered_key == "cluster" || lowered_key == "kubernetes_cluster" ||
        lowered_key == "namespace" || lowered_key == "pod" || lowered_key == "node") {
        return "orchestrator.kubernetes";
    }
    if (lowered_key == "container" || lowered_key == "image") {
        return "runtime.container";
    }
    if (lowered_key == "load_balancer" || lowered_key == "frontend") {
        return "loadbalancer.frontend";
    }
    if (lowered_key == "backend" || lowered_key == "target_group") {
        return "loadbalancer.backend";
    }
    if (lowered_key == "health_status" && !value.empty() &&
        (lowered_value != "healthy" || lowered_value.find("unhealthy") != std::string::npos)) {
        return "loadbalancer.target.unhealthy";
    }
    if (lowered_key == "drain_state" && lowered_value.find("not") != std::string::npos) {
        return "loadbalancer.drain.not_ready";
    }
    if (lowered_key == "terraform_action" || lowered_key == "resource_type" ||
        lowered_key == "resource_name" || lowered_key == "machine_type" ||
        lowered_key == "firewall_rule") {
        return "iac.terraform.plan";
    }
    if ((lowered_key == "provider" && (lowered_value == "google" || lowered_value == "gcp")) ||
        lowered_key == "gcp_project") {
        return "cloud.google.compute";
    }
    if (lowered_key == "ansible_group" || lowered_key == "ansible_controller" ||
        lowered_key == "playbook" || lowered_key == "role" || lowered_key == "inventory_host") {
        return "config.ansible.inventory";
    }
    if (lowered_key == "omnix_master" || lowered_key == "omnix_minion" ||
        lowered_key == "neighbor" || lowered_key == "link") {
        return "omnix.node.neighbor";
    }
    if (lowered_key == "storage_mount" || lowered_key == "disk" || lowered_key == "volume" ||
        lowered_key == "persistent_volume" || lowered_key == "bucket") {
        return "storage.dependency";
    }
    if (lowered_key.find("encrypted") != std::string::npos ||
        (lowered_value == "true" && lowered_key == "encrypted")) {
        return "siem.encrypted_evidence";
    }
    return {};
}

std::string lineage_for_pair(const VuplusGateReport::KeyPair& pair) {
    std::ostringstream lineage;
    lineage << pair.source << "[" << pair.value_start << ".." << pair.value_end << "]";
    return lineage.str();
}

std::vector<ShapedField> shaped_fields_from_pairs(const std::vector<VuplusGateReport::KeyPair>& pairs) {
    std::vector<ShapedField> fields;
    for (const VuplusGateReport::KeyPair& pair : pairs) {
        if (pair.key.find('{') != std::string::npos || pair.value.find('{') != std::string::npos ||
            pair.value.find('[') != std::string::npos) {
            continue;
        }
        ShapedField field;
        field.field = pair.key;
        field.value = pair.value;
        field.type = infer_shape_type(pair.value);
        field.source = pair.source;
        field.lineage = lineage_for_pair(pair);
        field.semantic_meaning = semantic_for_field(pair.key);
        field.mapped_signal = mapped_signal_for_field(pair.key, pair.value);
        field.confidence = field.semantic_meaning == "unclassified operational field" ? 0.64 : 0.88;
        if (!field.mapped_signal.empty()) {
            field.confidence += 0.03;
        }
        if (field.confidence > 0.94) {
            field.confidence = 0.94;
        }
        fields.push_back(std::move(field));
    }
    return fields;
}

std::vector<ShapingRule> shaping_rules_from_fields(const std::vector<ShapedField>& fields) {
    std::vector<ShapingRule> rules;
    for (const ShapedField& field : fields) {
        if (field.semantic_meaning == "unclassified operational field") {
            continue;
        }
        const auto duplicate = std::find_if(rules.begin(), rules.end(), [&](const ShapingRule& rule) {
            return rule.source == field.source && rule.field == field.field;
        });
        if (duplicate != rules.end()) {
            continue;
        }
        ShapingRule rule;
        rule.source = field.source;
        rule.field = field.field;
        rule.type = field.type;
        rule.semantic_meaning = field.semantic_meaning;
        rule.mapped_signal = field.mapped_signal;
        rule.confidence = field.confidence;
        rules.push_back(std::move(rule));
    }
    return rules;
}

std::optional<KeyCustodyMap> key_custody_from_text(const std::string& lowered,
                                                   const std::vector<VuplusGateReport::KeyPair>& pairs) {
    const bool encrypted = lowered.find("encrypted") != std::string::npos ||
        lowered.find("ciphertext") != std::string::npos ||
        lowered.find("tls") != std::string::npos ||
        lowered.find("pkcs12") != std::string::npos ||
        lowered.find(".p12") != std::string::npos ||
        lowered.find(".pfx") != std::string::npos;
    if (!encrypted) {
        return std::nullopt;
    }
    KeyCustodyMap custody;
    custody.encrypted_evidence = true;
    custody.visible_anchor = key_pair_value(pairs, "server");
    if (custody.visible_anchor.empty()) {
        custody.visible_anchor = key_pair_value(pairs, "host.name");
    }
    if (custody.visible_anchor.empty()) {
        custody.visible_anchor = key_pair_value(pairs, "host");
    }
    if (custody.visible_anchor.empty()) {
        custody.visible_anchor = "visible_server_or_log_source";
    }
    custody.route_hypothesis = {"server", "queue", "consumer", "app", "log-source"};
    custody.status = "operator_approval_required";
    custody.allowed_evidence = {"certificate fingerprint", "mtime", "owner", "path class", "key reference id"};
    custody.forbidden_evidence = {"private key material", "secret contents", "decrypted payload without approval"};
    custody.next_action = "Backtrace the server, queue, consumer, and log-source route; ask an approved operator to provide a key reference or run an approved decryptor locally.";
    return custody;
}

std::vector<EventViewerRetention> event_viewer_retention_from_pairs(const std::vector<VuplusGateReport::KeyPair>& pairs) {
    constexpr std::size_t kMinEventLogBytes = 1073741824ULL;
    std::vector<EventViewerRetention> records;
    for (const VuplusGateReport::KeyPair& pair : pairs) {
        const std::string key = lowercase_basic(pair.key);
        if (key == "channel") {
            EventViewerRetention record;
            record.channel = pair.value;
            record.retention_mode = "unknown";
            record.evidence_lines.push_back("channel=" + pair.value);
            records.push_back(std::move(record));
            continue;
        }
        if (records.empty()) {
            continue;
        }
        EventViewerRetention& current = records.back();
        if (key == "maxsizebytes" || key == "max_size_bytes" || key == "maxbytes") {
            current.max_size_bytes = parse_size_value(pair.value).value_or(0);
            current.below_minimum = current.max_size_bytes < kMinEventLogBytes;
            current.elevated_change_required = current.below_minimum;
            current.evidence_lines.push_back("maxSizeBytes=" + std::to_string(current.max_size_bytes));
        } else if (key == "retentionmode" || key == "retention" || key == "logmode") {
            current.retention_mode = pair.value;
            current.evidence_lines.push_back("retentionMode=" + pair.value);
        }
    }
    for (EventViewerRetention& record : records) {
        if (record.below_minimum) {
            record.recommendation = "CAB required: raise `" + record.channel +
                "` Event Viewer max size to at least 1073741824 bytes; example `wevtutil sl " +
                record.channel + " /ms:1073741824` requires elevated Admin/SYSTEM approval.";
        } else {
            record.recommendation = "Retention healthy: `" + record.channel +
                "` is at or above the 1GB evidence-retention floor.";
        }
    }
    return records;
}

std::vector<SessionCorrelation> session_correlations_from_text(const std::string& lowered,
                                                               const std::vector<VuplusGateReport::KeyPair>& pairs) {
    std::vector<SessionCorrelation> correlations;
    if (lowered.find("ssh") == std::string::npos &&
        lowered.find("lastlog") == std::string::npos &&
        lowered.find("who -a") == std::string::npos &&
        lowered.find("tty") == std::string::npos &&
        lowered.find("pts/") == std::string::npos) {
        return correlations;
    }

    SessionCorrelation correlation;
    correlation.actor = key_pair_value(pairs, "actor");
    if (correlation.actor.empty()) {
        correlation.actor = key_pair_value(pairs, "user");
    }
    if (correlation.actor.empty()) {
        correlation.actor = "unknown";
    }
    correlation.source = key_pair_value(pairs, "sourceIp");
    if (correlation.source.empty()) {
        correlation.source = key_pair_value(pairs, "source");
    }
    if (correlation.source.empty()) {
        correlation.source = "local_log_evidence";
    }
    correlation.tty = key_pair_value(pairs, "tty");
    if (correlation.tty.empty()) {
        correlation.tty = lowered.find("pts/") != std::string::npos ? "pts/*" : "unknown";
    }
    correlation.first_seen = key_pair_value(pairs, "firstSeen");
    correlation.last_seen = key_pair_value(pairs, "lastSeen");
    correlation.confidence = lowered.find("lastlog") != std::string::npos ? 0.78 : 0.62;
    correlation.evidence_refs.push_back("auth/syslog/session evidence");
    if (lowered.find("unmatched") != std::string::npos ||
        (lowered.find("ssh") != std::string::npos && lowered.find("lastlog") == std::string::npos)) {
        correlation.anomaly_rationale = "SSH/auth evidence did not have a matching lastlog/session confirmation in the artifact.";
        correlation.confidence = 0.82;
    } else {
        correlation.anomaly_rationale = "Syslog/auth evidence was cross-referenced with session/lastlog-style evidence.";
    }
    correlations.push_back(std::move(correlation));
    return correlations;
}

std::vector<HeuristicSignal> heuristic_signals_from_text(const std::string& lowered,
                                                         const std::vector<VuplusGateReport::KeyPair>& pairs) {
    std::vector<HeuristicSignal> signals;
    const auto add = [&](std::string id, std::string category, std::string severity, double confidence, std::string rationale) {
        HeuristicSignal signal;
        signal.id = std::move(id);
        signal.category = std::move(category);
        signal.severity = std::move(severity);
        signal.confidence = confidence;
        signal.rationale = std::move(rationale);
        signal.evidence_refs.push_back("local heuristic/RUM artifact");
        signals.push_back(std::move(signal));
    };

    if (lowered.find("rum") != std::string::npos || lowered.find("behavior") != std::string::npos ||
        lowered.find("latency") != std::string::npos || lowered.find("error burst") != std::string::npos) {
        if (lowered.find("latency") != std::string::npos || !key_pair_value(pairs, "p95LatencyMs").empty()) {
            add("rum.latency_spike", "rum", "medium", 0.74,
                "Encoded RUM-style latency evidence suggests a user-impacting response-time spike.");
        }
        if (lowered.find("error") != std::string::npos || !key_pair_value(pairs, "errorRate").empty()) {
            add("rum.error_burst", "rum", "medium", 0.72,
                "Encoded RUM-style error evidence suggests a clustered user-facing failure pattern.");
        }
        if (lowered.find("session") != std::string::npos || lowered.find("navigation") != std::string::npos) {
            add("behavior.session_anomaly", "behavior", "low", 0.66,
                "Behavioral session/navigation evidence is present for future Citizen-AI pattern recognition.");
        }
    }
    return signals;
}

AlarmCabRecommendation make_alarm_cab(const VuplusGateReport& report) {
    AlarmCabRecommendation cab;
    cab.alarm_id = key_pair_value(report.key_pairs, "alarmId");
    if (cab.alarm_id.empty()) {
        cab.alarm_id = key_pair_value(report.key_pairs, "alarm_id");
    }
    if (cab.alarm_id.empty()) {
        cab.alarm_id = has_vg_signal(report, "queue.XXB") ? "P1-RMQ-2B-XXB-STOPPED-REPORTING"
                                                          : "OMNIX-VG-CAB-RECOMMENDATION";
    }
    cab.recommendation_status = "recommendation_only";
    cab.proposed_change = "Review the Vuplus Gate recommendation and approve only after owner validation.";
    cab.approval_requirement = "Operator/CAB approval required; OmniX will not execute this change.";
    cab.owner = "Engineer Infra";
    cab.timestamp = key_pair_value(report.key_pairs, "timestamp");
    cab.threshold_window = key_pair_value(report.key_pairs, "thresholdWindow");
    if (cab.threshold_window.empty()) {
        cab.threshold_window = "initial_vuplus_gate_window";
    }
    cab.blast_radius = report.operational_blast_radius;
    cab.rollback_impact = report.rollback_impact;
    cab.signals = report.signals;
    cab.validation_checks = {
        "Confirm evidence source and owner approval.",
        "Validate affected asset state before action.",
        "Record before/after metrics and logs.",
        "Use `omnix next latest --compact` after CAB review."
    };
    for (const VuplusGateReport::KeyPair& pair : report.key_pairs) {
        const std::string key = lowercase_basic(pair.key);
        if (key == "host" || key == "server" || key == "queue" || key == "service" ||
            key == "app" || key == "database" || key == "mount" || key == "customer" || key == "site") {
            cab.affected_assets.push_back(pair.key + "=" + pair.value);
        }
    }
    for (const EventViewerRetention& retention : report.event_viewer_retention) {
        if (retention.below_minimum) {
            if (cab.alarm_id == "OMNIX-VG-CAB-RECOMMENDATION") {
                cab.alarm_id = "OMNIX-EVENTVIEWER-RETENTION-" + retention.channel;
            }
            cab.proposed_change = "Raise Windows Event Viewer channel `" + retention.channel +
                "` to at least 1GB retention.";
            cab.approval_requirement = "Requires elevated Administrator/SYSTEM approval and change-control review.";
            cab.affected_assets.push_back("eventviewer.channel=" + retention.channel);
            cab.validation_checks.push_back("Run `wevtutil gl " + retention.channel +
                                            "` after approval to confirm maxSize >= 1073741824.");
            break;
        }
    }
    if (cab.affected_assets.empty()) {
        cab.affected_assets.push_back("unknown_asset");
    }
    return cab;
}

std::size_t find_vg_json_value_end(const std::string& text, std::size_t value_start) {
    while (value_start < text.size() && std::isspace(static_cast<unsigned char>(text[value_start]))) {
        ++value_start;
    }
    if (value_start >= text.size()) {
        return value_start;
    }

    const char first = text[value_start];
    if (first == '"') {
        bool escaped = false;
        for (std::size_t index = value_start + 1; index < text.size(); ++index) {
            const char c = text[index];
            if (escaped) {
                escaped = false;
                continue;
            }
            if (c == '\\') {
                escaped = true;
                continue;
            }
            if (c == '"') {
                return index + 1;
            }
        }
        return text.size();
    }

    if (first == '{' || first == '[') {
        int depth = 0;
        bool in_string = false;
        bool escaped = false;
        for (std::size_t index = value_start; index < text.size(); ++index) {
            const char c = text[index];
            if (in_string) {
                if (escaped) {
                    escaped = false;
                } else if (c == '\\') {
                    escaped = true;
                } else if (c == '"') {
                    in_string = false;
                }
                continue;
            }
            if (c == '"') {
                in_string = true;
            } else if (c == '{' || c == '[') {
                ++depth;
            } else if (c == '}' || c == ']') {
                --depth;
                if (depth <= 0) {
                    return index + 1;
                }
            }
        }
        return text.size();
    }

    std::size_t index = value_start;
    while (index < text.size()) {
        const char c = text[index];
        if (c == ',' || c == '}' || c == ']' || c == '\n' || c == '\r') {
            break;
        }
        ++index;
    }
    return index;
}

void append_vg_key_pair(VuplusGateReport& report,
                        std::string key,
                        std::string value,
                        std::string source,
                        std::size_t value_start,
                        std::size_t value_end) {
    if (report.key_pairs.size() >= 64) {
        return;
    }
    key = trim_vg_value(std::move(key));
    value = trim_vg_value(std::move(value));
    if (key.empty() || value.empty()) {
        return;
    }
    for (const VuplusGateReport::KeyPair& existing : report.key_pairs) {
        if (existing.key == key && existing.value == value && existing.source == source &&
            existing.value_start == value_start && existing.value_end == value_end) {
            return;
        }
    }
    VuplusGateReport::KeyPair pair;
    pair.key = std::move(key);
    pair.value = std::move(value);
    pair.source = std::move(source);
    pair.value_start = value_start;
    pair.value_end = value_end;
    report.key_pairs.push_back(std::move(pair));
}

void scan_vg_embedded_pairs(VuplusGateReport& report, const std::string& blob, const std::string& source_key) {
    std::string token;
    std::size_t token_start = 0;
    const auto flush = [&](std::size_t token_end) {
        const std::string trimmed = trim_local(token);
        const std::size_t current_start = token_start;
        token.clear();
        if (trimmed.size() < 3) {
            return;
        }
        const std::size_t eq = trimmed.find('=');
        if (eq == std::string::npos || eq == 0 || eq + 1 >= trimmed.size()) {
            return;
        }
        std::string key = trimmed.substr(0, eq);
        std::string value = trimmed.substr(eq + 1);
        while (!value.empty() && (value.back() == ',' || value.back() == ';' || value.back() == '|')) {
            value.pop_back();
        }
        append_vg_key_pair(report,
                           std::move(key),
                           std::move(value),
                           "embedded:" + source_key,
                           current_start + eq + 1,
                           token_end);
    };

    for (std::size_t index = 0; index < blob.size(); ++index) {
        const char c = blob[index];
        if (std::isspace(static_cast<unsigned char>(c)) || c == ';' || c == '|') {
            flush(index);
        } else {
            if (token.empty()) {
                token_start = index;
            }
            token.push_back(c);
        }
    }
    flush(blob.size());
}

void scan_vg_json_key_pairs(VuplusGateReport& report, const std::string& text, const std::string& source) {
    std::size_t index = 0;
    while (index < text.size() && report.key_pairs.size() < 64) {
        if (text[index] != '"') {
            ++index;
            continue;
        }
        const std::size_t key_start = index + 1;
        bool escaped = false;
        std::size_t key_end = std::string::npos;
        for (std::size_t cursor = key_start; cursor < text.size(); ++cursor) {
            const char c = text[cursor];
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                key_end = cursor;
                break;
            }
        }
        if (key_end == std::string::npos) {
            break;
        }
        std::size_t colon = key_end + 1;
        while (colon < text.size() && std::isspace(static_cast<unsigned char>(text[colon]))) {
            ++colon;
        }
        if (colon >= text.size() || text[colon] != ':') {
            index = key_end + 1;
            continue;
        }
        std::size_t value_start = colon + 1;
        while (value_start < text.size() && std::isspace(static_cast<unsigned char>(text[value_start]))) {
            ++value_start;
        }
        const std::size_t value_end = find_vg_json_value_end(text, value_start);
        const std::string key = text.substr(key_start, key_end - key_start);
        const std::string value = value_end > value_start ? text.substr(value_start, value_end - value_start) : std::string{};
        append_vg_key_pair(report, key, value, source, value_start, value_end);

        const std::string lowered_key = lowercase_basic(key);
        if (lowered_key.find("data") != std::string::npos ||
            lowered_key.find("meta") != std::string::npos ||
            lowered_key.find("message") != std::string::npos ||
            lowered_key.find("original") != std::string::npos) {
            scan_vg_embedded_pairs(report, trim_vg_value(value), key);
        }
        if (value_start < text.size() && (text[value_start] == '{' || text[value_start] == '[')) {
            index = value_start + 1;
            continue;
        }
        index = value_end > key_end ? value_end : key_end + 1;
    }
}

std::string render_vuplus_gate_json(const VuplusGateReport& report) {
    std::ostringstream out;
    out << "{\"event_type\":\"omnix.vg.explain.v1\""
        << ",\"segment\":\"" << local_json_escape(report.segment)
        << "\",\"status\":\"" << local_json_escape(report.status)
        << "\",\"mode\":\"" << local_json_escape(report.mode)
        << "\",\"inputPath\":\"" << local_json_escape(report.input_path)
        << "\",\"dependencyMapPath\":\"" << local_json_escape(report.dependency_map_path)
        << "\",\"why\":\"" << local_json_escape(report.why)
        << "\",\"signals\":[";
    for (std::size_t index = 0; index < report.signals.size(); ++index) {
        if (index != 0) {
            out << ",";
        }
        out << "\"" << local_json_escape(report.signals[index]) << "\"";
    }
    out << "],\"confidence\":" << report.confidence
        << ",\"historicalCorrelation\":\"" << local_json_escape(report.historical_correlation)
        << "\",\"operationalBlastRadius\":\"" << local_json_escape(report.operational_blast_radius)
        << "\",\"rollbackImpact\":\"" << local_json_escape(report.rollback_impact)
        << "\",\"nextAction\":\"" << local_json_escape(report.next_action)
        << "\",\"remediationMode\":\"" << local_json_escape(report.remediation_mode)
        << "\",\"executionTopology\":\"" << local_json_escape(report.execution_topology)
        << "\",\"keyPairs\":[";
    for (std::size_t index = 0; index < report.key_pairs.size(); ++index) {
        if (index != 0) {
            out << ",";
        }
        const VuplusGateReport::KeyPair& pair = report.key_pairs[index];
        out << "{\"key\":\"" << local_json_escape(pair.key)
            << "\",\"value\":\"" << local_json_escape(pair.value)
            << "\",\"source\":\"" << local_json_escape(pair.source)
            << "\",\"valueStart\":" << pair.value_start
            << ",\"valueEnd\":" << pair.value_end
            << "}";
    }
    out << "],";
    render_event_viewer_retention_json(out, report.event_viewer_retention);
    out << ",";
    render_session_correlations_json(out, report.session_correlations);
    out << ",";
    render_heuristic_signals_json(out, report.heuristic_signals);
    out << ",";
    render_shaped_fields_json(out, report.shaped_fields);
    out << ",";
    render_shaping_rules_json(out, report.shaping_rules);
    out << ",";
    render_key_custody_json(out, report.key_custody);
    out << ",";
    render_alarm_cab_json(out, report.alarm_cab);
    out << ",\"warnings\":[";
    for (std::size_t index = 0; index < report.warnings.size(); ++index) {
        if (index != 0) {
            out << ",";
        }
        out << "\"" << local_json_escape(report.warnings[index]) << "\"";
    }
    out << "]}";
    return out.str();
}

void render_event_viewer_retention_json(std::ostringstream& out,
                                        const std::vector<EventViewerRetention>& records) {
    out << "\"eventViewerRetention\":[";
    for (std::size_t index = 0; index < records.size(); ++index) {
        if (index != 0) {
            out << ",";
        }
        const EventViewerRetention& record = records[index];
        out << "{\"channel\":\"" << local_json_escape(record.channel)
            << "\",\"maxSizeBytes\":" << record.max_size_bytes
            << ",\"retentionMode\":\"" << local_json_escape(record.retention_mode)
            << "\",\"belowMinimum\":" << (record.below_minimum ? "true" : "false")
            << ",\"elevatedChangeRequired\":" << (record.elevated_change_required ? "true" : "false")
            << ",\"recommendation\":\"" << local_json_escape(record.recommendation)
            << "\",\"evidence\":[";
        for (std::size_t evidence_index = 0; evidence_index < record.evidence_lines.size(); ++evidence_index) {
            if (evidence_index != 0) {
                out << ",";
            }
            out << "\"" << local_json_escape(record.evidence_lines[evidence_index]) << "\"";
        }
        out << "]}";
    }
    out << "]";
}

void render_session_correlations_json(std::ostringstream& out,
                                      const std::vector<SessionCorrelation>& correlations) {
    out << "\"sessionCorrelations\":[";
    for (std::size_t index = 0; index < correlations.size(); ++index) {
        if (index != 0) {
            out << ",";
        }
        const SessionCorrelation& correlation = correlations[index];
        out << "{\"actor\":\"" << local_json_escape(correlation.actor)
            << "\",\"source\":\"" << local_json_escape(correlation.source)
            << "\",\"tty\":\"" << local_json_escape(correlation.tty)
            << "\",\"firstSeen\":\"" << local_json_escape(correlation.first_seen)
            << "\",\"lastSeen\":\"" << local_json_escape(correlation.last_seen)
            << "\",\"confidence\":" << correlation.confidence
            << ",\"anomalyRationale\":\"" << local_json_escape(correlation.anomaly_rationale)
            << "\",\"evidenceRefs\":[";
        for (std::size_t evidence_index = 0; evidence_index < correlation.evidence_refs.size(); ++evidence_index) {
            if (evidence_index != 0) {
                out << ",";
            }
            out << "\"" << local_json_escape(correlation.evidence_refs[evidence_index]) << "\"";
        }
        out << "]}";
    }
    out << "]";
}

void render_heuristic_signals_json(std::ostringstream& out,
                                   const std::vector<HeuristicSignal>& signals) {
    out << "\"heuristicSignals\":[";
    for (std::size_t index = 0; index < signals.size(); ++index) {
        if (index != 0) {
            out << ",";
        }
        const HeuristicSignal& signal = signals[index];
        out << "{\"id\":\"" << local_json_escape(signal.id)
            << "\",\"category\":\"" << local_json_escape(signal.category)
            << "\",\"severity\":\"" << local_json_escape(signal.severity)
            << "\",\"confidence\":" << signal.confidence
            << ",\"rationale\":\"" << local_json_escape(signal.rationale)
            << "\",\"evidenceRefs\":[";
        for (std::size_t evidence_index = 0; evidence_index < signal.evidence_refs.size(); ++evidence_index) {
            if (evidence_index != 0) {
                out << ",";
            }
            out << "\"" << local_json_escape(signal.evidence_refs[evidence_index]) << "\"";
        }
        out << "]}";
    }
    out << "]";
}

void render_alarm_cab_json(std::ostringstream& out, const std::optional<AlarmCabRecommendation>& cab) {
    out << "\"alarmCab\":";
    if (!cab.has_value()) {
        out << "null";
        return;
    }
    out << "{\"alarmId\":\"" << local_json_escape(cab->alarm_id)
        << "\",\"recommendationStatus\":\"" << local_json_escape(cab->recommendation_status)
        << "\",\"proposedChange\":\"" << local_json_escape(cab->proposed_change)
        << "\",\"approvalRequirement\":\"" << local_json_escape(cab->approval_requirement)
        << "\",\"owner\":\"" << local_json_escape(cab->owner)
        << "\",\"timestamp\":\"" << local_json_escape(cab->timestamp)
        << "\",\"thresholdWindow\":\"" << local_json_escape(cab->threshold_window)
        << "\",\"blastRadius\":\"" << local_json_escape(cab->blast_radius)
        << "\",\"rollbackImpact\":\"" << local_json_escape(cab->rollback_impact)
        << "\",\"affectedAssets\":[";
    for (std::size_t index = 0; index < cab->affected_assets.size(); ++index) {
        if (index != 0) {
            out << ",";
        }
        out << "\"" << local_json_escape(cab->affected_assets[index]) << "\"";
    }
    out << "],\"signals\":[";
    for (std::size_t index = 0; index < cab->signals.size(); ++index) {
        if (index != 0) {
            out << ",";
        }
        out << "\"" << local_json_escape(cab->signals[index]) << "\"";
    }
    out << "],\"validationChecks\":[";
    for (std::size_t index = 0; index < cab->validation_checks.size(); ++index) {
        if (index != 0) {
            out << ",";
        }
        out << "\"" << local_json_escape(cab->validation_checks[index]) << "\"";
    }
    out << "]}";
}

void render_shaped_fields_json(std::ostringstream& out, const std::vector<ShapedField>& fields) {
    out << "\"shapedFields\":[";
    for (std::size_t index = 0; index < fields.size(); ++index) {
        if (index != 0) {
            out << ",";
        }
        const ShapedField& field = fields[index];
        out << "{\"field\":\"" << local_json_escape(field.field)
            << "\",\"value\":";
        if (field.type == "integer" || field.type == "number") {
            out << field.value;
        } else if (field.type == "boolean") {
            out << lowercase_basic(field.value);
        } else {
            out << "\"" << local_json_escape(field.value) << "\"";
        }
        out << ",\"type\":\"" << local_json_escape(field.type)
            << "\",\"source\":\"" << local_json_escape(field.source)
            << "\",\"lineage\":\"" << local_json_escape(field.lineage)
            << "\",\"semanticMeaning\":\"" << local_json_escape(field.semantic_meaning)
            << "\",\"mappedSignal\":\"" << local_json_escape(field.mapped_signal)
            << "\",\"confidence\":" << field.confidence
            << "}";
    }
    out << "]";
}

void render_shaping_rules_json(std::ostringstream& out, const std::vector<ShapingRule>& rules) {
    out << "\"shapingRules\":[";
    for (std::size_t index = 0; index < rules.size(); ++index) {
        if (index != 0) {
            out << ",";
        }
        const ShapingRule& rule = rules[index];
        out << "{\"match\":{\"source\":\"" << local_json_escape(rule.source)
            << "\",\"field\":\"" << local_json_escape(rule.field)
            << "\"},\"type\":\"" << local_json_escape(rule.type)
            << "\",\"semanticMeaning\":\"" << local_json_escape(rule.semantic_meaning)
            << "\",\"mappedSignal\":\"" << local_json_escape(rule.mapped_signal)
            << "\",\"confidence\":" << rule.confidence
            << "}";
    }
    out << "]";
}

void render_key_custody_json(std::ostringstream& out, const std::optional<KeyCustodyMap>& custody) {
    out << "\"keyCustody\":";
    if (!custody.has_value()) {
        out << "null";
        return;
    }
    out << "{\"encryptedEvidence\":" << (custody->encrypted_evidence ? "true" : "false")
        << ",\"visibleAnchor\":\"" << local_json_escape(custody->visible_anchor)
        << "\",\"routeHypothesis\":[";
    for (std::size_t index = 0; index < custody->route_hypothesis.size(); ++index) {
        if (index != 0) {
            out << ",";
        }
        out << "\"" << local_json_escape(custody->route_hypothesis[index]) << "\"";
    }
    out << "],\"status\":\"" << local_json_escape(custody->status)
        << "\",\"allowedEvidence\":[";
    for (std::size_t index = 0; index < custody->allowed_evidence.size(); ++index) {
        if (index != 0) {
            out << ",";
        }
        out << "\"" << local_json_escape(custody->allowed_evidence[index]) << "\"";
    }
    out << "],\"forbiddenEvidence\":[";
    for (std::size_t index = 0; index < custody->forbidden_evidence.size(); ++index) {
        if (index != 0) {
            out << ",";
        }
        out << "\"" << local_json_escape(custody->forbidden_evidence[index]) << "\"";
    }
    out << "],\"nextAction\":\"" << local_json_escape(custody->next_action) << "\"}";
}

VuplusGateReport run_vuplus_gate(const RequestProfile& profile) {
    VuplusGateReport report;
    report.mode = profile.vuplus_mode.empty() ? "doctor" : profile.vuplus_mode;
    report.input_path = profile.vuplus_input_path;
    report.dependency_map_path = profile.vuplus_dependency_map_path;
    report.remediation_mode = "recommendation_only";

    if (report.mode == "doctor") {
        report.status = "vg_ready";
        report.why = "Vuplus Gate is ready for local operational intelligence over logs, alerts, notes, thresholds, dependency maps, shutdown sequences, and recovery comparisons.";
        report.confidence = 0.9;
        report.historical_correlation = "Uses existing OmniX surfaces: ingest, defend detect, thresholds/GSMg, case, incident, why, next, and TZE diff.";
        report.operational_blast_radius = "Local fixture scope: app, queue, DB, mount, ingress, package/runtime surfaces, customer/site context.";
        report.rollback_impact = "No remediation is executed; rollback impact is advisory until a future allowlisted runbook phase.";
        report.next_action = "Run `omnix vg explain res/ops/elastic-siem-rabbitmq-xxb.json --compact`.";
        append_vg_signal(report, "vuplus.gate.ready");
        append_vg_signal(report, "remediation.recommendation_only");
        report.json = render_vuplus_gate_json(report);
        return report;
    }

    bool input_ok = false;
    const std::string content = read_small_text_file(report.input_path, &input_ok);
    if (!input_ok) {
        report.status = "vg_input_missing";
        report.why = "Vuplus Gate could not read the requested local ops artifact.";
        report.confidence = 0.0;
        report.historical_correlation = "No correlation is possible without a readable local artifact.";
        report.operational_blast_radius = "unknown";
        report.rollback_impact = "No rollback guidance can be trusted until evidence is loaded.";
        report.next_action = "Provide a readable artifact from `res/ops/` or an equivalent local JSON/log file.";
        report.warnings.push_back("input_missing:" + report.input_path);
        report.json = render_vuplus_gate_json(report);
        return report;
    }

    std::string dependency_content;
    if (!report.dependency_map_path.empty()) {
        bool dependency_ok = false;
        dependency_content = read_small_text_file(report.dependency_map_path, &dependency_ok);
        if (!dependency_ok) {
            report.warnings.push_back("dependency_map_missing:" + report.dependency_map_path);
        }
    }

    scan_vg_json_key_pairs(report, content, "input");
    if (!dependency_content.empty()) {
        scan_vg_json_key_pairs(report, dependency_content, "dependency_map");
    }
    if (!report.key_pairs.empty()) {
        append_vg_signal(report, "siem.keypair.boundary_extracted");
    }

    std::string extracted_pairs;
    for (const VuplusGateReport::KeyPair& pair : report.key_pairs) {
        extracted_pairs += pair.key + "=" + pair.value + "\n";
    }
    const std::string lowered = lowercase_basic(content + "\n" + dependency_content + "\n" + extracted_pairs);
    report.event_viewer_retention = event_viewer_retention_from_pairs(report.key_pairs);
    report.session_correlations = session_correlations_from_text(lowered, report.key_pairs);
    report.heuristic_signals = heuristic_signals_from_text(lowered, report.key_pairs);
    report.shaped_fields = shaped_fields_from_pairs(report.key_pairs);
    if (report.mode == "shape" || profile.vuplus_learn_shape) {
        report.shaping_rules = shaping_rules_from_fields(report.shaped_fields);
    }
    report.key_custody = key_custody_from_text(lowered, report.key_pairs);
    if (!report.shaped_fields.empty()) {
        append_vg_signal(report, "siem.shape.fields_inferred");
        for (const ShapedField& field : report.shaped_fields) {
            if (!field.mapped_signal.empty()) {
                append_vg_signal(report, field.mapped_signal);
            }
        }
    }
    if (!report.shaping_rules.empty()) {
        append_vg_signal(report, "siem.shape.rules_proposed");
    }
    if (report.key_custody.has_value()) {
        append_vg_signal(report, "siem.encrypted_evidence");
        append_vg_signal(report, "custody.operator_approval_required");
    }
    if (!report.event_viewer_retention.empty()) {
        append_vg_signal(report, "eventviewer.retention.inspected");
        for (const EventViewerRetention& retention : report.event_viewer_retention) {
            if (retention.below_minimum) {
                append_vg_signal(report, "eventviewer.retention.below_1gb");
            }
        }
    }
    if (!report.session_correlations.empty()) {
        append_vg_signal(report, "session.syslog_lastlog.correlation");
    }
    for (const HeuristicSignal& signal : report.heuristic_signals) {
        append_vg_signal(report, signal.id);
    }
    if (lowered.find("eyz-47281") != std::string::npos) {
        append_vg_signal(report, "log.signature.EYZ-47281");
    }
    if (lowered.find("consumer") != std::string::npos && lowered.find("0") != std::string::npos) {
        append_vg_signal(report, "consumer.count.zero");
    }
    if (lowered.find("queue=xxb") != std::string::npos || lowered.find("\"xxb\"") != std::string::npos) {
        append_vg_signal(report, "queue.XXB");
    }
    if (lowered.find("rabbitmq") != std::string::npos || lowered.find("rmq") != std::string::npos) {
        append_vg_signal(report, "broker.rabbitmq");
    }
    if (lowered.find("apt") != std::string::npos) {
        append_vg_signal(report, "dependency.package.apt");
    }
    if (lowered.find("npm") != std::string::npos) {
        append_vg_signal(report, "dependency.package.npm");
    }
    if (lowered.find("pnpm") != std::string::npos) {
        append_vg_signal(report, "dependency.package.pnpm");
    }
    if (lowered.find("wget") != std::string::npos) {
        append_vg_signal(report, "dependency.download.wget");
    }
    if (lowered.find("powershell") != std::string::npos) {
        append_vg_signal(report, "dependency.script.powershell");
    }
    if (lowered.find("winget") != std::string::npos || lowered.find("windows_package_manager") != std::string::npos) {
        append_vg_signal(report, "dependency.package.windows");
    }
    if (lowered.find("brew") != std::string::npos) {
        append_vg_signal(report, "dependency.package.brew");
    }
    if (lowered.find("database") != std::string::npos || lowered.find("payments-db") != std::string::npos) {
        append_vg_signal(report, "dependency.database");
    }
    if (lowered.find("mount") != std::string::npos || lowered.find("/data/payments") != std::string::npos) {
        append_vg_signal(report, "dependency.mount");
    }
    if (lowered.find("ingress") != std::string::npos || lowered.find("load_balancer") != std::string::npos) {
        append_vg_signal(report, "dependency.ingress");
    }
    if (lowered.find("successfulpath") != std::string::npos) {
        append_vg_signal(report, "recovery.success_path");
    }
    if (lowered.find("failedpath") != std::string::npos) {
        append_vg_signal(report, "recovery.failed_path");
    }

    report.confidence = 0.55;
    if (has_vg_signal(report, "log.signature.EYZ-47281")) {
        report.confidence += 0.12;
    }
    if (has_vg_signal(report, "consumer.count.zero")) {
        report.confidence += 0.08;
    }
    if (has_vg_signal(report, "queue.XXB")) {
        report.confidence += 0.08;
    }
    if (!dependency_content.empty()) {
        report.confidence += 0.08;
    }
    if (has_vg_signal(report, "siem.keypair.boundary_extracted")) {
        report.confidence += 0.04;
    }
    if (has_vg_signal(report, "recovery.success_path") ||
        has_vg_signal(report, "recovery.failed_path")) {
        report.confidence += 0.09;
    }
    if (has_vg_signal(report, "eventviewer.retention.below_1gb")) {
        report.confidence += 0.08;
    }
    if (has_vg_signal(report, "session.syslog_lastlog.correlation")) {
        report.confidence += 0.05;
    }
    if (has_vg_signal(report, "siem.shape.fields_inferred")) {
        report.confidence += 0.04;
    }
    if (has_vg_signal(report, "siem.encrypted_evidence")) {
        report.confidence += 0.04;
    }
    if (!report.heuristic_signals.empty()) {
        report.confidence += 0.05;
    }
    if (report.confidence > 0.95) {
        report.confidence = 0.95;
    }

    if (report.mode == "shape") {
        report.status = "vg_shaped";
        report.why = "Vuplus Gate inferred typed SIEM fields, semantic meanings, lineage, mapped signals, and reusable shaping rules from local evidence.";
        report.historical_correlation = "Shaping output turns semi-structured SIEM blobs into auditable fields before threshold, CAB, incident, or encrypted-evidence routing.";
        report.operational_blast_radius = "Data-shaping scope is local to the input artifact and does not mutate SIEM pipelines or secret material.";
        report.rollback_impact = "Rejecting a proposed shaping rule leaves the original artifact unchanged; accepting one should be reviewed through local audit.";
        report.next_action = "Review shapedFields and shapingRules; persist the JSON artifact with `--out` if the mapping should be reused.";
    } else if (report.mode == "cab") {
        report.status = "vg_cab_ready";
        report.why = "Vuplus Gate prepared a GUI-ready Alarm CAB JSON recommendation from local operational evidence.";
        report.historical_correlation = "CAB output preserves Vuplus signals, threshold windows, blast radius, rollback impact, and validation checks for future GUI/Web review.";
        report.operational_blast_radius = has_vg_signal(report, "eventviewer.retention.below_1gb")
            ? "Windows Event Viewer retention evidence can affect incident replay, escalation, and future forensic correlation."
            : "CAB artifact blast radius is derived from the local Vuplus evidence graph.";
        report.rollback_impact = has_vg_signal(report, "eventviewer.retention.below_1gb")
            ? "Leaving retention below 1GB risks losing local evidence before it can be correlated."
            : "Rollback impact remains advisory until the operator approves a concrete change.";
        report.next_action = "Review the Alarm CAB JSON with the owner; do not execute changes until approval and validation gates are recorded.";
        report.alarm_cab = make_alarm_cab(report);
    } else if (report.mode == "compare") {
        report.status = "vg_compared";
        report.why = "Vuplus Gate compared the successful and failed recovery paths and found dependency-order risk.";
        report.historical_correlation = "The successful path validates broker health, worker exhaustion, consumer recovery, and queue drain; the failed path jumps toward data-core actions before dependencies are quiet.";
        report.operational_blast_radius = "ABC worker, Queue XXB, RabbitMQ node, payments DB, /data/payments mount, and customer/site workflow.";
        report.rollback_impact = "Rolling back to the successful path lowers data-loss risk; continuing the failed path risks open pools, unacked messages, active writers, and unsafe storage actions.";
        report.next_action = "Use `omnix why latest` or future Versus Comparator output to backtrace the missing dependency-safe transition.";
    } else if (report.mode == "correlate") {
        report.status = "vg_correlated";
        report.why = "Vuplus Gate correlated the artifact with dependency and threshold-style signals.";
        report.historical_correlation = "Package/runtime changes and operational events should be compared against the incident timeline before remediation.";
        report.operational_blast_radius = has_vg_signal(report, "dependency.package.apt") ||
                has_vg_signal(report, "dependency.package.npm")
            ? "Package/runtime surface can affect app workers, queue clients, scripts, jump hosts, and incident-time reproducibility."
            : "Dependency graph spans ingress, services, queue, DB, mount, storage, and customer/site context.";
        report.rollback_impact = "Rollback must consider runtime package state, service dependency order, and whether queues or DB writers are still active.";
        report.next_action = "Preserve the correlated artifact into a case, then run `omnix next latest --compact` for the immediate operator step.";
    } else {
        report.status = "vg_explained";
        report.why = has_vg_signal(report, "eventviewer.retention.below_1gb")
            ? "Windows Event Viewer retention evidence is below the 1GB floor; OmniX recommends CAB review rather than automatic mutation."
            : has_vg_signal(report, "log.signature.EYZ-47281")
            ? "Queue XXB evidence points to app-worker memory exhaustion: RabbitMQ appears healthy, consumers dropped to zero, and EYZ-47281 appeared in worker logs."
            : "Vuplus Gate explained the local ops artifact from observable logs, alerts, notes, and dependency signals.";
        report.historical_correlation = "Matches the Queue XXB threshold/GSMg pattern: app worker exhaustion beats RabbitMQ node failure when broker health is green and consumers are zero.";
        report.operational_blast_radius = "Queue XXB -> abc-worker.service -> payments-db -> /data/payments -> CUST8 / NY_HOME_HEATING_OIL.";
        report.rollback_impact = "Waiting increases backlog and oldest-message age; restarting without validation risks unmeasured loss; escalating without evidence slows recovery.";
        report.next_action = "Recommend worker restart runbook only after operator review; validate service active, consumers reattached, queue depth drains, and no new heap error appears.";
    }

    if (!report.alarm_cab.has_value() && has_vg_signal(report, "eventviewer.retention.below_1gb")) {
        report.alarm_cab = make_alarm_cab(report);
        report.next_action = "Review the Event Viewer retention Alarm CAB recommendation; elevated approval is required before changing channel size.";
    }

    if (report.signals.empty()) {
        report.status = "vg_unsupported_artifact";
        report.why = "Vuplus Gate read the file but did not recognize supported ops signals in this V1 artifact.";
        report.confidence = 0.2;
        report.historical_correlation = "No supported local fixture pattern matched.";
        report.operational_blast_radius = "unknown";
        report.rollback_impact = "Unknown until the artifact is shaped into logs, alerts, dependency maps, or recovery comparison evidence.";
        report.next_action = "Shape the input as Elastic SIEM JSON, local syslog/auth evidence, package activity, dependency map, or recovery comparison.";
    }

    report.json = render_vuplus_gate_json(report);
    return report;
}

bool export_vuplus_gate_json(VuplusGateReport& report, const std::string& path) {
    if (path.empty()) {
        return true;
    }
    std::ofstream output(path);
    if (!output) {
        report.warnings.push_back("Unable to write Vuplus Gate JSON to `" + path + "`.");
        report.json = render_vuplus_gate_json(report);
        return false;
    }
    output << report.json << "\n";
    report.artifact_path = path;
    return true;
}

DefenseDetectionReport run_defense_detection(const RequestProfile& profile,
                                             const DefenseDiagnosticRequest& request) {
    DefenseDetectionReport report;
    report.mode = request.mode.empty() ? "all" : request.mode;
    report.since_window = profile.defense_since_window.empty() ? "24h" : profile.defense_since_window;
    report.quiet_hours = profile.defense_quiet_hours;
    report.admin_user = profile.defense_admin_user;
    report.max_lines = profile.defense_max_lines == 0 ? 40 : profile.defense_max_lines;
    report.eventviewer_channels = profile.defense_channels;
    report.source_path = profile.defense_source_path;
    report.execution_topology = "standalone_local_node";
    report.status = "defense_detection_complete";
    report.warnings.push_back("Detection-only mode: OmniX did not kill processes, alter services, change firewall state, or mutate packets.");
    report.warnings.push_back("Defense environmental intelligence remains local-only; OmniX does not send this evidence to configured API providers.");
    report.warnings.push_back("Stealth/PID hiding is intentionally unsupported; this detector stays transparent and auditable.");

    const auto run_mode = [&](const std::string& mode) {
        if (mode == "env") {
            collect_env_detection(report);
        } else if (mode == "sessions") {
            collect_session_detection(report);
        } else if (mode == "persistence") {
            collect_persistence_detection(report);
        } else if (mode == "packages") {
            collect_package_detection(report);
        } else if (mode == "services") {
            collect_service_detection(report);
        } else if (mode == "logs") {
            collect_log_detection(report);
        } else if (mode == "eventviewer") {
            collect_eventviewer_detection(report);
        }
    };

    if (report.mode == "all") {
        for (std::string_view mode : {"env", "sessions", "persistence", "packages", "services", "logs", "eventviewer"}) {
            run_mode(std::string(mode));
        }
    } else {
        run_mode(report.mode);
    }

    if (report.signals.empty()) {
        report.status = "defense_detection_empty";
        report.summary = "No local environmental change evidence was returned for the requested detector.";
    } else {
        std::sort(report.signals.begin(), report.signals.end(), [](const DefenseDetectionSignal& left,
                                                                   const DefenseDetectionSignal& right) {
            if (severity_rank(left.severity) != severity_rank(right.severity)) {
                return severity_rank(left.severity) > severity_rank(right.severity);
            }
            return left.confidence > right.confidence;
        });
        report.summary = "Detected " + std::to_string(report.signals.size()) +
            " local environmental signal(s) using read-only evidence.";
    }
    report.proposed_actions.push_back("Review the top signal rationale before containment.");
    report.proposed_actions.push_back("If network transit is relevant, explicitly run `omnix gg search --port <port>` or `omnix tview port <port>`.");
    for (const EventViewerRetention& retention : report.event_viewer_retention) {
        if (retention.below_minimum) {
            AlarmCabRecommendation cab;
            cab.alarm_id = "OMNIX-EVENTVIEWER-RETENTION-" + retention.channel;
            cab.recommendation_status = "recommendation_only";
            cab.proposed_change = "Raise Windows Event Viewer channel `" + retention.channel + "` to at least 1GB retention.";
            cab.approval_requirement = "Requires elevated Administrator/SYSTEM approval; OmniX does not mutate Event Viewer settings.";
            cab.owner = report.admin_user.empty() ? "Windows Administrator" : report.admin_user;
            cab.threshold_window = "eventviewer_retention_floor_1gb";
            cab.blast_radius = "Windows Event Viewer channel `" + retention.channel + "` and future incident evidence retention.";
            cab.rollback_impact = "Leaving retention below 1GB risks losing forensic/log evidence before Vuplus Gate can correlate incidents.";
            cab.affected_assets.push_back("eventviewer.channel=" + retention.channel);
            cab.signals.push_back("eventviewer.retention.below_1gb");
            cab.validation_checks.push_back("After approval, confirm `wevtutil gl " + retention.channel + "` reports maxSize >= 1073741824.");
            report.alarm_cab = cab;
            break;
        }
    }
    return report;
}

std::string dispatch_module_for(RequestIntent intent) {
    switch (intent) {
        case RequestIntent::Conversation:
            return "ConversationFlow";
        case RequestIntent::GeneralDefinitionQuery:
            return "DefinitionFlowInterpreter";
        case RequestIntent::ReviewModule:
        case RequestIntent::PatchProposal:
            return "SelfReviewEngine";
        case RequestIntent::IngestData:
        case RequestIntent::AnalyzeCase:
        case RequestIntent::DecideAction:
        case RequestIntent::InspectCase:
            return "AnalystFlowInterpreter";
        case RequestIntent::ReplayTzeRun:
        case RequestIntent::ChainTzeRun:
        case RequestIntent::RecursiveWhyDiff:
        case RequestIntent::DiffTzeRuns:
        case RequestIntent::ExplainTzeChange:
        case RequestIntent::ReportTzeRun:
        case RequestIntent::DiffReportTzeRuns:
        case RequestIntent::ExportTzeBundle:
        case RequestIntent::ImportTzeBundle:
        case RequestIntent::PruneTzeRuns:
        case RequestIntent::MarkTzeRunOutcome:
        case RequestIntent::CaseTimeline:
        case RequestIntent::MarkDecisionFeedback:
        case RequestIntent::MarkDecisionOutcome:
        case RequestIntent::ExportCaseBundle:
        case RequestIntent::ImportCaseBundle:
        case RequestIntent::ListIncidents:
        case RequestIntent::InspectIncident:
        case RequestIntent::ReportIncident:
            return "TzeRunLedger";
        case RequestIntent::PruneMemory:
            return "MemoryStore";
        case RequestIntent::PacketCapture:
            return "PacketCaptureEngine";
        case RequestIntent::DefenseDiagnostic:
            return "DefenseDiagnosticEngine";
        case RequestIntent::DefenseDetection:
            return "DefenseEnvironmentDetectEngine";
        case RequestIntent::VuplusGate:
            return "VuplusGate";
        case RequestIntent::NeuralMath:
            return "NeuralMathEngine";
        case RequestIntent::NeuralRoute:
            return "NeuralSignalRouter";
        case RequestIntent::TensorAction:
            return "TensorFramework";
        case RequestIntent::SetPersonaMode:
            return "PersonaEngine";
        case RequestIntent::DoctorProject:
            return "ProjectDoctor";
        case RequestIntent::ToolAction:
            return "ToolFlowInterpreter";
        case RequestIntent::ProbeProvider:
            return "ReasoningProvider";
        case RequestIntent::DefineSymbol:
        case RequestIntent::ExplainCommand:
            return "DefinitionFlowInterpreter";
        case RequestIntent::BuildProject:
            return "BuildFlowInterpreter";
        case RequestIntent::AuthorBuildRecipe:
            return "RecipeAuthoringEngine";
        case RequestIntent::InspectToolchain:
            return "BuildExecutor";
        case RequestIntent::ShowMemory:
            return "MemoryStore";
        case RequestIntent::Unknown:
            return "DefinitionEngine";
    }
    return "DefinitionEngine";
}

std::string render_assist_annotation_markdown(const AssistAnnotation& assist) {
    std::ostringstream out;
    out << "## Guarded Assist\n\n";
    out << "- Provider: " << assist.provider_id;
    if (!assist.model.empty()) {
        out << " (" << assist.model << ")";
    }
    out << "\n";
    out << "- Status: " << assist.status << "\n";
    out << "- Summary: " << assist.summary << "\n";
    if (!assist.highlights.empty()) {
        out << "- Highlights:\n";
        for (const std::string& highlight : assist.highlights) {
            out << "  - " << highlight << "\n";
        }
    }
    if (!assist.operator_takeaway.empty()) {
        out << "- Operator takeaway: " << assist.operator_takeaway << "\n";
    }
    if (!assist.warnings.empty()) {
        out << "- Warnings:\n";
        for (const std::string& warning : assist.warnings) {
            out << "  - " << warning << "\n";
        }
    }
    return out.str();
}

std::string render_next_step_annotation_text(const NextStepAssistPlan& plan) {
    std::ostringstream out;
    out << "\nAssist next step: " << plan.suggested_next_step
        << " [" << std::fixed << std::setprecision(2) << plan.confidence << "]";
    if (!plan.rationale.empty()) {
        out << "\nAssist rationale: " << plan.rationale;
    }
    if (!plan.safer_alternative.empty()) {
        out << "\nSafer alternative: " << plan.safer_alternative;
    }
    return out.str();
}

std::string render_case_summary_annotation_text(const CaseSummaryAssistPlan& plan) {
    std::ostringstream out;
    if (!plan.summary_title.empty()) {
        out << "\nAssist summary title: " << plan.summary_title;
    }
    out << "\nAssist executive summary: " << plan.executive_summary;
    if (!plan.highlights.empty()) {
        out << "\nAssist highlights:";
        for (const std::string& highlight : plan.highlights) {
            out << "\n - " << highlight;
        }
    }
    return out.str();
}

std::string render_freeform_assist_text(const FreeformAssistAnswer& answer) {
    std::ostringstream out;
    out << answer.answer;
    if (!answer.suggested_commands.empty()) {
        out << "\nSuggested commands:";
        for (const std::string& command : answer.suggested_commands) {
            out << "\n - " << command;
        }
    }
    if (!answer.safety_warnings.empty()) {
        out << "\nSafety notes:";
        for (const std::string& warning : answer.safety_warnings) {
            out << "\n - " << warning;
        }
    }
    return out.str();
}

std::string render_assist_annotation_text(const AssistAnnotation& assist) {
    std::ostringstream out;
    out << "\nGuarded Assist:\n";
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
    return out.str();
}

std::vector<std::string> allowlisted_tool_assist_actions() {
    return {"inspect-log", "inspect-build", "inspect-host", "regex-search", "deep-grep"};
}

std::string native_tool_verify_command(std::string_view logical_name) {
    if (logical_name == "nmap") {
        return "omnix tool nmap -- -V";
    }
    return "omnix tool " + std::string(logical_name) + " -- --version";
}

bool has_control_characters(std::string_view value) {
    for (char c : value) {
        if (std::iscntrl(static_cast<unsigned char>(c)) && c != '\t') {
            return true;
        }
    }
    return false;
}

bool validate_tool_assist_plan(const ToolAssistPlan& proposed,
                               ToolAssistPlan* validated_plan,
                               std::string* reason) {
    const std::vector<std::string> allowlist = allowlisted_tool_assist_actions();
    if (std::find(allowlist.begin(), allowlist.end(), proposed.tool_name) == allowlist.end()) {
        if (reason != nullptr) {
            *reason = "Proposed tool `" + proposed.tool_name + "` is not on the allowlist.";
        }
        return false;
    }
    if (proposed.arguments.size() > 8) {
        if (reason != nullptr) {
            *reason = "Proposed argument count exceeds the bounded allowlist policy.";
        }
        return false;
    }
    for (const std::string& argument : proposed.arguments) {
        if (argument.size() > 512 || has_control_characters(argument)) {
            if (reason != nullptr) {
                *reason = "Proposed argument payload failed basic validation.";
            }
            return false;
        }
    }

    const auto existing_path = [](const std::string& value) {
        std::error_code ec;
        return std::filesystem::exists(value, ec);
    };
    const auto regular_file = [](const std::string& value) {
        std::error_code ec;
        return std::filesystem::is_regular_file(value, ec);
    };
    const auto directory = [](const std::string& value) {
        std::error_code ec;
        return std::filesystem::is_directory(value, ec);
    };

    if (proposed.tool_name == "inspect-log") {
        if (proposed.arguments.size() != 1 || !regular_file(proposed.arguments.front())) {
            if (reason != nullptr) {
                *reason = "`inspect-log` requires exactly one existing regular file path.";
            }
            return false;
        }
    } else if (proposed.tool_name == "inspect-build") {
        if (proposed.arguments.size() != 1 || !existing_path(proposed.arguments.front())) {
            if (reason != nullptr) {
                *reason = "`inspect-build` requires exactly one existing local path.";
            }
            return false;
        }
    } else if (proposed.tool_name == "inspect-host") {
        if (!proposed.arguments.empty() &&
            !(proposed.arguments.size() == 1 && proposed.arguments.front() == "--linux")) {
            if (reason != nullptr) {
                *reason = "`inspect-host` accepts no arguments or a single `--linux` hint.";
            }
            return false;
        }
    } else if (proposed.tool_name == "regex-search") {
        if (proposed.arguments.size() < 2 || trim_local(proposed.arguments.front()).empty()) {
            if (reason != nullptr) {
                *reason = "`regex-search` requires a pattern followed by one or more existing paths.";
            }
            return false;
        }
        for (std::size_t index = 1; index < proposed.arguments.size(); ++index) {
            if (!existing_path(proposed.arguments[index])) {
                if (reason != nullptr) {
                    *reason = "`regex-search` path arguments must already exist locally.";
                }
                return false;
            }
        }
    } else if (proposed.tool_name == "deep-grep") {
        if (proposed.arguments.size() != 2 || trim_local(proposed.arguments.front()).empty() ||
            !directory(proposed.arguments[1])) {
            if (reason != nullptr) {
                *reason = "`deep-grep` requires a pattern and one existing directory root.";
            }
            return false;
        }
    }

    if (validated_plan != nullptr) {
        *validated_plan = proposed;
        validated_plan->validated = true;
        validated_plan->status = "validated";
    }
    if (reason != nullptr) {
        *reason = "Validated allowlisted tool `" + proposed.tool_name + "` with " +
            std::to_string(proposed.arguments.size()) + " argument(s).";
    }
    return true;
}

std::string lowercase_local(std::string_view value) {
    std::string lowered;
    lowered.reserve(value.size());
    for (char c : value) {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return lowered;
}

bool starts_with_local(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

std::string lowercase(std::string_view value) {
    std::string lowered;
    lowered.reserve(value.size());
    for (char c : value) {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return lowered;
}

bool is_legacy_source_path(std::string_view source_map_path) {
    const std::string lowered = lowercase(source_map_path);
    return lowered.find("tzu.cpp") != std::string::npos;
}

bool is_greeting_prompt(std::string_view prompt) {
    const std::string lowered = lowercase(prompt);
    return lowered == "hello" || lowered == "hi" || lowered == "hey" ||
        lowered == "hello omnix" || lowered == "hi omnix" || lowered == "hey omnix";
}

bool is_identity_prompt(std::string_view prompt) {
    const std::string lowered = lowercase(prompt);
    return lowered == "who are you" || lowered == "what are you" ||
        lowered == "what is omnix" || lowered == "who is omnix" ||
        lowered == "tell me about yourself";
}

bool is_operator_identity_prompt(std::string_view prompt) {
    const std::string lowered = lowercase(prompt);
    return lowered == "who am i" || lowered == "who am i?" ||
        lowered == "what is my persona" || lowered == "what's my persona";
}

bool is_context_summary_prompt(std::string_view prompt) {
    const std::string lowered = lowercase(prompt);
    return lowered == "what matters" || lowered == "what matters here" ||
        lowered == "what should i care about" || lowered == "tell me what matters";
}

std::string env_or(std::string_view name, std::string_view fallback = {}) {
    const char* value = std::getenv(std::string(name).c_str());
    if (value == nullptr || *value == '\0') {
        return std::string(fallback);
    }
    return value;
}

std::string local_operator_identity() {
    return env_or("USER", "unknown-user");
}

std::string local_host_identity() {
    return env_or("HOSTNAME", env_or("HOST", "unknown-host"));
}

std::string canonical_persona_mode(std::string_view value) {
    const std::string lowered = lowercase_local(trim_local(value));
    if (lowered == "premise" || lowered == "premise-mode" || lowered == "premise mode") {
        return "premise";
    }
    if (lowered == "cynic" || lowered == "cynic-mode" || lowered == "cynic mode") {
        return "cynic";
    }
    if (lowered == "professional" || lowered == "pro" || lowered == "professional-mode" ||
        lowered == "professional mode") {
        return "professional";
    }
    if (lowered == "neutral" || lowered == "default" || lowered == "plain") {
        return "neutral";
    }
    return {};
}

void apply_persona_mode_defaults(OperatorPersonaRecord& persona, std::string_view mode) {
    const std::string canonical = canonical_persona_mode(mode);
    persona.persona_mode = canonical.empty() ? "neutral" : canonical;
    persona.safety_posture = "display_only_safety_bounded";
    if (persona.persona_mode == "premise") {
        if (persona.preferred_label.empty()) {
            persona.preferred_label = "Premise";
        }
        if (persona.role_label.empty()) {
            persona.role_label = "Local Operator";
        }
        if (persona.self_description.empty()) {
            persona.self_description = "Local truth-first builder; persistent, playful, and verification-heavy.";
        }
        persona.tone_profile = "warm_playful_local_truth";
        persona.interaction_style = "pairing_persistent_momentum";
        persona.preferred_next_action_style = "concrete_next_step_with_context";
    } else if (persona.persona_mode == "cynic") {
        persona.tone_profile = "skeptical_risk_first";
        persona.interaction_style = "challenge_assumptions_and_find_edge_cases";
        persona.preferred_next_action_style = "name_the_risk_then_offer_the_safe_move";
    } else if (persona.persona_mode == "professional") {
        persona.tone_profile = "concise_formal_operator_briefing";
        persona.interaction_style = "direct_structured_execution";
        persona.preferred_next_action_style = "brief_status_then_action";
    } else {
        persona.persona_mode = "neutral";
        persona.tone_profile = "balanced_plain";
        persona.interaction_style = "clear_helpful_default";
        persona.preferred_next_action_style = "simple_next_step";
    }
}

std::string persona_mode_readout(const OperatorPersonaRecord& persona) {
    if (persona.persona_mode == "premise") {
        return "Premise Mode: local truth, playful grit, and persistent verification. Safety remains bounded.";
    }
    if (persona.persona_mode == "cynic") {
        return "Cynic Mode: skeptical, risk-first, and contradiction-hunting. Safety remains bounded.";
    }
    if (persona.persona_mode == "professional") {
        return "Professional Mode: concise, formal, and execution-focused. Safety remains bounded.";
    }
    return "Neutral Mode: balanced, plain, and safety-bounded.";
}

std::string render_context_summary(const MemorySnapshot& memory) {
    if (!memory.tze_runs.empty()) {
        const TzeRunRecord& last = memory.tze_runs.back();
        std::ostringstream out;
        out << "What matters right now is the latest persisted run";
        if (!last.prompt.empty()) {
            out << " from `" << last.prompt << "`";
        }
        out << ": status `" << last.status << "`.";
        if (last.definition_answer.has_value() && !last.definition_answer->summary.empty()) {
            out << " " << last.definition_answer->summary;
        } else if (!last.next_action.empty()) {
            out << " " << last.next_action;
        }
        return out.str();
    }
    if (!memory.history.empty()) {
        const MemoryHistoryEntry& last = memory.history.back();
        std::ostringstream out;
        out << "The most recent remembered interaction is `" << last.prompt << "`";
        if (!last.status.empty()) {
            out << " with status `" << last.status << "`";
        }
        out << ".";
        if (!last.summary.empty()) {
            out << " " << last.summary;
        }
        return out.str();
    }
    return "I do not have a recent run, case, or incident yet, so there is no active local context to summarize.";
}

LegacySourceRecord make_legacy_source_record(const xpp::MappingUnit& unit) {
    const xpp::SymbolIndex index = xpp::build_symbol_index(unit);
    LegacySourceRecord record;
    record.source_path = unit.source_name;
    record.source_label = is_legacy_source_path(unit.source_name) ? "Tzu.cpp" : "legacy-source";
    record.source_kind = is_legacy_source_path(unit.source_name) ? "legacy_uncut_blueprint" : "legacy_source";
    record.line_count = unit.lines.size();
    record.section_count = unit.sections.size();
    record.symbol_count = index.mappings.size();
    record.source_hash = std::to_string(std::hash<std::string>{}(unit.source_name + "|" + std::to_string(record.line_count)));
    record.id = "legacy-" + std::to_string(std::hash<std::string>{}(record.source_path + "|" + record.source_hash));
    return record;
}

std::string recovery_status_for(const xpp::SymbolMapping& mapping) {
    if (mapping.status == xpp::MappingStatus::Mapped) {
        return "implemented";
    }
    if (mapping.status == xpp::MappingStatus::Abstracted || mapping.status == xpp::MappingStatus::Stubbed) {
        return mapping.family == xpp::SemanticFamily::SecurityBlocked ? "research-only" : "partial";
    }
    if (mapping.status == xpp::MappingStatus::Unsupported) {
        return mapping.family == xpp::SemanticFamily::SecurityBlocked ? "blocked" : "missing";
    }
    return "missing";
}

std::vector<LegacySymbolCoverage> build_legacy_symbol_coverage(const xpp::MappingUnit& unit) {
    const xpp::SymbolIndex index = xpp::build_symbol_index(unit);
    static const std::vector<std::string> kLegacySymbols = {
        "xProcessingCache",
        "x.Define.Low",
        "x.DisplayPriorityProcessingGate",
        "x.DisplayFeedBackLoop",
        "xLanguageEngine",
        "x.determineOSLanguage",
        "x.checkFile",
        "x.Decompress",
        "TernaryDecompression",
        "Base4Decompression",
        "xXOmni::Premit",
        "xXOmni::RequestSecureKeyTunnel",
        "xXOmni::LegacyScan",
        "xXOmni::Detection",
        "xXOmni::ScopeDefense",
        "uAC_Traits",
        "x.reGENx",
        "uAC::OperationalUsageHabit",
        "uAC::DeletionDescrepancies",
        "uAC::SearchKeyContext",
    };

    std::vector<LegacySymbolCoverage> coverage;
    for (const std::string& symbol : kLegacySymbols) {
        const xpp::SymbolMapping* mapping = xpp::find_mapping(index, symbol);
        LegacySymbolCoverage entry;
        entry.symbol = symbol;
        entry.source_origin = unit.source_name;
        if (mapping == nullptr) {
            entry.recovery_status = "missing";
            entry.notes.push_back("No indexed mapping found in the parsed legacy source.");
            coverage.push_back(std::move(entry));
            continue;
        }
        entry.semantic_family = std::string(xpp::to_string(mapping->family));
        entry.mapped_cpp_target = mapping->mapped_cpp_target;
        entry.occurrence_count = mapping->occurrences.size();
        if (!mapping->occurrences.empty()) {
            entry.section_title = mapping->occurrences.front().section_title;
        }
        entry.recovery_status = recovery_status_for(*mapping);
        if (mapping->status == xpp::MappingStatus::Abstracted) {
            entry.notes.push_back("Mapped into a bounded runtime abstraction.");
        } else if (mapping->status == xpp::MappingStatus::Stubbed) {
            entry.notes.push_back("Present in current runtime seams but still shallow.");
        } else if (mapping->status == xpp::MappingStatus::Unsupported) {
            entry.notes.push_back("Tracked as blocked or research-only semantics.");
        }
        coverage.push_back(std::move(entry));
    }
    return coverage;
}

LegacyRecoveryStatus summarize_legacy_recovery(std::string_view source_label,
                                               const std::vector<LegacySymbolCoverage>& coverage) {
    LegacyRecoveryStatus status;
    status.source_label = std::string(source_label);
    for (const LegacySymbolCoverage& entry : coverage) {
        if (entry.recovery_status == "implemented") {
            ++status.implemented_count;
        } else if (entry.recovery_status == "partial") {
            ++status.partial_count;
        } else if (entry.recovery_status == "research-only") {
            ++status.research_only_count;
        } else if (entry.recovery_status == "blocked") {
            ++status.blocked_count;
        } else {
            ++status.missing_count;
        }
    }
    status.summary_lines.push_back("implemented=" + std::to_string(status.implemented_count));
    status.summary_lines.push_back("partial=" + std::to_string(status.partial_count));
    status.summary_lines.push_back("missing=" + std::to_string(status.missing_count));
    status.summary_lines.push_back("research-only=" + std::to_string(status.research_only_count));
    status.summary_lines.push_back("blocked=" + std::to_string(status.blocked_count));
    return status;
}

std::vector<std::string> allowlisted_command_assist_patterns() {
    return {
        "build <project-or-path>",
        "recipe author <source-path>",
        "preflight <project-or-path>",
        "doctor <project-or-path>",
        "ingest <path-or-command>",
        "analyze <case-or-source>",
        "decide <case-or-source>",
        "case list",
        "case search <term>",
        "case timeline <id-or-source>",
        "case <id-or-source>",
        "incident list",
        "incident report <id>",
        "incident <id>",
        "define <symbol-or-term>",
        "explain <command-or-symbol>",
        "review <path-or-module>",
        "patch-proposal <path-or-module>",
        "provider probe",
        "persona mode <premise|cynic|professional|neutral>",
        "memory <history|prefs|definitions|language|security|uac|cases|runs|tze|legacy|persona|operator|assist>",
        "tze latest",
        "tze replay <run-id|latest>",
        "tze chain <run-id|latest>",
        "tze diff <left-run-id> <right-run-id>",
        "tze diff-latest",
        "tze explain-change <left-run-id> <right-run-id>",
        "tze explain-change-latest",
        "tze report <run-id|latest>",
        "tool list",
        "tool locate <name>",
        "tool doctor <name>",
        "tool <name> -- <args...>",
    };
}

bool is_loopback_target(std::string_view value) {
    const std::string lowered = lowercase_local(trim_local(value));
    return lowered == "127.0.0.1" || lowered == "localhost" || lowered == "::1";
}

bool is_loopback_subnet_target(std::string_view value) {
    return lowercase_local(trim_local(value)) == "127.0.0.0/24";
}

std::vector<std::string> split_whitespace_local(std::string_view value) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start < value.size()) {
        while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start]))) {
            ++start;
        }
        if (start >= value.size()) {
            break;
        }
        std::size_t end = start;
        while (end < value.size() && !std::isspace(static_cast<unsigned char>(value[end]))) {
            ++end;
        }
        parts.emplace_back(value.substr(start, end - start));
        start = end;
    }
    return parts;
}

std::optional<std::string> find_safe_scan_target(std::string_view prompt) {
    const std::string lowered = lowercase_local(prompt);
    if (lowered.find("local /24") != std::string::npos ||
        lowered.find("local/24") != std::string::npos ||
        lowered.find("/24 local") != std::string::npos ||
        lowered.find("/24 locally") != std::string::npos ||
        lowered.find("locally /24") != std::string::npos ||
        lowered.find("localhost /24") != std::string::npos ||
        lowered.find("loopback /24") != std::string::npos ||
        lowered.find("loopback/24") != std::string::npos) {
        return std::string("127.0.0.0/24");
    }
    for (const std::string& token : split_whitespace_local(prompt)) {
        if (is_loopback_target(token) || is_loopback_subnet_target(token)) {
            return token;
        }
    }
    return std::nullopt;
}

std::string trim_scan_target_token(std::string_view value) {
    std::size_t start = 0;
    while (start < value.size()) {
        const char c = value[start];
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '[') {
            break;
        }
        ++start;
    }

    std::size_t end = value.size();
    while (end > start) {
        const char c = value[end - 1];
        if (std::isalnum(static_cast<unsigned char>(c)) || c == ']' || c == '/' || c == '-') {
            break;
        }
        --end;
    }
    return std::string(value.substr(start, end - start));
}

bool looks_like_network_target(std::string_view value) {
    if (value.empty()) {
        return false;
    }

    bool saw_digit = false;
    bool saw_alpha = false;
    bool saw_dot = false;
    for (char c : value) {
        if (std::isdigit(static_cast<unsigned char>(c))) {
            saw_digit = true;
            continue;
        }
        if (std::isalpha(static_cast<unsigned char>(c))) {
            saw_alpha = true;
            continue;
        }
        if (c == '.' || c == ':' || c == '/' || c == '-' || c == '[' || c == ']') {
            if (c == '.') {
                saw_dot = true;
            }
            continue;
        }
        return false;
    }

    return (saw_digit && (saw_dot || value.find(':') != std::string_view::npos ||
                          value.find('/') != std::string_view::npos || value.find('-') != std::string_view::npos)) ||
        (saw_alpha && saw_dot);
}

std::optional<std::string> find_explicit_unsafe_scan_target(std::string_view prompt) {
    for (const std::string& raw_token : split_whitespace_local(prompt)) {
        const std::string token = trim_scan_target_token(raw_token);
        if (token.empty()) {
            continue;
        }
        if (is_loopback_target(token) || is_loopback_subnet_target(token)) {
            continue;
        }
        if (looks_like_network_target(token)) {
            return token;
        }
    }
    return std::nullopt;
}

bool validate_safe_tool_invocation(std::string_view requested_tool_name,
                                   const std::vector<std::string>& proposed_arguments,
                                   bool allow_default_arguments,
                                   std::string* canonical_tool_name,
                                   std::vector<std::string>* canonical_arguments,
                                   std::string* reason) {
    const std::string tool_name = lowercase_local(trim_local(requested_tool_name));
    std::vector<std::string> arguments = proposed_arguments;

    auto accept = [&](std::vector<std::string> accepted_arguments, std::string detail) {
        if (canonical_tool_name != nullptr) {
            *canonical_tool_name = tool_name;
        }
        if (canonical_arguments != nullptr) {
            *canonical_arguments = std::move(accepted_arguments);
        }
        if (reason != nullptr) {
            *reason = std::move(detail);
        }
        return true;
    };

    if (tool_name == "nmap") {
        if (arguments.empty()) {
            if (!allow_default_arguments) {
                if (reason != nullptr) {
                    *reason = "`tool nmap` requires explicit safe arguments such as `-V`, `-F 127.0.0.1`, or `-sn 127.0.0.0/24`.";
                }
                return false;
            }
            return accept({"-V"}, "Defaulted `nmap` to the safe version probe `-V`.");
        }
        if (arguments.size() == 1 && (arguments.front() == "-V" || arguments.front() == "--version")) {
            return accept(arguments, "Validated safe `nmap` version probe.");
        }
        if (arguments.size() == 2 && arguments.front() == "-F" && is_loopback_target(arguments.back())) {
            return accept(arguments, "Validated safe local `nmap` fast scan against loopback.");
        }
        if (arguments.size() == 2 && arguments.front() == "-sn" && is_loopback_subnet_target(arguments.back())) {
            return accept(arguments, "Validated safe loopback-subnet `nmap` host discovery scan.");
        }
        if (reason != nullptr) {
            *reason = "Only guarded `nmap -V`, `nmap --version`, `nmap -F <loopback>`, or `nmap -sn 127.0.0.0/24` commands are allowed.";
        }
        return false;
    }

    if (tool_name == "inspect-host") {
        if (arguments.empty()) {
            return accept({}, "Validated safe `inspect-host` system posture inspection.");
        }
        if (arguments.size() == 1 && arguments.front() == "--linux") {
            return accept(arguments, "Validated safe Linux `inspect-host` system posture inspection.");
        }
        if (reason != nullptr) {
            *reason = "Only guarded `inspect-host` or `inspect-host --linux` invocations are allowed.";
        }
        return false;
    }

    if (tool_name == "tshark" || tool_name == "wireshark" || tool_name == "dumpcap") {
        if (arguments.empty()) {
            if (!allow_default_arguments) {
                if (reason != nullptr) {
                    *reason = "`tool " + tool_name + "` requires an explicit safe argument such as `--version`.";
                }
                return false;
            }
            return accept({"--version"}, "Defaulted `" + tool_name + "` to the safe version probe `--version`.");
        }
        if (arguments.size() == 1 &&
            (arguments.front() == "--version" || arguments.front() == "-v" || arguments.front() == "-V")) {
            return accept(arguments, "Validated safe `" + tool_name + "` version probe.");
        }
        if (reason != nullptr) {
            *reason = "Only guarded `" + tool_name + " --version` style probes are currently allowed.";
        }
        return false;
    }

    if (tool_name == "ssh") {
        if (arguments.empty()) {
            if (!allow_default_arguments) {
                if (reason != nullptr) {
                    *reason = "`tool ssh` requires an explicit safe argument such as `-V`.";
                }
                return false;
            }
            return accept({"-V"}, "Defaulted `ssh` to the safe version probe `-V`.");
        }
        if (arguments.size() == 1 && arguments.front() == "-V") {
            return accept(arguments, "Validated safe `ssh` version probe.");
        }
        if (reason != nullptr) {
            *reason = "Only guarded `ssh -V` is currently allowed through natural-language tool routing.";
        }
        return false;
    }

    if (reason != nullptr) {
        *reason = "Natural-language tool routing is not yet enabled for `" + tool_name + "`.";
    }
    return false;
}

bool infer_safe_tool_invocation_from_prompt(std::string_view prompt,
                                            std::string_view hinted_tool_name,
                                            std::string* canonical_tool_name,
                                            std::vector<std::string>* canonical_arguments,
                                            std::string* reason) {
    std::string tool_name = trim_local(hinted_tool_name);
    const std::string lowered = lowercase_local(prompt);
    if (tool_name.empty()) {
        static const std::vector<std::string> kDeterministicToolNames = {"nmap", "tshark", "wireshark", "dumpcap", "ssh", "inspect-host"};
        for (const std::string& candidate : kDeterministicToolNames) {
            if (lowered.find(candidate) != std::string::npos) {
                tool_name = candidate;
                break;
            }
        }
        if (tool_name.empty() &&
            (lowered.find("secure my system") != std::string::npos ||
             lowered.find("harden my system") != std::string::npos ||
             lowered.find("secure system") != std::string::npos ||
             lowered.find("system posture") != std::string::npos)) {
            tool_name = "inspect-host";
        }
    }
    if (tool_name.empty()) {
        if (reason != nullptr) {
            *reason = "No supported tool name could be inferred from the prompt.";
        }
        return false;
    }

    std::vector<std::string> arguments;
    if (tool_name == "nmap") {
        const bool wants_scan = lowered.find("scan") != std::string::npos ||
            lowered.find("/24") != std::string::npos ||
            lowered.find("subnet") != std::string::npos ||
            lowered.find("portscan") != std::string::npos;
        if (wants_scan) {
            if (const std::optional<std::string> unsafe_target = find_explicit_unsafe_scan_target(prompt);
                unsafe_target.has_value()) {
                if (reason != nullptr) {
                    *reason = "Guarded `nmap` scan requests are limited to loopback. Requested target `" +
                        *unsafe_target + "` is outside the allowlist; use `127.0.0.1` or `127.0.0.0/24` explicitly.";
                }
                return false;
            }
            const std::string target = find_safe_scan_target(prompt).value_or("127.0.0.1");
            if (is_loopback_subnet_target(target)) {
                arguments = {"-sn", target};
            } else {
                arguments = {"-F", target};
            }
        }
    } else if (tool_name == "inspect-host") {
        if (lowered.find("linux") != std::string::npos) {
            arguments = {"--linux"};
        }
    }
    return validate_safe_tool_invocation(tool_name, arguments, true, canonical_tool_name, canonical_arguments, reason);
}

bool validate_command_assist_plan(const CommandAssistPlan& proposed,
                                  const RequestProfile& base_profile,
                                  CommandAssistPlan* validated_plan,
                                  RequestProfile* routed_profile,
                                  IntentResolution* routed_resolution,
                                  std::string* reason) {
    if (proposed.canonical_command.empty()) {
        if (reason != nullptr) {
            *reason = "Command assist did not return a canonical command.";
        }
        return false;
    }
    if (proposed.confidence < 0.0 || proposed.confidence > 1.0) {
        if (reason != nullptr) {
            *reason = "Command assist confidence must be between 0.0 and 1.0.";
        }
        return false;
    }
    if (proposed.requires_confirmation) {
        if (reason != nullptr) {
            *reason = "Command assist requested operator confirmation, which is not enabled in guarded auto-routing.";
        }
        return false;
    }

    const std::string canonical = trim_local(proposed.canonical_command);
    const std::string lowered = lowercase_local(canonical);
    RequestProfile routed = base_profile;
    IntentResolution resolution;
    resolution.normalized_prompt = canonical;
    resolution.confidence = proposed.confidence;

    auto set_target = [&](std::size_t prefix_size) {
        return trim_local(canonical.substr(prefix_size));
    };
    auto assign_simple = [&](RequestIntent intent, std::string primary_target = std::string()) mutable {
        resolution.intent = intent;
        resolution.primary_target = std::move(primary_target);
        routed.resolved_intent = intent;
        return true;
    };

    bool matched = false;
    if (starts_with_local(lowered, "build ")) {
        const std::string target = set_target(6);
        matched = !target.empty();
        if (matched) {
            assign_simple(RequestIntent::BuildProject, target);
            routed.project_reference = target;
            routed.execute_build = true;
        }
    } else if (starts_with_local(lowered, "recipe author ")) {
        const std::string target = set_target(14);
        matched = !target.empty();
        if (matched) {
            assign_simple(RequestIntent::AuthorBuildRecipe, target);
            routed.project_reference = target;
            routed.build_source_path = target;
        }
    } else if (starts_with_local(lowered, "preflight ")) {
        const std::string target = set_target(10);
        matched = !target.empty();
        if (matched) {
            assign_simple(RequestIntent::BuildProject, target);
            routed.project_reference = target;
            routed.execute_build = true;
            routed.preflight_only = true;
        }
    } else if (starts_with_local(lowered, "doctor ")) {
        const std::string target = set_target(7);
        matched = !target.empty();
        if (matched) {
            assign_simple(RequestIntent::DoctorProject, target);
            routed.project_reference = target;
        }
    } else if (starts_with_local(lowered, "ingest ")) {
        const std::string target = set_target(7);
        matched = !target.empty();
        if (matched) {
            assign_simple(RequestIntent::IngestData, target);
            routed.analyst_reference = target;
        }
    } else if (starts_with_local(lowered, "analyze ")) {
        const std::string target = set_target(8);
        matched = !target.empty();
        if (matched) {
            assign_simple(RequestIntent::AnalyzeCase, target);
            routed.analyst_reference = target;
        }
    } else if (starts_with_local(lowered, "decide ")) {
        const std::string target = set_target(7);
        matched = !target.empty();
        if (matched) {
            assign_simple(RequestIntent::DecideAction, target);
            routed.analyst_reference = target;
        }
    } else if (starts_with_local(lowered, "defend diag ")) {
        const std::string target = set_target(12);
        matched = !target.empty();
        if (matched) {
            const std::size_t split = target.find(' ');
            const std::string mode = split == std::string::npos ? target : target.substr(0, split);
            const std::string detail = split == std::string::npos ? std::string{} : trim_local(target.substr(split + 1));
            assign_simple(RequestIntent::DefenseDiagnostic, target);
            routed.defense_mode = mode;
            routed.defense_target = detail.empty() ? target : detail;
            if (mode == "port") {
                if (const std::optional<int> port = parse_port_text(detail); port.has_value()) {
                    routed.defense_port = *port;
                }
            } else if (mode == "pid") {
                if (const std::optional<int> pid = parse_port_text(detail); pid.has_value()) {
                    routed.defense_pid = *pid;
                }
            }
        }
    } else if (starts_with_local(lowered, "persona mode ")) {
        const std::string target = set_target(13);
        matched = !target.empty();
        if (matched) {
            assign_simple(RequestIntent::SetPersonaMode, target);
            routed.persona_mode = target;
        }
    } else if (lowered == "case list") {
        matched = true;
        assign_simple(RequestIntent::InspectCase, "list");
        routed.analyst_reference = "list";
    } else if (starts_with_local(lowered, "case search ")) {
        const std::string target = set_target(12);
        matched = !target.empty();
        if (matched) {
            assign_simple(RequestIntent::InspectCase, target);
            routed.analyst_reference = target;
        }
    } else if (starts_with_local(lowered, "case timeline ")) {
        const std::string target = set_target(14);
        matched = !target.empty();
        if (matched) {
            assign_simple(RequestIntent::CaseTimeline, target);
            routed.analyst_reference = target;
        }
    } else if (starts_with_local(lowered, "case ")) {
        const std::string target = set_target(5);
        matched = !target.empty();
        if (matched) {
            assign_simple(RequestIntent::InspectCase, target);
            routed.analyst_reference = target;
        }
    } else if (lowered == "incident list") {
        matched = true;
        assign_simple(RequestIntent::ListIncidents);
    } else if (starts_with_local(lowered, "incident report ")) {
        const std::string target = set_target(16);
        matched = !target.empty();
        if (matched) {
            assign_simple(RequestIntent::ReportIncident, target);
            routed.incident_reference = target;
        }
    } else if (starts_with_local(lowered, "incident ")) {
        const std::string target = set_target(9);
        matched = !target.empty();
        if (matched) {
            assign_simple(RequestIntent::InspectIncident, target);
            routed.incident_reference = target;
        }
    } else if (starts_with_local(lowered, "define ")) {
        const std::string target = set_target(7);
        matched = !target.empty();
        if (matched) {
            assign_simple(RequestIntent::DefineSymbol, target);
        }
    } else if (starts_with_local(lowered, "explain ")) {
        const std::string target = set_target(8);
        matched = !target.empty();
        if (matched) {
            assign_simple(RequestIntent::ExplainCommand, target);
        }
    } else if (starts_with_local(lowered, "review ")) {
        const std::string target = set_target(7);
        matched = !target.empty();
        if (matched) {
            assign_simple(RequestIntent::ReviewModule, target);
            routed.review_target = target;
        }
    } else if (starts_with_local(lowered, "patch-proposal ")) {
        const std::string target = set_target(15);
        matched = !target.empty();
        if (matched) {
            assign_simple(RequestIntent::PatchProposal, target);
            routed.review_target = target;
        }
    } else if (lowered == "provider probe") {
        matched = true;
        assign_simple(RequestIntent::ProbeProvider);
    } else if (starts_with_local(lowered, "memory ")) {
        const std::string view = set_target(7);
        static const std::vector<std::string> views = {
            "history", "prefs", "definitions", "language", "security", "uac", "cases", "runs", "tze", "legacy", "persona", "operator", "assist"
        };
        matched = std::find(views.begin(), views.end(), view) != views.end();
        if (matched) {
            assign_simple(RequestIntent::ShowMemory);
            resolution.memory_view = view;
            routed.memory_view = view;
        }
    } else if (lowered == "tze latest") {
        matched = true;
        assign_simple(RequestIntent::ReplayTzeRun, "latest");
        routed.tze_run_reference = "latest";
    } else if (starts_with_local(lowered, "tze replay ")) {
        const std::string target = set_target(11);
        matched = !target.empty();
        if (matched) {
            assign_simple(RequestIntent::ReplayTzeRun, target);
            routed.tze_run_reference = target;
        }
    } else if (starts_with_local(lowered, "tze chain ")) {
        const std::string target = set_target(10);
        matched = !target.empty();
        if (matched) {
            assign_simple(RequestIntent::ChainTzeRun, target);
            routed.tze_run_reference = target;
        }
    } else if (lowered == "tze diff-latest") {
        matched = true;
        assign_simple(RequestIntent::DiffTzeRuns, "latest");
        resolution.memory_view = "previous";
        routed.tze_run_reference = "latest";
        routed.tze_compare_reference = "previous";
    } else if (starts_with_local(lowered, "tze diff ")) {
        const std::string payload = set_target(9);
        const std::size_t split = payload.find(' ');
        matched = split != std::string::npos;
        if (matched) {
            const std::string left = trim_local(payload.substr(0, split));
            const std::string right = trim_local(payload.substr(split + 1));
            matched = !left.empty() && !right.empty();
            if (matched) {
                assign_simple(RequestIntent::DiffTzeRuns, left);
                resolution.memory_view = right;
                routed.tze_run_reference = left;
                routed.tze_compare_reference = right;
            }
        }
    } else if (lowered == "tze explain-change-latest") {
        matched = true;
        assign_simple(RequestIntent::ExplainTzeChange, "latest");
        resolution.memory_view = "previous";
        routed.tze_run_reference = "latest";
        routed.tze_compare_reference = "previous";
    } else if (starts_with_local(lowered, "tze explain-change ")) {
        const std::string payload = set_target(19);
        const std::size_t split = payload.find(' ');
        matched = split != std::string::npos;
        if (matched) {
            const std::string left = trim_local(payload.substr(0, split));
            const std::string right = trim_local(payload.substr(split + 1));
            matched = !left.empty() && !right.empty();
            if (matched) {
                assign_simple(RequestIntent::ExplainTzeChange, left);
                resolution.memory_view = right;
                routed.tze_run_reference = left;
                routed.tze_compare_reference = right;
            }
        }
    } else if (starts_with_local(lowered, "tze report ")) {
        const std::string target = set_target(11);
        matched = !target.empty();
        if (matched) {
            assign_simple(RequestIntent::ReportTzeRun, target);
            routed.tze_run_reference = target;
        }
    } else if (lowered == "tool list") {
        matched = true;
        assign_simple(RequestIntent::ToolAction, "list");
        routed.tool_mode = ToolCommandMode::List;
    } else if (starts_with_local(lowered, "tool locate ")) {
        const std::string target = set_target(12);
        matched = !target.empty();
        if (matched) {
            assign_simple(RequestIntent::ToolAction, target);
            routed.tool_mode = ToolCommandMode::Locate;
            routed.requested_tool_name = target;
        }
    } else if (starts_with_local(lowered, "tool doctor ")) {
        const std::string target = set_target(12);
        matched = !target.empty();
        if (matched) {
            assign_simple(RequestIntent::ToolAction, target);
            routed.tool_mode = ToolCommandMode::Doctor;
            routed.requested_tool_name = target;
        }
    } else if (starts_with_local(lowered, "tool ")) {
        const std::string payload = set_target(5);
        std::string tool_name = payload;
        std::vector<std::string> tool_arguments;
        const std::size_t split = payload.find(" -- ");
        if (split != std::string::npos) {
            tool_name = trim_local(payload.substr(0, split));
            tool_arguments = split_whitespace_local(trim_local(payload.substr(split + 4)));
        }
        matched = !tool_name.empty();
        if (matched) {
            std::string canonical_tool_name;
            std::vector<std::string> canonical_arguments;
            std::string tool_reason;
            if (!validate_safe_tool_invocation(tool_name,
                                               tool_arguments,
                                               false,
                                               &canonical_tool_name,
                                               &canonical_arguments,
                                               &tool_reason)) {
                if (reason != nullptr) {
                    *reason = tool_reason;
                }
                return false;
            }
            assign_simple(RequestIntent::ToolAction, canonical_tool_name);
            routed.tool_mode = ToolCommandMode::Run;
            routed.requested_tool_name = canonical_tool_name;
            routed.tool_arguments = canonical_arguments;
        }
    }

    if (!matched) {
        if (reason != nullptr) {
            *reason = "Command assist returned a command outside the guarded Omni allowlist.";
        }
        return false;
    }

    if (!proposed.command_family.empty()) {
        const std::string expected_family = lowercase_local(std::string(to_string(resolution.intent)));
        const std::string declared_family = lowercase_local(proposed.command_family);
        if (declared_family.find(expected_family) == std::string::npos &&
            expected_family.find(declared_family) == std::string::npos) {
            if (reason != nullptr) {
                *reason = "Command assist command family `" + proposed.command_family +
                    "` did not match the validated Omni intent `" + expected_family + "`.";
            }
            return false;
        }
    }

    if (validated_plan != nullptr) {
        *validated_plan = proposed;
        validated_plan->validated = true;
        validated_plan->status = "validated";
    }
    if (routed_profile != nullptr) {
        *routed_profile = routed;
    }
    if (routed_resolution != nullptr) {
        *routed_resolution = resolution;
    }
    if (reason != nullptr) {
        *reason = "Validated canonical Omni command `" + canonical + "`.";
    }
    return true;
}

bool validate_next_step_assist_plan(const NextStepAssistPlan& proposed,
                                    NextStepAssistPlan* validated_plan,
                                    std::string* reason) {
    if (proposed.suggested_next_step.empty()) {
        if (reason != nullptr) {
            *reason = "Next-step assist did not return a suggested next step.";
        }
        return false;
    }
    if (proposed.confidence < 0.0 || proposed.confidence > 1.0) {
        if (reason != nullptr) {
            *reason = "Next-step assist confidence must be between 0.0 and 1.0.";
        }
        return false;
    }
    if (validated_plan != nullptr) {
        *validated_plan = proposed;
        validated_plan->validated = true;
        validated_plan->status = "assist_used";
        validated_plan->metadata.status = "assist_used";
    }
    if (reason != nullptr) {
        *reason = "Validated guarded next-step assist plan.";
    }
    return true;
}

bool validate_case_summary_assist_plan(const CaseSummaryAssistPlan& proposed,
                                       CaseSummaryAssistPlan* validated_plan,
                                       std::string* reason) {
    if (proposed.executive_summary.empty()) {
        if (reason != nullptr) {
            *reason = "Case-summary assist did not return an executive summary.";
        }
        return false;
    }
    if (proposed.confidence < 0.0 || proposed.confidence > 1.0) {
        if (reason != nullptr) {
            *reason = "Case-summary assist confidence must be between 0.0 and 1.0.";
        }
        return false;
    }
    if (validated_plan != nullptr) {
        *validated_plan = proposed;
        validated_plan->validated = true;
        validated_plan->status = "assist_used";
        validated_plan->metadata.status = "assist_used";
    }
    if (reason != nullptr) {
        *reason = "Validated guarded case-summary assist plan.";
    }
    return true;
}

bool is_security_guidance_only_prompt(std::string_view prompt) {
    const std::string lowered = lowercase_local(trim_local(prompt));
    if (lowered.empty()) {
        return false;
    }
    return lowered.find("am i secure") != std::string::npos ||
        lowered.find("am i safe") != std::string::npos ||
        lowered.find("tell me if i am secure") != std::string::npos ||
        lowered.find("tell me if this machine is secure") != std::string::npos ||
        lowered.find("is this machine secure") != std::string::npos ||
        lowered.find("is this system secure") != std::string::npos ||
        lowered.find("is this machine safe") != std::string::npos ||
        lowered.find("what should i check before saying this machine is secure") != std::string::npos ||
        (lowered.find("without running tools") != std::string::npos && lowered.find("secure") != std::string::npos);
}

std::string first_nonempty_or(std::initializer_list<std::string> values, std::string fallback) {
    for (const std::string& value : values) {
        if (!trim_local(value).empty()) {
            return value;
        }
    }
    return fallback;
}

std::string summarize_stage_statuses(const TzeRunRecord& run) {
    if (run.stages.empty()) {
        return "No persisted TZE stages were available for this run.";
    }
    std::ostringstream out;
    const std::size_t limit = std::min<std::size_t>(run.stages.size(), 5);
    for (std::size_t index = 0; index < limit; ++index) {
        if (index != 0) {
            out << " -> ";
        }
        out << run.stages[index].stage_id << "[" << run.stages[index].status << "]";
    }
    if (run.stages.size() > limit) {
        out << " -> ...";
    }
    return out.str();
}

struct RecursiveMemoryHit {
    std::string source;
    std::string concept_text;
    std::string domain;
    std::string summary;
    std::string evidence_ref;
    double score = 0.0;
};

std::string normalize_recursive_concept(std::string_view value) {
    std::string normalized;
    bool last_space = false;
    for (char c : value) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc)) {
            normalized.push_back(static_cast<char>(std::tolower(uc)));
            last_space = false;
        } else if (std::isspace(uc) || c == '-' || c == '_') {
            if (!normalized.empty() && !last_space) {
                normalized.push_back(' ');
                last_space = true;
            }
        }
    }
    while (!normalized.empty() && normalized.back() == ' ') {
        normalized.pop_back();
    }
    return normalized;
}

std::string recursive_domain_hint_from_text(std::string_view text) {
    const std::string lowered = lowercase_local(text);
    if (lowered.find("technology") != std::string::npos ||
        lowered.find(" tech") != std::string::npos ||
        lowered.find("computing") != std::string::npos ||
        lowered.find("computer") != std::string::npos ||
        lowered.find("software") != std::string::npos) {
        return "technology";
    }
    if (lowered.find("science") != std::string::npos || lowered.find("physics") != std::string::npos) {
        return "science";
    }
    if (lowered.find("security") != std::string::npos) {
        return "security";
    }
    if (lowered.find("biography") != std::string::npos || lowered.rfind("who is ", 0) == 0) {
        return "Biography";
    }
    return {};
}

std::string strip_recursive_domain_phrase(std::string value) {
    std::string lowered = lowercase_local(value);
    static const std::vector<std::string> markers = {
        " in terms of technology", " in technology", " in tech", " when discussing technology",
        " in terms of science", " in science", " in physics", " when discussing science",
        " in terms of security", " in security",
        " in terms of biography", " in biography",
    };
    for (const std::string& marker : markers) {
        const std::size_t pos = lowered.find(marker);
        if (pos != std::string::npos) {
            value = value.substr(0, pos);
            break;
        }
    }
    return trim_local(value);
}

std::filesystem::path recursive_glossary_path(const TzeRunRecord& run) {
    std::filesystem::path start = run.source_map_path.empty()
        ? std::filesystem::current_path()
        : std::filesystem::path(run.source_map_path).parent_path();
    for (std::filesystem::path cursor = start; !cursor.empty(); cursor = cursor.parent_path()) {
        const std::filesystem::path candidate = cursor / "res" / "local_glossary.tsv";
        std::error_code error;
        if (std::filesystem::exists(candidate, error)) {
            return candidate;
        }
        if (cursor == cursor.root_path()) {
            break;
        }
    }
    return {};
}

void add_recursive_memory_hit(std::vector<RecursiveMemoryHit>& hits, RecursiveMemoryHit hit) {
    if (hit.summary.empty()) {
        return;
    }
    const std::string key = normalize_recursive_concept(hit.concept_text) + "|" + lowercase_local(hit.domain) + "|" + hit.summary;
    for (const RecursiveMemoryHit& existing : hits) {
        const std::string existing_key =
            normalize_recursive_concept(existing.concept_text) + "|" + lowercase_local(existing.domain) + "|" + existing.summary;
        if (existing_key == key) {
            return;
        }
    }
    hits.push_back(std::move(hit));
}

std::vector<RecursiveMemoryHit> recursive_memory_search(const TzeRunRecord& run, const MemorySnapshot& memory) {
    std::vector<RecursiveMemoryHit> hits;
    std::vector<std::string> queries;
    if (run.definition_answer.has_value()) {
        queries.push_back(run.definition_answer->query);
        queries.push_back(run.definition_answer->normalized_concept);
    }
    queries.push_back(run.target);
    queries.push_back(run.prompt);
    for (std::string& query : queries) {
        query = strip_recursive_domain_phrase(query);
    }

    std::string wanted_domain = run.definition_answer.has_value() ? run.definition_answer->domain_hint : std::string{};
    if (wanted_domain.empty()) {
        wanted_domain = recursive_domain_hint_from_text(run.prompt + " " + run.target);
    }

    auto query_matches = [&](std::string_view candidate_concept, std::string_view candidate_domain) {
        const std::string normalized_candidate = normalize_recursive_concept(candidate_concept);
        if (normalized_candidate.empty()) {
            return false;
        }
        const bool domain_ok =
            wanted_domain.empty() || candidate_domain.empty() ||
            lowercase_local(candidate_domain) == lowercase_local(wanted_domain);
        if (!domain_ok) {
            return false;
        }
        for (const std::string& query : queries) {
            const std::string normalized_query = normalize_recursive_concept(query);
            if (normalized_query.empty()) {
                continue;
            }
            if (normalized_candidate == normalized_query ||
                normalized_query.find(normalized_candidate) != std::string::npos ||
                normalized_candidate.find(normalized_query) != std::string::npos) {
                return true;
            }
        }
        return false;
    };

    for (const StoredDefinition& definition : memory.definitions) {
        if (is_expired(definition)) {
            continue;
        }
        const std::string concept_text = definition.normalized_concept.empty() ? definition.term : definition.normalized_concept;
        if (query_matches(concept_text, definition.domain_hint)) {
            add_recursive_memory_hit(hits, {
                definition.source_type.empty() ? "learned_definition_cache" : definition.source_type,
                definition.term.empty() ? concept_text : definition.term,
                definition.domain_hint,
                definition.summary,
                "memory.definitions",
                definition.confidence > 0.0 ? definition.confidence : 0.76,
            });
        }
    }

    for (const TzeRunRecord& prior : memory.tze_runs) {
        if (!prior.definition_answer.has_value() || !prior.definition_answer->found) {
            continue;
        }
        const DefinitionAnswer& answer = *prior.definition_answer;
        if (query_matches(answer.normalized_concept.empty() ? answer.query : answer.normalized_concept,
                          answer.domain_hint)) {
            add_recursive_memory_hit(hits, {
                "prior_tze_definition",
                answer.query,
                answer.domain_hint,
                answer.summary,
                prior.id,
                answer.confidence > 0.0 ? answer.confidence : 0.74,
            });
        }
    }

    const std::filesystem::path glossary = recursive_glossary_path(run);
    if (!glossary.empty()) {
        std::ifstream input(glossary);
        for (std::string line; std::getline(input, line);) {
            if (line.empty() || line.rfind("#", 0) == 0) {
                continue;
            }
            std::vector<std::string> fields;
            std::string field;
            std::istringstream row(line);
            while (std::getline(row, field, '|')) {
                fields.push_back(trim_local(field));
            }
            if (fields.size() < 3 || lowercase_local(fields[0]) == "term") {
                continue;
            }
            if (query_matches(fields[0], fields[1])) {
                add_recursive_memory_hit(hits, {
                    "local_glossary",
                    fields[0],
                    fields[1],
                    fields[2],
                    glossary.string(),
                    fields[1].empty() ? 0.76 : 0.84,
                });
            }
        }
    }

    for (const MemoryHistoryEntry& entry : memory.history) {
        if (is_expired(entry)) {
            continue;
        }
        if (entry.summary.empty()) {
            continue;
        }
        const std::string haystack = lowercase_local(entry.prompt + " " + entry.summary);
        for (const std::string& query : queries) {
            const std::string normalized_query = normalize_recursive_concept(query);
            if (!normalized_query.empty() && haystack.find(normalized_query) != std::string::npos) {
                add_recursive_memory_hit(hits, {
                    "memory_history",
                    query,
                    wanted_domain,
                    entry.summary,
                    entry.timestamp,
                    0.62,
                });
                break;
            }
        }
    }

    std::sort(hits.begin(), hits.end(), [](const RecursiveMemoryHit& lhs, const RecursiveMemoryHit& rhs) {
        return lhs.score > rhs.score;
    });
    if (hits.size() > 5) {
        hits.resize(5);
    }
    return hits;
}

bool apply_recursive_route_learning(ProcessingReport& report,
                                    const MemorySnapshot& memory,
                                    std::string_view source_map_path) {
    if (!report.definition_answer.has_value() || report.definition_answer->found) {
        return false;
    }
    if (report.definition_answer->selected_source_type == "clarification_required" &&
        report.definition_answer->domain_hint.empty() &&
        normalize_recursive_concept(report.definition_answer->query).find(' ') == std::string::npos) {
        return false;
    }
    if (report.answer_status != "unknown_query" && report.answer_status != "clarify_needed") {
        return false;
    }

    TzeRunRecord probe;
    probe.id = report.tze_run_id.empty() ? "current" : report.tze_run_id;
    probe.intent = report.resolved_intent;
    probe.prompt = report.raw_prompt;
    probe.target = report.definition_answer->query;
    probe.status = report.answer_status;
    probe.source_map_path = std::string(source_map_path);
    probe.next_action = report.next_action;
    probe.definition_answer = report.definition_answer;
    const std::vector<RecursiveMemoryHit> hits = recursive_memory_search(probe, memory);
    if (hits.empty() || hits.front().score < 0.80) {
        return false;
    }

    const RecursiveMemoryHit& top = hits.front();
    DefinitionAnswer answer = *report.definition_answer;
    answer.found = true;
    answer.summary = top.summary;
    answer.query = top.concept_text.empty() ? answer.query : top.concept_text;
    answer.normalized_concept = normalize_recursive_concept(answer.query);
    answer.domain_hint = top.domain;
    answer.selected_source_type = "recursive_route_learning";
    answer.selected_source_label = top.source + ":" + top.evidence_ref;
    answer.selected_authority_tier =
        top.source == "local_glossary" || top.source == "prior_tze_definition"
            ? "operator_override"
            : "memory_artifact";
    answer.confidence = std::max(0.80, top.score);
    answer.comparison_rationale =
        "Recursive Route Learning used local operating memory caches before final unresolved output.";
    answer.sources = {top.source};

    report.definition_answer = answer;
    report.answer_status = "defined";
    report.answer_explanation = answer.summary;
    report.next_action = "Ask used Recursive Route Learning. Use `omnix why latest` to backtrace the route provenance.";
    report.tze_stages.push_back({
        "x.Recursive.RouteLearning",
        "Search operating memory caches before returning unresolved definition output",
        "SessionCoordinator",
        "recursive_route_learning_used",
        "Resolved `" + answer.query + "` from " + top.source + " with score " + std::to_string(top.score) + ".",
        {probe.prompt, answer.domain_hint},
        {answer.summary},
    });
    report.storage_writes.push_back("x.Store(recursive.route_learning -> " + answer.query + ")");
    return true;
}

std::string recursive_success_pattern_for(const TzeRunRecord& run) {
    if (run.intent == "tool_action") {
        return "Resolve the requested tool, validate it as built-in/native, execute the exact bounded command, capture output, then retain only the final artifact.";
    }
    if (run.intent == "packet_capture") {
        return "Select interface/filter, open capture with permissions, observe packets, summarize flows, cap payload previews, then persist the packet artifact.";
    }
    if (run.intent == "probe_provider") {
        return "Load provider configuration, validate required keys or local model, probe availability, then return guarded assist readiness without exposing secrets.";
    }
    if (run.intent == "build_project" || run.intent == "doctor_project" || run.intent == "author_build_recipe") {
        return "Inspect source/toolchain, choose or author a recipe, preflight dependencies, run the build spine, validate artifacts, then store the compact result.";
    }
    if (run.intent == "neural_route") {
        return "Load local evidence JSONL, extract numeric features, score labels with fixed weights, explain top contributors, then persist only compact route metadata.";
    }
    if (run.intent == "diff_tze_runs" || run.intent == "explain_tze_change" || run.intent == "replay_tze_run") {
        return "Resolve run references, replay or compare persisted TZE stages, preserve provenance, then recommend the next inspection step.";
    }
    return "Decode intent, prepare cache, rank local evidence, execute the deterministic module, postprocess the final artifact, then store compact provenance.";
}

std::string recursive_diff_category_for(const TzeRunRecord& run) {
    const std::string lowered_status = lowercase_local(run.status);
    if (lowered_status.find("missing") != std::string::npos) {
        return "missing context";
    }
    if (lowered_status.find("unknown") != std::string::npos) {
        return "false goal";
    }
    if (lowered_status.find("blocked") != std::string::npos || lowered_status.find("failed") != std::string::npos) {
        return "skipped dependency";
    }
    if (lowered_status.find("invalid") != std::string::npos) {
        return "bad assumption";
    }
    if (lowered_status.find("attention") != std::string::npos) {
        return "broken threshold";
    }
    return "no blocking diff";
}

RecursiveDiffReport build_recursive_diff_report_for_run(const TzeRunRecord& run,
                                                        const MemorySnapshot& memory,
                                                        std::string_view observer_context) {
    RecursiveDiffReport report;
    report.status = "recursive_why_diff_complete";
    report.route_learning_status = "route_learning_not_needed";
    report.source_run_id = run.id;
    report.diff_category = recursive_diff_category_for(run);
    report.confidence = run.status.empty() ? 0.45 : (report.diff_category == "no blocking diff" ? 0.82 : 0.68);
    const std::vector<RecursiveMemoryHit> memory_hits = recursive_memory_search(run, memory);
    const bool route_was_used = std::any_of(run.stages.begin(), run.stages.end(), [](const TzeStageRecord& stage) {
        return stage.stage_id == "x.Recursive.RouteLearning" &&
            stage.status == "recursive_route_learning_used";
    });
    const bool has_learned_route =
        !memory_hits.empty() &&
        (lowercase_local(run.status).find("unknown") != std::string::npos ||
         lowercase_local(run.status).find("clarify") != std::string::npos);
    if (route_was_used) {
        report.diff_category = "route learned";
        report.route_learning_status = "recursive_route_learning_used";
        report.confidence = std::max(report.confidence, 0.88);
    } else if (has_learned_route) {
        report.diff_category = "missing step";
        report.route_learning_status = "recursive_route_learning_used";
        report.confidence = std::max(report.confidence, 0.86);
    } else if (!memory_hits.empty()) {
        report.route_learning_status = "recursive_route_learning_observed";
    }

    report.current_state = "Run `" + run.id + "` ended with status `" +
        (run.status.empty() ? "unknown" : run.status) + "` for prompt `" +
        (run.prompt.empty() ? "(empty)" : run.prompt) + "`.";
    report.likely_goal = first_nonempty_or(
        {run.next_action,
         run.target.empty() ? std::string{} : "Complete or explain `" + run.target + "`.",
         run.intent.empty() ? std::string{} : "Satisfy intent `" + run.intent + "`."},
        "Identify the next safe step from the available local evidence.");
    report.logs_and_reasoning = summarize_stage_statuses(run);
    if (!run.produced_artifact.empty()) {
        report.logs_and_reasoning += " | artifact: " + run.produced_artifact;
    }
    if (!memory_hits.empty()) {
        report.logs_and_reasoning += " | route-learning: memory-search: ";
        for (std::size_t index = 0; index < memory_hits.size(); ++index) {
            if (index != 0) {
                report.logs_and_reasoning += "; ";
            }
            report.logs_and_reasoning += memory_hits[index].source + ":" + memory_hits[index].concept_text;
            if (!memory_hits[index].domain.empty()) {
                report.logs_and_reasoning += "[" + memory_hits[index].domain + "]";
            }
        }
    }
    report.successful_path_pattern = recursive_success_pattern_for(run);
    if (route_was_used && !memory_hits.empty()) {
        const RecursiveMemoryHit& top = memory_hits.front();
        report.successful_path_pattern =
            "Recursive Route Learning ran inside normal ask: normalize the concept, extract the domain hint, search operating memory caches, then answer with local provenance before returning unresolved output.";
        report.difference_found =
            "No blocking diff remains: the ask path already used Recursive Route Learning and selected `" + top.concept_text + "`";
        if (!top.domain.empty()) {
            report.difference_found += " in domain `" + top.domain + "`";
        }
        report.difference_found += " from " + top.source + ".";
        report.best_estimate_answer = "Ask used the learned route: `" + top.concept_text + "`";
        if (!top.domain.empty()) {
            report.best_estimate_answer += " / " + top.domain;
        }
        report.best_estimate_answer += " => " + top.summary;
        report.next_action = "The route is now active in ask. Use `omnix context reset` if this association should be temporary only.";
    } else if (has_learned_route) {
        const RecursiveMemoryHit& top = memory_hits.front();
        report.successful_path_pattern =
            "Recursive Route Learning searches operating memory caches before final output: normalize the concept, extract domain hint `" +
            (top.domain.empty() ? std::string("(none)") : top.domain) +
            "`, compare memory definitions, prior TZE definition runs, history summaries, and local glossary entries, then choose the strongest operator-authored hit.";
        report.difference_found =
            "Recursive Route Learning found the route the observed path missed. Memory search found `" + top.concept_text + "`";
        if (!top.domain.empty()) {
            report.difference_found += " in domain `" + top.domain + "`";
        }
        report.difference_found += " from " + top.source + ", but the failing run treated the prompt as unresolved.";
        report.best_estimate_answer = "Use the learned route: `" + top.concept_text + "`";
        if (!top.domain.empty()) {
            report.best_estimate_answer += " / " + top.domain;
        }
        report.best_estimate_answer += " => " + top.summary;
        report.next_action = "Back-add this Recursive Route Learning candidate by patching definition normalization or operator teaching, then rerun the original ask.";
    } else if (report.diff_category == "no blocking diff") {
        report.difference_found = "The observed path already reached a successful or explainable state. The main remaining gap is deciding whether to continue, compare, or archive.";
        report.best_estimate_answer = "The run is currently explainable; use the next action to continue the chain or compare it with another run.";
        report.next_action = first_nonempty_or({run.next_action, "Run `omnix tze replay " + run.id + "` for full provenance."},
                                               "Run `omnix tze runs` to choose a run for deeper comparison.");
    } else {
        report.difference_found = "The observed path diverged from the success pattern at `" + report.diff_category +
            "`: compare status `" + run.status + "` against the expected next action.";
        report.best_estimate_answer = "The best next answer is to repair the `" + report.diff_category + "` before rerunning the target command.";
        report.next_action = first_nonempty_or({run.next_action, "Run `omnix tze replay " + run.id + "` for full provenance."},
                                               "Run `omnix tze runs` to choose a run for deeper comparison.");
    }
    report.why_this_matters =
        "Recursive Why/Diff treats success as an order-of-operations problem: like Tower of Hanoi T(n)=2T(n-1)+1, each safe move depends on the prior dependency being in the right state.";

    report.slots = {
        {"SLOT_1", "Initial Problem State", {
             "prompt=" + (run.prompt.empty() ? std::string("(empty)") : run.prompt),
             "intent=" + (run.intent.empty() ? std::string("unknown") : run.intent),
             "target=" + (run.target.empty() ? std::string("(none)") : run.target),
         }, {run.id}, 0.80},
        {"SLOT_2", "Intermediate Reasoning / Logs", {
             "stage_count=" + std::to_string(run.stages.size()),
             "stage_path=" + summarize_stage_statuses(run),
             "status=" + (run.status.empty() ? std::string("unknown") : run.status),
         }, {run.id + "#stages"}, 0.76},
        {"SLOT_3", "Unknown Goal / Target Answer", {
             report.likely_goal,
             "diff_category=" + report.diff_category,
         }, {run.id + "#next_action"}, 0.66},
        {"SLOT_4", "Present-Time Observer", {
             std::string(observer_context.empty() ? "observer=omnix why" : observer_context),
             "memory_runs=" + std::to_string(memory.tze_runs.size()),
         }, {"present"}, 0.72},
    };
    if (!memory_hits.empty()) {
        std::vector<std::string> facts;
        std::vector<std::string> refs;
        for (const RecursiveMemoryHit& hit : memory_hits) {
            std::string fact = hit.source + " matched " + hit.concept_text;
            if (!hit.domain.empty()) {
                fact += " domain=" + hit.domain;
            }
            fact += " score=" + std::to_string(hit.score);
            facts.push_back(std::move(fact));
            refs.push_back(hit.evidence_ref);
        }
        report.slots.push_back({"SLOT_R", "Recursive Route Learning / Operating Memory Cache Search", facts, refs, 0.88});
    }

    report.success_path.path_type = "success";
    report.success_path.ordered_steps = {
        "Prepare and decode the request.",
        "Rank local evidence and prior memory.",
        "Run Recursive Route Learning across operating memory caches for exact, domain, and prior successful route matches.",
        report.successful_path_pattern,
        "Postprocess and persist compact provenance.",
    };
    report.success_path.assumptions = {"A valid deterministic sequence exists.", "The target run has enough stored provenance to compare."};
    report.success_path.evidence_refs = {run.id, "res/RecursionANDTroubleshooting"};
    report.success_path.score = report.confidence;

    report.failure_path.path_type = "failure";
    report.failure_path.ordered_steps = {
        "Observed status `" + (run.status.empty() ? std::string("unknown") : run.status) + "`.",
        "Observed stage trace: " + summarize_stage_statuses(run),
        "Classified diff as `" + report.diff_category + "`.",
    };
    if (has_learned_route) {
        report.failure_path.ordered_steps.push_back("Recursive Route Learning candidate existed but was not used before final unresolved output.");
    }
    report.failure_path.assumptions = {"Persisted run data is the source of truth.", "No provider reasoning was required for v1."};
    report.failure_path.evidence_refs = {run.id};
    report.failure_path.score = 1.0 - (report.confidence / 2.0);
    return report;
}

std::string render_recursive_diff_answer(const RecursiveDiffReport& report) {
    std::ostringstream out;
    out << "Current State\n" << report.current_state << "\n\n"
        << "Likely Goal\n" << report.likely_goal << "\n\n"
        << "What the Logs/Reasoning Show\n" << report.logs_and_reasoning << "\n\n"
        << "Successful Path Pattern\n" << report.successful_path_pattern << "\n\n"
        << "Difference Found\n" << report.difference_found << "\n\n"
        << "Best Estimate Answer\n" << report.best_estimate_answer << "\n\n"
        << "Why This Matters\n" << report.why_this_matters << "\n\n"
        << "Next Action\n" << report.next_action;
    return out.str();
}

}  // namespace

SessionCoordinator::SessionCoordinator()
    : provider_(make_reasoning_provider_from_env()) {}

ProcessingReport SessionCoordinator::run(const RequestProfile& profile) const {
    ProcessingReport report;
    report.version_string = OMNIX_VERSION;
    report.raw_prompt = profile.raw_prompt;
    report.source_map_path = profile.source_map_path;
    report.reasoning_provider = provider_ != nullptr ? std::string(provider_->id()) : "null";
    RequestProfile routed_profile = profile;

    MemorySnapshot memory = memory_.load(routed_profile.memory_root_path);
    const std::optional<xpp::MappingUnit> source_unit = try_parse_source_map(routed_profile.source_map_path);
    const SourceStageIndex source_stage_index = source_unit.has_value()
        ? build_source_stage_index(*source_unit)
        : SourceStageIndex{};
    report.memory_reads = {
        memory.paths.history_path.string(),
        memory.paths.tze_runs_path.string(),
        memory.paths.definitions_path.string(),
        memory.paths.preferences_path.string(),
        memory.paths.projects_path.string(),
        memory.paths.authored_recipes_path.string(),
        memory.paths.native_tools_path.string(),
        memory.paths.security_audits_path.string(),
        memory.paths.language_contexts_path.string(),
        memory.paths.uac_states_path.string(),
        memory.paths.legacy_sources_path.string(),
        memory.paths.cases_path.string(),
    };
    report.source_preference_path = memory.paths.preferences_path.string() + ":source_preference_order";

    IntentResolution resolution;
    if (!routed_profile.raw_prompt.empty()) {
        resolution = intents_.resolve(routed_profile.raw_prompt);
    }
    if (routed_profile.resolved_intent != RequestIntent::Unknown) {
        resolution.intent = routed_profile.resolved_intent;
    }
    std::string blocked_tool_route_detail;
    if (resolution.intent == RequestIntent::Unknown && routed_profile.execute_build) {
        resolution.intent = RequestIntent::BuildProject;
    }
    if (!routed_profile.project_reference.empty()) {
        resolution.primary_target = routed_profile.project_reference;
    }
    if (!routed_profile.memory_view.empty() &&
        !((resolution.intent == RequestIntent::DefenseDiagnostic ||
           resolution.intent == RequestIntent::DefenseDetection) &&
          routed_profile.memory_view == "history")) {
        resolution.memory_view = routed_profile.memory_view;
    }
    if (!routed_profile.analyst_reference.empty()) {
        resolution.primary_target = routed_profile.analyst_reference;
    }
    if (!routed_profile.decision_reference.empty()) {
        resolution.memory_view = routed_profile.decision_reference;
    }
    if (!routed_profile.incident_reference.empty()) {
        resolution.primary_target = routed_profile.incident_reference;
    }
    if (!routed_profile.tze_run_reference.empty()) {
        resolution.primary_target = routed_profile.tze_run_reference;
    }
    if (!routed_profile.tze_compare_reference.empty()) {
        resolution.memory_view = routed_profile.tze_compare_reference;
    }
    if (routed_profile.tool_mode != ToolCommandMode::None || !routed_profile.requested_tool_name.empty()) {
        resolution.intent = routed_profile.resolved_intent == RequestIntent::TensorAction
            ? RequestIntent::TensorAction
            : RequestIntent::ToolAction;
        resolution.primary_target = routed_profile.requested_tool_name;
    }
    if (!routed_profile.packet_capture_mode.empty() || routed_profile.packet_port > 0 ||
        !routed_profile.packet_pcap_path.empty()) {
        resolution.intent = RequestIntent::PacketCapture;
        if (resolution.primary_target.empty() && routed_profile.packet_port > 0) {
            resolution.primary_target = std::to_string(routed_profile.packet_port);
        }
        if (resolution.memory_view.empty()) {
            resolution.memory_view = routed_profile.packet_capture_mode;
        }
    }
    if (!routed_profile.defense_mode.empty() || routed_profile.defense_port > 0 || routed_profile.defense_pid > 0) {
        if (routed_profile.resolved_intent == RequestIntent::DefenseDetection) {
            resolution.intent = RequestIntent::DefenseDetection;
        } else {
            resolution.intent = RequestIntent::DefenseDiagnostic;
        }
        if (resolution.primary_target.empty()) {
            resolution.primary_target = routed_profile.defense_target;
        }
        if (resolution.memory_view.empty()) {
            resolution.memory_view = routed_profile.defense_mode;
        }
    }
    if (resolution.intent == RequestIntent::DefenseDiagnostic && routed_profile.defense_mode.empty()) {
        routed_profile.defense_mode = resolution.memory_view.empty() ? "summary" : resolution.memory_view;
        routed_profile.defense_target = resolution.primary_target;
    }
    if (resolution.intent == RequestIntent::DefenseDetection && routed_profile.defense_mode.empty()) {
        routed_profile.defense_mode = resolution.memory_view.empty() ? "all" : resolution.memory_view;
        routed_profile.defense_target = resolution.primary_target;
    }
    if (!routed_profile.vuplus_mode.empty()) {
        resolution.intent = RequestIntent::VuplusGate;
        resolution.primary_target = routed_profile.vuplus_input_path;
        resolution.memory_view = routed_profile.vuplus_mode;
    }
    if (!routed_profile.persona_mode.empty()) {
        resolution.intent = RequestIntent::SetPersonaMode;
        resolution.primary_target = routed_profile.persona_mode;
    }
    if (routed_profile.definition_concept.empty() &&
        resolution.intent == RequestIntent::GeneralDefinitionQuery &&
        !resolution.primary_target.empty()) {
        routed_profile.definition_concept = resolution.primary_target;
    }
    if (routed_profile.definition_domain_hint.empty() && !resolution.definition_domain_hint.empty()) {
        routed_profile.definition_domain_hint = resolution.definition_domain_hint;
    }
    if (routed_profile.definition_comparison_rationale.empty() && !resolution.comparison_rationale.empty()) {
        routed_profile.definition_comparison_rationale = resolution.comparison_rationale;
    }
    routed_profile.resolved_intent = resolution.intent;

    if ((resolution.intent == RequestIntent::ToolAction || resolution.intent == RequestIntent::TensorAction) &&
        routed_profile.tool_mode == ToolCommandMode::None &&
        routed_profile.requested_tool_name.empty()) {
        std::string tool_name;
        std::vector<std::string> tool_arguments;
        std::string tool_route_detail;
        if (infer_safe_tool_invocation_from_prompt(routed_profile.raw_prompt,
                                                   resolution.primary_target,
                                                   &tool_name,
                                                   &tool_arguments,
                                                   &tool_route_detail)) {
            routed_profile.tool_mode = ToolCommandMode::Run;
            routed_profile.requested_tool_name = tool_name;
            routed_profile.tool_arguments = tool_arguments;
            resolution.primary_target = tool_name;
        } else if (!tool_route_detail.empty()) {
            blocked_tool_route_detail = tool_route_detail;
        }
    }

    const bool defense_detection_local_only =
        resolution.intent == RequestIntent::DefenseDetection && routed_profile.assist_requested;
    const bool vuplus_gate_local_only =
        resolution.intent == RequestIntent::VuplusGate && routed_profile.assist_requested;
    if (defense_detection_local_only) {
        routed_profile.assist_requested = false;
        report.assist_status = "assist_bypassed";
    } else if (vuplus_gate_local_only) {
        routed_profile.assist_requested = false;
        report.assist_status = "assist_bypassed";
    }

    const bool command_assist_eligible =
        routed_profile.assist_requested &&
        (resolution.intent == RequestIntent::Unknown || resolution.confidence < 0.8) &&
        !is_security_guidance_only_prompt(routed_profile.raw_prompt) &&
        routed_profile.tool_mode == ToolCommandMode::None &&
        routed_profile.requested_tool_name.empty() &&
        routed_profile.project_reference.empty() &&
        routed_profile.analyst_reference.empty() &&
        routed_profile.decision_reference.empty() &&
        routed_profile.incident_reference.empty() &&
        routed_profile.tze_run_reference.empty() &&
        routed_profile.tze_compare_reference.empty() &&
        routed_profile.feedback_value.empty() &&
        !routed_profile.execute_build;

    std::optional<CommandAssistPlan> validated_command_plan;
    std::string command_validation_detail;
    if (command_assist_eligible) {
        const std::vector<std::string> allowlisted_commands = allowlisted_command_assist_patterns();
        std::optional<CommandAssistPlan> proposed_plan =
            provider_->propose_command_route(routed_profile.raw_prompt, allowlisted_commands);
        if (proposed_plan.has_value()) {
            RequestProfile assisted_profile;
            IntentResolution assisted_resolution;
            CommandAssistPlan accepted_plan;
            if (validate_command_assist_plan(*proposed_plan,
                                             routed_profile,
                                             &accepted_plan,
                                             &assisted_profile,
                                             &assisted_resolution,
                                             &command_validation_detail)) {
                validated_command_plan = accepted_plan;
                routed_profile = assisted_profile;
                resolution = assisted_resolution;
                report.assist_status = "assist_used";
                report.command_assist_plan = accepted_plan;
            } else {
                report.assist_status = "assist_bypassed";
                report.command_assist_plan = *proposed_plan;
                report.command_assist_plan->status = "rejected";
            }
        }
    }
    report.tze_run_id = make_tze_run_id(memory, routed_profile.raw_prompt, resolution.intent, resolution.primary_target);
    report.resolved_intent = std::string(to_string(resolution.intent));

    std::string instruction_slot = routed_profile.instruction_slot;
    if (instruction_slot.empty()) {
        instruction_slot =
            (resolution.intent == RequestIntent::BuildProject || resolution.intent == RequestIntent::AuthorBuildRecipe)
            ? "aZ::1"
            : "aZ::99";
    }
    report.decoded_instruction = knowledge_.decode_instruction(instruction_slot);
    const std::string command_label = command_label_for(resolution.intent, report.decoded_instruction);
    QueryRuntime query_runtime;
    report.query_session = query_runtime.open_session(
        command_label,
        routed_profile.raw_prompt.empty() ? resolution.primary_target : routed_profile.raw_prompt);
    query_runtime.index_values(*report.query_session,
                               "runtime-context",
                               {command_label,
                                report.resolved_intent,
                                resolution.primary_target,
                                instruction_slot,
                                routed_profile.raw_prompt});

    report.cache = cache_.prepare(command_label, routed_profile.estimated_size, routed_profile.first_run);
    cache_.define(report.cache, {"seek_Unbound", routed_profile.persist_on_success ? "retain.Success" : "retain.Fail"});
    report.tze_stages.push_back(make_stage_record(
        "xProcessingCache",
        "Open request cache and work buffer",
        "CacheCoordinator",
        "ok",
        "Prepared cache `" + report.cache.name + "` for `" + command_label + "` with " +
            std::to_string(report.cache.operations.size()) + " cache operation(s).",
        {routed_profile.raw_prompt.empty() ? command_label : routed_profile.raw_prompt},
        report.cache.operations,
        source_stage_index));

    report.references = knowledge_.prioritize(command_label);
    apply_source_preferences(report.references, memory);
    report.references = query_runtime.rank_references(*report.query_session, "knowledge-priority", report.references);
    report.tze_stages.push_back(make_stage_record(
        "x.Define.Low",
        "Resolve user intent and decode instruction slot",
        "IntentResolver",
        "ok",
        "Resolved `" + report.resolved_intent + "` and decoded instruction `" + report.decoded_instruction + "`.",
        {routed_profile.instruction_slot.empty() ? instruction_slot : routed_profile.instruction_slot, resolution.primary_target},
        {command_label, report.resolved_intent},
        source_stage_index));

    report.uac_state = PreprocessorRuntime::resolve_uac_state(
        routed_profile.raw_prompt.empty() ? command_label : routed_profile.raw_prompt,
        memory,
        report.query_session.has_value() ? &*report.query_session : nullptr);
    report.uac_state->instruction_family_hint = instruction_family_hint_for(resolution.intent);
    push_unique(report.memory_writes, memory.paths.uac_states_path.string());
    report.storage_writes.push_back("x.Store(uac.epoch -> " + report.uac_state->epoch_marker + ")");
    report.storage_writes.push_back("x.Store(uac.family -> " + report.uac_state->instruction_family_hint + ")");
    report.tze_stages.push_back(make_stage_record(
        "x.Preprocessor",
        "Normalize the prompt and prepare bounded uAC preprocessor state",
        "PreprocessorRuntime",
        "ok",
        "Prepared normalized prompt, token set, instruction-family hint, and bounded uAC recovery state.",
        {report.uac_state->normalized_prompt},
        {report.uac_state->instruction_family_hint,
         "tokens=" + std::to_string(report.uac_state->query_tokens.size()),
         report.uac_state->epoch_marker,
         report.uac_state->key_budget_value},
        source_stage_index));

    std::vector<std::string> prioritized_sources;
    for (const KnowledgeReference& reference : report.references) {
        prioritized_sources.push_back(reference.source + ":" + std::to_string(reference.priority));
    }
    report.tze_stages.push_back(make_stage_record(
        "x.DisplayPriorityProcessingGate",
        "Order knowledge sources and preferred modules",
        "KnowledgeEngine",
        "ok",
        "Ranked " + std::to_string(report.references.size()) + " knowledge source(s) using preference path `" +
            report.source_preference_path + "` via query session `" + report.query_session->id + "`.",
        {report.source_preference_path},
        prioritized_sources,
        source_stage_index));

    report.feedback_loop = knowledge_.replay_feedback(command_label);
    report.feedback_loop = query_runtime.rank_feedback(*report.query_session, "feedback-loop", report.feedback_loop);
    report.tze_stages.push_back(make_stage_record(
        "x.DisplayFeedBackLoop",
        "Replay prior outcomes and learned preferences",
        "KnowledgeEngine",
        "ok",
        "Loaded " + std::to_string(report.feedback_loop.size()) + " feedback item(s) for `" + command_label + "`.",
        {command_label},
        report.feedback_loop,
        source_stage_index));
    std::string provider_stage_status = "deterministic_only";
    std::string provider_stage_detail =
        "Provider `" + std::string(provider_->id()) + "` is inactive; OmniX remains deterministic-only.";
    std::vector<std::string> provider_stage_outputs = {"assist_bypassed"};
    std::optional<ProviderProbeReport> provider_gate_probe;
    if (provider_->configured() && (routed_profile.assist_requested || resolution.intent == RequestIntent::ProbeProvider)) {
        provider_gate_probe = provider_->probe();
    }
    if (defense_detection_local_only) {
        provider_stage_status = "local_defense_only";
        provider_stage_detail =
            "Defense environmental intelligence is local-only; API/provider assist was bypassed for this request.";
        provider_stage_outputs = {"api_assist_disallowed", "local_evidence_only"};
    } else if (vuplus_gate_local_only) {
        provider_stage_status = "local_vuplus_only";
        provider_stage_detail =
            "Vuplus Gate operational intelligence is local-only; API/provider assist was bypassed for this request.";
        provider_stage_outputs = {"api_assist_disallowed", "local_ops_evidence_only"};
    } else if (provider_->configured()) {
        if (routed_profile.assist_requested) {
            if (provider_gate_probe.has_value() && provider_gate_probe->status == "ready") {
                provider_stage_status = "assist_ready";
                provider_stage_detail =
                    "Provider `" + std::string(provider_->id()) +
                    "` is reachable and guarded assist is enabled for this request.";
                provider_stage_outputs = {"assist_ready"};
            } else if (provider_gate_probe.has_value()) {
                provider_stage_status = "assist_unavailable";
                provider_stage_detail =
                    "Provider `" + std::string(provider_->id()) + "` is selected, but guarded assist is unavailable for this request: " +
                    provider_gate_probe->summary;
                provider_stage_outputs = {"deterministic_fallback"};
            } else {
                provider_stage_status = "assist_unavailable";
                provider_stage_detail =
                    "Provider `" + std::string(provider_->id()) +
                    "` is selected, but guarded assist is unavailable for this request.";
                provider_stage_outputs = {"deterministic_fallback"};
            }
        } else if (resolution.intent == RequestIntent::ProbeProvider &&
                   provider_gate_probe.has_value() &&
                   provider_gate_probe->status == "ready") {
            provider_stage_status = "assist_available";
            provider_stage_detail =
                "Provider `" + std::string(provider_->id()) +
                "` is reachable and guarded assist is available when an assist-enabled command is used.";
            provider_stage_outputs = {"assist_available"};
        } else {
            provider_stage_status = "configured_idle";
            provider_stage_detail =
                "Provider `" + std::string(provider_->id()) +
                "` is selected, but assist is off for this request.";
            provider_stage_outputs = {"assist_idle"};
        }
    }
    report.tze_stages.push_back({
        "x.Assist.Provider",
        "Reasoning provider gate",
        "ReasoningProvider",
        provider_stage_status,
        provider_stage_detail,
        {std::string(provider_->id())},
        provider_stage_outputs,
    });
    if (report.command_assist_plan.has_value()) {
        report.tze_stages.push_back({
            "x.Assist.CommandPlan",
            "Ask the provider for a canonical guarded Omni command",
            "ReasoningProvider",
            report.command_assist_plan->status.empty() ? "assist_planned" : report.command_assist_plan->status,
            "Provider `" + std::string(provider_->id()) + "` proposed canonical command `" +
                report.command_assist_plan->canonical_command + "`.",
            {routed_profile.raw_prompt},
            {report.command_assist_plan->canonical_command},
        });
        report.tze_stages.push_back({
            "x.Assist.CommandValidate",
            "Validate the provider-proposed canonical command against Omni guardrails",
            "SessionCoordinator",
            report.command_assist_plan->validated ? "assist_validated" : "assist_rejected",
            command_validation_detail.empty()
                ? "Command assist plan was recorded."
                : command_validation_detail,
            {report.command_assist_plan->canonical_command},
            report.command_assist_plan->validated
                ? std::vector<std::string>{report.command_assist_plan->command_family}
                : std::vector<std::string>{"deterministic_fallback"},
        });
    }

    report.toolchain = builder_.probe_modules();
    report.security = security_.verify(routed_profile);

    const auto try_openai_freeform = [&](const DefinitionAnswer& answer, std::string_view fallback_detail) {
        if (answer.found || std::string(provider_->id()) != "openai" ||
            !provider_gate_probe.has_value() || provider_gate_probe->status != "ready") {
            return false;
        }
        std::optional<FreeformAssistAnswer> freeform =
            provider_->resolve_freeform(routed_profile.raw_prompt.empty() ? resolution.primary_target : routed_profile.raw_prompt);
        if (!freeform.has_value()) {
            report.tze_stages.push_back({
                "x.Assist.Freeform",
                "Ask OpenAI for a guarded freeform answer after local resolution misses",
                "ReasoningProvider",
                "assist_freeform_unavailable",
                "OpenAI freeform fallback did not return a validated answer.",
                {routed_profile.raw_prompt},
                {std::string(fallback_detail)},
            });
            return false;
        }
        report.assist_status = "assist_freeform_used";
        report.freeform_assist_answer = freeform;
        report.answer_status = "assist_freeform";
        report.answer_explanation = render_freeform_assist_text(*freeform);
        report.next_action = freeform->suggested_commands.empty()
            ? "Review the answer and rerun with an explicit OmniX command if local evidence is needed."
            : "Run one suggested OmniX command explicitly if you want local evidence.";
        report.tze_stages.push_back({
            "x.Assist.Freeform",
            "Ask OpenAI for a guarded freeform answer after local resolution misses",
            "ReasoningProvider",
            "assist_freeform_used",
            "OpenAI returned a validated freeform answer after local memory, definitions, command routing, and tool planning missed.",
            {routed_profile.raw_prompt},
            freeform->used_context.empty() ? std::vector<std::string>{"openai_freeform"} : freeform->used_context,
        });
        return true;
    };

    std::string dispatch_module = dispatch_module_for(resolution.intent);

    if (resolution.intent == RequestIntent::ProbeProvider) {
        report.provider_probe_report = provider_->probe();
        report.answer_status = report.provider_probe_report->status == "ready"
            ? "provider_ready"
            : (report.provider_probe_report->status == "inactive" ? "provider_inactive" : "provider_probe_failed");
        report.answer_explanation = report.provider_probe_report->summary;
        if (report.provider_probe_report->status == "ready") {
            report.next_action =
                "Provider readiness is confirmed. Use `omnix ask <prompt> --assist`, `omnix shell --assist`, or `/assist on` in the shell to enable guarded assist.";
        } else if (report.provider_probe_report->status == "inactive") {
            report.next_action = "Set `OMNIX_REASONING_PROVIDER=ollama` and rerun `omnix provider probe` when you want to assess a local model.";
        } else if (report.provider_probe_report->status == "config_incomplete") {
            report.next_action = report.provider_probe_report->provider_id == "openai"
                ? "Set `OPENAI_API_KEY` / `OMNIX_OPENAI_API_KEY` and `OPENAI_MODEL` / `OMNIX_OPENAI_MODEL`, then rerun `omnix provider probe`."
                : "Set `OMNIX_OLLAMA_MODEL` and rerun `omnix provider probe`.";
        } else if (report.provider_probe_report->status == "unsupported_provider") {
            report.next_action = "Use `OMNIX_REASONING_PROVIDER=ollama`, `OMNIX_REASONING_PROVIDER=openai`, or unset the provider selection.";
        } else if (report.provider_probe_report->status == "invalid_config") {
            report.next_action = report.provider_probe_report->provider_id == "openai"
                ? "Fix `OMNIX_OPENAI_BASE_URL` or `OPENAI_BASE_URL` and rerun `omnix provider probe`."
                : "Fix `OMNIX_OLLAMA_BASE_URL` and rerun `omnix provider probe`.";
        } else if (report.provider_probe_report->status == "model_missing") {
            report.next_action = report.provider_probe_report->provider_id == "openai"
                ? "Choose an OpenAI model available to this API key, then rerun `omnix provider probe`."
                : (report.provider_probe_report->model == "deepnimsec-omni:latest"
                    ? "Run `./scripts/omnix_deepnimsec.sh --refresh-model`, then rerun `omnix provider probe`."
                    : "Pull or load the requested Ollama model, then rerun `omnix provider probe`.");
        } else if (report.provider_probe_report->status == "model_unusable") {
            report.next_action = report.provider_probe_report->provider_id == "ollama" &&
                    report.provider_probe_report->model == "deepnimsec-omni:latest"
                ? "Run `./scripts/omnix_deepnimsec.sh --refresh-model`, then rerun `omnix provider probe`."
                : "Rebuild or reload the requested model, then rerun `omnix provider probe`.";
        } else {
            report.next_action = report.provider_probe_report->provider_id == "openai"
                ? "Verify the OpenAI API key, optional project/organization settings, and network access, then rerun `omnix provider probe`."
                : "Start Ollama locally or correct the provider configuration, then rerun `omnix provider probe`.";
        }
    } else if (resolution.intent == RequestIntent::InspectToolchain) {
        report.answer_status = "toolchain";
        report.answer_explanation = "Inspected the local toolchain modules for this OmniX session.";
        report.next_action = "Use `omnix build <project>` once the required modules are available.";
    } else if (resolution.intent == RequestIntent::IngestData ||
               resolution.intent == RequestIntent::AnalyzeCase ||
               resolution.intent == RequestIntent::DecideAction ||
               resolution.intent == RequestIntent::InspectCase) {
        RequestProfile analyst_profile = routed_profile;
        analyst_profile.analyst_reference = !routed_profile.analyst_reference.empty()
            ? routed_profile.analyst_reference
            : resolution.primary_target;
        analyst_flow_.run(analyst_profile,
                          analyst_profile.analyst_reference,
                          memory,
                          report,
                          tools_,
                          memory_);
    } else if (resolution.intent == RequestIntent::CaseTimeline) {
        const std::string case_ref = !routed_profile.analyst_reference.empty() ? routed_profile.analyst_reference : resolution.primary_target;
        report.answer_explanation = memory_.render_case_timeline(memory, case_ref);
        report.answer_status = report.answer_explanation.rfind("No case matched", 0) == 0 ? "case_timeline_missing" : "case_timeline";
        report.next_action = "Use `omnix case " + case_ref + "` for the current case snapshot or `omnix decide " + case_ref + "` to refresh next actions.";
        report.storage_writes.push_back("x.Store(case.timeline -> " + case_ref + ")");
    } else if (resolution.intent == RequestIntent::RecursiveWhyDiff) {
        const std::string run_ref = !routed_profile.tze_run_reference.empty()
            ? routed_profile.tze_run_reference
            : (resolution.primary_target.empty() ? std::string("latest") : resolution.primary_target);
        const TzeRunRecord* target_run = memory_.find_tze_run(memory, run_ref);
        if (target_run == nullptr) {
            report.answer_status = "recursive_why_missing";
            report.answer_explanation = "No TZE run matched `" + run_ref + "` for Recursive Why/Diff.";
            report.next_action = "Run `omnix tze runs` to choose a persisted run, then rerun `omnix why <run-id>`.";
        } else {
            report.recursive_diff_report =
                build_recursive_diff_report_for_run(*target_run, memory, "observer=omnix why");
            report.answer_status = report.recursive_diff_report->status;
            report.answer_explanation = render_recursive_diff_answer(*report.recursive_diff_report);
            report.next_action = report.recursive_diff_report->next_action;
            report.storage_writes.push_back("x.Store(tze.recursive-why -> " + target_run->id + ")");
            if (report.recursive_diff_report->route_learning_status == "recursive_route_learning_used") {
                report.storage_writes.push_back("x.Store(tze.recursive-route-learning -> " + target_run->id + ")");
            }
            report.tze_stages.push_back({
                "x.Recursive.WhyDiff",
                "Compare observed failure path against deterministic success path",
                "RecursiveWhyDiffEngine",
                report.answer_status,
                "Recursive Why/Diff compared run `" + target_run->id + "` and classified `" +
                    report.recursive_diff_report->diff_category + "`.",
                {target_run->id, target_run->status},
                {report.recursive_diff_report->diff_category, report.recursive_diff_report->best_estimate_answer},
            });
            if (report.recursive_diff_report->route_learning_status != "route_learning_not_needed") {
                report.tze_stages.push_back({
                    "x.Recursive.RouteLearning",
                    "Mine operating memory caches for a learned route candidate",
                    "RecursiveRouteLearningEngine",
                    report.recursive_diff_report->route_learning_status,
                    "Recursive Route Learning searched definitions, prior TZE runs, history, and glossary evidence.",
                    {target_run->id, report.recursive_diff_report->logs_and_reasoning},
                    {report.recursive_diff_report->best_estimate_answer},
                });
            }
        }
    } else if (resolution.intent == RequestIntent::DoctorProject) {
        RequestProfile doctor_profile = routed_profile;
        doctor_profile.project_reference = !routed_profile.project_reference.empty()
            ? routed_profile.project_reference
            : resolution.primary_target;
        const std::string target = !doctor_profile.project_reference.empty()
            ? doctor_profile.project_reference
            : doctor_profile.raw_prompt;
        const std::string canonical_tool = tools_.canonical_name(target);
        const bool known_tool = tools_.is_known_tool(canonical_tool);
        if (known_tool) {
            report.resolved_project = canonical_tool;
            report.tool_doctor_report = tools_.doctor(canonical_tool, memory, true);
            if (report.tool_doctor_report->status == "native_ready") {
                report.tool_resolution = tools_.resolve(canonical_tool, memory, true);
                if (report.tool_resolution->found) {
                    memory_.remember_native_tool(memory, {
                        report.tool_resolution->logical_name,
                        report.tool_resolution->provider_type,
                        report.tool_resolution->executable_path,
                        report.tool_resolution->applet_name,
                        report.tool_resolution->version_fingerprint,
                        report.tool_resolution->capability_flags,
                        report.tool_resolution->environment_signature,
                        0,
                        0,
                        report.tool_resolution->cache_origin,
                        "verified",
                    });
                    report.memory_writes.push_back(memory.paths.native_tools_path.string());
                }
            }

            const std::optional<ProjectAlias> tool_alias = aliases_.find(canonical_tool);
            if (tool_alias.has_value()) {
                const ProjectResolution project = projects_.resolve(target, doctor_profile, memory, builder_, aliases_);
                if (project.acquisition.has_value()) {
                    report.acquisition_result = project.acquisition;
                }
                report.preflight_report = builder_.preflight(doctor_profile,
                                                             project.alias,
                                                             project.resolved ? project.source_path : std::filesystem::path{});
                report.doctor_report = builder_.doctor(doctor_profile,
                                                       project.alias,
                                                       project.resolved ? project.source_path : std::filesystem::path{});
                report.feedback_loop = relevant_history(memory, tool_alias->canonical_name);

                if (report.tool_doctor_report->status == "native_ready") {
                    report.doctor_report->status = "native_ready";
                    report.doctor_report->summary = "Verified native `" + canonical_tool + "` at " +
                        report.tool_doctor_report->executable_path +
                        ". OmniX will reuse it before any managed source build fallback.";
                    report.answer_status = report.doctor_report->status;
                    report.answer_explanation = report.doctor_report->summary;
                    report.next_action = "Use `" + native_tool_verify_command(canonical_tool) + "` or run `omnix build " +
                        tool_alias->canonical_name + "` only when you want the managed source build.";
                } else if (report.preflight_report->ready && report.doctor_report->status == "doctor_ready") {
                    report.doctor_report->status = "build_ready";
                    report.doctor_report->summary = "Native `" + canonical_tool +
                        "` was not found, but the managed source-build recipe is ready.";
                    report.answer_status = report.doctor_report->status;
                    report.answer_explanation = report.doctor_report->summary;
                    report.next_action = "Run `omnix build " + tool_alias->canonical_name + "` to build and stage `" +
                        tool_alias->canonical_name + "` with OmniX.";
                } else {
                    report.doctor_report->status = "doctor_attention_needed";
                    report.doctor_report->summary = "Native `" + canonical_tool +
                        "` was not found and the managed source-build doctor still has attention items.";
                    report.answer_status = report.doctor_report->status;
                    report.answer_explanation = report.doctor_report->summary;
                    report.next_action = "Use the package guidance below, then rerun `omnix doctor " + canonical_tool +
                        "` or `omnix build " + tool_alias->canonical_name + "`.";
                }
                report.storage_writes.push_back("x.Store(tool.doctor -> " + canonical_tool + ")");
                report.storage_writes.push_back("x.Store(tool.next_action -> " + report.next_action + ")");
            } else {
                report.answer_status = report.tool_doctor_report->status;
                report.answer_explanation = report.tool_doctor_report->summary;
                report.next_action = report.tool_doctor_report->recommended_next_command;
            }
            report.storage_writes.push_back("x.Store(native.doctor -> " + canonical_tool + ")");
        } else {
        const ProjectResolution project = projects_.resolve(target, doctor_profile, memory, builder_, aliases_);
        if (project.alias.has_value()) {
            report.resolved_project = project.alias->canonical_name;
        } else if (!project.canonical_name.empty()) {
            report.resolved_project = project.canonical_name;
        } else {
            report.resolved_project = target;
        }
        if (project.acquisition.has_value()) {
            report.acquisition_result = project.acquisition;
        }
        if (project.resolved) {
            report.resolved_project_path = project.source_path.string();
            report.source_inspection = builder_.inspect_source(project.source_path);
        }

        report.preflight_report = builder_.preflight(doctor_profile,
                                                     project.alias,
                                                     project.resolved ? project.source_path : std::filesystem::path{});
        report.doctor_report = builder_.doctor(doctor_profile,
                                               project.alias,
                                               project.resolved ? project.source_path : std::filesystem::path{});
        report.feedback_loop = relevant_history(memory, report.resolved_project);
        report.answer_status = report.doctor_report->status;
        report.answer_explanation = report.doctor_report->summary;
        report.next_action = report.preflight_report->ready
            ? "Run `omnix build <project>` to execute the selected portable recipe."
            : "Use the doctor guidance to install the missing dependencies, then rerun preflight.";
        }
    } else if (!blocked_tool_route_detail.empty()) {
        report.answer_status = "guardrail_blocked";
        report.answer_explanation = blocked_tool_route_detail;
        report.next_action =
            "Use `Run NMAP`, `Run NMAP Scan`, or `Run NMAP with a local /24 scan` when you want a guarded local probe.";
    } else if (resolution.intent == RequestIntent::NeuralMath) {
        NeuralMathEngine neural_math;
        const std::string mode = routed_profile.neural_math_mode.empty() ? "perceptron" : routed_profile.neural_math_mode;
        if (mode != "perceptron") {
            NeuralMathReport invalid;
            invalid.status = "neural_math_invalid_mode";
            invalid.summary = "Neural math phase 1 supports only `perceptron`.";
            invalid.model_type = mode;
            invalid.dataset = routed_profile.neural_dataset;
            report.neural_math_report = invalid;
        } else {
            report.neural_math_report = neural_math.run_perceptron(routed_profile.neural_dataset,
                                                                   routed_profile.neural_epochs,
                                                                   routed_profile.neural_learning_rate);
        }
        report.answer_status = report.neural_math_report->status;
        report.answer_explanation = report.neural_math_report->summary;
        report.next_action = report.answer_status == "not_linearly_separable"
            ? "Use a hidden-layer MLP simulation in the next NeuralNetwork phase; XOR is the teaching case."
            : "Run `omnix nn math perceptron --dataset xor` to see why the next phase needs an MLP.";
        report.storage_writes.push_back("x.Store(neural.math -> " + report.neural_math_report->dataset + ")");
        report.tze_stages.push_back({
            "x.Neural.Math",
            "Simulate local neural-network math without external ML dependencies",
            "NeuralMathEngine",
            report.answer_status,
            report.answer_explanation,
            {report.neural_math_report->model_type,
             report.neural_math_report->dataset,
             "epochs=" + std::to_string(report.neural_math_report->epochs_requested),
             "learning_rate=" + std::to_string(report.neural_math_report->learning_rate)},
            report.neural_math_report->math_trace,
        });
    } else if (resolution.intent == RequestIntent::NeuralRoute) {
        NeuralSignalRouter router;
        if (routed_profile.neural_route_mode != "tview") {
            NeuralRouteReport invalid;
            invalid.status = "neural_route_invalid_mode";
            invalid.summary = "Neural routing phase 1 supports only `nn route tview <file.jsonl>`.";
            invalid.input_path = routed_profile.neural_route_input_path;
            report.neural_route_report = invalid;
        } else {
            report.neural_route_report = router.route_tview_jsonl(routed_profile.neural_route_input_path);
        }
        if (report.neural_route_report.has_value() && !routed_profile.neural_route_output_path.empty() &&
            report.neural_route_report->status == "neural_route_complete") {
            std::string export_error;
            if (router.export_jsonl(*report.neural_route_report, routed_profile.neural_route_output_path, &export_error)) {
                report.neural_route_report->artifact_path = routed_profile.neural_route_output_path;
                report.storage_writes.push_back("x.Store(neural.route.jsonl -> " + routed_profile.neural_route_output_path + ")");
            } else {
                report.neural_route_report->warnings.push_back(export_error);
            }
        }
        report.answer_status = report.neural_route_report->status;
        report.answer_explanation = report.neural_route_report->summary;
        report.next_action = report.answer_status == "neural_route_complete"
            ? "Backtrace the weighted factors with `omnix tze replay latest` or `omnix tze report latest`."
            : "Provide a TView JSONL file from `omnix tview ... --out file.jsonl`.";
        report.storage_writes.push_back("x.Store(neural.route -> " + report.answer_status + ")");
        std::vector<std::string> outputs;
        if (!report.neural_route_report->predictions.empty()) {
            const NeuralRoutePrediction& top = report.neural_route_report->predictions.front();
            outputs.push_back(top.label + " confidence=" + std::to_string(top.confidence));
            for (const MathAttribution& attribution : top.attributions) {
                outputs.push_back(attribution.name + "=" + std::to_string(attribution.contribution));
            }
        }
        report.tze_stages.push_back({
            "x.Neural.SignalRouter",
            "Route TView evidence with local fixed-weight neural-style scoring",
            "NeuralSignalRouter",
            report.answer_status,
            report.answer_explanation,
            {routed_profile.neural_route_input_path, "mode=tview"},
            outputs,
        });
    } else if (resolution.intent == RequestIntent::PacketCapture) {
        PacketCaptureRequest packet_request = packet_request_from_profile(routed_profile, resolution);
        if (packet_request.mode == "doctor") {
            report.packet_capture_report = packet_capture_.doctor(packet_request);
        } else if (packet_request.mode == "pcap") {
            report.packet_capture_report = packet_capture_.read_pcap(packet_request);
        } else if (packet_request.port <= 0) {
            PacketCaptureReport invalid;
            invalid.mode = packet_request.mode;
            invalid.status = "capture_invalid_request";
            invalid.summary = "`omnix tview port <port>` requires a TCP port between 1 and 65535.";
            report.packet_capture_report = std::move(invalid);
        } else {
            report.packet_capture_report = packet_capture_.capture(packet_request);
        }
        if (report.packet_capture_report.has_value() && !packet_request.export_path.empty() &&
            (report.packet_capture_report->status == "capture_complete" ||
             report.packet_capture_report->status == "capture_empty")) {
            std::string export_error;
            if (packet_capture_.export_jsonl(*report.packet_capture_report, packet_request.export_path, &export_error)) {
                report.packet_capture_report->export_path = packet_request.export_path;
                report.storage_writes.push_back("x.Store(packet.jsonl -> " + packet_request.export_path + ")");
            } else {
                report.packet_capture_report->warnings.push_back(export_error);
            }
        }
        report.answer_status = report.packet_capture_report->status;
        report.answer_explanation = report.packet_capture_report->summary;
        report.next_action = report.packet_capture_report->status == "capture_attention_needed" ||
                report.packet_capture_report->status == "capture_blocked"
            ? "Run `omnix tview doctor` for manual packet-capture permission guidance."
            : "Use `omnix tview pcap <file> --port <port>` for deterministic offline packet review.";
        report.storage_writes.push_back("x.Store(packet.capture -> " + report.answer_status + ")");
    } else if (resolution.intent == RequestIntent::DefenseDiagnostic) {
        const DefenseDiagnosticRequest defense_request = defense_request_from_profile(routed_profile, resolution);
        report.defense_diagnostic_report = run_defense_diagnostic(defense_request);
        report.answer_status = report.defense_diagnostic_report->status;
        report.answer_explanation = report.defense_diagnostic_report->summary;
        report.next_action = "Use the proposed actions manually only after validating the evidence.";
        report.storage_writes.push_back("x.Store(defense.diagnostic -> " + report.answer_status + ")");
    } else if (resolution.intent == RequestIntent::DefenseDetection) {
        const DefenseDiagnosticRequest detection_request = defense_detection_request_from_profile(routed_profile, resolution);
        report.defense_detection_report = run_defense_detection(routed_profile, detection_request);
        if (!routed_profile.output_path.empty()) {
            if (export_defense_detection_json(*report.defense_detection_report, routed_profile.output_path)) {
                report.produced_artifact = routed_profile.output_path;
                push_unique(report.memory_writes, routed_profile.output_path);
                report.storage_writes.push_back("x.Store(defense.detect.json -> " + routed_profile.output_path + ")");
            }
        }
        report.answer_status = report.defense_detection_report->status;
        report.answer_explanation = report.defense_detection_report->summary;
        report.next_action = report.defense_detection_report->proposed_actions.empty()
            ? "Review detection evidence before any containment."
            : report.defense_detection_report->proposed_actions.front();
        std::vector<std::string> outputs;
        if (!report.defense_detection_report->signals.empty()) {
            const DefenseDetectionSignal& top = report.defense_detection_report->signals.front();
            outputs.push_back(top.category + ":" + top.id + " severity=" + top.severity);
            if (!top.recommended_next_action.empty()) {
                outputs.push_back(top.recommended_next_action);
            }
        }
        report.tze_stages.push_back({
            "x.Defense.EnvironmentDetect",
            "Collect bounded local environmental change evidence",
            "DefenseEnvironmentDetectEngine",
            report.answer_status,
            report.answer_explanation,
            {report.defense_detection_report->mode, "since=" + report.defense_detection_report->since_window},
            outputs,
        });
        report.storage_writes.push_back("x.Store(defense.detect -> " + report.answer_status + ")");
    } else if (resolution.intent == RequestIntent::VuplusGate) {
        report.vuplus_gate_report = run_vuplus_gate(routed_profile);
        if (!routed_profile.output_path.empty()) {
            export_vuplus_gate_json(*report.vuplus_gate_report, routed_profile.output_path);
            if (!report.vuplus_gate_report->artifact_path.empty()) {
                report.produced_artifact = report.vuplus_gate_report->artifact_path;
                push_unique(report.memory_writes, report.vuplus_gate_report->artifact_path);
                report.storage_writes.push_back("x.Store(vuplus.gate.json -> " + report.vuplus_gate_report->artifact_path + ")");
            }
        }
        report.answer_status = report.vuplus_gate_report->status;
        report.answer_explanation = report.vuplus_gate_report->why;
        report.next_action = report.vuplus_gate_report->next_action;
        std::vector<std::string> outputs = report.vuplus_gate_report->signals;
        outputs.push_back("confidence=" + std::to_string(report.vuplus_gate_report->confidence));
        outputs.push_back("remediation=" + report.vuplus_gate_report->remediation_mode);
        report.tze_stages.push_back({
            "x.Vuplus.Gate",
            "Explain and correlate local operational intelligence evidence",
            "VuplusGate",
            report.answer_status,
            report.answer_explanation,
            {report.vuplus_gate_report->mode,
             report.vuplus_gate_report->input_path,
             report.vuplus_gate_report->dependency_map_path},
            outputs,
        });
        report.storage_writes.push_back("x.Store(vuplus.gate -> " + report.answer_status + ")");
    } else if (resolution.intent == RequestIntent::ToolAction || resolution.intent == RequestIntent::TensorAction) {
        tool_flow_.run(routed_profile, memory, report, builder_, tools_, memory_);
        if (report.tool_invocation_report.has_value() &&
            report.tool_invocation_report->logical_name == "nmap") {
            if (const std::optional<int> open_port = first_open_tcp_port(*report.tool_invocation_report);
                open_port.has_value()) {
                report.next_action = "Investigate the open loopback TCP service with `omnix tview port " +
                    std::to_string(*open_port) + "`.";
            }
        }
        if (resolution.intent == RequestIntent::TensorAction) {
            std::vector<std::string> outputs;
            if (report.tool_invocation_report.has_value()) {
                outputs.push_back("tool=" + report.tool_invocation_report->logical_name);
                outputs.push_back("status=" + report.tool_invocation_report->status);
                if (!report.tool_invocation_report->output_excerpt.empty()) {
                    outputs.push_back(first_non_empty_line(report.tool_invocation_report->output_excerpt.front()));
                }
            }
            report.tze_stages.push_back({
                "x.Tensor.Framework",
                "Route native tensor literacy and local model-context actions",
                "TensorFramework",
                report.answer_status,
                report.answer_explanation,
                routed_profile.tool_arguments,
                outputs,
            });
        }
    } else if (resolution.intent == RequestIntent::ReplayTzeRun) {
        const std::string run_id = !routed_profile.tze_run_reference.empty() ? routed_profile.tze_run_reference : resolution.primary_target;
        const std::string resolved_id = memory_.resolve_tze_run_id(memory, run_id);
        report.answer_status = memory_.find_tze_run(memory, run_id) == nullptr ? "tze_run_missing" : "tze_run_replayed";
        report.answer_explanation = memory_.render_tze_run(memory, run_id);
        report.next_action = report.answer_status == "tze_run_replayed"
            ? "Use `omnix tze diff " + (resolved_id.empty() ? run_id : resolved_id) + " <other-run-id>` to compare this replay against another persisted run."
            : "Use `omnix memory runs` to list the available persisted TZE runs.";
        report.storage_writes.push_back("x.Store(tze.replay -> " + run_id + ")");
    } else if (resolution.intent == RequestIntent::ChainTzeRun) {
        const std::string run_id = !routed_profile.tze_run_reference.empty() ? routed_profile.tze_run_reference : resolution.primary_target;
        report.answer_status = memory_.find_tze_run(memory, run_id) == nullptr ? "tze_chain_missing" : "tze_chain_rendered";
        report.answer_explanation = memory_.render_tze_chain(memory, run_id);
        report.next_action = report.answer_status == "tze_chain_rendered"
            ? "Use `omnix tze replay " + memory_.resolve_tze_run_id(memory, run_id) + "` or `omnix tze diff-latest` to inspect chain members further."
            : "Use `omnix tze runs` to list the available persisted TZE runs.";
        report.storage_writes.push_back("x.Store(tze.chain -> " + run_id + ")");
    } else if (resolution.intent == RequestIntent::DiffTzeRuns) {
        const std::string left_id = !routed_profile.tze_run_reference.empty() ? routed_profile.tze_run_reference : resolution.primary_target;
        const std::string right_id = !routed_profile.tze_compare_reference.empty() ? routed_profile.tze_compare_reference : resolution.memory_view;
        const std::string resolved_left = memory_.resolve_tze_run_id(memory, left_id);
        const std::string resolved_right = memory_.resolve_tze_run_id(memory, right_id);
        const bool left_found = memory_.find_tze_run(memory, left_id) != nullptr;
        const bool right_found = memory_.find_tze_run(memory, right_id) != nullptr;
        report.answer_status = left_found && right_found ? "tze_run_diffed" : "tze_run_diff_missing";
        report.answer_explanation = memory_.render_tze_diff(memory, left_id, right_id);
        report.next_action = report.answer_status == "tze_run_diffed"
            ? "Use `omnix tze replay " + (resolved_left.empty() ? left_id : resolved_left) + "` or `omnix tze replay " +
                (resolved_right.empty() ? right_id : resolved_right) + "` to inspect either trace in full."
            : "Use `omnix memory runs` to list the available persisted TZE runs before diffing.";
        report.storage_writes.push_back("x.Store(tze.diff -> " + left_id + "::" + right_id + ")");
    } else if (resolution.intent == RequestIntent::ExplainTzeChange) {
        const std::string left_id = !routed_profile.tze_run_reference.empty() ? routed_profile.tze_run_reference : resolution.primary_target;
        const std::string right_id = !routed_profile.tze_compare_reference.empty() ? routed_profile.tze_compare_reference : resolution.memory_view;
        const bool left_found = memory_.find_tze_run(memory, left_id) != nullptr;
        const bool right_found = memory_.find_tze_run(memory, right_id) != nullptr;
        report.answer_status = left_found && right_found ? "tze_change_explained" : "tze_change_missing";
        report.answer_explanation = memory_.render_tze_change_explanation(memory, left_id, right_id);
        report.next_action = report.answer_status == "tze_change_explained"
            ? "Use `omnix tze diff " + memory_.resolve_tze_run_id(memory, left_id) + " " +
                memory_.resolve_tze_run_id(memory, right_id) + "` for the raw delta or `omnix tze report latest` for an artifact."
            : "Use `omnix memory runs` to list the available persisted TZE runs before explaining the change.";
        report.storage_writes.push_back("x.Store(tze.explain-change -> " + left_id + "::" + right_id + ")");
    } else if (resolution.intent == RequestIntent::ReportTzeRun) {
        const std::string run_id = !routed_profile.tze_run_reference.empty() ? routed_profile.tze_run_reference : resolution.primary_target;
        const std::string artifact = memory_.write_tze_run_report(memory, run_id, routed_profile.output_path);
        report.answer_status = artifact.empty() ? "tze_report_missing" : "tze_report_written";
        report.answer_explanation = artifact.empty()
            ? "No TZE run matched `" + run_id + "`."
            : "Saved the TZE run report for `" + memory_.resolve_tze_run_id(memory, run_id) + "` at `" + artifact + "`.";
        report.next_action = artifact.empty()
            ? "Use `omnix memory runs` to list the available persisted TZE runs."
            : "Open the saved TZE report at `" + artifact + "` or compare it with another run using `omnix tze diff`.";
        if (!artifact.empty()) {
            report.produced_artifact = artifact;
            push_unique(report.memory_writes, artifact);
        }
        report.storage_writes.push_back("x.Store(tze.report -> " + run_id + ")");
    } else if (resolution.intent == RequestIntent::DiffReportTzeRuns) {
        const std::string left_id = !routed_profile.tze_run_reference.empty() ? routed_profile.tze_run_reference : resolution.primary_target;
        const std::string right_id = !routed_profile.tze_compare_reference.empty() ? routed_profile.tze_compare_reference : resolution.memory_view;
        const std::string artifact = memory_.write_tze_diff_report(memory, left_id, right_id, routed_profile.output_path);
        report.answer_status = artifact.empty() ? "tze_diff_report_missing" : "tze_diff_report_written";
        report.answer_explanation = artifact.empty()
            ? memory_.render_tze_diff(memory, left_id, right_id)
            : "Saved the TZE diff report at `" + artifact + "`.";
        report.next_action = artifact.empty()
            ? "Use `omnix memory runs` to list the available persisted TZE runs."
            : "Open the diff report or replay either run directly with `omnix tze replay`.";
        if (!artifact.empty()) {
            report.produced_artifact = artifact;
            push_unique(report.memory_writes, artifact);
        }
        report.storage_writes.push_back("x.Store(tze.diff-report -> " + left_id + "::" + right_id + ")");
    } else if (resolution.intent == RequestIntent::ExportTzeBundle) {
        const std::string run_id = !routed_profile.tze_run_reference.empty() ? routed_profile.tze_run_reference : resolution.primary_target;
        const std::string artifact = memory_.write_tze_bundle(memory, run_id, routed_profile.output_path);
        report.answer_status = artifact.empty() ? "tze_bundle_missing" : "tze_bundle_written";
        report.answer_explanation = artifact.empty()
            ? "No TZE run matched `" + run_id + "`."
            : "Saved the compact TZE bundle at `" + artifact + "`.";
        report.next_action = artifact.empty()
            ? "Use `omnix tze runs` to list the available persisted TZE runs."
            : "Transfer the bundle or re-import it later with `omnix tze import " + artifact + "`.";
        if (!artifact.empty()) {
            report.produced_artifact = artifact;
            push_unique(report.memory_writes, artifact);
        }
        report.storage_writes.push_back("x.Store(tze.export -> " + run_id + ")");
    } else if (resolution.intent == RequestIntent::ImportTzeBundle) {
        const std::string bundle_path = !routed_profile.tze_run_reference.empty() ? routed_profile.tze_run_reference : resolution.primary_target;
        const std::string summary = memory_.import_tze_bundle(memory, bundle_path);
        report.answer_status = summary.empty() ? "tze_bundle_import_missing" : "tze_bundle_imported";
        report.answer_explanation = summary.empty()
            ? "No readable TZE bundle was found at `" + bundle_path + "`."
            : summary;
        report.next_action = summary.empty()
            ? "Check the bundle path and retry the import."
            : "Use `omnix tze runs` or `omnix memory cases` to inspect the imported state.";
        if (!summary.empty()) {
            push_unique(report.memory_writes, memory.paths.tze_runs_path.string());
            push_unique(report.memory_writes, memory.paths.cases_path.string());
        }
        report.storage_writes.push_back("x.Store(tze.import -> " + bundle_path + ")");
    } else if (resolution.intent == RequestIntent::PruneTzeRuns) {
        report.answer_status = "tze_pruned";
        report.answer_explanation = memory_.prune_tze_runs(memory, routed_profile.prune_keep_count, routed_profile.important_only);
        report.next_action = "Use `omnix tze runs` to inspect the compacted ledger.";
        report.storage_writes.push_back("x.Store(tze.prune -> keep=" + std::to_string(routed_profile.prune_keep_count) + ")");
        push_unique(report.memory_writes, memory.paths.tze_runs_path.string());
    } else if (resolution.intent == RequestIntent::PruneMemory) {
        report.answer_status = "memory_pruned";
        report.answer_explanation = memory_.prune_memory(memory, routed_profile.prune_keep_count, routed_profile.important_only);
        report.next_action = "Use `omnix memory history` or `omnix tze runs` to inspect the compacted memory state.";
        report.storage_writes.push_back("x.Store(memory.prune -> keep=" + std::to_string(routed_profile.prune_keep_count) + ")");
        push_unique(report.memory_writes, memory.paths.history_path.string());
        push_unique(report.memory_writes, memory.paths.tze_runs_path.string());
    } else if (resolution.intent == RequestIntent::MarkTzeRunOutcome) {
        const std::string run_id = !routed_profile.tze_run_reference.empty() ? routed_profile.tze_run_reference : resolution.primary_target;
        std::string resolved_id;
        const bool marked = memory_.mark_tze_run_feedback(memory,
                                                          run_id,
                                                          routed_profile.feedback_value,
                                                          routed_profile.feedback_note,
                                                          &resolved_id);
        report.answer_status = marked ? "tze_feedback_recorded" : "tze_feedback_missing";
        report.answer_explanation = marked
            ? "Recorded `" + routed_profile.feedback_value + "` feedback for `" + resolved_id + "`."
            : "No TZE run matched `" + run_id + "` for feedback recording.";
        report.next_action = marked
            ? "Rerun `omnix decide <case-or-source>` to let deterministic planning reuse this outcome."
            : "Use `omnix memory runs` to list the available persisted TZE runs.";
        if (marked) {
            push_unique(report.memory_writes, memory.paths.tze_runs_path.string());
        }
        report.storage_writes.push_back("x.Store(tze.feedback -> " + run_id + ":" + routed_profile.feedback_value + ")");
    } else if (resolution.intent == RequestIntent::MarkDecisionFeedback) {
        const std::string case_ref = !routed_profile.analyst_reference.empty() ? routed_profile.analyst_reference : resolution.primary_target;
        const std::string decision_ref = !routed_profile.decision_reference.empty() ? routed_profile.decision_reference : resolution.memory_view;
        std::string resolved_case_id;
        std::string resolved_decision_id;
        const bool marked = memory_.mark_decision_feedback(memory,
                                                           case_ref,
                                                           decision_ref,
                                                           routed_profile.feedback_value,
                                                           routed_profile.feedback_note,
                                                           &resolved_case_id,
                                                           &resolved_decision_id);
        report.answer_status = marked ? "decision_feedback_recorded" : "decision_feedback_missing";
        report.answer_explanation = marked
            ? "Recorded `" + routed_profile.feedback_value + "` feedback for decision `" + resolved_decision_id + "` on case `" + resolved_case_id + "`."
            : "No matching case/decision pair was found for feedback recording.";
        report.next_action = marked
            ? "Rerun `omnix decide " + resolved_case_id + "` so planning can reuse this operator signal."
            : "Use `omnix case " + case_ref + "` to inspect the available decision ids.";
        if (marked) {
            push_unique(report.memory_writes, memory.paths.cases_path.string());
        }
        report.storage_writes.push_back("x.Store(decision.feedback -> " + case_ref + "::" + decision_ref + ":" + routed_profile.feedback_value + ")");
    } else if (resolution.intent == RequestIntent::MarkDecisionOutcome) {
        const std::string case_ref = !routed_profile.analyst_reference.empty() ? routed_profile.analyst_reference : resolution.primary_target;
        const std::string decision_ref = !routed_profile.decision_reference.empty() ? routed_profile.decision_reference : resolution.memory_view;
        std::string resolved_case_id;
        std::string resolved_decision_id;
        const bool marked = memory_.mark_decision_outcome(memory,
                                                          case_ref,
                                                          decision_ref,
                                                          routed_profile.feedback_value,
                                                          routed_profile.feedback_note,
                                                          &resolved_case_id,
                                                          &resolved_decision_id);
        report.answer_status = marked ? "decision_outcome_recorded" : "decision_outcome_missing";
        report.answer_explanation = marked
            ? "Recorded outcome `" + routed_profile.feedback_value + "` for decision `" + resolved_decision_id + "` on case `" + resolved_case_id + "`."
            : "No matching case/decision pair was found for outcome recording.";
        report.next_action = marked
            ? "Rerun `omnix decide " + resolved_case_id + "` so deterministic planning can reuse the outcome."
            : "Use `omnix case " + case_ref + "` to inspect the available decision ids.";
        if (marked) {
            push_unique(report.memory_writes, memory.paths.cases_path.string());
        }
        report.storage_writes.push_back("x.Store(decision.outcome -> " + case_ref + "::" + decision_ref + ":" + routed_profile.feedback_value + ")");
    } else if (resolution.intent == RequestIntent::ExportCaseBundle) {
        const std::string case_ref = !routed_profile.analyst_reference.empty() ? routed_profile.analyst_reference : resolution.primary_target;
        const std::string artifact = memory_.write_case_bundle(memory, case_ref, routed_profile.output_path);
        report.answer_status = artifact.empty() ? "case_bundle_missing" : "case_bundle_written";
        report.answer_explanation = artifact.empty()
            ? "No case matched `" + case_ref + "`."
            : "Saved the compact case bundle at `" + artifact + "`.";
        report.next_action = artifact.empty()
            ? "Use `omnix case list` to inspect the available cases."
            : "Transfer the bundle or re-import it later with `omnix case import " + artifact + "`.";
        if (!artifact.empty()) {
            report.produced_artifact = artifact;
            push_unique(report.memory_writes, artifact);
        }
        report.storage_writes.push_back("x.Store(case.export -> " + case_ref + ")");
    } else if (resolution.intent == RequestIntent::ImportCaseBundle) {
        const std::string bundle_path = !routed_profile.analyst_reference.empty() ? routed_profile.analyst_reference : resolution.primary_target;
        const std::string summary = memory_.import_case_bundle(memory, bundle_path);
        report.answer_status = summary.empty() ? "case_bundle_import_missing" : "case_bundle_imported";
        report.answer_explanation = summary.empty()
            ? "No readable case bundle was found at `" + bundle_path + "`."
            : summary;
        report.next_action = summary.empty()
            ? "Check the bundle path and retry the import."
            : "Use `omnix case list` or `omnix incident list` to inspect the imported state.";
        if (!summary.empty()) {
            push_unique(report.memory_writes, memory.paths.cases_path.string());
            push_unique(report.memory_writes, memory.paths.tze_runs_path.string());
        }
        report.storage_writes.push_back("x.Store(case.import -> " + bundle_path + ")");
    } else if (resolution.intent == RequestIntent::ListIncidents) {
        report.answer_status = "incident_listed";
        report.answer_explanation = memory_.render_incident_list(memory);
        report.next_action = "Run `omnix incident <id>` to inspect an incident or `omnix incident report <id>` to save an artifact.";
        report.storage_writes.push_back("x.Store(incident.list)");
    } else if (resolution.intent == RequestIntent::InspectIncident) {
        const std::string incident_ref = !routed_profile.incident_reference.empty() ? routed_profile.incident_reference : resolution.primary_target;
        report.answer_explanation = memory_.render_incident(memory, incident_ref);
        report.answer_status = report.answer_explanation.rfind("No incident matched", 0) == 0 ? "incident_missing" : "incident_loaded";
        report.next_action = report.answer_status == "incident_loaded"
            ? "Run `omnix incident report " + incident_ref + "` to save a portable incident report."
            : "Use `omnix incident list` to inspect the available incidents.";
        report.storage_writes.push_back("x.Store(incident.inspect -> " + incident_ref + ")");
    } else if (resolution.intent == RequestIntent::ReportIncident) {
        const std::string incident_ref = !routed_profile.incident_reference.empty() ? routed_profile.incident_reference : resolution.primary_target;
        const std::string artifact = memory_.write_incident_report(memory, incident_ref, routed_profile.output_path);
        report.answer_status = artifact.empty() ? "incident_report_missing" : "incident_report_written";
        report.answer_explanation = artifact.empty()
            ? "No incident matched `" + incident_ref + "`."
            : "Saved the incident report at `" + artifact + "`.";
        report.next_action = artifact.empty()
            ? "Use `omnix incident list` to inspect the available incidents."
            : "Open the saved report at `" + artifact + "` or inspect the source cases with `omnix case`.";
        if (!artifact.empty()) {
            report.produced_artifact = artifact;
            push_unique(report.memory_writes, artifact);
        }
        report.storage_writes.push_back("x.Store(incident.report -> " + incident_ref + ")");
    } else if (resolution.intent == RequestIntent::ReviewModule) {
        const std::string target = !routed_profile.review_target.empty() ? routed_profile.review_target : resolution.primary_target;
        report.review_artifact = self_review_.review_target(target, memory, std::filesystem::current_path());
        report.answer_status = report.review_artifact->status == "reviewed" ? "review_ready" : report.review_artifact->status;
        report.answer_explanation = report.review_artifact->summary;
        report.next_action = report.review_artifact->status == "reviewed"
            ? "Run `omnix patch-proposal " + target + "` to generate a guarded patch proposal artifact."
            : "Check the target path or module name and retry the review.";
        if (!report.review_artifact->artifact_path.empty()) {
            report.produced_artifact = report.review_artifact->artifact_path;
            push_unique(report.memory_writes, report.review_artifact->artifact_path);
        }
        report.storage_writes.push_back("x.Store(review -> " + target + ")");
    } else if (resolution.intent == RequestIntent::PatchProposal) {
        const std::string target = !routed_profile.review_target.empty() ? routed_profile.review_target : resolution.primary_target;
        const ReviewArtifact review = self_review_.review_target(target, memory, std::filesystem::current_path());
        report.review_artifact = review;
        report.patch_proposal_artifact = self_review_.propose_patch(target, memory, review, std::filesystem::current_path());
        report.answer_status = report.patch_proposal_artifact->status == "proposed" ? "patch_proposal_ready" : report.patch_proposal_artifact->status;
        report.answer_explanation = report.patch_proposal_artifact->summary;
        report.next_action = report.patch_proposal_artifact->status == "proposed"
            ? "Review the proposal artifact and apply changes manually through a guarded edit flow."
            : "Resolve the review target first, then rerun the patch proposal.";
        if (!report.patch_proposal_artifact->artifact_path.empty()) {
            report.produced_artifact = report.patch_proposal_artifact->artifact_path;
            push_unique(report.memory_writes, report.patch_proposal_artifact->artifact_path);
        }
        report.storage_writes.push_back("x.Store(patch-proposal -> " + target + ")");
    } else if (resolution.intent == RequestIntent::ShowMemory) {
        const std::string view = resolution.memory_view.empty() ? "history" : resolution.memory_view;
        report.answer_status = "memory";
        report.answer_explanation = memory_.render_view(memory, view);
        report.next_action = "Run `omnix ask` or `omnix define` to add more learning data.";
    } else if (resolution.intent == RequestIntent::SetPersonaMode) {
        const std::string requested_mode = !routed_profile.persona_mode.empty()
            ? routed_profile.persona_mode
            : resolution.primary_target;
        const std::string canonical_mode = canonical_persona_mode(requested_mode);
        if (canonical_mode.empty()) {
            report.answer_status = "persona_mode_unknown";
            report.answer_explanation =
                "Unknown persona mode `" + requested_mode + "`. Available modes: premise, cynic, professional, neutral.";
            report.next_action = "Run `omnix persona mode premise`, `cynic`, `professional`, or `neutral`.";
        } else {
            OperatorPersonaRecord persona = memory.operator_persona.value_or(OperatorPersonaRecord{});
            apply_persona_mode_defaults(persona, canonical_mode);
            persona.local_username = persona.local_username.empty() ? local_operator_identity() : persona.local_username;
            persona.host_identifier = persona.host_identifier.empty() ? local_host_identity() : persona.host_identifier;
            persona.last_source_map = routed_profile.source_map_path;
            persona.last_memory_root = routed_profile.memory_root_path.empty() ? "(default)" : routed_profile.memory_root_path;
            memory_.remember_operator_persona(memory, persona);
            report.answer_status = "persona_mode_set";
            report.answer_explanation = persona_mode_readout(persona);
            report.next_action = "Run `omnix memory persona` or ask `Who am I?` to inspect the active mode.";
            push_unique(report.memory_writes, memory.paths.preferences_path.string());
            report.storage_writes.push_back("x.Store(persona.mode -> " + persona.persona_mode + ")");
        }
    } else if (resolution.intent == RequestIntent::Conversation) {
        report.answer_status = "ok";
        if (is_context_summary_prompt(routed_profile.raw_prompt)) {
            report.answer_explanation = render_context_summary(memory);
            report.next_action = memory.tze_runs.empty()
                ? "Run `omnix ask \"What is the Sun\"`, `secure my system`, or `Run NMAP` to create local context."
                : "Use `omnix tze replay latest` or rerun the last activity with a narrower prompt.";
        } else if (is_greeting_prompt(routed_profile.raw_prompt)) {
            const std::string prefix = memory.operator_persona.has_value() && !memory.operator_persona->persona_mode.empty()
                ? "[" + memory.operator_persona->persona_mode + " mode] "
                : "";
            report.answer_explanation =
                prefix + "Hello. I’m OmniX, your local TZE-native analyst and orchestration shell. "
                "I can inspect this machine, analyze evidence, replay TZE runs, and route guarded native operations.";
            report.next_action =
                "Try `who are you`, `secure my system`, `Run NMAP`, or `omnix memory history`.";
        } else if (is_operator_identity_prompt(routed_profile.raw_prompt)) {
            if (memory.operator_persona.has_value() &&
                (!memory.operator_persona->preferred_label.empty() ||
                 !memory.operator_persona->persona_mode.empty())) {
                const OperatorPersonaRecord& persona = *memory.operator_persona;
                std::ostringstream who;
                who << "You are " << (persona.preferred_label.empty() ? "the local operator" : persona.preferred_label);
                if (!persona.role_label.empty()) {
                    who << ", acting as " << persona.role_label;
                }
                who << ".";
                if (!persona.self_description.empty()) {
                    who << " " << persona.self_description;
                }
                if (!persona.local_username.empty()) {
                    who << " Local user: " << persona.local_username << ".";
                }
                if (!persona.host_identifier.empty()) {
                    who << " Host: " << persona.host_identifier << ".";
                }
                if (!persona.persona_mode.empty()) {
                    who << " Active mode: " << persona.persona_mode << ".";
                }
                if (!persona.tone_profile.empty()) {
                    who << " Tone: " << persona.tone_profile << ".";
                }
                if (!persona.safety_posture.empty()) {
                    who << " Safety posture: " << persona.safety_posture << ".";
                }
                report.answer_explanation = who.str();
            } else {
                report.answer_explanation =
                    "I do not have a richer stored operator persona yet. "
                    "Right now I can identify you as local user `" + local_operator_identity() +
                    "` on host `" + local_host_identity() + "`.";
            }
            report.next_action = "Use `omnix memory persona` to inspect the current operator/persona view.";
        } else if (is_identity_prompt(routed_profile.raw_prompt)) {
            // Intentionally aspirational readout kept as a manual Turning Test marker.
            report.answer_explanation =
                "I’m OmniX: a deterministic analyst, case manager, and TZE orchestration engine with guarded local assist. "
                "I can ingest logs, analyze cases, explain runs, route safe tools like Nmap, and keep a replayable memory of what we do.";
            report.next_action =
                "Try `secure my system`, `Run NMAP with a local /24 scan`, or `define xProcessingCache`.";
        } else {
            report.answer_explanation =
                "I’m here and ready. Ask for a system check, a case review, a TZE replay, or a guarded local tool run.";
            report.next_action =
                "Try `secure my system`, `Run NMAP`, or `who are you`.";
        }
        if (!routed_profile.shell_correction_note.empty()) {
            report.answer_explanation += "\nNormalization: " + routed_profile.shell_correction_note;
        }
    } else if (resolution.intent == RequestIntent::GeneralDefinitionQuery ||
               resolution.intent == RequestIntent::DefineSymbol ||
               resolution.intent == RequestIntent::ExplainCommand) {
        const std::string target = resolution.primary_target.empty() ? routed_profile.raw_prompt : resolution.primary_target;
        definition_flow_.run(routed_profile, target, memory, report, definitions_, memory_);
        apply_recursive_route_learning(report, memory, routed_profile.source_map_path);
        if (routed_profile.assist_requested &&
            report.definition_answer.has_value() &&
            !report.definition_answer->found &&
            (report.answer_status == "unknown_query" || report.answer_status == "clarify_needed")) {
            try_openai_freeform(*report.definition_answer, "definition_unresolved");
        }
    } else if (resolution.intent == RequestIntent::AuthorBuildRecipe) {
        RequestProfile author_profile = routed_profile;
        author_profile.project_reference = !routed_profile.project_reference.empty()
            ? routed_profile.project_reference
            : resolution.primary_target;
        author_profile.build_source_path = author_profile.project_reference;
        recipe_authoring_.run(author_profile, memory, report, builder_, cache_, memory_, *provider_, security_);
    } else if (resolution.intent == RequestIntent::BuildProject || routed_profile.execute_build) {
        RequestProfile build_profile = routed_profile;
        build_profile.raw_prompt = routed_profile.raw_prompt;
        build_profile.project_reference = !routed_profile.project_reference.empty()
            ? routed_profile.project_reference
            : resolution.primary_target;
        build_profile.execute_build = true;
        build_flow_.run(build_profile, memory, report, builder_, cache_, knowledge_, memory_, tools_, aliases_, projects_, *provider_, security_);
    } else if (resolution.intent == RequestIntent::Unknown && routed_profile.assist_requested) {
        if (is_security_guidance_only_prompt(routed_profile.raw_prompt)) {
            const DefinitionAnswer answer = definitions_.lookup(routed_profile.raw_prompt,
                                                                routed_profile.source_map_path,
                                                                memory);
            report.definition_answer = answer;
            if (!try_openai_freeform(answer, "security_guidance_only")) {
                report.answer_status = "unknown_intent";
                report.answer_explanation = answer.summary;
                report.next_action =
                    "Run explicit read-only diagnostics such as `omnix defend diag cpu`, `omnix defend diag memory`, or `omnix defend diag port <port>`.";
            }
        } else {
        const std::vector<std::string> allowlisted_tools = allowlisted_tool_assist_actions();
        std::optional<ToolAssistPlan> proposed_plan =
            provider_->propose_tool_action(routed_profile.raw_prompt, allowlisted_tools);
        if (proposed_plan.has_value()) {
            report.tool_assist_plan = proposed_plan;
            report.tze_stages.push_back({
                "x.Assist.Plan",
                "Ask the provider for a bounded tool-action plan",
                "ReasoningProvider",
                "assist_planned",
                "Provider `" + std::string(provider_->id()) + "` proposed tool `" + proposed_plan->tool_name + "`.",
                {routed_profile.raw_prompt},
                {proposed_plan->tool_name},
            });

            ToolAssistPlan validated_plan;
            std::string validation_detail;
            if (validate_tool_assist_plan(*proposed_plan, &validated_plan, &validation_detail)) {
                report.assist_status = "assist_used";
                report.tool_assist_plan = validated_plan;
                report.tze_stages.push_back({
                    "x.Assist.Validate",
                    "Validate the provider-proposed tool action against Omni guardrails",
                    "SessionCoordinator",
                    "assist_validated",
                    validation_detail,
                    {validated_plan.tool_name},
                    validated_plan.arguments,
                });

                RequestProfile tool_profile = routed_profile;
                tool_profile.resolved_intent = RequestIntent::ToolAction;
                tool_profile.tool_mode = ToolCommandMode::Run;
                tool_profile.requested_tool_name = validated_plan.tool_name;
                tool_profile.tool_arguments = validated_plan.arguments;
                tool_profile.assist_requested = false;
                dispatch_module = "ToolFlowInterpreter";
                tool_flow_.run(tool_profile, memory, report, builder_, tools_, memory_);
                report.answer_explanation =
                    "Guarded assist selected `" + validated_plan.tool_name + "`.\n"
                    "Rationale: " + validated_plan.rationale + "\n" +
                    report.answer_explanation;
                if (!validated_plan.safety_notes.empty()) {
                    report.answer_explanation += "\nSafety notes:\n";
                    for (const std::string& note : validated_plan.safety_notes) {
                        report.answer_explanation += " - " + note + "\n";
                    }
                }
                report.next_action = report.next_action.empty()
                    ? "Review the tool output and rerun with a more specific prompt if needed."
                    : report.next_action;
            } else {
                report.assist_status = "assist_bypassed";
                report.tze_stages.push_back({
                    "x.Assist.Validate",
                    "Validate the provider-proposed tool action against Omni guardrails",
                    "SessionCoordinator",
                    "assist_rejected",
                    validation_detail,
                    {proposed_plan->tool_name},
                    proposed_plan->arguments,
                });
                const DefinitionAnswer answer = definitions_.lookup(routed_profile.raw_prompt.empty() ? resolution.primary_target : routed_profile.raw_prompt,
                                                                    routed_profile.source_map_path,
                                                                    memory);
                report.definition_answer = answer;
                if (!try_openai_freeform(answer, validation_detail)) {
                    report.answer_status = "unknown_intent";
                    report.answer_explanation = answer.summary +
                        "\nGuarded assist rejected the proposed tool plan: " + validation_detail;
                    report.next_action = "Try a more specific prompt or run one of the allowlisted tools directly.";
                }
            }
        } else {
            report.assist_status = "assist_bypassed";
            report.tze_stages.push_back({
                "x.Assist.Plan",
                "Ask the provider for a bounded tool-action plan",
                "ReasoningProvider",
                "assist_bypassed",
                "Provider `" + std::string(provider_->id()) + "` did not return a valid allowlisted tool plan.",
                {routed_profile.raw_prompt},
                {"deterministic_fallback"},
            });
            const DefinitionAnswer answer = definitions_.lookup(routed_profile.raw_prompt.empty() ? resolution.primary_target : routed_profile.raw_prompt,
                                                                routed_profile.source_map_path,
                                                                memory);
            report.definition_answer = answer;
            if (!try_openai_freeform(answer, "tool_plan_unavailable")) {
                report.answer_status = "unknown_intent";
                report.answer_explanation = answer.summary;
                report.next_action = "Try a more specific prompt or run a tool command explicitly.";
            }
        }
        }
    } else {
        const DefinitionAnswer answer = definitions_.lookup(routed_profile.raw_prompt.empty() ? resolution.primary_target : routed_profile.raw_prompt,
                                                            routed_profile.source_map_path,
                                                            memory);
        report.definition_answer = answer;
        report.answer_status = "unknown_intent";
        report.answer_explanation = answer.summary;
        report.next_action = "Try `omnix ask \"Build NMAP\"`, `omnix define xProcessingCache`, or `omnix memory history`.";
    }

    if (routed_profile.assist_requested &&
        (resolution.intent == RequestIntent::ExplainTzeChange || resolution.intent == RequestIntent::ReportTzeRun)) {
        AssistRequest assist_request;
        assist_request.task_id = resolution.intent == RequestIntent::ExplainTzeChange
            ? "tze_explain_change"
            : "tze_report";
        if (resolution.intent == RequestIntent::ExplainTzeChange) {
            assist_request.target_label = (!routed_profile.tze_run_reference.empty() ? routed_profile.tze_run_reference : resolution.primary_target) +
                "::" +
                (!routed_profile.tze_compare_reference.empty() ? routed_profile.tze_compare_reference : resolution.memory_view);
            assist_request.deterministic_text = report.answer_explanation;
        } else {
            assist_request.target_label = !routed_profile.tze_run_reference.empty() ? routed_profile.tze_run_reference : resolution.primary_target;
            assist_request.deterministic_text = memory_.render_tze_run(memory, assist_request.target_label);
        }

        const std::optional<AssistAnnotation> assist = provider_->assist_annotation(assist_request);
        if (assist.has_value()) {
            report.assist_status = "assist_used";
            report.assist_annotation = assist;
            if (resolution.intent == RequestIntent::ExplainTzeChange) {
                report.answer_explanation += render_assist_annotation_text(*assist);
            } else if (!report.produced_artifact.empty()) {
                std::ofstream output(report.produced_artifact, std::ios::app);
                output << "\n" << render_assist_annotation_markdown(*assist);
                push_unique(report.memory_writes, report.produced_artifact);
                report.answer_explanation += "\nGuarded assist appended a validated summary to `" +
                    report.produced_artifact + "`.";
            }
        } else {
            if (report.assist_status.empty()) {
                report.assist_status = "assist_bypassed";
            }
        }

        report.tze_stages.push_back({
            "x.Assist.Guarded",
            "Annotate deterministic output with validated assistive summaries",
            "ReasoningProvider",
            report.assist_status.empty() ? "assist_bypassed" : report.assist_status,
            report.assist_annotation.has_value()
                ? "Provider `" + std::string(provider_->id()) + "` returned a validated assist annotation."
                : "Provider `" + std::string(provider_->id()) + "` did not return a validated assist annotation; deterministic output was preserved.",
            {assist_request.task_id, assist_request.target_label},
            report.assist_annotation.has_value() ? std::vector<std::string>{report.assist_annotation->summary}
                                                 : std::vector<std::string>{"deterministic_fallback"},
        });
    }

    if (routed_profile.assist_requested &&
        (resolution.intent == RequestIntent::InspectIncident || resolution.intent == RequestIntent::ReportIncident ||
         resolution.intent == RequestIntent::InspectCase)) {
        const std::string target_label = resolution.intent == RequestIntent::InspectIncident
            ? (!routed_profile.incident_reference.empty() ? routed_profile.incident_reference : resolution.primary_target)
            : (!routed_profile.analyst_reference.empty() ? routed_profile.analyst_reference : resolution.primary_target);
        const std::optional<CaseSummaryAssistPlan> proposed = provider_->propose_case_summary(target_label, report.answer_explanation);
        if (proposed.has_value()) {
            CaseSummaryAssistPlan validated;
            std::string validation_detail;
            if (validate_case_summary_assist_plan(*proposed, &validated, &validation_detail)) {
                report.assist_status = "assist_used";
                report.case_summary_assist_plan = validated;
                report.answer_explanation += render_case_summary_annotation_text(validated);
                if (!report.produced_artifact.empty()) {
                    std::ofstream output(report.produced_artifact, std::ios::app);
                    output << "\n\n## Guarded Assist Summary\n\n";
                    if (!validated.summary_title.empty()) {
                        output << "- Title: " << validated.summary_title << "\n";
                    }
                    output << "- Executive summary: " << validated.executive_summary << "\n";
                    for (const std::string& highlight : validated.highlights) {
                        output << "- Highlight: " << highlight << "\n";
                    }
                    push_unique(report.memory_writes, report.produced_artifact);
                }
                report.tze_stages.push_back({
                    "x.Assist.CaseSummary",
                    "Append a validated case or incident summary",
                    "ReasoningProvider",
                    "assist_used",
                    validation_detail,
                    {target_label},
                    {validated.executive_summary},
                });
            } else {
                if (report.assist_status.empty()) {
                    report.assist_status = "assist_bypassed";
                }
                report.tze_stages.push_back({
                    "x.Assist.CaseSummary",
                    "Append a validated case or incident summary",
                    "ReasoningProvider",
                    "assist_rejected",
                    validation_detail,
                    {target_label},
                    {"deterministic_fallback"},
                });
            }
        }
    }

    if (routed_profile.assist_requested &&
        (resolution.intent == RequestIntent::ReviewModule || resolution.intent == RequestIntent::PatchProposal)) {
        AssistRequest assist_request;
        assist_request.task_id = resolution.intent == RequestIntent::ReviewModule ? "review" : "patch_proposal";
        assist_request.target_label = !routed_profile.review_target.empty() ? routed_profile.review_target : resolution.primary_target;
        assist_request.deterministic_text = report.answer_explanation;
        const std::optional<AssistAnnotation> assist = provider_->assist_annotation(assist_request);
        if (assist.has_value()) {
            report.assist_status = "assist_used";
            report.assist_annotation = assist;
            if (report.review_artifact.has_value()) {
                report.review_artifact->assist_annotation = assist;
            }
            if (report.patch_proposal_artifact.has_value()) {
                report.patch_proposal_artifact->assist_annotation = assist;
            }
        } else if (report.assist_status.empty()) {
            report.assist_status = "assist_bypassed";
        }
    }

    report.tze_stages.push_back({
        "x.Dispatch",
        "Execute the selected runtime flow",
        dispatch_module,
        report.answer_status.empty() ? "ok" : report.answer_status,
        "Delegated execution to `" + dispatch_module + "` for intent `" + report.resolved_intent + "`.",
        {resolution.primary_target, report.resolved_project},
        {report.answer_status, report.next_action},
    });

    report.postprocess_record = build_postprocess_record(report);
    report.storage_writes.push_back("x.Store(final.artifact -> " + report.postprocess_record->final_artifact_summary + ")");
    report.storage_writes.push_back("x.Store(retention -> " + report.postprocess_record->retention_decision + ")");
    report.tze_stages.push_back(make_stage_record(
        "x.PostProcessor",
        "Classify the outcome, prune transient chain text, and retain the final artifact summary",
        "SessionCoordinator",
        report.postprocess_record->status,
        "Reduced the run outcome to a compact retained artifact with explicit provenance and retention.",
        {report.answer_status, report.answer_explanation},
        {report.postprocess_record->final_artifact_summary,
         report.postprocess_record->provenance,
         report.postprocess_record->retention_decision},
        source_stage_index));

    report.storage_writes.push_back("x.Store(summary -> " + report.cache.name + ")");
    report.storage_writes.push_back("x.Store(preferences -> xMap_Perm_Prioritys.SearchExtranet)");

    if (source_unit.has_value()) {
        attach_source_backed_mappings(*source_unit, report);
        if (is_legacy_source_path(source_unit->source_name)) {
            report.legacy_source = make_legacy_source_record(*source_unit);
            report.legacy_symbol_coverage = build_legacy_symbol_coverage(*source_unit);
            report.legacy_recovery_status =
                summarize_legacy_recovery(report.legacy_source->source_label, report.legacy_symbol_coverage);
            memory_.remember_legacy_source(memory, *report.legacy_source);
            push_unique(report.memory_writes, memory.paths.legacy_sources_path.string());
            report.storage_writes.push_back("x.Store(legacy.source -> " + report.legacy_source->source_label + ")");
        }
    }
    cache_.destroy(report.cache, routed_profile.persist_on_success);

    push_unique(report.memory_writes, memory.paths.tze_runs_path.string());
    push_unique(report.memory_writes, memory.paths.history_path.string());
    report.tze_stages.push_back(make_stage_record(
        "x.Store",
        "Persist run ledger, history, and updated storage slots",
        "MemoryStore",
        "ok",
        "Persisted the TZE run ledger and interaction history for `" + report.tze_run_id + "`.",
        report.storage_writes,
        report.memory_writes,
        source_stage_index));

    TzeRunRecord run_record;
    run_record.id = report.tze_run_id;
    run_record.timestamp = run_timestamp();
    run_record.intent = report.resolved_intent;
    run_record.prompt = report.raw_prompt;
    run_record.target = !report.resolved_project.empty() ? report.resolved_project : resolution.primary_target;
    run_record.linked_case_id = report.case_record.has_value() ? report.case_record->id : std::string{};
    run_record.status = report.answer_status;
    run_record.source_map_path = report.source_map_path;
    run_record.reasoning_provider = report.reasoning_provider;
    run_record.provider_probe_status = report.provider_probe_report.has_value()
        ? report.provider_probe_report->status
        : std::string{};
    run_record.assist_status = report.assist_status;
    run_record.provider_probe_report = report.provider_probe_report;
    run_record.assist_annotation = report.assist_annotation;
    run_record.command_assist_plan = report.command_assist_plan;
    run_record.tool_assist_plan = report.tool_assist_plan;
    run_record.build_assist_plan = report.build_assist_plan;
    run_record.recipe_authoring_artifact = report.recipe_authoring_artifact;
    run_record.next_step_assist_plan = report.next_step_assist_plan;
    run_record.case_summary_assist_plan = report.case_summary_assist_plan;
    run_record.freeform_assist_answer = report.freeform_assist_answer;
    run_record.recursive_diff_report = report.recursive_diff_report;
    run_record.next_action = report.next_action;
    run_record.produced_artifact = report.produced_artifact;
    if (std::find(report.memory_writes.begin(),
                  report.memory_writes.end(),
                  memory.paths.security_audits_path.string()) != report.memory_writes.end()) {
        run_record.security_audit = report.security;
    }
    run_record.language_resolution = report.language_resolution;
    run_record.uac_state = report.uac_state;
    run_record.postprocess_record = report.postprocess_record;
    run_record.legacy_source = report.legacy_source;
    run_record.legacy_bridge_report = report.legacy_bridge_report;
    run_record.legacy_research_artifacts = report.legacy_research_artifacts;
    run_record.legacy_correlations = report.legacy_correlations;
    run_record.legacy_symbol_coverage = report.legacy_symbol_coverage;
    run_record.legacy_recovery_status = report.legacy_recovery_status;
    run_record.query_session = report.query_session;
    run_record.review_artifact = report.review_artifact;
    run_record.patch_proposal_artifact = report.patch_proposal_artifact;
    run_record.definition_answer = report.definition_answer;
    run_record.neural_math_report = report.neural_math_report;
    run_record.neural_route_report = report.neural_route_report;
    run_record.stages = report.tze_stages;
    memory_.remember_tze_run(memory, run_record);

    memory_.record_interaction(memory, report);
    memory_.persist_snapshot(memory);
    return report;
}

}  // namespace tze
