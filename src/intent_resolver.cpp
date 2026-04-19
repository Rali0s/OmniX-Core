#include "tze/intent_resolver.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
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

std::string lowercase(std::string_view value) {
    std::string lowered;
    lowered.reserve(value.size());
    for (char c : value) {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return lowered;
}

bool starts_with(std::string_view value, std::string_view prefix) {
    return value.substr(0, prefix.size()) == prefix;
}

std::string strip_prefix(std::string_view value, std::string_view prefix) {
    return trim(value.substr(prefix.size()));
}

bool is_word_boundary(std::string_view value, std::size_t index, std::size_t length) {
    const auto word_char = [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-';
    };
    const bool left_ok = index == 0 || !word_char(value[index - 1]);
    const std::size_t end = index + length;
    const bool right_ok = end >= value.size() || !word_char(value[end]);
    return left_ok && right_ok;
}

bool contains_word(std::string_view value, std::string_view needle) {
    std::size_t offset = 0;
    while (offset < value.size()) {
        const std::size_t found = value.find(needle, offset);
        if (found == std::string::npos) {
            return false;
        }
        if (is_word_boundary(value, found, needle.size())) {
            return true;
        }
        offset = found + needle.size();
    }
    return false;
}

std::optional<std::string> detect_runnable_tool(std::string_view lowered) {
    static const std::vector<std::string> kRunnableTools = {
        "nmap",
        "tshark",
        "wireshark",
        "dumpcap",
        "ssh",
        "grep",
        "sed",
        "awk",
        "ruby",
        "perl",
        "busybox",
    };
    for (const std::string& tool : kRunnableTools) {
        if (contains_word(lowered, tool)) {
            return tool;
        }
    }
    return std::nullopt;
}

bool is_system_security_request(std::string_view lowered) {
    return lowered.find("secure my system") != std::string::npos ||
        lowered.find("secure system") != std::string::npos ||
        lowered.find("harden my system") != std::string::npos ||
        lowered.find("harden system") != std::string::npos ||
        lowered.find("inspect my system") != std::string::npos ||
        lowered.find("system posture") != std::string::npos;
}

}  // namespace

IntentResolution IntentResolver::resolve(std::string_view prompt) const {
    IntentResolution resolution;
    resolution.normalized_prompt = trim(prompt);
    const std::string lowered = lowercase(resolution.normalized_prompt);

    if (lowered.empty()) {
        resolution.intent = RequestIntent::Unknown;
        resolution.confidence = 0.0;
        resolution.suggestions = {"ingest ./log.txt", "analyze case-123", "Build NMAP", "memory history"};
        return resolution;
    }

    if (starts_with(lowered, "ingest ")) {
        resolution.intent = RequestIntent::IngestData;
        resolution.primary_target = strip_prefix(resolution.normalized_prompt, resolution.normalized_prompt.substr(0, 7));
        resolution.confidence = 0.98;
        return resolution;
    }

    if (starts_with(lowered, "analyze ")) {
        resolution.intent = RequestIntent::AnalyzeCase;
        resolution.primary_target = strip_prefix(resolution.normalized_prompt, resolution.normalized_prompt.substr(0, 8));
        resolution.confidence = 0.98;
        return resolution;
    }

    if (starts_with(lowered, "decide ")) {
        if (starts_with(lowered, "decide feedback ")) {
            resolution.intent = RequestIntent::MarkDecisionFeedback;
            const std::string payload = strip_prefix(resolution.normalized_prompt, resolution.normalized_prompt.substr(0, 16));
            const std::size_t first = payload.find(' ');
            if (first != std::string::npos) {
                resolution.primary_target = trim(payload.substr(0, first));
                const std::string rest = trim(payload.substr(first + 1));
                const std::size_t second = rest.find(' ');
                if (second != std::string::npos) {
                    resolution.memory_view = trim(rest.substr(0, second));
                }
            } else {
                resolution.primary_target = payload;
            }
            resolution.confidence = 0.99;
            return resolution;
        }
        if (starts_with(lowered, "decide outcome ")) {
            resolution.intent = RequestIntent::MarkDecisionOutcome;
            const std::string payload = strip_prefix(resolution.normalized_prompt, resolution.normalized_prompt.substr(0, 15));
            const std::size_t first = payload.find(' ');
            if (first != std::string::npos) {
                resolution.primary_target = trim(payload.substr(0, first));
                const std::string rest = trim(payload.substr(first + 1));
                const std::size_t second = rest.find(' ');
                if (second != std::string::npos) {
                    resolution.memory_view = trim(rest.substr(0, second));
                }
            } else {
                resolution.primary_target = payload;
            }
            resolution.confidence = 0.99;
            return resolution;
        }
        resolution.intent = RequestIntent::DecideAction;
        resolution.primary_target = strip_prefix(resolution.normalized_prompt, resolution.normalized_prompt.substr(0, 7));
        resolution.confidence = 0.98;
        return resolution;
    }

    if (starts_with(lowered, "case ")) {
        if (starts_with(lowered, "case timeline ")) {
            resolution.intent = RequestIntent::CaseTimeline;
            resolution.primary_target = strip_prefix(resolution.normalized_prompt, resolution.normalized_prompt.substr(0, 14));
            resolution.confidence = 0.99;
            return resolution;
        }
        if (starts_with(lowered, "case export ")) {
            resolution.intent = RequestIntent::ExportCaseBundle;
            resolution.primary_target = strip_prefix(resolution.normalized_prompt, resolution.normalized_prompt.substr(0, 12));
            resolution.confidence = 0.99;
            return resolution;
        }
        if (starts_with(lowered, "case import ")) {
            resolution.intent = RequestIntent::ImportCaseBundle;
            resolution.primary_target = strip_prefix(resolution.normalized_prompt, resolution.normalized_prompt.substr(0, 12));
            resolution.confidence = 0.99;
            return resolution;
        }
        resolution.intent = RequestIntent::InspectCase;
        resolution.primary_target = strip_prefix(resolution.normalized_prompt, resolution.normalized_prompt.substr(0, 5));
        resolution.confidence = 0.98;
        return resolution;
    }

    if (starts_with(lowered, "tze replay ")) {
        resolution.intent = RequestIntent::ReplayTzeRun;
        resolution.primary_target = strip_prefix(resolution.normalized_prompt, resolution.normalized_prompt.substr(0, 11));
        resolution.confidence = 0.99;
        return resolution;
    }

    if (starts_with(lowered, "tze chain ")) {
        resolution.intent = RequestIntent::ChainTzeRun;
        resolution.primary_target = strip_prefix(resolution.normalized_prompt, resolution.normalized_prompt.substr(0, 10));
        resolution.confidence = 0.99;
        return resolution;
    }

    if (lowered == "tze latest") {
        resolution.intent = RequestIntent::ReplayTzeRun;
        resolution.primary_target = "latest";
        resolution.confidence = 0.99;
        return resolution;
    }

    if (starts_with(lowered, "tze diff ")) {
        resolution.intent = RequestIntent::DiffTzeRuns;
        const std::string payload = strip_prefix(resolution.normalized_prompt, resolution.normalized_prompt.substr(0, 9));
        const std::size_t split = payload.find(' ');
        if (split != std::string::npos) {
            resolution.primary_target = trim(payload.substr(0, split));
            resolution.memory_view = trim(payload.substr(split + 1));
        } else {
            resolution.primary_target = payload;
        }
        resolution.confidence = 0.99;
        return resolution;
    }

    if (lowered == "tze diff-latest") {
        resolution.intent = RequestIntent::DiffTzeRuns;
        resolution.primary_target = "latest";
        resolution.memory_view = "previous";
        resolution.confidence = 0.99;
        return resolution;
    }

    if (starts_with(lowered, "tze explain-change ")) {
        resolution.intent = RequestIntent::ExplainTzeChange;
        const std::string payload = strip_prefix(resolution.normalized_prompt, resolution.normalized_prompt.substr(0, 19));
        const std::size_t split = payload.find(' ');
        if (split != std::string::npos) {
            resolution.primary_target = trim(payload.substr(0, split));
            resolution.memory_view = trim(payload.substr(split + 1));
        } else {
            resolution.primary_target = payload;
        }
        resolution.confidence = 0.99;
        return resolution;
    }

    if (lowered == "tze explain-change-latest") {
        resolution.intent = RequestIntent::ExplainTzeChange;
        resolution.primary_target = "latest";
        resolution.memory_view = "previous";
        resolution.confidence = 0.99;
        return resolution;
    }

    if (starts_with(lowered, "tze report ")) {
        resolution.intent = RequestIntent::ReportTzeRun;
        resolution.primary_target = strip_prefix(resolution.normalized_prompt, resolution.normalized_prompt.substr(0, 11));
        resolution.confidence = 0.99;
        return resolution;
    }

    if (starts_with(lowered, "tze diff-report ")) {
        resolution.intent = RequestIntent::DiffReportTzeRuns;
        const std::string payload = strip_prefix(resolution.normalized_prompt, resolution.normalized_prompt.substr(0, 16));
        const std::size_t split = payload.find(' ');
        if (split != std::string::npos) {
            resolution.primary_target = trim(payload.substr(0, split));
            resolution.memory_view = trim(payload.substr(split + 1));
        } else {
            resolution.primary_target = payload;
        }
        resolution.confidence = 0.99;
        return resolution;
    }

    if (starts_with(lowered, "tze export ")) {
        resolution.intent = RequestIntent::ExportTzeBundle;
        resolution.primary_target = strip_prefix(resolution.normalized_prompt, resolution.normalized_prompt.substr(0, 11));
        resolution.confidence = 0.99;
        return resolution;
    }

    if (starts_with(lowered, "tze import ")) {
        resolution.intent = RequestIntent::ImportTzeBundle;
        resolution.primary_target = strip_prefix(resolution.normalized_prompt, resolution.normalized_prompt.substr(0, 11));
        resolution.confidence = 0.99;
        return resolution;
    }

    if (starts_with(lowered, "tze prune")) {
        resolution.intent = RequestIntent::PruneTzeRuns;
        resolution.confidence = 0.99;
        return resolution;
    }

    if (starts_with(lowered, "tze mark ")) {
        resolution.intent = RequestIntent::MarkTzeRunOutcome;
        const std::string payload = strip_prefix(resolution.normalized_prompt, resolution.normalized_prompt.substr(0, 9));
        const std::size_t split = payload.find(' ');
        if (split != std::string::npos) {
            resolution.primary_target = trim(payload.substr(0, split));
        } else {
            resolution.primary_target = payload;
        }
        resolution.confidence = 0.99;
        return resolution;
    }

    if (starts_with(lowered, "memory prune")) {
        resolution.intent = RequestIntent::PruneMemory;
        resolution.confidence = 0.99;
        return resolution;
    }

    if (lowered == "incident list") {
        resolution.intent = RequestIntent::ListIncidents;
        resolution.confidence = 0.99;
        return resolution;
    }

    if (lowered == "provider probe") {
        resolution.intent = RequestIntent::ProbeProvider;
        resolution.confidence = 0.99;
        return resolution;
    }

    if (starts_with(lowered, "incident report ")) {
        resolution.intent = RequestIntent::ReportIncident;
        resolution.primary_target = strip_prefix(resolution.normalized_prompt, resolution.normalized_prompt.substr(0, 16));
        resolution.confidence = 0.99;
        return resolution;
    }

    if (starts_with(lowered, "incident ")) {
        resolution.intent = RequestIntent::InspectIncident;
        resolution.primary_target = strip_prefix(resolution.normalized_prompt, resolution.normalized_prompt.substr(0, 9));
        resolution.confidence = 0.99;
        return resolution;
    }

    if (starts_with(lowered, "build ")) {
        resolution.intent = RequestIntent::BuildProject;
        resolution.primary_target = strip_prefix(resolution.normalized_prompt, resolution.normalized_prompt.substr(0, 6));
        resolution.confidence = 0.98;
        return resolution;
    }

    if (starts_with(lowered, "doctor ")) {
        resolution.intent = RequestIntent::DoctorProject;
        resolution.primary_target = strip_prefix(resolution.normalized_prompt, resolution.normalized_prompt.substr(0, 7));
        resolution.confidence = 0.98;
        return resolution;
    }

    if (is_system_security_request(lowered)) {
        resolution.intent = RequestIntent::ToolAction;
        resolution.primary_target = "inspect-host";
        resolution.confidence = 0.88;
        return resolution;
    }

    if (starts_with(lowered, "tool ")) {
        const std::string payload = strip_prefix(resolution.normalized_prompt, resolution.normalized_prompt.substr(0, 5));
        if (!payload.empty() && payload != "list" &&
            !starts_with(lowered, "tool locate ") &&
            !starts_with(lowered, "tool doctor ")) {
            resolution.intent = RequestIntent::ToolAction;
            const std::size_t split = payload.find(" -- ");
            resolution.primary_target = trim(split == std::string::npos ? payload : payload.substr(0, split));
            resolution.confidence = 0.96;
            return resolution;
        }
    }

    if (starts_with(lowered, "define ")) {
        resolution.intent = RequestIntent::DefineSymbol;
        resolution.primary_target = strip_prefix(resolution.normalized_prompt, resolution.normalized_prompt.substr(0, 7));
        resolution.confidence = 0.98;
        return resolution;
    }

    if (starts_with(lowered, "what is ")) {
        resolution.intent = RequestIntent::DefineSymbol;
        resolution.primary_target = strip_prefix(resolution.normalized_prompt, resolution.normalized_prompt.substr(0, 8));
        resolution.confidence = 0.95;
        return resolution;
    }

    if (starts_with(lowered, "explain ")) {
        resolution.intent = RequestIntent::ExplainCommand;
        resolution.primary_target = strip_prefix(resolution.normalized_prompt, resolution.normalized_prompt.substr(0, 8));
        resolution.confidence = 0.92;
        return resolution;
    }

    if (lowered == "toolchain" || lowered.find("toolchain") != std::string::npos ||
        lowered.find("compiler") != std::string::npos) {
        resolution.intent = RequestIntent::InspectToolchain;
        resolution.confidence = 0.9;
        return resolution;
    }

    if (starts_with(lowered, "memory")) {
        resolution.intent = RequestIntent::ShowMemory;
        resolution.memory_view = strip_prefix(resolution.normalized_prompt, resolution.normalized_prompt.substr(0, 6));
        if (resolution.memory_view.empty()) {
            resolution.memory_view = "history";
        }
        resolution.confidence = 0.95;
        return resolution;
    }

    if (starts_with(lowered, "show memory")) {
        resolution.intent = RequestIntent::ShowMemory;
        resolution.memory_view = strip_prefix(resolution.normalized_prompt, resolution.normalized_prompt.substr(0, 11));
        if (resolution.memory_view.empty()) {
            resolution.memory_view = "history";
        }
        resolution.confidence = 0.95;
        return resolution;
    }

    if (lowered.find("what does ") != std::string::npos || lowered.find("meaning of ") != std::string::npos) {
        resolution.intent = RequestIntent::DefineSymbol;
        const std::size_t anchor = lowered.find("what does ") != std::string::npos ? lowered.find("what does ") + 10 : lowered.find("meaning of ") + 11;
        resolution.primary_target = trim(resolution.normalized_prompt.substr(anchor));
        resolution.confidence = 0.85;
        return resolution;
    }

    if (const std::optional<std::string> tool = detect_runnable_tool(lowered); tool.has_value()) {
        const bool wants_tool_run = contains_word(lowered, "run") ||
            contains_word(lowered, "use") ||
            contains_word(lowered, "scan") ||
            contains_word(lowered, "execute") ||
            contains_word(lowered, "monitor");
        if (wants_tool_run) {
            resolution.intent = RequestIntent::ToolAction;
            resolution.primary_target = *tool;
            resolution.confidence = 0.84;
            return resolution;
        }
    }

    if (lowered.find("build") != std::string::npos) {
        resolution.intent = RequestIntent::BuildProject;
        const std::size_t build_at = lowered.find("build");
        resolution.primary_target = trim(resolution.normalized_prompt.substr(build_at + 5));
        resolution.confidence = 0.65;
        return resolution;
    }

    if (lowered.find("analy") != std::string::npos) {
        resolution.intent = RequestIntent::AnalyzeCase;
        const std::size_t anchor = lowered.find("analy");
        resolution.primary_target = trim(resolution.normalized_prompt.substr(anchor + 7));
        resolution.confidence = 0.7;
        return resolution;
    }

    if (lowered.find("decid") != std::string::npos) {
        resolution.intent = RequestIntent::DecideAction;
        const std::size_t anchor = lowered.find("decid");
        resolution.primary_target = trim(resolution.normalized_prompt.substr(anchor + 6));
        resolution.confidence = 0.7;
        return resolution;
    }

    if (lowered.find("doctor") != std::string::npos || lowered.find("dependency") != std::string::npos) {
        resolution.intent = RequestIntent::DoctorProject;
        const std::size_t anchor = lowered.find("doctor") != std::string::npos ? lowered.find("doctor") + 6 : 0;
        resolution.primary_target = trim(resolution.normalized_prompt.substr(anchor));
        resolution.confidence = 0.6;
        return resolution;
    }

    if (lowered.find("history") != std::string::npos || lowered.find("prefs") != std::string::npos ||
        lowered.find("definitions") != std::string::npos) {
        resolution.intent = RequestIntent::ShowMemory;
        if (lowered.find("prefs") != std::string::npos || lowered.find("preferences") != std::string::npos) {
            resolution.memory_view = "prefs";
        } else if (lowered.find("definitions") != std::string::npos) {
            resolution.memory_view = "definitions";
        } else {
            resolution.memory_view = "history";
        }
        resolution.confidence = 0.6;
        return resolution;
    }

    resolution.intent = RequestIntent::Unknown;
    resolution.confidence = 0.2;
    resolution.primary_target = resolution.normalized_prompt;
    resolution.suggestions = {"ingest ./log.txt", "analyze ./log.txt", "Build NMAP", "memory history"};
    return resolution;
}

}  // namespace tze
