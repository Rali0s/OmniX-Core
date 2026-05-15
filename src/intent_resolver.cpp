#include "tze/intent_resolver.hpp"
#include "tze/shell_lexicon.hpp"

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

std::optional<int> extract_port_number(std::string_view lowered) {
    const std::size_t port_at = lowered.find("port");
    if (port_at == std::string::npos) {
        return std::nullopt;
    }
    std::size_t cursor = port_at + 4;
    while (cursor < lowered.size() && !std::isdigit(static_cast<unsigned char>(lowered[cursor]))) {
        ++cursor;
    }
    std::string digits;
    while (cursor < lowered.size() && std::isdigit(static_cast<unsigned char>(lowered[cursor]))) {
        digits.push_back(lowered[cursor++]);
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

bool is_system_security_request(std::string_view lowered) {
    return lowered.find("secure my system") != std::string::npos ||
        lowered.find("secure system") != std::string::npos ||
        lowered.find("harden my system") != std::string::npos ||
        lowered.find("harden system") != std::string::npos ||
        lowered.find("inspect my system") != std::string::npos ||
        lowered.find("system posture") != std::string::npos;
}

bool is_greeting_request(std::string_view lowered) {
    return lowered == "hello" || lowered == "hi" || lowered == "hey" ||
        lowered == "hello omnix" || lowered == "hi omnix" || lowered == "hey omnix";
}

bool is_identity_request(std::string_view lowered) {
    return lowered == "who are you" || lowered == "what are you" ||
        lowered == "what is omnix" || lowered == "who is omnix" ||
        lowered == "tell me about yourself";
}

bool is_context_summary_request(std::string_view lowered) {
    return lowered == "what matters" || lowered == "what matters here" ||
        lowered == "what should i care about" || lowered == "tell me what matters";
}

std::string strip_article_prefix(std::string value) {
    if (starts_with(value, "your ")) {
        return trim(value.substr(5));
    }
    if (starts_with(value, "my ")) {
        return trim(value.substr(3));
    }
    if (starts_with(value, "the ")) {
        return trim(value.substr(4));
    }
    if (starts_with(value, "a ")) {
        return trim(value.substr(2));
    }
    if (starts_with(value, "an ")) {
        return trim(value.substr(3));
    }
    return value;
}

std::string strip_lookup_context_suffix(std::string value) {
    const std::string lowered = lowercase(value);
    static const std::vector<std::string> markers = {
        " inside ",
        " located at ",
        " as per ",
        " according to ",
        " from local file",
        " in local file",
        " in your root directory",
        " in the root directory",
        " inside your root directory",
        " inside the root directory",
    };
    for (const std::string& marker : markers) {
        const std::size_t marker_pos = lowered.find(marker);
        if (marker_pos != std::string::npos) {
            return trim(value.substr(0, marker_pos));
        }
    }
    return trim(value);
}

std::string extract_domain_hint(std::string_view lowered) {
    if (contains_word(lowered, "science") || contains_word(lowered, "physics")) {
        return "science";
    }
    if (contains_word(lowered, "computing") || contains_word(lowered, "computer") ||
        contains_word(lowered, "programming") || contains_word(lowered, "technology") ||
        contains_word(lowered, "tech") || contains_word(lowered, "software")) {
        return "technology";
    }
    if (contains_word(lowered, "security")) {
        return "security";
    }
    if (contains_word(lowered, "math") || contains_word(lowered, "mathematics")) {
        return "math";
    }
    return {};
}

std::string strip_domain_qualifier(std::string value) {
    const std::string lowered = lowercase(value);
    static const std::vector<std::string> markers = {
        " in terms of science",
        " in therms of science",
        " in terms of physics",
        " in therms of physics",
        " in terms of technology",
        " in therms of technology",
        " in terms of tech",
        " in therms of tech",
        " in terms of computing",
        " in therms of computing",
        " in terms of computer",
        " in therms of computer",
        " in terms of programming",
        " in therms of programming",
        " in terms of software",
        " in therms of software",
        " in terms of security",
        " in therms of security",
        " in terms of math",
        " in therms of math",
        " in terms of mathematics",
        " in therms of mathematics",
        " when discussing science",
        " when discussing physics",
        " when discussing technology",
        " when discussing tech",
        " when discussing computing",
        " when discussing computer",
        " when discussing programming",
        " when discussing software",
        " when discussing security",
        " when discussing math",
        " when discussing mathematics",
        " when referring to science",
        " when referring to physics",
        " when referring to technology",
        " when referring to tech",
        " when referring to computing",
        " when referring to computer",
        " when referring to programming",
        " when referring to software",
        " when referring to security",
        " when referring to math",
        " when referring to mathematics",
        " when reffering to science",
        " when reffering to physics",
        " when reffering to technology",
        " when reffering to tech",
        " when reffering to computing",
        " when reffering to computer",
        " when reffering to programming",
        " when reffering to software",
        " when reffering to security",
        " when reffering to math",
        " when reffering to mathematics",
    };
    for (const std::string& marker : markers) {
        const std::size_t marker_pos = lowered.find(marker);
        if (marker_pos != std::string::npos) {
            return trim(value.substr(0, marker_pos));
        }
    }
    return trim(value);
}

bool is_runtime_symbol_like(std::string_view lowered_target) {
    return starts_with(lowered_target, "x.") || starts_with(lowered_target, "x::") ||
        lowered_target.find("xprocessingcache") != std::string::npos ||
        lowered_target.find("genx") != std::string::npos ||
        lowered_target.find("xxomni") != std::string::npos ||
        lowered_target.find("uac") != std::string::npos;
}

bool looks_like_plain_concept(std::string_view value) {
    if (value.empty()) {
        return false;
    }
    bool saw_alpha = false;
    for (char c : value) {
        if (std::isalpha(static_cast<unsigned char>(c))) {
            saw_alpha = true;
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c)) || c == ' ' || c == '-' || c == '_') {
            continue;
        }
        return false;
    }
    return saw_alpha;
}

std::optional<std::string> extract_local_concept_retrieval_target(std::string_view normalized_prompt,
                                                                  std::string_view lowered_prompt) {
    static const std::vector<std::string> prefixes = {
        "output your ",
        "output the ",
        "show your ",
        "show me your ",
        "tell me your ",
        "tell me about your ",
        "locate your ",
    };
    for (const std::string& prefix : prefixes) {
        if (!starts_with(lowered_prompt, prefix)) {
            continue;
        }
        std::string target = trim(std::string(normalized_prompt.substr(prefix.size())));
        target = strip_lookup_context_suffix(target);
        target = strip_domain_qualifier(target);
        target = strip_article_prefix(trim(target));
        const std::string lowered_target = lowercase(target);
        if (!target.empty() && looks_like_plain_concept(lowered_target) &&
            !is_runtime_symbol_like(lowered_target)) {
            return target;
        }
    }
    return std::nullopt;
}

}  // namespace

IntentResolution IntentResolver::resolve(std::string_view prompt) const {
    IntentResolution resolution;
    resolution.normalized_prompt = trim(prompt);
    const std::string lowered = lowercase(resolution.normalized_prompt);
    const MemorySnapshot empty_memory{};
    ShellLexicon lexicon;
    const std::optional<ShellLexiconEntry> normalized = lexicon.normalize(resolution.normalized_prompt, empty_memory, false);

    if (lowered.empty()) {
        resolution.intent = RequestIntent::Unknown;
        resolution.confidence = 0.0;
        resolution.suggestions = {"ingest ./log.txt", "analyze case-123", "Build NMAP", "memory history"};
        return resolution;
    }

    if (is_greeting_request(lowered) || is_identity_request(lowered) ||
        (normalized.has_value() &&
         (normalized->category == "greeting" || normalized->category == "omni_identity" ||
          normalized->category == "operator_identity"))) {
        resolution.intent = RequestIntent::Conversation;
        resolution.primary_target = normalized.has_value() ? normalized->canonical : resolution.normalized_prompt;
        resolution.confidence = 0.99;
        return resolution;
    }

    if (is_context_summary_request(lowered) ||
        (normalized.has_value() && normalized->category == "context_summary_request")) {
        resolution.intent = RequestIntent::Conversation;
        resolution.primary_target = "what matters";
        resolution.comparison_rationale =
            "Behavioral comparison selected a contextual-summary route over concept definition or command routing.";
        resolution.confidence = 0.98;
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

    if (starts_with(lowered, "persona mode ") ||
        lowered == "premise mode" ||
        lowered == "cynic mode" ||
        lowered == "professional mode" ||
        lowered == "neutral mode") {
        resolution.intent = RequestIntent::SetPersonaMode;
        resolution.primary_target = starts_with(lowered, "persona mode ")
            ? strip_prefix(resolution.normalized_prompt, resolution.normalized_prompt.substr(0, 13))
            : trim(resolution.normalized_prompt.substr(0, lowered.find(' ')));
        resolution.confidence = 0.98;
        return resolution;
    }

    if (starts_with(lowered, "defend diag ") ||
        lowered.find("show cpu hog") != std::string::npos ||
        lowered.find("cpu diag") != std::string::npos ||
        lowered.find("memory diag") != std::string::npos ||
        lowered.find("show memory") != std::string::npos ||
        lowered.find("inspect pid") != std::string::npos ||
        lowered.find("pid ") != std::string::npos ||
        lowered.find("view logs") != std::string::npos ||
        lowered.find("log view") != std::string::npos ||
        lowered.find("close port") != std::string::npos ||
        lowered.find("how do i close") != std::string::npos) {
        resolution.intent = RequestIntent::DefenseDiagnostic;
        resolution.primary_target = resolution.normalized_prompt;
        if (lowered.find("cpu") != std::string::npos) {
            resolution.memory_view = "cpu";
        } else if (lowered.find("memory") != std::string::npos) {
            resolution.memory_view = "memory";
        } else if (lowered.find("pid") != std::string::npos) {
            resolution.memory_view = "pid";
        } else if (lowered.find("log") != std::string::npos) {
            resolution.memory_view = "logs";
        } else if (lowered.find("port") != std::string::npos) {
            resolution.memory_view = "port";
        } else {
            resolution.memory_view = "summary";
        }
        resolution.confidence = 0.88;
        return resolution;
    }

    if (starts_with(lowered, "tview ") || starts_with(lowered, "tcp view ") ||
        starts_with(lowered, "packet view ") ||
        ((lowered.find("investigate") != std::string::npos ||
          lowered.find("inspect") != std::string::npos ||
          lowered.find("view") != std::string::npos ||
          lowered.find("capture") != std::string::npos) &&
         lowered.find("port") != std::string::npos)) {
        resolution.intent = RequestIntent::PacketCapture;
        if (const std::optional<int> port = extract_port_number(lowered); port.has_value()) {
            resolution.primary_target = std::to_string(*port);
            resolution.memory_view = "live";
        } else if (lowered.find("doctor") != std::string::npos) {
            resolution.primary_target = "doctor";
            resolution.memory_view = "doctor";
        } else {
            resolution.primary_target = resolution.normalized_prompt;
            resolution.memory_view = "live";
        }
        resolution.confidence = 0.9;
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

    if (starts_with(lowered, "recipe author ")) {
        resolution.intent = RequestIntent::AuthorBuildRecipe;
        resolution.primary_target = strip_prefix(resolution.normalized_prompt, resolution.normalized_prompt.substr(0, 14));
        resolution.confidence = 0.99;
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

    if (const std::optional<std::string> concept_target =
            extract_local_concept_retrieval_target(resolution.normalized_prompt, lowered);
        concept_target.has_value()) {
        resolution.intent = RequestIntent::GeneralDefinitionQuery;
        resolution.primary_target = *concept_target;
        resolution.definition_domain_hint = extract_domain_hint(lowered);
        resolution.comparison_rationale =
            "Behavioral comparison selected a local concept retrieval route from an operator-facing retrieval verb.";
        resolution.confidence = 0.9;
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
        resolution.primary_target = strip_prefix(resolution.normalized_prompt, resolution.normalized_prompt.substr(0, 7));
        std::string lowered_target = lowercase(resolution.primary_target);
        if (starts_with(lowered_target, "who is ")) {
            resolution.primary_target = trim(resolution.primary_target.substr(7));
        } else if (starts_with(lowered_target, "what is ")) {
            resolution.primary_target = trim(resolution.primary_target.substr(8));
        }
        resolution.primary_target = strip_domain_qualifier(resolution.primary_target);
        resolution.primary_target = strip_article_prefix(trim(resolution.primary_target));
        lowered_target = lowercase(resolution.primary_target);
        if (!resolution.primary_target.empty() &&
            !is_runtime_symbol_like(lowered_target) &&
            looks_like_plain_concept(lowered_target)) {
            resolution.intent = RequestIntent::GeneralDefinitionQuery;
            resolution.definition_domain_hint = extract_domain_hint(lowered);
            resolution.comparison_rationale =
                "Behavioral comparison selected a concept-definition route from the explicit `define <concept>` phrase.";
            resolution.confidence = 0.94;
        } else {
            resolution.intent = RequestIntent::DefineSymbol;
            resolution.confidence = 0.98;
        }
        return resolution;
    }

    if (starts_with(lowered, "what is ")) {
        resolution.primary_target = strip_prefix(resolution.normalized_prompt, resolution.normalized_prompt.substr(0, 8));
        resolution.definition_domain_hint = extract_domain_hint(lowered);
        resolution.primary_target = strip_domain_qualifier(resolution.primary_target);
        resolution.primary_target = strip_article_prefix(trim(resolution.primary_target));
        const std::string lowered_target = lowercase(resolution.primary_target);
        if (is_runtime_symbol_like(lowered_target)) {
            resolution.intent = RequestIntent::DefineSymbol;
            resolution.confidence = 0.97;
        } else {
            resolution.intent = RequestIntent::GeneralDefinitionQuery;
            resolution.comparison_rationale = resolution.definition_domain_hint.empty()
                ? "Behavioral comparison selected a concept-definition route from the explicit `what is <concept>` phrase."
                : "Behavioral comparison selected a concept-definition route because the prompt uses `what is <concept>` with a domain hint.";
            resolution.confidence = resolution.definition_domain_hint.empty() ? 0.95 : 0.99;
        }
        return resolution;
    }

    if (starts_with(lowered, "who is ")) {
        resolution.primary_target = strip_prefix(resolution.normalized_prompt, resolution.normalized_prompt.substr(0, 7));
        resolution.definition_domain_hint = extract_domain_hint(lowered);
        resolution.primary_target = strip_domain_qualifier(resolution.primary_target);
        resolution.primary_target = strip_article_prefix(trim(resolution.primary_target));
        resolution.intent = RequestIntent::GeneralDefinitionQuery;
        resolution.comparison_rationale = resolution.definition_domain_hint.empty()
            ? "Behavioral comparison selected a concept-definition route from the explicit `who is <person-or-entity>` phrase."
            : "Behavioral comparison selected a concept-definition route because the prompt uses `who is <person-or-entity>` with a domain hint.";
        resolution.confidence = resolution.definition_domain_hint.empty() ? 0.95 : 0.99;
        return resolution;
    }

    if (starts_with(lowered, "explain ")) {
        resolution.intent = RequestIntent::ExplainCommand;
        resolution.primary_target = strip_prefix(resolution.normalized_prompt, resolution.normalized_prompt.substr(0, 8));
        resolution.confidence = 0.92;
        return resolution;
    }

    if (starts_with(lowered, "review ")) {
        resolution.intent = RequestIntent::ReviewModule;
        resolution.primary_target = strip_prefix(resolution.normalized_prompt, resolution.normalized_prompt.substr(0, 7));
        resolution.confidence = 0.96;
        return resolution;
    }

    if (starts_with(lowered, "patch-proposal ")) {
        resolution.intent = RequestIntent::PatchProposal;
        resolution.primary_target = strip_prefix(resolution.normalized_prompt, resolution.normalized_prompt.substr(0, 15));
        resolution.confidence = 0.96;
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
        resolution.intent = RequestIntent::GeneralDefinitionQuery;
        const std::size_t anchor = lowered.find("what does ") != std::string::npos ? lowered.find("what does ") + 10 : lowered.find("meaning of ") + 11;
        resolution.primary_target = strip_article_prefix(trim(resolution.normalized_prompt.substr(anchor)));
        resolution.definition_domain_hint = extract_domain_hint(lowered);
        resolution.comparison_rationale =
            "Behavioral comparison selected a concept-definition route from an explicit meaning/definition phrase.";
        resolution.confidence = 0.9;
        return resolution;
    }

    if (looks_like_plain_concept(lowered) && !is_runtime_symbol_like(lowered) && lowered.find(' ') == std::string::npos) {
        resolution.intent = RequestIntent::GeneralDefinitionQuery;
        resolution.primary_target = resolution.normalized_prompt;
        resolution.comparison_rationale =
            "Prompt is a single plain-language concept and needs clarification between a concept definition and a contextual-summary interpretation.";
        resolution.confidence = 0.56;
        resolution.suggestions = {"What is " + resolution.normalized_prompt + "?", "What matters here?"};
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

    if (lowered.find("create a build recipe for ") != std::string::npos ||
        lowered.find("author a build recipe for ") != std::string::npos ||
        lowered.find("create build recipe for ") != std::string::npos) {
        const std::size_t anchor =
            lowered.find("create a build recipe for ") != std::string::npos
                ? lowered.find("create a build recipe for ") + 26
                : (lowered.find("author a build recipe for ") != std::string::npos
                    ? lowered.find("author a build recipe for ") + 26
                    : lowered.find("create build recipe for ") + 24);
        resolution.intent = RequestIntent::AuthorBuildRecipe;
        resolution.primary_target = trim(resolution.normalized_prompt.substr(anchor));
        resolution.confidence = 0.94;
        return resolution;
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
