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
        case RequestIntent::NeuralMath:
            return "NeuralMath";
        case RequestIntent::NeuralRoute:
            return "NeuralSignalRouter";
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
            return "defense";
        case RequestIntent::NeuralMath:
            return "neural-math";
        case RequestIntent::NeuralRoute:
            return "neural-route";
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
        case RequestIntent::NeuralMath:
            return "NeuralMathEngine";
        case RequestIntent::NeuralRoute:
            return "NeuralSignalRouter";
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
    if (!routed_profile.memory_view.empty()) {
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
        resolution.intent = RequestIntent::ToolAction;
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
        resolution.intent = RequestIntent::DefenseDiagnostic;
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

    if (resolution.intent == RequestIntent::ToolAction &&
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
    if (provider_->configured()) {
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
    } else if (resolution.intent == RequestIntent::ToolAction) {
        tool_flow_.run(routed_profile, memory, report, builder_, tools_, memory_);
        if (report.tool_invocation_report.has_value() &&
            report.tool_invocation_report->logical_name == "nmap") {
            if (const std::optional<int> open_port = first_open_tcp_port(*report.tool_invocation_report);
                open_port.has_value()) {
                report.next_action = "Investigate the open loopback TCP service with `omnix tview port " +
                    std::to_string(*open_port) + "`.";
            }
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
