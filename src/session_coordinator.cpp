#include "tze/session_coordinator.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "tze/query_runtime.hpp"
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

std::string dispatch_module_for(RequestIntent intent) {
    switch (intent) {
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

std::vector<std::string> allowlisted_command_assist_patterns() {
    return {
        "build <project-or-path>",
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
        "provider probe",
        "memory <history|prefs|definitions|language|security|uac|cases|runs|tze>",
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
            lowered.find("subnet") != std::string::npos;
        if (wants_scan) {
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
    } else if (lowered == "provider probe") {
        matched = true;
        assign_simple(RequestIntent::ProbeProvider);
    } else if (starts_with_local(lowered, "memory ")) {
        const std::string view = set_target(7);
        static const std::vector<std::string> views = {
            "history", "prefs", "definitions", "language", "security", "uac", "cases", "runs", "tze"
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

}  // namespace

SessionCoordinator::SessionCoordinator()
    : provider_(make_reasoning_provider_from_env()) {}

ProcessingReport SessionCoordinator::run(const RequestProfile& profile) const {
    ProcessingReport report;
    report.version_string = OMNIX_VERSION;
    report.raw_prompt = profile.raw_prompt;
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
        memory.paths.native_tools_path.string(),
        memory.paths.security_audits_path.string(),
        memory.paths.language_contexts_path.string(),
        memory.paths.uac_states_path.string(),
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
        }
    }

    const bool command_assist_eligible =
        routed_profile.assist_requested &&
        (resolution.intent == RequestIntent::Unknown || resolution.confidence < 0.8) &&
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
        instruction_slot = resolution.intent == RequestIntent::BuildProject ? "aZ::1" : "aZ::99";
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
    report.tze_stages.push_back({
        "x.Assist.Provider",
        "Reasoning provider gate",
        "ReasoningProvider",
        provider_->configured() ? "configured_dormant" : "deterministic_only",
        provider_->configured()
            ? "Provider `" + std::string(provider_->id()) +
                "` is configured for probe-only use; OmniX remains deterministic-only until assist is explicitly enabled."
            : "Provider `" + std::string(provider_->id()) + "` is inactive; OmniX remains deterministic-only.",
        {std::string(provider_->id())},
        {provider_->configured() ? "assist_probe_only" : "assist_bypassed"},
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

    std::string dispatch_module = dispatch_module_for(resolution.intent);

    if (resolution.intent == RequestIntent::ProbeProvider) {
        report.provider_probe_report = provider_->probe();
        report.answer_status = report.provider_probe_report->status == "ready"
            ? "provider_ready"
            : (report.provider_probe_report->status == "inactive" ? "provider_inactive" : "provider_probe_failed");
        report.answer_explanation = report.provider_probe_report->summary;
        if (report.provider_probe_report->status == "ready") {
            report.next_action = "Provider readiness is confirmed. OmniX will still remain deterministic-only until assistive execution is explicitly enabled.";
        } else if (report.provider_probe_report->status == "inactive") {
            report.next_action = "Set `OMNIX_REASONING_PROVIDER=ollama` and rerun `omnix provider probe` when you want to assess a local model.";
        } else if (report.provider_probe_report->status == "config_incomplete") {
            report.next_action = "Set `OMNIX_OLLAMA_MODEL` and rerun `omnix provider probe`.";
        } else if (report.provider_probe_report->status == "unsupported_provider") {
            report.next_action = "Use `OMNIX_REASONING_PROVIDER=ollama` or unset the provider selection.";
        } else if (report.provider_probe_report->status == "invalid_config") {
            report.next_action = "Fix `OMNIX_OLLAMA_BASE_URL` and rerun `omnix provider probe`.";
        } else if (report.provider_probe_report->status == "model_missing") {
            report.next_action = "Pull or load the requested Ollama model, then rerun `omnix provider probe`.";
        } else {
            report.next_action = "Start Ollama locally or correct the provider configuration, then rerun `omnix provider probe`.";
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
                } else if (report.preflight_report->ready) {
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
                        "` was not found and the source-build recipe still has unmet prerequisites.";
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
    } else if (resolution.intent == RequestIntent::ToolAction) {
        tool_flow_.run(routed_profile, memory, report, builder_, tools_, memory_);
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
    } else if (resolution.intent == RequestIntent::ShowMemory) {
        const std::string view = resolution.memory_view.empty() ? "history" : resolution.memory_view;
        report.answer_status = "memory";
        report.answer_explanation = memory_.render_view(memory, view);
        report.next_action = "Run `omnix ask` or `omnix define` to add more learning data.";
    } else if (resolution.intent == RequestIntent::DefineSymbol || resolution.intent == RequestIntent::ExplainCommand) {
        const std::string target = resolution.primary_target.empty() ? routed_profile.raw_prompt : resolution.primary_target;
        definition_flow_.run(routed_profile, target, memory, report, definitions_, memory_);
    } else if (resolution.intent == RequestIntent::BuildProject || routed_profile.execute_build) {
        RequestProfile build_profile = routed_profile;
        build_profile.raw_prompt = routed_profile.raw_prompt;
        build_profile.project_reference = !routed_profile.project_reference.empty()
            ? routed_profile.project_reference
            : resolution.primary_target;
        build_profile.execute_build = true;
        build_flow_.run(build_profile, memory, report, builder_, cache_, knowledge_, memory_, tools_, aliases_, projects_, *provider_, security_);
    } else if (resolution.intent == RequestIntent::Unknown && routed_profile.assist_requested) {
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
                report.answer_status = "unknown_intent";
                report.answer_explanation = answer.summary +
                    "\nGuarded assist rejected the proposed tool plan: " + validation_detail;
                report.next_action = "Try a more specific prompt or run one of the allowlisted tools directly.";
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
            report.answer_status = "unknown_intent";
            report.answer_explanation = answer.summary;
            report.next_action = "Try a more specific prompt or run a tool command explicitly.";
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

    report.tze_stages.push_back({
        "x.Dispatch",
        "Execute the selected runtime flow",
        dispatch_module,
        report.answer_status.empty() ? "ok" : report.answer_status,
        "Delegated execution to `" + dispatch_module + "` for intent `" + report.resolved_intent + "`.",
        {resolution.primary_target, report.resolved_project},
        {report.answer_status, report.next_action},
    });

    report.storage_writes.push_back("x.Store(summary -> " + report.cache.name + ")");
    report.storage_writes.push_back("x.Store(preferences -> xMap_Perm_Prioritys.SearchExtranet)");

    if (source_unit.has_value()) {
        attach_source_backed_mappings(*source_unit, report);
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
    run_record.next_action = report.next_action;
    run_record.produced_artifact = report.produced_artifact;
    if (std::find(report.memory_writes.begin(),
                  report.memory_writes.end(),
                  memory.paths.security_audits_path.string()) != report.memory_writes.end()) {
        run_record.security_audit = report.security;
    }
    run_record.language_resolution = report.language_resolution;
    run_record.uac_state = report.uac_state;
    run_record.query_session = report.query_session;
    run_record.stages = report.tze_stages;
    memory_.remember_tze_run(memory, run_record);

    memory_.record_interaction(memory, report);
    memory_.persist_snapshot(memory);
    return report;
}

}  // namespace tze
