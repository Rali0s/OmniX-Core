#include "tze/definition_engine.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include "tze/operand.hpp"
#include "xpp/index.hpp"
#include "xpp/parser.hpp"

namespace tze {
namespace {

std::string lowercase(std::string_view value) {
    std::string lowered;
    lowered.reserve(value.size());
    for (char c : value) {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return lowered;
}

bool contains_case_insensitive(std::string_view haystack, std::string_view needle) {
    return lowercase(haystack).find(lowercase(needle)) != std::string::npos;
}

std::string canonical_tool_name(std::string_view value) {
    std::string lowered = lowercase(value);
    for (char& c : lowered) {
        if (c == '_' || c == ' ') {
            c = '-';
        }
    }
    if (lowered == "pearl") {
        return "perl";
    }
    return lowered;
}

void add_unique(std::vector<std::string>& values, std::string value) {
    if (value.empty()) {
        return;
    }
    if (std::find(values.begin(), values.end(), value) == values.end()) {
        values.push_back(std::move(value));
    }
}

struct ToolDefinition {
    const char* name;
    const char* summary;
    const char* mapped_target;
    const char* family;
};

const std::vector<ToolDefinition>& tool_catalogue() {
    static const std::vector<ToolDefinition> kTools = {
        {"nmap", "Native network mapper. OmniX prefers a verified local nmap binary before falling back to source builds.", "native::nmap", "tool"},
        {"tshark", "Native packet capture and protocol inspection CLI. OmniX can reuse TShark directly before falling back to the managed Wireshark CLI build recipe.", "native::tshark", "tool"},
        {"wireshark", "Native Wireshark GUI/runtime discovery surface. OmniX treats it as a local packet-analysis tool, while the managed build path currently targets the TShark CLI recipe first.", "native::wireshark", "tool"},
        {"dumpcap", "Native packet capture helper commonly paired with TShark and Wireshark for bounded local capture workflows.", "native::dumpcap", "tool"},
        {"grep", "Native line matcher for text and regex search. OmniX can reuse grep directly or through BusyBox applets.", "native::grep", "tool"},
        {"sed", "Native stream editor for text transforms. OmniX can invoke sed directly or via BusyBox applets.", "native::sed", "tool"},
        {"awk", "Native pattern scanning and text processing language. OmniX can invoke awk directly or via BusyBox applets.", "native::awk", "tool"},
        {"ruby", "Native Ruby interpreter available for local scripting workflows.", "native::ruby", "tool"},
        {"perl", "Native Perl interpreter used directly and as a regex-search fallback when grep is unavailable.", "native::perl", "tool"},
        {"busybox", "Compact multi-call binary that can supply applet-backed tools such as grep, sed, and awk.", "native::busybox", "tool"},
        {"ssh", "Native local SSH client discovery and execution surface. This milestone only covers local client reuse, not remote orchestration.", "native::ssh", "tool"},
        {"regex-search", "Virtual OmniX regex search command. It prefers native grep, then BusyBox grep, then Perl.", "omnix::virtual::regex_search", "tool"},
        {"deep-grep", "Virtual OmniX recursive regex search command. It prefers recursive grep, then BusyBox grep, then a find-plus-grep fallback.", "omnix::virtual::deep_grep", "tool"},
        {"inspect-log", "Built-in OmniX analyst module for deterministic structured log inspection across JSON, build, SSH/auth, and tool output.", "omnix::analyst::inspect_log", "tool"},
        {"inspect-build", "Built-in OmniX analyst module for build-root and build-log inspection, including build-system detection and readiness reporting.", "omnix::analyst::inspect_build", "tool"},
        {"inspect-host", "Built-in OmniX analyst module for Linux-first local host inspection across users, sudoers, package mirrors, logs, lastlog, cron, systemd, initrd, and native tools.", "omnix::analyst::inspect_host", "tool"},
        {"report-case", "Built-in OmniX analyst module that generates a saved local report for a case with evidence, decisions, and related links.", "omnix::analyst::report_case", "tool"},
        {"text-pipeline", "Built-in OmniX analyst module for safe grep/sed/awk pipelines over local files.", "omnix::analyst::text_pipeline", "tool"},
    };
    return kTools;
}

}  // namespace

DefinitionAnswer DefinitionEngine::lookup(std::string_view query,
                                          std::string_view source_map_path,
                                          const MemorySnapshot& memory) const {
    DefinitionAnswer answer;
    answer.query = std::string(query);
    const std::string lowered_query = lowercase(query);
    const std::string normalized_query = xpp::normalize_symbol(query);
    const std::string normalized_tool_query = canonical_tool_name(query);

    for (const StoredDefinition& stored : memory.definitions) {
        if (lowercase(stored.term) == lowered_query || xpp::normalize_symbol(stored.term) == normalized_query) {
            answer.found = true;
            answer.summary = stored.summary;
            answer.mapped_cpp_target = stored.mapped_cpp_target;
            answer.semantic_family = stored.semantic_family;
            answer.sources.push_back("memory");
            break;
        }
    }

    if (!source_map_path.empty()) {
        try {
            const std::string source = xpp::read_text_file(std::string(source_map_path));
            const xpp::MappingUnit unit = xpp::parse_xpp(source, std::string(source_map_path));
            const xpp::SymbolIndex index = xpp::build_symbol_index(unit);
            if (const xpp::SymbolMapping* mapping = xpp::find_mapping(index, query); mapping != nullptr) {
                answer.found = true;
                if (answer.summary.empty()) {
                    answer.summary = mapping->inferred_meaning;
                }
                if (answer.mapped_cpp_target.empty()) {
                    answer.mapped_cpp_target = mapping->mapped_cpp_target;
                }
                if (answer.semantic_family.empty()) {
                    answer.semantic_family = std::string(xpp::to_string(mapping->family));
                }
                add_unique(answer.sources, "source_map");
            } else {
                for (const xpp::SymbolMapping& mapping : index.mappings) {
                    if (contains_case_insensitive(mapping.raw_symbol, query) ||
                        contains_case_insensitive(mapping.normalized_symbol, query)) {
                        add_unique(answer.suggestions, mapping.raw_symbol);
                        if (answer.suggestions.size() >= 6) {
                            break;
                        }
                    }
                }
            }
        } catch (const std::exception&) {
            // Optional source-backed definitions should not block the CLI.
        }
    }

    for (const OperandDefinition& operand : operand_catalogue()) {
        if (lowercase(operand.name) == lowered_query || contains_case_insensitive(operand.name, query)) {
            answer.found = true;
            if (answer.summary.empty()) {
                answer.summary = operand.summary + " " + operand.detailed_context;
            }
            if (answer.semantic_family.empty()) {
                answer.semantic_family = "operand_catalogue";
            }
            add_unique(answer.sources, "operand_catalogue");
        } else if (contains_case_insensitive(operand.name, query) || contains_case_insensitive(operand.summary, query)) {
            add_unique(answer.suggestions, operand.name);
        }
    }

    for (const ToolDefinition& tool : tool_catalogue()) {
        if (canonical_tool_name(tool.name) == normalized_tool_query ||
            contains_case_insensitive(tool.name, query)) {
            answer.found = true;
            if (answer.summary.empty()) {
                answer.summary = tool.summary;
            }
            if (answer.mapped_cpp_target.empty()) {
                answer.mapped_cpp_target = tool.mapped_target;
            }
            if (answer.semantic_family.empty()) {
                answer.semantic_family = tool.family;
            }
            add_unique(answer.sources, "native_tool_catalogue");
        } else if (contains_case_insensitive(tool.name, query) || contains_case_insensitive(tool.summary, query)) {
            add_unique(answer.suggestions, tool.name);
        }
    }

    for (const NativeToolRecord& record : memory.native_tools) {
        if (canonical_tool_name(record.logical_name) != normalized_tool_query) {
            continue;
        }
        answer.found = true;
        if (answer.summary.empty()) {
            answer.summary = "Cached native provider for `" + record.logical_name + "` at " + record.executable_path + ".";
        } else {
            answer.summary += " Preferred native provider: " + record.executable_path + ".";
        }
        if (!record.version_fingerprint.empty()) {
            answer.summary += " Version: " + record.version_fingerprint + ".";
        }
        if (answer.mapped_cpp_target.empty()) {
            answer.mapped_cpp_target = record.provider_type + ":" + record.executable_path;
        }
        if (answer.semantic_family.empty()) {
            answer.semantic_family = "tool";
        }
        add_unique(answer.sources, "native_tool_inventory");
    }

    if (!answer.found && answer.summary.empty()) {
        answer.summary = "No exact definition was found in the current source map, operand catalogue, or local memory.";
    }

    return answer;
}

}  // namespace tze
