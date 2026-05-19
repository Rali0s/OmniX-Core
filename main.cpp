#include "tze/processing_engine.hpp"
#include "tze/instance_identity.hpp"
#include "tze/reasoning_provider.hpp"
#include "tze/shell_lexicon.hpp"
#include "xpp/emitter.hpp"
#include "xpp/index.hpp"
#include "xpp/parser.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(__APPLE__) || defined(__linux__) || defined(__unix__)
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifndef OMNIX_VERSION
#define OMNIX_VERSION "0.1.0-dev"
#endif

namespace {

enum class OutputMode {
    Auto,
    Compact,
    Verbose,
};

struct CommonCliOptions {
    std::string source_map_path;
    std::string memory_root_path;
    std::string build_dir;
    std::string build_target;
    std::string build_type = "Release";
    std::string install_prefix;
    std::string git_ref_override;
    std::string selected_recipe_id;
    std::string language_confirmation = "auto";
    std::string output_path;
    std::string feedback_note;
    std::size_t keep_count = 12;
    tze::AcquisitionPolicy acquisition_policy = tze::AcquisitionPolicy::FetchIfMissing;
    bool clean = false;
    bool perform_install = true;
    bool offline = false;
    bool important_only = false;
    bool assist = false;
    OutputMode output_mode = OutputMode::Auto;
};

std::string replace_all_copy(std::string value, std::string_view needle, std::string_view replacement) {
    std::size_t offset = 0;
    while ((offset = value.find(needle, offset)) != std::string::npos) {
        value.replace(offset, needle.size(), replacement);
        offset += replacement.size();
    }
    return value;
}

std::string lowercase_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
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

std::string render_stage_label(const tze::TzeStageRecord& stage) {
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

std::string json_escape(std::string_view value) {
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

std::string now_timestamp_cli() {
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

std::string read_text_file_local(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("Unable to read file: " + path.string());
    }
    std::ostringstream out;
    out << input.rdbuf();
    return out.str();
}

void write_text_file_local(const std::filesystem::path& path, std::string_view content) {
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path());
    }
    std::ofstream output(path);
    if (!output) {
        throw std::runtime_error("Unable to write file: " + path.string());
    }
    output << content;
}

std::string trim_cli(std::string_view value) {
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

std::filesystem::path default_memory_root_path() {
    const char* home = std::getenv("HOME");
    if (home != nullptr && *home != '\0') {
        return std::filesystem::path(home) / ".omnix";
    }
    return std::filesystem::current_path() / ".omnix";
}

std::filesystem::path memory_root_for(const CommonCliOptions& options) {
    return options.memory_root_path.empty() ? default_memory_root_path()
                                            : std::filesystem::path(options.memory_root_path);
}

std::filesystem::path salt_control_root_for(const CommonCliOptions& options) {
    return memory_root_for(options) / "salt_control";
}

bool command_available(std::string_view name) {
#if defined(__APPLE__) || defined(__linux__) || defined(__unix__)
    const std::string command = "command -v " + std::string(name) + " >/dev/null 2>&1";
    return std::system(command.c_str()) == 0;
#else
    (void)name;
    return false;
#endif
}

std::filesystem::path find_project_root(std::filesystem::path start) {
    if (start.empty()) {
        start = std::filesystem::current_path();
    }

    if (std::filesystem::is_regular_file(start)) {
        start = start.parent_path();
    }

    for (std::filesystem::path cursor = start; !cursor.empty(); cursor = cursor.parent_path()) {
        if (std::filesystem::exists(cursor / "CMakeLists.txt")) {
            return cursor;
        }
        if (cursor == cursor.root_path()) {
            break;
        }
    }

    return {};
}

std::filesystem::path optional_source_file(const std::filesystem::path& anchor = std::filesystem::current_path()) {
    const std::filesystem::path project_root = find_project_root(anchor);
    if (!project_root.empty()) {
        const std::filesystem::path candidate = project_root / "res" / "tze.txt";
        if (std::filesystem::exists(candidate)) {
            return candidate;
        }
    }
    return {};
}

std::filesystem::path default_source_file() {
    const std::filesystem::path candidate = optional_source_file();
    if (!candidate.empty()) {
        return candidate;
    }
    throw std::runtime_error("Unable to locate res/tze.txt from the current working directory.");
}

std::filesystem::path default_legacy_source_file() {
    return "/Volumes/CoE/Tzu.cpp";
}

std::filesystem::path default_emit_dir(const std::filesystem::path& source_file) {
    const std::filesystem::path project_root = find_project_root(source_file);
    if (!project_root.empty()) {
        return project_root / "build" / "generated" / "xpp";
    }
    return std::filesystem::current_path() / "generated" / "xpp";
}

void print_usage() {
    std::cout << "Usage:\n";
    std::cout << "  omnix --version\n";
    std::cout << "  omnix ask <prompt> [--assist] [--compact|--verbose] [--source-map file] [--memory-root dir] [--lang-confirm auto|yes|no] [--build-dir dir] [--target name] [--build-type type] [--install-prefix dir] [--recipe id] [--ref git-ref] [--clean] [--build-only] [--no-install] [--offline] [--local-only]\n";
    std::cout << "  omnix ingest <path|command> [--compact|--verbose] [--memory-root dir] [--source-map file]\n";
    std::cout << "  omnix analyze <case|source> [--compact|--verbose] [--memory-root dir] [--source-map file]\n";
    std::cout << "  omnix decide <case|source> [--compact|--verbose] [--memory-root dir] [--source-map file]\n";
    std::cout << "  omnix defend diag <cpu|memory|logs|pid|port> [target] [--compact|--verbose] [--memory-root dir]\n";
    std::cout << "  omnix defend detect <env|sessions|persistence|packages|services|logs|eventviewer|all> [--since 24h] [--quiet-hours HH:MM-HH:MM] [--admin-user name] [--channels list] [--source file] [--max-lines n] [--out file.json] [--compact|--verbose] [--memory-root dir]\n";
    std::cout << "  omnix decide feedback <case-id> <decision-id> <helpful|not-helpful> [--note text] [--memory-root dir]\n";
    std::cout << "  omnix decide outcome <case-id> <decision-id> <success|failed|partial> [--note text] [--memory-root dir]\n";
    std::cout << "  omnix case <id|source> [--assist] [--compact|--verbose] [--memory-root dir] [--source-map file]\n";
    std::cout << "  omnix case list [--compact|--verbose] [--memory-root dir] [--source-map file]\n";
    std::cout << "  omnix case search <term> [--compact|--verbose] [--memory-root dir] [--source-map file]\n";
    std::cout << "  omnix case timeline <id|source> [--compact|--verbose] [--memory-root dir]\n";
    std::cout << "  omnix case export <id|source> [--out file] [--memory-root dir]\n";
    std::cout << "  omnix case import <bundle.json> [--memory-root dir]\n";
    std::cout << "  omnix incident list [--compact|--verbose] [--memory-root dir]\n";
    std::cout << "  omnix incident <id> [--assist] [--compact|--verbose] [--memory-root dir]\n";
    std::cout << "  omnix incident report <id> [--assist] [--compact|--verbose] [--out file] [--memory-root dir]\n";
    std::cout << "  omnix define <symbol-or-term> [file] [--compact|--verbose] [--memory-root dir] [--lang-confirm auto|yes|no]\n";
    std::cout << "  omnix explain <command-or-symbol> [file] [--compact|--verbose] [--memory-root dir] [--lang-confirm auto|yes|no]\n";
    std::cout << "  omnix review <path-or-module> [--assist] [--compact|--verbose] [--memory-root dir]\n";
    std::cout << "  omnix patch-proposal <path-or-module> [--assist] [--compact|--verbose] [--memory-root dir]\n";
    std::cout << "  omnix build <project-or-path> [--assist] [--compact|--verbose] [--source-map file] [--memory-root dir] [--build-dir dir] [--target name] [--build-type type] [--install-prefix dir] [--recipe id] [--ref git-ref] [--clean] [--build-only] [--no-install] [--offline] [--local-only]\n";
    std::cout << "  omnix recipe author <source-path> [--assist] [--compact|--verbose] [--source-map file] [--memory-root dir] [--build-dir dir] [--target name] [--build-type type] [--install-prefix dir] [--clean] [--build-only] [--no-install]\n";
    std::cout << "  omnix preflight <project-or-path> [--assist] [--compact|--verbose] [--memory-root dir] [--target name] [--install-prefix dir] [--recipe id] [--ref git-ref] [--offline] [--local-only]\n";
    std::cout << "  omnix doctor <project-or-path> [--compact|--verbose] [--memory-root dir] [--recipe id] [--offline] [--local-only]\n";
    std::cout << "  omnix provider probe [--compact|--verbose] [--memory-root dir]\n";
    std::cout << "  omnix id [--compact|--verbose] [--memory-root dir]\n";
    std::cout << "  omnix api status|doctor|configure <openai|ollama>|template <openai|ollama|huggingface> [--compact|--verbose]\n";
    std::cout << "  omnix jinja inspect|render|plan|execute <file.j2> [--vars vars.json] [--out file] [--confirm] [--compact|--verbose]\n";
    std::cout << "  omnix node doctor|id|status|heartbeat|enroll [--out file.json] [--compact|--verbose] [--memory-root dir]\n";
    std::cout << "  omnix master doctor|init|node list|node approve <fingerprint>|job plan <type>|job dispatch|job status [--target node] [--out file.json] [--compact|--verbose] [--memory-root dir]\n";
    std::cout << "  omnix persona mode <premise|cynic|professional|neutral> [--compact|--verbose] [--memory-root dir]\n";
    std::cout << "  omnix why [latest|run-id] [--compact|--verbose] [--memory-root dir]\n";
    std::cout << "  omnix next [latest|run-id] [--compact|--verbose] [--memory-root dir]\n";
    std::cout << "  omnix context reset [--compact|--verbose] [--memory-root dir]\n";
    std::cout << "  omnix link install|remove|doctor [--with-tze] [--with-gg] [--prefix dir] [--force]\n";
    std::cout << "  omnix vg doctor|shape|explain|correlate|compare|cab [artifact.json] [--learn-shape] [--dependency-map file] [--out report.json] [--compact|--verbose] [--memory-root dir]\n";
    std::cout << "  omnix gg doctor|search|run|audit|actions [args...] [--compact|--verbose] [--memory-root dir]\n";
    std::cout << "  omnix tensor doctor|inspect|validate|run mlp|ask [args...] [--compact|--verbose] [--memory-root dir]\n";
    std::cout << "  omnix nn math perceptron --dataset or|and|xor [--epochs n] [--learning-rate r] [--compact|--verbose] [--memory-root dir]\n";
    std::cout << "  omnix nn route tview <file.jsonl> [--out routes.jsonl] [--compact|--verbose] [--memory-root dir]\n";
    std::cout << "  omnix tview port <port> [--interface name] [--count n] [--seconds n] [--payload-bytes n] [--out file.jsonl] [--compact|--verbose] [--memory-root dir]\n";
    std::cout << "  omnix tview pcap <file> [--port n] [--payload-bytes n] [--out file.jsonl] [--compact|--verbose] [--memory-root dir]\n";
    std::cout << "  omnix tview doctor [--compact|--verbose] [--memory-root dir]\n";
    std::cout << "  omnix shell [--source-map file] [--memory-root dir] [--assist] [--compact|--verbose]\n";
    std::cout << "  omnix memory [history|prefs|definitions|language|security|uac|cases|runs|tze|legacy|persona|operator|assist] [--compact|--verbose] [--memory-root dir]\n";
    std::cout << "  omnix memory reset-context|prune-expired [--compact|--verbose] [--memory-root dir]\n";
    std::cout << "  omnix memory prune [--keep n] [--important-only] [--memory-root dir]\n";
    std::cout << "  omnix tze runs [--compact|--verbose] [--memory-root dir]\n";
    std::cout << "  omnix tze latest [--compact|--verbose] [--memory-root dir]\n";
    std::cout << "  omnix tze replay <run-id> [--compact|--verbose] [--memory-root dir]\n";
    std::cout << "  omnix tze chain <run-id|latest> [--compact|--verbose] [--memory-root dir]\n";
    std::cout << "  omnix tze diff <left-run-id> <right-run-id> [--compact|--verbose] [--memory-root dir]\n";
    std::cout << "  omnix tze diff-latest [--compact|--verbose] [--important-only] [--memory-root dir]\n";
    std::cout << "  omnix tze explain-change <left-run-id> <right-run-id> [--assist] [--compact|--verbose] [--memory-root dir]\n";
    std::cout << "  omnix tze explain-change-latest [--assist] [--compact|--verbose] [--important-only] [--memory-root dir]\n";
    std::cout << "  omnix tze report <run-id> [--assist] [--compact|--verbose] [--out file] [--memory-root dir]\n";
    std::cout << "  omnix tze diff-report <left-run-id> <right-run-id> [--out file] [--memory-root dir]\n";
    std::cout << "  omnix tze export <run-id|latest> [--out file] [--memory-root dir]\n";
    std::cout << "  omnix tze import <bundle.json> [--memory-root dir]\n";
    std::cout << "  omnix tze prune [--keep n] [--important-only] [--memory-root dir]\n";
    std::cout << "  omnix tze mark <run-id> <helpful|not-helpful> [--note text] [--memory-root dir]\n";
    std::cout << "  omnix tool list [--compact|--verbose] [--memory-root dir]\n";
    std::cout << "  omnix tool locate <name> [--compact|--verbose] [--memory-root dir]\n";
    std::cout << "  omnix tool doctor <name> [--compact|--verbose] [--memory-root dir]\n";
    std::cout << "  omnix tool <name> -- <args...> [--compact|--verbose] [--memory-root dir]\n";
    std::cout << "    Built-in analyst modules: inspect-log, inspect-build, inspect-host, report-case, text-pipeline, mlp-lens, tensor, thresholds, symlink, gg\n";
    std::cout << "  omnix legacy coverage [file]\n";
    std::cout << "  omnix legacy report [file]\n";
    std::cout << "  omnix map <file>\n";
    std::cout << "  omnix search <symbol> [file]\n";
    std::cout << "  omnix emit-cpp <file> [output-dir]\n";
    std::cout << "  omnix build-cmake [source-dir] [--target name] [--clean] [--build-dir dir] [--build-type type]\n";
}

std::string legacy_status_for(const xpp::SymbolMapping& mapping) {
    if (mapping.raw_symbol == "xProcessingCache" ||
        mapping.raw_symbol == "x.Define.Low" ||
        mapping.raw_symbol == "x.DisplayPriorityProcessingGate" ||
        mapping.raw_symbol == "x.DisplayFeedBackLoop" ||
        mapping.raw_symbol == "x.Store") {
        return "implemented";
    }
    if (mapping.raw_symbol.find("xXOmni") != std::string::npos ||
        mapping.mapped_cpp_target.find("LanguageEngine::") != std::string::npos ||
        mapping.mapped_cpp_target.find("PreprocessorRuntime::") != std::string::npos ||
        mapping.mapped_cpp_target.find("SecurityManager::") != std::string::npos ||
        mapping.mapped_cpp_target.find("OmniBridge::") != std::string::npos) {
        if (mapping.inferred_meaning.find("blocked") != std::string::npos) {
            return "research-only";
        }
        return "partial";
    }
    if (mapping.mapped_cpp_target.empty() || mapping.family == xpp::SemanticFamily::Unknown) {
        return "missing";
    }
    return "partial";
}

std::vector<tze::LegacySymbolCoverage> legacy_coverages_for(const xpp::MappingUnit& unit) {
    const xpp::SymbolIndex index = xpp::build_symbol_index(unit);
    std::vector<tze::LegacySymbolCoverage> coverages;
    coverages.reserve(index.mappings.size());
    for (const xpp::SymbolMapping& mapping : index.mappings) {
        tze::LegacySymbolCoverage coverage;
        coverage.symbol = mapping.raw_symbol;
        coverage.semantic_family = xpp::to_string(mapping.family);
        coverage.recovery_status = legacy_status_for(mapping);
        coverage.mapped_cpp_target = mapping.mapped_cpp_target;
        coverage.notes.push_back(mapping.inferred_meaning);
        if (!mapping.occurrences.empty()) {
            const xpp::SymbolOccurrence& first = mapping.occurrences.front();
            coverage.section_title = first.section_title;
            coverage.source_origin = first.section_title + ":" + std::to_string(first.line);
            coverage.occurrence_count = mapping.occurrences.size();
        }
        coverages.push_back(std::move(coverage));
    }
    std::stable_sort(coverages.begin(), coverages.end(), [](const tze::LegacySymbolCoverage& lhs,
                                                            const tze::LegacySymbolCoverage& rhs) {
        if (lhs.recovery_status != rhs.recovery_status) {
            return lhs.recovery_status < rhs.recovery_status;
        }
        return lhs.symbol < rhs.symbol;
    });
    return coverages;
}

tze::LegacyRecoveryStatus summarize_legacy_recovery(const std::filesystem::path& source_file,
                                                    const std::vector<tze::LegacySymbolCoverage>& coverages) {
    tze::LegacyRecoveryStatus status;
    status.source_label = source_file.filename().string();
    for (const tze::LegacySymbolCoverage& coverage : coverages) {
        if (coverage.recovery_status == "implemented") {
            ++status.implemented_count;
        } else if (coverage.recovery_status == "partial") {
            ++status.partial_count;
        } else if (coverage.recovery_status == "research-only") {
            ++status.research_only_count;
        } else {
            ++status.missing_count;
        }
    }
    status.summary_lines = {
        "Structured merge status for " + status.source_label + ": " +
        std::to_string(status.implemented_count) + " implemented, " +
        std::to_string(status.partial_count) + " partial, " +
        std::to_string(status.missing_count) + " missing/research.",
        "Core Spine Reconciliation",
        "Deep Language and Decompression Recovery",
        "xXOmni Bridge Recovery",
        "Full uAC Recovery Model",
        "Legacy Research Security Track",
        "Conformance and Archaeology Reports",
    };
    return status;
}

void run_legacy_coverage(const std::filesystem::path& source_file) {
    const std::string source = xpp::read_text_file(source_file);
    const xpp::MappingUnit unit = xpp::parse_xpp(source, source_file.string());
    const std::vector<tze::LegacySymbolCoverage> coverages = legacy_coverages_for(unit);
    const tze::LegacyRecoveryStatus summary = summarize_legacy_recovery(source_file, coverages);

    std::cout << "Legacy source: " << source_file << "\n";
    if (!summary.summary_lines.empty()) {
        std::cout << summary.summary_lines.front() << "\n";
    }
    for (std::size_t i = 1; i < summary.summary_lines.size(); ++i) {
        const std::string& track = summary.summary_lines[i];
        std::cout << " - track: " << track << "\n";
    }
    const std::size_t preview = std::min<std::size_t>(coverages.size(), 24);
    for (std::size_t i = 0; i < preview; ++i) {
        const auto& coverage = coverages[i];
        std::cout << " - " << coverage.symbol << " [" << coverage.recovery_status << "]";
        if (!coverage.mapped_cpp_target.empty()) {
            std::cout << " -> " << coverage.mapped_cpp_target;
        }
        if (!coverage.source_origin.empty()) {
            std::cout << " @ " << coverage.source_origin;
        }
        std::cout << "\n";
    }
}

void run_legacy_report(const std::filesystem::path& source_file) {
    const std::string source = xpp::read_text_file(source_file);
    const xpp::MappingUnit unit = xpp::parse_xpp(source, source_file.string());
    const std::vector<tze::LegacySymbolCoverage> coverages = legacy_coverages_for(unit);
    const tze::LegacyRecoveryStatus summary = summarize_legacy_recovery(source_file, coverages);

    std::cout << "Legacy archaeology report\n";
    std::cout << "Source: " << source_file << "\n";
    if (!summary.summary_lines.empty()) {
        std::cout << summary.summary_lines.front() << "\n";
    }
    std::cout << "Tracks:\n";
    for (std::size_t i = 1; i < summary.summary_lines.size(); ++i) {
        const std::string& track = summary.summary_lines[i];
        std::cout << " - " << track << "\n";
    }
    std::cout << "Key recovered branches:\n";
    std::cout << " - build spine: implemented/merged\n";
    std::cout << " - language/decompression ladder: partial\n";
    std::cout << " - xXOmni bridge/correlation: partial\n";
    std::cout << " - uAC recovery semantics: partial\n";
    std::cout << " - risky security semantics: research-only\n";
}

void run_map(const std::filesystem::path& source_file) {
    const std::string source = xpp::read_text_file(source_file);
    const xpp::MappingUnit unit = xpp::parse_xpp(source, source_file.string());
    const xpp::SymbolIndex index = xpp::build_symbol_index(unit);

    std::cout << "Source: " << source_file << "\n";
    std::cout << "Dialect: " << xpp::to_string(unit.dialect) << "\n";
    std::cout << "Lines: " << unit.lines.size() << "\n";
    std::cout << "Sections: " << unit.sections.size() << "\n";
    for (const xpp::SectionNode& section : unit.sections) {
        std::cout << " - " << section.title << " (lines " << section.line_start << "-" << section.line_end
                  << ", nodes " << section.nodes.size() << ")\n";
    }
    std::cout << "Indexed symbols: " << index.mappings.size() << "\n";
}

void run_search(const std::string& query, const std::filesystem::path& source_file) {
    const std::string source = xpp::read_text_file(source_file);
    const xpp::MappingUnit unit = xpp::parse_xpp(source, source_file.string());
    const xpp::SymbolIndex index = xpp::build_symbol_index(unit);
    const xpp::SymbolMapping* mapping = xpp::find_mapping(index, query);

    if (mapping == nullptr) {
        throw std::runtime_error("Symbol not found: " + query);
    }

    std::cout << "Source: " << source_file << "\n";
    std::cout << "Symbol: " << mapping->raw_symbol << "\n";
    std::cout << "Normalized: " << mapping->normalized_symbol << "\n";
    std::cout << "Status: " << xpp::to_string(mapping->status) << "\n";
    std::cout << "Family: " << xpp::to_string(mapping->family) << "\n";
    std::cout << "Meaning: " << mapping->inferred_meaning << "\n";
    std::cout << "Mapped C++ target: " << mapping->mapped_cpp_target << "\n";
    std::cout << "Occurrences: " << mapping->occurrences.size() << "\n";

    const std::size_t preview = std::min<std::size_t>(mapping->occurrences.size(), 8);
    for (std::size_t index_value = 0; index_value < preview; ++index_value) {
        const xpp::SymbolOccurrence& occurrence = mapping->occurrences[index_value];
        std::cout << " - " << occurrence.section_title << " line " << occurrence.line
                  << " (" << xpp::to_string(occurrence.node_kind) << ")\n";
    }
}

void run_emit(const std::filesystem::path& source_file, const std::filesystem::path& output_dir) {
    const std::string source = xpp::read_text_file(source_file);
    const xpp::MappingUnit unit = xpp::parse_xpp(source, source_file.string());
    const xpp::SymbolIndex index = xpp::build_symbol_index(unit);
    const xpp::EmitReport report = xpp::emit_cpp(unit, index, {output_dir, "generated::xpp", true});

    std::cout << "Source: " << source_file << "\n";
    std::cout << "Output: " << output_dir << "\n";
    for (const xpp::EmitArtifact& artifact : report.artifacts) {
        std::cout << " - " << artifact.section_title << " -> " << artifact.path;
        if (artifact.contains_unsupported) {
            std::cout << " [contains inert unsupported stubs]";
        }
        std::cout << "\n";
    }
    std::cout << "Manifest: " << report.manifest_path << "\n";
}

void print_toolchain(const std::vector<tze::ToolchainModuleStatus>& modules) {
    std::cout << "Toolchain:\n";
    for (const tze::ToolchainModuleStatus& module : modules) {
        std::cout << " - " << module.name << ": " << (module.available ? "available" : "missing");
        if (!module.version.empty()) {
            std::cout << " (" << module.version << ")";
        }
        std::cout << "\n";
    }
}

bool stdout_is_tty() {
#if defined(__APPLE__) || defined(__linux__) || defined(__unix__)
    return ::isatty(fileno(stdout)) != 0;
#else
    return false;
#endif
}

std::string first_line(std::string_view value) {
    const std::size_t newline = value.find('\n');
    if (newline == std::string::npos) {
        return std::string(value);
    }
    return std::string(value.substr(0, newline));
}

std::string compact_summary(const tze::ProcessingReport& report) {
    if (report.provider_probe_report.has_value() && !report.provider_probe_report->summary.empty()) {
        return report.provider_probe_report->summary;
    }
    if (report.build_execution.has_value() && !report.build_execution->summary.empty()) {
        return report.build_execution->summary;
    }
    if (report.preflight_report.has_value() && !report.preflight_report->summary.empty()) {
        return report.preflight_report->summary;
    }
    if (report.doctor_report.has_value() && !report.doctor_report->summary.empty()) {
        return report.doctor_report->summary;
    }
    if (report.tool_invocation_report.has_value() && !report.tool_invocation_report->summary.empty()) {
        return report.tool_invocation_report->summary;
    }
    if (report.tool_doctor_report.has_value() && !report.tool_doctor_report->summary.empty()) {
        return report.tool_doctor_report->summary;
    }
    if (report.tool_resolution.has_value() && !report.tool_resolution->summary.empty()) {
        return report.tool_resolution->summary;
    }
    if (report.freeform_assist_answer.has_value() && !report.freeform_assist_answer->answer.empty()) {
        return report.freeform_assist_answer->answer;
    }
    if (report.neural_math_report.has_value() && !report.neural_math_report->summary.empty()) {
        return report.neural_math_report->summary;
    }
    if (report.neural_route_report.has_value() && !report.neural_route_report->summary.empty()) {
        return report.neural_route_report->summary;
    }
    if (report.vuplus_gate_report.has_value() && !report.vuplus_gate_report->why.empty()) {
        return report.vuplus_gate_report->why;
    }
    if (report.recursive_diff_report.has_value() && !report.recursive_diff_report->best_estimate_answer.empty()) {
        return report.recursive_diff_report->best_estimate_answer;
    }
    if (report.definition_answer.has_value() && !report.definition_answer->summary.empty()) {
        return report.definition_answer->summary;
    }
    if (!report.answer_explanation.empty()) {
        return first_line(report.answer_explanation);
    }
    return {};
}

bool should_show_next_action_in_compact(const tze::ProcessingReport& report) {
    if (report.answer_status.empty()) {
        return false;
    }
    return report.answer_status.find("failed") != std::string::npos ||
        report.answer_status.find("missing") != std::string::npos ||
        report.answer_status.find("attention") != std::string::npos ||
        report.answer_status == "build_ready" ||
        report.answer_status == "clarify_needed" ||
        report.answer_status == "doctor_ready" ||
        report.answer_status == "provider_inactive" ||
        report.answer_status == "provider_probe_failed" ||
        report.answer_status == "unknown_intent";
}

bool use_verbose_output(OutputMode mode, bool prefer_verbose) {
    if (mode == OutputMode::Verbose) {
        return true;
    }
    if (mode == OutputMode::Compact) {
        return false;
    }
    if (prefer_verbose) {
        return true;
    }
    return !stdout_is_tty();
}

void print_processing_report_compact(const tze::ProcessingReport& report) {
    if (report.tool_invocation_report.has_value() &&
        (report.tool_invocation_report->logical_name == "mlp-lens" ||
         report.tool_invocation_report->logical_name == "tensor") &&
        !report.tool_invocation_report->output_excerpt.empty()) {
        std::cout << report.tool_invocation_report->output_excerpt.front() << "\n";
        return;
    }
    if (!report.answer_status.empty()) {
        std::cout << report.answer_status;
        const std::string summary = compact_summary(report);
        if (!summary.empty()) {
            std::cout << ": " << summary;
        }
        std::cout << "\n";
    } else if (!compact_summary(report).empty()) {
        std::cout << compact_summary(report) << "\n";
    }

    if (!report.resolved_project.empty()) {
        std::cout << "project: " << report.resolved_project << "\n";
    }
    if (report.answer_status == "tool_inventory" && !report.answer_explanation.empty()) {
        std::istringstream lines(report.answer_explanation);
        std::string line;
        std::size_t emitted = 0;
        bool skipped_title = false;
        while (std::getline(lines, line)) {
            if (!skipped_title) {
                skipped_title = true;
                continue;
            }
            if (line.empty()) {
                continue;
            }
            std::cout << line << "\n";
            if (++emitted >= 18) {
                std::cout << " - ...\n";
                break;
            }
        }
    }
    if (report.case_record.has_value()) {
        std::cout << "case: " << report.case_record->id << "\n";
    }
    if (report.command_assist_plan.has_value() && report.assist_status == "assist_used") {
        std::cout << "assist command: " << report.command_assist_plan->canonical_command << "\n";
    }
    if (report.tool_assist_plan.has_value() && report.assist_status == "assist_used") {
        std::cout << "assist tool: " << report.tool_assist_plan->tool_name << "\n";
    }
    if (report.build_assist_plan.has_value() && report.assist_status == "assist_used") {
        std::cout << "assist recipe: " << report.build_assist_plan->selected_recipe_id << "\n";
    }
    if (report.recipe_authoring_artifact.has_value()) {
        std::cout << "authored recipe: " << report.recipe_authoring_artifact->generated_recipe_id << "\n";
    }
    if (report.next_step_assist_plan.has_value() && report.assist_status == "assist_used") {
        std::cout << "assist next: " << report.next_step_assist_plan->suggested_next_step << "\n";
    }
    if (report.case_summary_assist_plan.has_value() && report.assist_status == "assist_used") {
        std::cout << "assist summary: " << report.case_summary_assist_plan->executive_summary << "\n";
    }
    if (report.definition_answer.has_value() &&
        report.definition_answer->selected_source_type == "recursive_route_learning") {
        std::cout << "definition-source: recursive_route_learning";
        if (!report.definition_answer->domain_hint.empty()) {
            std::cout << " domain=" << report.definition_answer->domain_hint;
        }
        if (report.definition_answer->confidence > 0.0) {
            std::cout << " confidence=" << report.definition_answer->confidence;
        }
        std::cout << "\n";
    }
    if (report.preflight_report.has_value() && !report.preflight_report->recipe_id.empty()) {
        std::cout << "recipe: " << report.preflight_report->recipe_id << "\n";
    } else if (report.build_execution.has_value() && !report.build_execution->selected_recipe_id.empty()) {
        std::cout << "recipe: " << report.build_execution->selected_recipe_id << "\n";
    }
    if (report.tool_resolution.has_value() && report.tool_resolution->found &&
        !report.tool_resolution->executable_path.empty()) {
        std::cout << "provider: " << report.tool_resolution->executable_path << "\n";
    }
    if (report.tool_invocation_report.has_value()) {
        const tze::ToolInvocationReport& invocation = *report.tool_invocation_report;
        if (!invocation.command_line.empty()) {
            std::cout << "command: " << invocation.command_line << "\n";
        }
        if (!invocation.output_excerpt.empty()) {
            std::cout << "result: " << invocation.output_excerpt.front() << "\n";
        }
    }
    if (report.packet_capture_report.has_value()) {
        const tze::PacketCaptureReport& capture = *report.packet_capture_report;
        std::cout << "packets: " << capture.packet_count << "\n";
        if (!capture.filter.empty()) {
            std::cout << "filter: " << capture.filter << "\n";
        }
        if (!capture.packets.empty()) {
            const tze::PacketRecord& packet = capture.packets.front();
            std::cout << "result: " << packet.src_ip << ":" << packet.src_port
                      << " -> " << packet.dst_ip << ":" << packet.dst_port
                      << " flags=" << packet.tcp_flags
                      << " payload=" << packet.payload_length
                      << " code=" << packet.analysis_code << "\n";
        }
    }
    if (report.defense_diagnostic_report.has_value()) {
        const tze::DefenseDiagnosticReport& defense = *report.defense_diagnostic_report;
        if (!defense.evidence_lines.empty()) {
            std::cout << "result: " << defense.evidence_lines.front() << "\n";
        }
        if (!defense.proposed_actions.empty()) {
            std::cout << "next-defense: " << defense.proposed_actions.front() << "\n";
        }
    }
    if (report.defense_detection_report.has_value()) {
        const tze::DefenseDetectionReport& detection = *report.defense_detection_report;
        std::cout << "signals: " << detection.signals.size() << "\n";
        if (!detection.signals.empty()) {
            const tze::DefenseDetectionSignal& top = detection.signals.front();
            std::cout << "top-signal: " << top.category << "/" << top.id
                      << " severity=" << top.severity
                      << " confidence=" << top.confidence << "\n";
            if (!top.recommended_next_action.empty()) {
                std::cout << "next-defense: " << top.recommended_next_action << "\n";
            }
        } else if (!detection.proposed_actions.empty()) {
            std::cout << "next-defense: " << detection.proposed_actions.front() << "\n";
        }
        if (!detection.artifact_path.empty()) {
            std::cout << "evidence: " << detection.artifact_path << "\n";
        }
        if (!detection.event_viewer_retention.empty()) {
            for (const tze::EventViewerRetention& retention : detection.event_viewer_retention) {
                std::cout << "eventviewer: " << retention.channel
                          << " maxSizeBytes=" << retention.max_size_bytes
                          << " belowMinimum=" << (retention.below_minimum ? "true" : "false") << "\n";
            }
        }
        if (!detection.session_correlations.empty()) {
            std::cout << "session-correlations: " << detection.session_correlations.size() << "\n";
        }
        if (detection.alarm_cab.has_value()) {
            std::cout << "alarm-cab: " << detection.alarm_cab->alarm_id
                      << " status=" << detection.alarm_cab->recommendation_status << "\n";
        }
    }
    if (report.vuplus_gate_report.has_value()) {
        const tze::VuplusGateReport& vg = *report.vuplus_gate_report;
        std::cout << "segment: " << vg.segment << "\n";
        std::cout << "mode: " << vg.mode << "\n";
        std::cout << "why: " << vg.why << "\n";
        std::cout << "signals: " << vg.signals.size() << "\n";
        for (const std::string& signal : vg.signals) {
            std::cout << "signal: " << signal << "\n";
        }
        if (!vg.key_pairs.empty()) {
            std::cout << "key-pairs: " << vg.key_pairs.size() << "\n";
            const std::size_t limit = std::min<std::size_t>(vg.key_pairs.size(), 24);
            for (std::size_t index = 0; index < limit; ++index) {
                const tze::VuplusGateReport::KeyPair& pair = vg.key_pairs[index];
                std::cout << "key-pair: " << pair.key << "=" << pair.value
                          << " source=" << pair.source
                          << " value-range=" << pair.value_start << ".." << pair.value_end << "\n";
            }
            if (vg.key_pairs.size() > limit) {
                std::cout << "key-pair: ...\n";
            }
        }
        std::cout << "confidence: " << vg.confidence << "\n";
        std::cout << "historical-correlation: " << vg.historical_correlation << "\n";
        std::cout << "operational-blast-radius: " << vg.operational_blast_radius << "\n";
        std::cout << "rollback-impact: " << vg.rollback_impact << "\n";
        std::cout << "remediation-mode: " << vg.remediation_mode << "\n";
        std::cout << "execution-topology: " << vg.execution_topology << "\n";
        if (!vg.event_viewer_retention.empty()) {
            for (const tze::EventViewerRetention& retention : vg.event_viewer_retention) {
                std::cout << "eventviewer: " << retention.channel
                          << " maxSizeBytes=" << retention.max_size_bytes
                          << " belowMinimum=" << (retention.below_minimum ? "true" : "false") << "\n";
            }
        }
        if (!vg.session_correlations.empty()) {
            std::cout << "session-correlations: " << vg.session_correlations.size() << "\n";
        }
        if (!vg.heuristic_signals.empty()) {
            std::cout << "heuristic-signals: " << vg.heuristic_signals.size() << "\n";
            for (const tze::HeuristicSignal& signal : vg.heuristic_signals) {
                std::cout << "heuristic: " << signal.id << " confidence=" << signal.confidence << "\n";
            }
        }
        if (!vg.shaped_fields.empty()) {
            std::cout << "shaped-fields: " << vg.shaped_fields.size() << "\n";
            const std::size_t limit = std::min<std::size_t>(vg.shaped_fields.size(), 16);
            for (std::size_t index = 0; index < limit; ++index) {
                const tze::ShapedField& field = vg.shaped_fields[index];
                std::cout << "shape: " << field.field
                          << " type=" << field.type
                          << " semantic=" << field.semantic_meaning
                          << " signal=" << field.mapped_signal
                          << " lineage=" << field.lineage << "\n";
            }
            if (vg.shaped_fields.size() > limit) {
                std::cout << "shape: ...\n";
            }
        }
        if (!vg.shaping_rules.empty()) {
            std::cout << "shaping-rules: " << vg.shaping_rules.size() << "\n";
        }
        if (vg.key_custody.has_value()) {
            std::cout << "key-custody: " << vg.key_custody->status
                      << " anchor=" << vg.key_custody->visible_anchor << "\n";
        }
        if (vg.alarm_cab.has_value()) {
            std::cout << "alarm-cab: " << vg.alarm_cab->alarm_id
                      << " status=" << vg.alarm_cab->recommendation_status << "\n";
        }
        if (!vg.next_action.empty()) {
            std::cout << "next-vg: " << vg.next_action << "\n";
        }
        if (!vg.artifact_path.empty()) {
            std::cout << "vg-artifact: " << vg.artifact_path << "\n";
        }
    }
    if (report.neural_math_report.has_value()) {
        const tze::NeuralMathReport& neural = *report.neural_math_report;
        std::cout << "dataset: " << neural.dataset << "\n";
        std::cout << "accuracy: " << neural.accuracy << "\n";
        if (!neural.weights.empty()) {
            std::cout << "weights:";
            for (double weight : neural.weights) {
                std::cout << " " << weight;
            }
            std::cout << " bias=" << neural.bias << "\n";
        }
    }
    if (report.neural_route_report.has_value()) {
        const tze::NeuralRouteReport& route = *report.neural_route_report;
        std::cout << "packets: " << route.packet_count << "\n";
        std::cout << "flows: " << route.flow_count << "\n";
        if (!route.predictions.empty()) {
            const tze::NeuralRoutePrediction& top = route.predictions.front();
            std::cout << "label: " << top.label << "\n";
            std::cout << "confidence: " << top.confidence << "\n";
            if (!top.attributions.empty()) {
                std::cout << "top-factor: " << top.attributions.front().name
                          << "=" << top.attributions.front().contribution << "\n";
            }
        }
        if (!route.artifact_path.empty()) {
            std::cout << "route-artifact: " << route.artifact_path << "\n";
        }
    }
    if (report.recursive_diff_report.has_value()) {
        const tze::RecursiveDiffReport& recursive = *report.recursive_diff_report;
        if (!recursive.route_learning_status.empty() &&
            recursive.route_learning_status != "route_learning_not_needed") {
            std::cout << "Recursive Route Learning: " << recursive.route_learning_status << "\n";
        }
        std::cout << "Current State: " << recursive.current_state << "\n";
        std::cout << "Likely Goal: " << recursive.likely_goal << "\n";
        std::cout << "What the Logs/Reasoning Show: " << recursive.logs_and_reasoning << "\n";
        std::cout << "Successful Path Pattern: " << recursive.successful_path_pattern << "\n";
        std::cout << "Difference Found: " << recursive.difference_found << "\n";
        std::cout << "Best Estimate Answer: " << recursive.best_estimate_answer << "\n";
        std::cout << "Why This Matters: " << recursive.why_this_matters << "\n";
        std::cout << "Next Action: " << recursive.next_action << "\n";
        std::cout << "source-run: " << recursive.source_run_id << "\n";
        std::cout << "diff-category: " << recursive.diff_category << "\n";
        std::cout << "confidence: " << recursive.confidence << "\n";
        if (!recursive.next_action.empty()) {
            std::cout << "next-recursive: " << recursive.next_action << "\n";
        }
    }
    if (report.legacy_source.has_value()) {
        std::cout << "legacy: " << report.legacy_source->source_label << "\n";
    }
    if (!report.produced_artifact.empty()) {
        std::cout << "artifact: " << report.produced_artifact << "\n";
    }
    if (should_show_next_action_in_compact(report) && !report.next_action.empty()) {
        std::cout << "next: " << report.next_action << "\n";
    }
}

void print_processing_report_verbose(const tze::ProcessingReport& report) {
    if (!report.tze_run_id.empty()) {
        std::cout << "TZE run: " << report.tze_run_id << "\n";
    }
    if (!report.resolved_intent.empty()) {
        std::cout << "Intent: " << report.resolved_intent << "\n";
    }
    if (!report.decoded_instruction.empty()) {
        std::cout << "Instruction: " << report.decoded_instruction << "\n";
    }
    if (!report.cache.name.empty()) {
        std::cout << "Cache: " << human_readable_storage_text(report.cache.name);
        if (human_readable_storage_text(report.cache.name) != report.cache.name) {
            std::cout << " (legacy=" << report.cache.name << ")";
        }
        std::cout << " (" << report.cache.size_bytes << " bytes)\n";
    }
    if (!report.resolved_project.empty()) {
        std::cout << "Project: " << report.resolved_project << "\n";
    }
    if (!report.resolved_project_path.empty()) {
        std::cout << "Project path: " << report.resolved_project_path << "\n";
    }
    if (!report.answer_status.empty()) {
        std::cout << "Verdict: " << report.answer_status << "\n";
    }
    if (!report.version_string.empty()) {
        std::cout << "Version: " << report.version_string << "\n";
    }
    if (!report.reasoning_provider.empty()) {
        std::cout << "Reasoning provider: " << report.reasoning_provider << "\n";
    }
    if (!report.source_map_path.empty()) {
        std::cout << "Source map: " << report.source_map_path << "\n";
    }
    if (report.legacy_source.has_value()) {
        std::cout << "Legacy source: " << report.legacy_source->source_label
                  << " (" << report.legacy_source->line_count
                  << " lines, " << report.legacy_source->section_count
                  << " sections, " << report.legacy_source->symbol_count << " symbols)\n";
    }
    if (report.legacy_bridge_report.has_value()) {
        std::cout << "Legacy bridge: " << report.legacy_bridge_report->status << "\n";
        std::cout << "Legacy bridge mode: " << report.legacy_bridge_report->bridge_mode << "\n";
    }
    if (report.legacy_recovery_status.has_value()) {
        if (!report.legacy_recovery_status->summary_lines.empty()) {
            std::cout << "Legacy recovery: " << report.legacy_recovery_status->summary_lines.front() << "\n";
        }
    }
    if (report.provider_probe_report.has_value()) {
        const tze::ProviderProbeReport& probe = *report.provider_probe_report;
        std::cout << "Provider probe: " << probe.status << "\n";
        if (!probe.base_url.empty()) {
            std::cout << "Provider base URL: " << probe.base_url << "\n";
        }
        if (!probe.model.empty()) {
            std::cout << "Provider model: " << probe.model << "\n";
        }
        if (!probe.checks.empty()) {
            std::cout << "Provider checks:\n";
            for (const std::string& check : probe.checks) {
                std::cout << " - " << check << "\n";
            }
        }
        if (!probe.warnings.empty()) {
            std::cout << "Provider warnings:\n";
            for (const std::string& warning : probe.warnings) {
                std::cout << " - " << warning << "\n";
            }
        }
    }
    if (!report.assist_status.empty()) {
        std::cout << "Assist: " << report.assist_status << "\n";
    }
    if (report.assist_annotation.has_value()) {
        const tze::AssistAnnotation& assist = *report.assist_annotation;
        std::cout << "Assist provider: " << assist.provider_id;
        if (!assist.model.empty()) {
            std::cout << " (" << assist.model << ")";
        }
        std::cout << "\n";
        if (!assist.summary.empty()) {
            std::cout << "Assist summary: " << assist.summary << "\n";
        }
        if (!assist.highlights.empty()) {
            std::cout << "Assist highlights:\n";
            for (const std::string& highlight : assist.highlights) {
                std::cout << " - " << highlight << "\n";
            }
        }
        if (!assist.operator_takeaway.empty()) {
            std::cout << "Assist takeaway: " << assist.operator_takeaway << "\n";
        }
        if (!assist.warnings.empty()) {
            std::cout << "Assist warnings:\n";
            for (const std::string& warning : assist.warnings) {
                std::cout << " - " << warning << "\n";
            }
        }
    }
    if (report.command_assist_plan.has_value()) {
        const tze::CommandAssistPlan& plan = *report.command_assist_plan;
        std::cout << "Assist command plan: " << plan.canonical_command << "\n";
        std::cout << "Assist command family: " << plan.command_family << "\n";
        std::cout << "Assist command confidence: " << plan.confidence << "\n";
        if (!plan.rationale.empty()) {
            std::cout << "Assist command rationale: " << plan.rationale << "\n";
        }
        if (!plan.safety_notes.empty()) {
            std::cout << "Assist command safety notes:\n";
            for (const std::string& note : plan.safety_notes) {
                std::cout << " - " << note << "\n";
            }
        }
    }
    if (report.tool_assist_plan.has_value()) {
        const tze::ToolAssistPlan& plan = *report.tool_assist_plan;
        std::cout << "Assist tool plan: " << plan.tool_name << "\n";
        if (!plan.arguments.empty()) {
            std::cout << "Assist tool args:\n";
            for (const std::string& argument : plan.arguments) {
                std::cout << " - " << argument << "\n";
            }
        }
        if (!plan.rationale.empty()) {
            std::cout << "Assist rationale: " << plan.rationale << "\n";
        }
        if (!plan.safety_notes.empty()) {
            std::cout << "Assist safety notes:\n";
            for (const std::string& note : plan.safety_notes) {
                std::cout << " - " << note << "\n";
            }
        }
    }
    if (report.build_assist_plan.has_value()) {
        const tze::BuildAssistPlan& plan = *report.build_assist_plan;
        std::cout << "Assist build plan: " << plan.selected_recipe_id << "\n";
        if (!plan.fallback_recipe_id.empty()) {
            std::cout << "Assist fallback recipe: " << plan.fallback_recipe_id << "\n";
        }
        std::cout << "Assist build confidence: " << plan.confidence << "\n";
        if (!plan.rationale.empty()) {
            std::cout << "Assist build rationale: " << plan.rationale << "\n";
        }
        if (!plan.safety_notes.empty()) {
            std::cout << "Assist build safety notes:\n";
            for (const std::string& note : plan.safety_notes) {
                std::cout << " - " << note << "\n";
            }
        }
    }
    if (report.recipe_authoring_plan.has_value()) {
        const tze::RecipeAuthoringPlan& plan = *report.recipe_authoring_plan;
        std::cout << "Recipe authoring plan: " << plan.recipe.id << "\n";
        std::cout << "Recipe build system: " << plan.recipe.build_system << "\n";
        std::cout << "Recipe confidence: " << plan.confidence << "\n";
        if (!plan.rationale.empty()) {
            std::cout << "Recipe rationale: " << plan.rationale << "\n";
        }
    }
    if (report.recipe_authoring_artifact.has_value()) {
        const tze::RecipeAuthoringArtifact& artifact = *report.recipe_authoring_artifact;
        std::cout << "Recipe authoring status: " << artifact.status << "\n";
        std::cout << "Recipe activated: " << (artifact.activated ? "yes" : "no") << "\n";
        if (!artifact.generated_recipe_id.empty()) {
            std::cout << "Recipe artifact id: " << artifact.generated_recipe_id << "\n";
        }
        if (!artifact.validation_feedback.empty()) {
            std::cout << "Recipe validation feedback:\n";
            for (const std::string& line : artifact.validation_feedback) {
                std::cout << " - " << line << "\n";
            }
        }
    }
    if (report.next_step_assist_plan.has_value()) {
        const tze::NextStepAssistPlan& plan = *report.next_step_assist_plan;
        std::cout << "Assist next-step plan: " << plan.suggested_next_step << "\n";
        std::cout << "Assist next-step confidence: " << plan.confidence << "\n";
        if (!plan.safer_alternative.empty()) {
            std::cout << "Assist safer alternative: " << plan.safer_alternative << "\n";
        }
        if (!plan.rationale.empty()) {
            std::cout << "Assist next-step rationale: " << plan.rationale << "\n";
        }
    }
    if (report.case_summary_assist_plan.has_value()) {
        const tze::CaseSummaryAssistPlan& plan = *report.case_summary_assist_plan;
        if (!plan.summary_title.empty()) {
            std::cout << "Assist summary title: " << plan.summary_title << "\n";
        }
        std::cout << "Assist summary confidence: " << plan.confidence << "\n";
        if (!plan.executive_summary.empty()) {
            std::cout << "Assist executive summary: " << plan.executive_summary << "\n";
        }
        if (!plan.highlights.empty()) {
            std::cout << "Assist summary highlights:\n";
            for (const std::string& highlight : plan.highlights) {
                std::cout << " - " << highlight << "\n";
            }
        }
    }
    if (report.freeform_assist_answer.has_value()) {
        const tze::FreeformAssistAnswer& answer = *report.freeform_assist_answer;
        std::cout << "Assist freeform provider: " << answer.provider_id;
        if (!answer.model.empty()) {
            std::cout << " (" << answer.model << ")";
        }
        std::cout << "\n";
        std::cout << "Assist freeform confidence: " << answer.confidence << "\n";
        if (!answer.rationale.empty()) {
            std::cout << "Assist freeform rationale: " << answer.rationale << "\n";
        }
        if (!answer.suggested_commands.empty()) {
            std::cout << "Assist suggested commands:\n";
            for (const std::string& command : answer.suggested_commands) {
                std::cout << " - " << command << "\n";
            }
        }
        if (!answer.safety_warnings.empty()) {
            std::cout << "Assist safety warnings:\n";
            for (const std::string& warning : answer.safety_warnings) {
                std::cout << " - " << warning << "\n";
            }
        }
    }
    if (report.recursive_diff_report.has_value()) {
        const tze::RecursiveDiffReport& recursive = *report.recursive_diff_report;
        std::cout << "Recursive Why/Diff status: " << recursive.status << "\n";
        if (!recursive.route_learning_status.empty()) {
            std::cout << "Recursive Route Learning status: " << recursive.route_learning_status << "\n";
        }
        std::cout << "Recursive source run: " << recursive.source_run_id << "\n";
        std::cout << "Recursive confidence: " << recursive.confidence << "\n";
        std::cout << "Current State\n" << recursive.current_state << "\n";
        std::cout << "Likely Goal\n" << recursive.likely_goal << "\n";
        std::cout << "What the Logs/Reasoning Show\n" << recursive.logs_and_reasoning << "\n";
        std::cout << "Successful Path Pattern\n" << recursive.successful_path_pattern << "\n";
        std::cout << "Difference Found\n" << recursive.difference_found << "\n";
        std::cout << "Best Estimate Answer\n" << recursive.best_estimate_answer << "\n";
        std::cout << "Why This Matters\n" << recursive.why_this_matters << "\n";
        std::cout << "Next Action\n" << recursive.next_action << "\n";
        if (!recursive.slots.empty()) {
            std::cout << "Reasoning slots:\n";
            for (const tze::ReasoningSlotRecord& slot : recursive.slots) {
                std::cout << " - " << slot.slot_id << " " << slot.label << " confidence=" << slot.confidence << "\n";
                for (const std::string& fact : slot.facts) {
                    std::cout << "   - " << fact << "\n";
                }
            }
        }
    }
    if (report.neural_math_report.has_value()) {
        const tze::NeuralMathReport& neural = *report.neural_math_report;
        std::cout << "Neural math status: " << neural.status << "\n";
        std::cout << "Neural math model: " << neural.model_type << "\n";
        std::cout << "Neural math dataset: " << neural.dataset << "\n";
        std::cout << "Neural math epochs: " << neural.epochs_ran << " / " << neural.epochs_requested << "\n";
        std::cout << "Neural math learning rate: " << neural.learning_rate << "\n";
        std::cout << "Neural math accuracy: " << neural.accuracy << "\n";
        if (!neural.weights.empty()) {
            std::cout << "Neural math weights:";
            for (double weight : neural.weights) {
                std::cout << " " << weight;
            }
            std::cout << " bias=" << neural.bias << "\n";
        }
        if (!neural.predictions.empty()) {
            std::cout << "Neural math predictions:\n";
            for (const tze::NeuralMathSample& sample : neural.predictions) {
                std::cout << " - [";
                for (std::size_t index = 0; index < sample.inputs.size(); ++index) {
                    if (index != 0) {
                        std::cout << ",";
                    }
                    std::cout << sample.inputs[index];
                }
                std::cout << "] expected=" << sample.expected << " predicted=" << sample.predicted << "\n";
            }
        }
        if (!neural.math_trace.empty()) {
            std::cout << "Neural math trace:\n";
            for (const std::string& line : neural.math_trace) {
                std::cout << " - " << line << "\n";
            }
        }
        if (!neural.warnings.empty()) {
            std::cout << "Neural math warnings:\n";
            for (const std::string& warning : neural.warnings) {
                std::cout << " - " << warning << "\n";
            }
        }
    }
    if (report.neural_route_report.has_value()) {
        const tze::NeuralRouteReport& route = *report.neural_route_report;
        std::cout << "Neural route status: " << route.status << "\n";
        std::cout << "Neural route input: " << route.input_path << "\n";
        std::cout << "Neural route packets: " << route.packet_count << "\n";
        std::cout << "Neural route flows: " << route.flow_count << "\n";
        if (!route.features.feature_summary.empty()) {
            std::cout << "Neural route features: " << route.features.feature_summary.front() << "\n";
        }
        if (!route.predictions.empty()) {
            std::cout << "Neural route predictions:\n";
            for (const tze::NeuralRoutePrediction& prediction : route.predictions) {
                std::cout << " - " << prediction.label << " confidence=" << prediction.confidence;
                if (!prediction.rationale.empty()) {
                    std::cout << " | " << prediction.rationale;
                }
                std::cout << "\n";
                for (const tze::MathAttribution& attribution : prediction.attributions) {
                    std::cout << "   math: " << attribution.name
                              << " raw=" << attribution.raw_value
                              << " weight=" << attribution.weight
                              << " contribution=" << attribution.contribution;
                    if (!attribution.source.empty()) {
                        std::cout << " source=" << attribution.source;
                    }
                    if (!attribution.rationale.empty()) {
                        std::cout << " | " << attribution.rationale;
                    }
                    std::cout << "\n";
                }
            }
        }
        if (!route.warnings.empty()) {
            std::cout << "Neural route warnings:\n";
            for (const std::string& warning : route.warnings) {
                std::cout << " - " << warning << "\n";
            }
        }
    }
    if (report.review_artifact.has_value()) {
        std::cout << "Review target: " << report.review_artifact->target << "\n";
        std::cout << "Review findings: " << report.review_artifact->findings.size() << "\n";
    }
    if (report.patch_proposal_artifact.has_value()) {
        std::cout << "Patch proposal target: " << report.patch_proposal_artifact->target << "\n";
        std::cout << "Patch proposal files: " << report.patch_proposal_artifact->target_files.size() << "\n";
    }
    if (!report.produced_artifact.empty()) {
        std::cout << "Produced artifact: " << report.produced_artifact << "\n";
    }
    if (!report.answer_explanation.empty()) {
        std::cout << "Explanation:\n" << report.answer_explanation << "\n";
    }
    if (report.definition_answer.has_value()) {
        const tze::DefinitionAnswer& answer = *report.definition_answer;
        std::cout << "Definition query: " << answer.query << "\n";
        if (!answer.normalized_concept.empty()) {
            std::cout << "Normalized concept: " << answer.normalized_concept << "\n";
        }
        if (!answer.domain_hint.empty()) {
            std::cout << "Domain hint: " << answer.domain_hint << "\n";
        }
        if (!answer.mapped_cpp_target.empty()) {
            std::cout << "Mapped target: " << answer.mapped_cpp_target << "\n";
        }
        if (!answer.semantic_family.empty()) {
            std::cout << "Semantic family: " << answer.semantic_family << "\n";
        }
        if (!answer.selected_source_type.empty()) {
            std::cout << "Definition source: " << answer.selected_source_type << "\n";
        }
        if (!answer.selected_source_label.empty()) {
            std::cout << "Definition source label: " << answer.selected_source_label << "\n";
        }
        if (answer.confidence > 0.0) {
            std::cout << "Definition confidence: " << answer.confidence << "\n";
        }
        if (!answer.comparison_rationale.empty()) {
            std::cout << "Comparison rationale: " << answer.comparison_rationale << "\n";
        }
        if (!answer.sources.empty()) {
            std::cout << "Definition sources:\n";
            for (const std::string& source : answer.sources) {
                std::cout << " - " << source << "\n";
            }
        }
        if (!answer.suggestions.empty()) {
            std::cout << "Suggestions:\n";
            for (const std::string& suggestion : answer.suggestions) {
                std::cout << " - " << suggestion << "\n";
            }
        }
    }
    if (report.language_resolution.has_value()) {
        const tze::LanguageResolutionRecord& language = *report.language_resolution;
        std::cout << "Language context: " << language.combined_context
                  << " [confidence=" << language.confidence << "]\n";
        std::cout << "Language passes: " << language.passes << "\n";
        if (language.manual_confirmation_required) {
            std::cout << "Language confirmation: required";
            if (!language.manual_confirmation_response.empty()) {
                std::cout << " (" << language.manual_confirmation_response << ")";
            }
            std::cout << "\n";
            if (!language.manual_confirmation_prompt.empty()) {
                std::cout << "Language prompt: " << language.manual_confirmation_prompt << "\n";
            }
        } else if (language.manual_confirmation_used) {
            std::cout << "Language confirmation: " << language.manual_confirmation_response << "\n";
        }
    }
    if (report.uac_state.has_value()) {
        const tze::UacStateRecord& uac = *report.uac_state;
        std::cout << "uAC epoch: " << uac.epoch_marker << "\n";
        std::cout << "uAC machine: " << uac.machine_identifier << "\n";
        std::cout << "uAC namespace: " << human_readable_storage_text(uac.store_namespace);
        if (human_readable_storage_text(uac.store_namespace) != uac.store_namespace) {
            std::cout << " (legacy=" << uac.store_namespace << ")";
        }
        std::cout << "\n";
        if (!uac.recovery_hints.empty()) {
            std::cout << "uAC recovery hints:\n";
            for (const std::string& hint : uac.recovery_hints) {
                std::cout << " - " << hint << "\n";
            }
        }
    }
    const bool render_security_audit = !report.security.simulated_actions.empty() ||
        (!report.security.blocked_paths.empty() && report.security.threat_label != "privilege-gate");
    if (render_security_audit) {
        std::cout << "Security status: " << report.security.status << "\n";
        std::cout << "Security mode: " << report.security.behavior_mode << "\n";
        if (!report.security.threat_label.empty()) {
            std::cout << "Threat label: " << report.security.threat_label << "\n";
        }
        if (!report.security.threat_bracket.empty()) {
            std::cout << "Threat bracket: " << report.security.threat_bracket << "\n";
        }
        if (!report.security.simulated_actions.empty()) {
            std::cout << "Simulated actions:\n";
            for (const std::string& action : report.security.simulated_actions) {
                std::cout << " - " << action << "\n";
            }
        }
        if (!report.security.blocked_paths.empty()) {
            std::cout << "Blocked paths:\n";
            for (const std::string& path : report.security.blocked_paths) {
                std::cout << " - " << path << "\n";
            }
        }
    }
    if (report.acquisition_result.has_value()) {
        const tze::AcquisitionResult& acquisition = *report.acquisition_result;
        std::cout << "Acquisition: " << acquisition.status << "\n";
        std::cout << "Acquisition summary: " << acquisition.summary << "\n";
        if (!acquisition.resolved_source_path.empty()) {
            std::cout << "Acquired source: " << acquisition.resolved_source_path << "\n";
        }
    }
    if (report.preflight_report.has_value()) {
        const tze::PreflightReport& preflight = *report.preflight_report;
        std::cout << "Preflight: " << preflight.status << "\n";
        std::cout << "Preflight summary: " << preflight.summary << "\n";
        if (!preflight.recipe_id.empty()) {
            std::cout << "Recipe: " << preflight.recipe_id << "\n";
        }
        if (!preflight.recipe_selection_reason.empty()) {
            std::cout << "Recipe selection: " << preflight.recipe_selection_reason << "\n";
        }
        if (!preflight.available_recipe_ids.empty()) {
            std::cout << "Available recipes:\n";
            for (const std::string& recipe : preflight.available_recipe_ids) {
                std::cout << " - " << recipe << "\n";
            }
        }
        if (!preflight.build_system.empty()) {
            std::cout << "Planned build system: " << preflight.build_system << "\n";
        }
        if (!preflight.environment_signature.empty()) {
            std::cout << "Environment: " << preflight.environment_signature << "\n";
        }
        if (!preflight.install_prefix.empty()) {
            std::cout << "Install prefix: " << preflight.install_prefix << "\n";
        }
        if (!preflight.dependency_hints.empty()) {
            std::cout << "Dependency hints:\n";
            for (const std::string& hint : preflight.dependency_hints) {
                std::cout << " - " << hint << "\n";
            }
        }
        if (!preflight.expected_steps.empty()) {
            std::cout << "Expected steps:\n";
            for (const std::string& step : preflight.expected_steps) {
                std::cout << " - " << step << "\n";
            }
        }
        if (!preflight.missing_modules.empty()) {
            std::cout << "Preflight missing modules:\n";
            for (const std::string& module : preflight.missing_modules) {
                std::cout << " - " << module << "\n";
            }
        }
    }
    if (report.doctor_report.has_value()) {
        const tze::DoctorReport& doctor = *report.doctor_report;
        std::cout << "Doctor: " << doctor.status << "\n";
        std::cout << "Doctor summary: " << doctor.summary << "\n";
        if (!doctor.detected_platform.empty()) {
            std::cout << "Detected platform: " << doctor.detected_platform << "\n";
        }
        if (!doctor.detected_package_manager.empty()) {
            std::cout << "Detected package manager: " << doctor.detected_package_manager << "\n";
        }
        if (!doctor.build_system.empty()) {
            std::cout << "Doctor build system: " << doctor.build_system << "\n";
        }
        if (!doctor.dependency_checks.empty()) {
            std::cout << "Dependency checks:\n";
            for (const std::string& check : doctor.dependency_checks) {
                std::cout << " - " << check << "\n";
            }
        }
        if (!doctor.configure_flags.empty()) {
            std::cout << "Configure/build flags:\n";
            for (const std::string& flag : doctor.configure_flags) {
                std::cout << " - " << flag << "\n";
            }
        }
        if (!doctor.build_guidance.empty()) {
            std::cout << "Build guidance:\n";
            for (const std::string& line : doctor.build_guidance) {
                std::cout << " - " << line << "\n";
            }
        }
        if (!doctor.package_guidance.empty()) {
            for (const tze::PackageManagerGuidance& guidance : doctor.package_guidance) {
                std::cout << (guidance.primary ? "Primary guidance" : "Other guidance")
                          << " [" << guidance.label << "]:\n";
                for (const std::string& command : guidance.commands) {
                    std::cout << " - " << command << "\n";
                }
            }
        }
        if (!doctor.bootstrap_guidance.empty()) {
            std::cout << "Bootstrap guidance:\n";
            for (const std::string& line : doctor.bootstrap_guidance) {
                std::cout << " - " << line << "\n";
            }
        }
        if (!doctor.next_steps.empty()) {
            std::cout << "Next steps:\n";
            for (const std::string& line : doctor.next_steps) {
                std::cout << " - " << line << "\n";
            }
        }
    }
    if (report.tool_resolution.has_value()) {
        const tze::ToolResolution& tool = *report.tool_resolution;
        std::cout << "Tool resolution: " << (tool.found ? "found" : "missing") << "\n";
        if (!tool.logical_name.empty()) {
            std::cout << "Tool: " << tool.logical_name << "\n";
        }
        if (!tool.provider_type.empty()) {
            std::cout << "Provider: " << tool.provider_type << "\n";
        }
        if (!tool.executable_path.empty()) {
            std::cout << "Executable: " << tool.executable_path << "\n";
        }
        if (!tool.applet_name.empty()) {
            std::cout << "Applet: " << tool.applet_name << "\n";
        }
        if (!tool.version_fingerprint.empty()) {
            std::cout << "Version fingerprint: " << tool.version_fingerprint << "\n";
        }
        if (!tool.cache_origin.empty()) {
            std::cout << "Resolution origin: " << tool.cache_origin << "\n";
        }
        if (!tool.validation_signature.empty()) {
            std::cout << "Validation: " << tool.validation_signature << "\n";
        }
    }
    if (report.tool_doctor_report.has_value()) {
        const tze::ToolDoctorReport& doctor = *report.tool_doctor_report;
        std::cout << "Tool doctor: " << doctor.status << "\n";
        std::cout << "Tool doctor summary: " << doctor.summary << "\n";
        if (!doctor.selected_provider.empty()) {
            std::cout << "Selected provider: " << doctor.selected_provider << "\n";
        }
        if (!doctor.executable_path.empty()) {
            std::cout << "Provider path: " << doctor.executable_path << "\n";
        }
        if (!doctor.cache_origin.empty()) {
            std::cout << "Provider origin: " << doctor.cache_origin << "\n";
        }
        if (!doctor.discovered_paths.empty()) {
            std::cout << "Discovered paths:\n";
            for (const std::string& path : doctor.discovered_paths) {
                std::cout << " - " << path << "\n";
            }
        }
        if (!doctor.busybox_applets.empty()) {
            std::cout << "BusyBox applets:\n";
            for (const std::string& applet : doctor.busybox_applets) {
                std::cout << " - " << applet << "\n";
            }
        }
        if (!doctor.capability_notes.empty()) {
            std::cout << "Capability notes:\n";
            for (const std::string& note : doctor.capability_notes) {
                std::cout << " - " << note << "\n";
            }
        }
    }
    if (report.tool_invocation_report.has_value()) {
        const tze::ToolInvocationReport& invocation = *report.tool_invocation_report;
        std::cout << "Tool invocation: " << invocation.status << "\n";
        if (!invocation.command_line.empty()) {
            std::cout << "Command line: " << invocation.command_line << "\n";
        }
        std::cout << "Exit code: " << invocation.exit_code << "\n";
        if (!invocation.output_excerpt.empty()) {
            std::cout << "Output excerpt:\n";
            for (const std::string& line : invocation.output_excerpt) {
                std::cout << " - " << line << "\n";
            }
        }
    }
    if (report.packet_capture_report.has_value()) {
        const tze::PacketCaptureReport& capture = *report.packet_capture_report;
        std::cout << "OmniXTView: " << capture.status << "\n";
        std::cout << "OmniXTView summary: " << capture.summary << "\n";
        if (!capture.interface_name.empty()) {
            std::cout << "Capture interface: " << capture.interface_name << "\n";
        }
        if (!capture.pcap_path.empty()) {
            std::cout << "Capture file: " << capture.pcap_path << "\n";
        }
        if (!capture.filter.empty()) {
            std::cout << "Capture filter: " << capture.filter << "\n";
        }
        if (!capture.export_path.empty()) {
            std::cout << "Capture JSONL export: " << capture.export_path << "\n";
        }
        if (!capture.interfaces.empty()) {
            std::cout << "Capture interfaces:\n";
            for (const std::string& iface : capture.interfaces) {
                std::cout << " - " << iface << "\n";
            }
        }
        if (!capture.privilege_diagnostics.empty()) {
            std::cout << "Capture privilege diagnostics:\n";
            for (const std::string& line : capture.privilege_diagnostics) {
                std::cout << " - " << line << "\n";
            }
        }
        if (!capture.flow_summary.empty()) {
            std::cout << "Capture flows:\n";
            for (const std::string& flow : capture.flow_summary) {
                std::cout << " - " << flow << "\n";
            }
        }
        if (!capture.packets.empty()) {
            std::cout << "Packets:\n";
            for (const tze::PacketRecord& packet : capture.packets) {
                std::cout << " - " << packet.timestamp << " "
                          << packet.src_ip << ":" << packet.src_port
                          << " -> " << packet.dst_ip << ":" << packet.dst_port
                          << " flags=" << packet.tcp_flags
                          << " payload=" << packet.payload_length
                          << " class=" << packet.classification
                          << " code=" << packet.analysis_code << "\n";
                if (!packet.payload_text_utf8.empty()) {
                    std::cout << "   text_utf8(" << packet.payload_text_status << "): "
                              << packet.payload_text_utf8 << "\n";
                }
                if (!packet.ascii_preview.empty()) {
                    std::cout << "   ascii: " << packet.ascii_preview << "\n";
                }
                if (!packet.hex_preview.empty()) {
                    std::cout << "   hex: " << packet.hex_preview << "\n";
                }
                for (const std::string& line : packet.plaintext_decode) {
                    std::cout << "   decode: " << line << "\n";
                }
            }
        }
        if (!capture.warnings.empty()) {
            std::cout << "Capture warnings:\n";
            for (const std::string& warning : capture.warnings) {
                std::cout << " - " << warning << "\n";
            }
        }
    }
    if (report.defense_diagnostic_report.has_value()) {
        const tze::DefenseDiagnosticReport& defense = *report.defense_diagnostic_report;
        std::cout << "Defense diagnostic: " << defense.status << "\n";
        std::cout << "Defense summary: " << defense.summary << "\n";
        if (!defense.mode.empty()) {
            std::cout << "Defense mode: " << defense.mode << "\n";
        }
        if (!defense.target.empty()) {
            std::cout << "Defense target: " << defense.target << "\n";
        }
        if (!defense.evidence_lines.empty()) {
            std::cout << "Defense evidence:\n";
            for (const std::string& line : defense.evidence_lines) {
                std::cout << " - " << line << "\n";
            }
        }
        if (!defense.proposed_actions.empty()) {
            std::cout << "Defense proposed actions:\n";
            for (const std::string& action : defense.proposed_actions) {
                std::cout << " - " << action << "\n";
            }
        }
        if (!defense.warnings.empty()) {
            std::cout << "Defense warnings:\n";
            for (const std::string& warning : defense.warnings) {
                std::cout << " - " << warning << "\n";
            }
        }
    }
    if (report.defense_detection_report.has_value()) {
        const tze::DefenseDetectionReport& detection = *report.defense_detection_report;
        std::cout << "Defense detection: " << detection.status << "\n";
        std::cout << "Defense detection summary: " << detection.summary << "\n";
        std::cout << "Defense detection mode: " << detection.mode << "\n";
        if (!detection.since_window.empty()) {
            std::cout << "Defense detection since: " << detection.since_window << "\n";
        }
        if (!detection.quiet_hours.empty()) {
            std::cout << "Defense detection quiet hours: " << detection.quiet_hours << "\n";
        }
        if (!detection.admin_user.empty()) {
            std::cout << "Defense detection admin user: " << detection.admin_user << "\n";
        }
        if (!detection.artifact_path.empty()) {
            std::cout << "Defense detection artifact: " << detection.artifact_path << "\n";
        }
        if (!detection.signals.empty()) {
            std::cout << "Defense detection signals:\n";
            for (const tze::DefenseDetectionSignal& signal : detection.signals) {
                std::cout << " - " << signal.category << "/" << signal.id
                          << " severity=" << signal.severity
                          << " confidence=" << signal.confidence << "\n";
                if (!signal.rationale.empty()) {
                    std::cout << "   rationale: " << signal.rationale << "\n";
                }
                if (!signal.recommended_next_action.empty()) {
                    std::cout << "   next: " << signal.recommended_next_action << "\n";
                }
                std::size_t emitted = 0;
                for (const std::string& line : signal.evidence_lines) {
                    std::cout << "   evidence: " << line << "\n";
                    if (++emitted >= 8) {
                        if (signal.evidence_lines.size() > emitted) {
                            std::cout << "   evidence: ...\n";
                        }
                        break;
                    }
                }
            }
        }
        if (!detection.proposed_actions.empty()) {
            std::cout << "Defense detection proposed actions:\n";
            for (const std::string& action : detection.proposed_actions) {
                std::cout << " - " << action << "\n";
            }
        }
        if (!detection.event_viewer_retention.empty()) {
            std::cout << "Defense Event Viewer retention:\n";
            for (const tze::EventViewerRetention& retention : detection.event_viewer_retention) {
                std::cout << " - " << retention.channel
                          << " maxSizeBytes=" << retention.max_size_bytes
                          << " retentionMode=" << retention.retention_mode
                          << " belowMinimum=" << (retention.below_minimum ? "true" : "false") << "\n";
                if (!retention.recommendation.empty()) {
                    std::cout << "   recommendation: " << retention.recommendation << "\n";
                }
            }
        }
        if (!detection.session_correlations.empty()) {
            std::cout << "Defense session correlations:\n";
            for (const tze::SessionCorrelation& correlation : detection.session_correlations) {
                std::cout << " - actor=" << correlation.actor
                          << " source=" << correlation.source
                          << " tty=" << correlation.tty
                          << " confidence=" << correlation.confidence << "\n";
                if (!correlation.anomaly_rationale.empty()) {
                    std::cout << "   rationale: " << correlation.anomaly_rationale << "\n";
                }
            }
        }
        if (detection.alarm_cab.has_value()) {
            std::cout << "Defense Alarm CAB: " << detection.alarm_cab->alarm_id << "\n";
            std::cout << " - Proposed change: " << detection.alarm_cab->proposed_change << "\n";
            std::cout << " - Approval: " << detection.alarm_cab->approval_requirement << "\n";
        }
        if (!detection.warnings.empty()) {
            std::cout << "Defense detection warnings:\n";
            for (const std::string& warning : detection.warnings) {
                std::cout << " - " << warning << "\n";
            }
        }
    }
    if (report.vuplus_gate_report.has_value()) {
        const tze::VuplusGateReport& vg = *report.vuplus_gate_report;
        std::cout << "Vuplus Gate status: " << vg.status << "\n";
        std::cout << "Vuplus Gate mode: " << vg.mode << "\n";
        std::cout << "Vuplus Gate input: " << vg.input_path << "\n";
        if (!vg.dependency_map_path.empty()) {
            std::cout << "Vuplus Gate dependency map: " << vg.dependency_map_path << "\n";
        }
        std::cout << "Vuplus why: " << vg.why << "\n";
        std::cout << "Vuplus confidence: " << vg.confidence << "\n";
        std::cout << "Vuplus historical correlation: " << vg.historical_correlation << "\n";
        std::cout << "Vuplus operational blast radius: " << vg.operational_blast_radius << "\n";
        std::cout << "Vuplus rollback impact: " << vg.rollback_impact << "\n";
        std::cout << "Vuplus remediation mode: " << vg.remediation_mode << "\n";
        std::cout << "Vuplus execution topology: " << vg.execution_topology << "\n";
        if (!vg.signals.empty()) {
            std::cout << "Vuplus signals:\n";
            for (const std::string& signal : vg.signals) {
                std::cout << " - " << signal << "\n";
            }
        }
        if (!vg.key_pairs.empty()) {
            std::cout << "Vuplus key pairs:\n";
            const std::size_t limit = std::min<std::size_t>(vg.key_pairs.size(), 16);
            for (std::size_t index = 0; index < limit; ++index) {
                const tze::VuplusGateReport::KeyPair& pair = vg.key_pairs[index];
                std::cout << " - " << pair.key << "=" << pair.value
                          << " | source=" << pair.source
                          << " | value-range=" << pair.value_start << ".." << pair.value_end << "\n";
            }
            if (vg.key_pairs.size() > limit) {
                std::cout << " - ...\n";
            }
        }
        if (!vg.event_viewer_retention.empty()) {
            std::cout << "Vuplus Event Viewer retention:\n";
            for (const tze::EventViewerRetention& retention : vg.event_viewer_retention) {
                std::cout << " - " << retention.channel
                          << " maxSizeBytes=" << retention.max_size_bytes
                          << " belowMinimum=" << (retention.below_minimum ? "true" : "false") << "\n";
                if (!retention.recommendation.empty()) {
                    std::cout << "   recommendation: " << retention.recommendation << "\n";
                }
            }
        }
        if (!vg.session_correlations.empty()) {
            std::cout << "Vuplus session correlations:\n";
            for (const tze::SessionCorrelation& correlation : vg.session_correlations) {
                std::cout << " - actor=" << correlation.actor
                          << " source=" << correlation.source
                          << " tty=" << correlation.tty
                          << " confidence=" << correlation.confidence << "\n";
                if (!correlation.anomaly_rationale.empty()) {
                    std::cout << "   rationale: " << correlation.anomaly_rationale << "\n";
                }
            }
        }
        if (!vg.heuristic_signals.empty()) {
            std::cout << "Vuplus heuristic signals:\n";
            for (const tze::HeuristicSignal& signal : vg.heuristic_signals) {
                std::cout << " - " << signal.id
                          << " severity=" << signal.severity
                          << " confidence=" << signal.confidence << "\n";
                if (!signal.rationale.empty()) {
                    std::cout << "   rationale: " << signal.rationale << "\n";
                }
            }
        }
        if (!vg.shaped_fields.empty()) {
            std::cout << "Vuplus shaped fields:\n";
            const std::size_t limit = std::min<std::size_t>(vg.shaped_fields.size(), 24);
            for (std::size_t index = 0; index < limit; ++index) {
                const tze::ShapedField& field = vg.shaped_fields[index];
                std::cout << " - " << field.field
                          << "=" << field.value
                          << " type=" << field.type
                          << " semantic=" << field.semantic_meaning
                          << " signal=" << field.mapped_signal
                          << " lineage=" << field.lineage
                          << " confidence=" << field.confidence << "\n";
            }
            if (vg.shaped_fields.size() > limit) {
                std::cout << " - ...\n";
            }
        }
        if (!vg.shaping_rules.empty()) {
            std::cout << "Vuplus shaping rules:\n";
            for (const tze::ShapingRule& rule : vg.shaping_rules) {
                std::cout << " - match(source=" << rule.source
                          << ", field=" << rule.field
                          << ") type=" << rule.type
                          << " semantic=" << rule.semantic_meaning
                          << " signal=" << rule.mapped_signal << "\n";
            }
        }
        if (vg.key_custody.has_value()) {
            std::cout << "Vuplus key custody:\n";
            std::cout << " - status: " << vg.key_custody->status << "\n";
            std::cout << " - visible anchor: " << vg.key_custody->visible_anchor << "\n";
            std::cout << " - next: " << vg.key_custody->next_action << "\n";
        }
        if (vg.alarm_cab.has_value()) {
            std::cout << "Vuplus Alarm CAB: " << vg.alarm_cab->alarm_id << "\n";
            std::cout << " - Proposed change: " << vg.alarm_cab->proposed_change << "\n";
            std::cout << " - Approval: " << vg.alarm_cab->approval_requirement << "\n";
        }
        if (!vg.next_action.empty()) {
            std::cout << "Vuplus next action: " << vg.next_action << "\n";
        }
        if (!vg.artifact_path.empty()) {
            std::cout << "Vuplus artifact: " << vg.artifact_path << "\n";
        }
        if (!vg.warnings.empty()) {
            std::cout << "Vuplus warnings:\n";
            for (const std::string& warning : vg.warnings) {
                std::cout << " - " << warning << "\n";
            }
        }
    }
    if (report.source_inspection.has_value()) {
        const tze::SourceInspection& inspection = *report.source_inspection;
        std::cout << "Inspection: " << inspection.summary << "\n";
        if (!inspection.detected_files.empty()) {
            std::cout << "Detected files:\n";
            for (const std::string& file : inspection.detected_files) {
                std::cout << " - " << file << "\n";
            }
        }
        if (!inspection.missing_modules.empty()) {
            std::cout << "Missing modules:\n";
            for (const std::string& module : inspection.missing_modules) {
                std::cout << " - " << module << "\n";
            }
        }
    }
    if (report.build_execution.has_value()) {
        const tze::BuildExecution& build = *report.build_execution;
        std::cout << "Build status: " << build.status << "\n";
        std::cout << "Build summary: " << build.summary << "\n";
        if (!build.build_dir.empty()) {
            std::cout << "Build dir: " << build.build_dir << "\n";
        }
        if (!build.log_path.empty()) {
            std::cout << "Build log: " << build.log_path << "\n";
        }
        if (!build.diagnostic_excerpt.empty()) {
            std::cout << "Build diagnostics:\n";
            for (const std::string& line : build.diagnostic_excerpt) {
                std::cout << " - " << line << "\n";
            }
        }
        if (!build.log_excerpt.empty() && build.status != "built" && build.status != "installed") {
            std::cout << "Build log excerpt:\n";
            for (const std::string& line : build.log_excerpt) {
                std::cout << " - " << line << "\n";
            }
        }
        if (!build.artifact_hint.empty()) {
            std::cout << "Artifact: " << build.artifact_hint << "\n";
        }
        if (!build.selected_recipe_id.empty()) {
            std::cout << "Selected recipe: " << build.selected_recipe_id << "\n";
        }
        if (!build.recipe_selection_reason.empty()) {
            std::cout << "Recipe selection: " << build.recipe_selection_reason << "\n";
        }
        if (!build.environment_signature.empty()) {
            std::cout << "Build environment: " << build.environment_signature << "\n";
        }
        if (!build.install_status.empty()) {
            std::cout << "Install status: " << build.install_status << "\n";
        }
        if (!build.install_prefix.empty()) {
            std::cout << "Install prefix: " << build.install_prefix << "\n";
        }
        if (!build.verified_artifacts.empty()) {
            std::cout << "Verified artifacts:\n";
            for (const std::string& artifact : build.verified_artifacts) {
                std::cout << " - " << artifact << "\n";
            }
        }
        if (!build.verified_install_outputs.empty()) {
            std::cout << "Verified install outputs:\n";
            for (const std::string& artifact : build.verified_install_outputs) {
                std::cout << " - " << artifact << "\n";
            }
        }
        if (!build.commands.empty()) {
            std::cout << "Build commands:\n";
            for (const std::string& command : build.commands) {
                std::cout << " - " << command << "\n";
            }
        }
    }
    if (report.permission_context.has_value()) {
        const tze::PermissionContext& permission = *report.permission_context;
        std::cout << "Permission role: " << permission.role << "\n";
        std::cout << "Permission flags: view_raw=" << (permission.can_view_raw ? "true" : "false")
                  << " run_actions=" << (permission.can_run_actions ? "true" : "false")
                  << " store_feedback=" << (permission.can_store_feedback ? "true" : "false") << "\n";
    }
    if (report.case_record.has_value()) {
        const tze::CaseRecord& entry = *report.case_record;
        std::cout << "Case id: " << entry.id << "\n";
        std::cout << "Case title: " << entry.title << "\n";
        if (!entry.primary_source.empty()) {
            std::cout << "Case source: " << entry.primary_source << "\n";
        }
        std::cout << "Case status: " << entry.status << "\n";
        if (!entry.created_by_run_id.empty()) {
            std::cout << "Case created by run: " << entry.created_by_run_id << "\n";
        }
        if (!entry.analyzed_by_run_id.empty()) {
            std::cout << "Case analyzed by run: " << entry.analyzed_by_run_id << "\n";
        }
        if (!entry.decided_by_run_id.empty()) {
            std::cout << "Case decided by run: " << entry.decided_by_run_id << "\n";
        }
        if (!entry.reported_by_run_id.empty()) {
            std::cout << "Case reported by run: " << entry.reported_by_run_id << "\n";
        }
    }
    if (!report.case_matches.empty()) {
        std::cout << "Case matches:\n";
        for (const tze::CaseRecord& entry : report.case_matches) {
            std::cout << " - " << entry.id << " | " << entry.status << " | " << entry.title;
            if (!entry.primary_source.empty()) {
                std::cout << " | source=" << entry.primary_source;
            }
            std::cout << "\n";
        }
    }
    if (!report.observations.empty()) {
        std::cout << "Observations:\n";
        for (const tze::ObservationRecord& observation : report.observations) {
            std::cout << " - " << observation.id << " [" << observation.source_kind << "] " << observation.source_ref
                      << " => " << observation.summary << "\n";
        }
    }
    if (!report.normalized_objects.empty()) {
        std::cout << "Normalized objects:\n";
        for (const tze::NormalizedObject& object : report.normalized_objects) {
            std::cout << " - " << object.id << " [" << object.object_type << "] " << object.summary << "\n";
        }
    }
    if (!report.evidence_links.empty()) {
        std::cout << "Evidence links:\n";
        for (const tze::EvidenceLink& link : report.evidence_links) {
            std::cout << " - " << link.id << " " << link.source_observation_id << " -> " << link.target_object_id
                      << " (" << link.relation << ")\n";
        }
    }
    if (!report.analyst_comments.empty()) {
        std::cout << "Analyst comments:\n";
        for (const tze::AnalystComment& comment : report.analyst_comments) {
            std::cout << " - " << comment.author << ": " << comment.text << "\n";
        }
    }
    if (!report.decision_candidates.empty()) {
        std::cout << "Decision candidates:\n";
        for (const tze::DecisionCandidate& decision : report.decision_candidates) {
            std::cout << " - {" << decision.id << "} [score=" << decision.score
                      << " likelihood=" << decision.probability_likelihood
                      << " confidence=" << decision.confidence
                      << " valid=" << (decision.valid ? "yes" : "no")
                      << " coverage=" << decision.evidence_coverage
                      << " prior=" << decision.prior_success_score
                      << "] " << decision.title;
            if (!decision.recommended_command.empty()) {
                std::cout << " => " << decision.recommended_command;
            }
            std::cout << "\n";
            if (!decision.supporting_signals.empty()) {
                std::cout << "   signals: ";
                for (std::size_t index = 0; index < decision.supporting_signals.size(); ++index) {
                    if (index != 0) {
                        std::cout << ", ";
                    }
                    std::cout << decision.supporting_signals[index];
                }
                std::cout << "\n";
            }
            if (!decision.validation_checks.empty()) {
                std::cout << "   checks: ";
                for (std::size_t index = 0; index < decision.validation_checks.size(); ++index) {
                    if (index != 0) {
                        std::cout << ", ";
                    }
                    std::cout << decision.validation_checks[index];
                }
                std::cout << "\n";
            }
            if (!decision.score_trace.empty()) {
                std::cout << "   model: ";
                for (std::size_t index = 0; index < decision.score_trace.size(); ++index) {
                    if (index != 0) {
                        std::cout << ", ";
                    }
                    std::cout << decision.score_trace[index];
                }
                std::cout << "\n";
            }
            if (!decision.operator_feedback.empty()) {
                std::cout << "   feedback: " << decision.operator_feedback;
                if (!decision.feedback_timestamp.empty()) {
                    std::cout << " @ " << decision.feedback_timestamp;
                }
                if (!decision.feedback_note.empty()) {
                    std::cout << " | " << decision.feedback_note;
                }
                std::cout << "\n";
            }
            if (!decision.outcome_status.empty()) {
                std::cout << "   outcome: " << decision.outcome_status;
                if (!decision.outcome_timestamp.empty()) {
                    std::cout << " @ " << decision.outcome_timestamp;
                }
                if (!decision.outcome_note.empty()) {
                    std::cout << " | " << decision.outcome_note;
                }
                std::cout << "\n";
            }
        }
    }
    if (!report.case_links.empty()) {
        std::cout << "Case links:\n";
        for (const tze::CaseLink& link : report.case_links) {
            std::cout << " - [" << link.strength << "] " << link.left_case_id << " <-> " << link.right_case_id
                      << " | " << link.link_type << "=" << link.link_value;
            if (!link.rationale.empty()) {
                std::cout << " | " << link.rationale;
            }
            std::cout << "\n";
        }
    }
    if (!report.case_clusters.empty()) {
        std::cout << "Case clusters:\n";
        for (const tze::CaseCluster& cluster : report.case_clusters) {
            std::cout << " - [" << cluster.correlation_score << "] " << cluster.cluster_type
                      << " | " << cluster.title
                      << " | cases=" << cluster.case_count
                      << " | likelihood=" << cluster.likelihood << "\n";
            if (!cluster.summary.empty()) {
                std::cout << "   " << cluster.summary << "\n";
            }
        }
    }
    if (!report.next_action.empty()) {
        std::cout << "Next action: " << report.next_action << "\n";
    }
    if (report.query_session.has_value()) {
        std::cout << "Query session: " << report.query_session->id;
        if (!report.query_session->final_results.empty()) {
            std::cout << " | results=";
            for (std::size_t index = 0; index < report.query_session->final_results.size(); ++index) {
                if (index != 0) {
                    std::cout << ", ";
                }
                std::cout << report.query_session->final_results[index];
            }
        }
        std::cout << "\n";
    }
    if (!report.tze_stages.empty()) {
        std::cout << "TZE stages:\n";
        for (const tze::TzeStageRecord& stage : report.tze_stages) {
            std::cout << " - " << render_stage_label(stage) << " [" << stage.status << "] " << stage.module;
            if (!stage.detail.empty()) {
                std::cout << " | " << human_readable_storage_text(stage.detail);
            }
            if (!stage.source_section.empty() || stage.source_line != 0) {
                std::cout << " | source=";
                if (!stage.source_section.empty()) {
                    std::cout << stage.source_section;
                }
                if (stage.source_line != 0) {
                    if (!stage.source_section.empty()) {
                        std::cout << ":";
                    }
                    std::cout << stage.source_line;
                }
            }
            std::cout << "\n";
        }
    }
    if (!report.memory_reads.empty()) {
        std::cout << "Memory used:\n";
        for (const std::string& path : report.memory_reads) {
            std::cout << " - " << path << "\n";
        }
    }
    if (!report.memory_writes.empty()) {
        std::cout << "Memory updated:\n";
        for (const std::string& path : report.memory_writes) {
            std::cout << " - " << path << "\n";
        }
    }
    if (!report.toolchain.empty()) {
        print_toolchain(report.toolchain);
    }
}

void print_processing_report(const tze::ProcessingReport& report,
                             OutputMode mode = OutputMode::Auto,
                             bool prefer_verbose = false) {
    if (use_verbose_output(mode, prefer_verbose)) {
        print_processing_report_verbose(report);
        return;
    }
    print_processing_report_compact(report);
}

CommonCliOptions parse_common_options(const std::vector<std::string>& args, std::size_t start_index, std::vector<std::string>* positional) {
    CommonCliOptions options;
    for (std::size_t index = start_index; index < args.size(); ++index) {
        const std::string& arg = args[index];
        if (arg == "--source-map") {
            if (index + 1 >= args.size()) {
                throw std::runtime_error("--source-map requires a value.");
            }
            options.source_map_path = args[++index];
            continue;
        }
        if (arg == "--memory-root") {
            if (index + 1 >= args.size()) {
                throw std::runtime_error("--memory-root requires a value.");
            }
            options.memory_root_path = args[++index];
            continue;
        }
        if (arg == "--target") {
            if (index + 1 >= args.size()) {
                throw std::runtime_error("--target requires a value.");
            }
            options.build_target = args[++index];
            continue;
        }
        if (arg == "--build-dir") {
            if (index + 1 >= args.size()) {
                throw std::runtime_error("--build-dir requires a value.");
            }
            options.build_dir = args[++index];
            continue;
        }
        if (arg == "--build-type") {
            if (index + 1 >= args.size()) {
                throw std::runtime_error("--build-type requires a value.");
            }
            options.build_type = args[++index];
            continue;
        }
        if (arg == "--install-prefix") {
            if (index + 1 >= args.size()) {
                throw std::runtime_error("--install-prefix requires a value.");
            }
            options.install_prefix = args[++index];
            continue;
        }
        if (arg == "--out") {
            if (index + 1 >= args.size()) {
                throw std::runtime_error("--out requires a value.");
            }
            options.output_path = args[++index];
            continue;
        }
        if (arg == "--note") {
            if (index + 1 >= args.size()) {
                throw std::runtime_error("--note requires a value.");
            }
            options.feedback_note = args[++index];
            continue;
        }
        if (arg == "--keep") {
            if (index + 1 >= args.size()) {
                throw std::runtime_error("--keep requires a value.");
            }
            options.keep_count = static_cast<std::size_t>(std::stoull(args[++index]));
            continue;
        }
        if (arg == "--ref") {
            if (index + 1 >= args.size()) {
                throw std::runtime_error("--ref requires a value.");
            }
            options.git_ref_override = args[++index];
            continue;
        }
        if (arg == "--recipe") {
            if (index + 1 >= args.size()) {
                throw std::runtime_error("--recipe requires a value.");
            }
            options.selected_recipe_id = args[++index];
            continue;
        }
        if (arg == "--lang-confirm") {
            if (index + 1 >= args.size()) {
                throw std::runtime_error("--lang-confirm requires a value.");
            }
            options.language_confirmation = args[++index];
            continue;
        }
        if (arg == "--clean") {
            options.clean = true;
            continue;
        }
        if (arg == "--build-only" || arg == "--no-install") {
            options.perform_install = false;
            continue;
        }
        if (arg == "--offline") {
            options.offline = true;
            continue;
        }
        if (arg == "--important-only") {
            options.important_only = true;
            continue;
        }
        if (arg == "--assist") {
            options.assist = true;
            continue;
        }
        if (arg == "--verbose") {
            options.output_mode = OutputMode::Verbose;
            continue;
        }
        if (arg == "--compact") {
            options.output_mode = OutputMode::Compact;
            continue;
        }
        if (arg == "--local-only") {
            options.acquisition_policy = tze::AcquisitionPolicy::LocalOnly;
            continue;
        }
        positional->push_back(arg);
    }
    return options;
}

tze::RequestProfile make_base_profile(const CommonCliOptions& options) {
    tze::RequestProfile profile;
    profile.operator_handle = "omnix-cli";
    profile.operator_is_admin = true;
    profile.persist_on_success = true;
    profile.estimated_size = 256 * 1024;
    profile.acquisition_policy = options.acquisition_policy;
    profile.memory_root_path = options.memory_root_path;
    profile.clean_build = options.clean;
    profile.perform_install = options.perform_install;
    profile.offline = options.offline;
    profile.build_dir = options.build_dir;
    profile.build_target = options.build_target;
    profile.build_type = options.build_type;
    profile.install_prefix = options.install_prefix;
    profile.git_ref_override = options.git_ref_override;
    profile.selected_recipe_id = options.selected_recipe_id;
    profile.language_confirmation = options.language_confirmation;
    profile.assist_requested = options.assist;
    profile.feedback_note = options.feedback_note;
    profile.prune_keep_count = options.keep_count;
    profile.important_only = options.important_only;
    profile.output_path = options.output_path;
    if (!options.source_map_path.empty()) {
        profile.source_map_path = options.source_map_path;
    }
    return profile;
}

std::string join_positional_arguments(const std::vector<std::string>& positional) {
    std::ostringstream joined;
    for (std::size_t index = 0; index < positional.size(); ++index) {
        if (index != 0) {
            joined << ' ';
        }
        joined << positional[index];
    }
    return joined.str();
}

std::vector<std::string> split_whitespace(std::string_view text) {
    std::vector<std::string> tokens;
    std::string current;
    for (char c : text) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!current.empty()) {
                tokens.push_back(current);
                current.clear();
            }
            continue;
        }
        current.push_back(c);
    }
    if (!current.empty()) {
        tokens.push_back(current);
    }
    return tokens;
}

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

struct ToolCliInvocation {
    CommonCliOptions options;
    tze::ToolCommandMode mode = tze::ToolCommandMode::None;
    std::string tool_name;
    std::vector<std::string> tool_arguments;
};

ToolCliInvocation parse_tool_invocation(const std::vector<std::string>& args) {
    if (args.size() < 3) {
        throw std::runtime_error("tool requires a subcommand or tool name.");
    }

    ToolCliInvocation invocation;
    const std::string subcommand = args[2];
    if (subcommand == "list") {
        invocation.mode = tze::ToolCommandMode::List;
        std::vector<std::string> positional;
        invocation.options = parse_common_options(args, 3, &positional);
        return invocation;
    }

    if (subcommand == "locate" || subcommand == "doctor") {
        std::vector<std::string> positional;
        invocation.options = parse_common_options(args, 3, &positional);
        if (positional.empty()) {
            throw std::runtime_error("tool locate/doctor requires a tool name.");
        }
        invocation.mode = subcommand == "locate" ? tze::ToolCommandMode::Locate : tze::ToolCommandMode::Doctor;
        invocation.tool_name = positional.front();
        if (positional.size() > 1) {
            invocation.tool_arguments.assign(positional.begin() + 1, positional.end());
        }
        return invocation;
    }

    invocation.mode = tze::ToolCommandMode::Run;
    invocation.tool_name = subcommand;
    bool passthrough = false;
    for (std::size_t index = 3; index < args.size(); ++index) {
        const std::string& arg = args[index];
        if (!passthrough && arg == "--") {
            passthrough = true;
            continue;
        }
        if (!passthrough && arg == "--memory-root") {
            if (index + 1 >= args.size()) {
                throw std::runtime_error("--memory-root requires a value.");
            }
            invocation.options.memory_root_path = args[++index];
            continue;
        }
        if (!passthrough && arg == "--source-map") {
            if (index + 1 >= args.size()) {
                throw std::runtime_error("--source-map requires a value.");
            }
            invocation.options.source_map_path = args[++index];
            continue;
        }
        if (!passthrough && arg == "--compact") {
            invocation.options.output_mode = OutputMode::Compact;
            continue;
        }
        if (!passthrough && arg == "--verbose") {
            invocation.options.output_mode = OutputMode::Verbose;
            continue;
        }
        invocation.tool_arguments.push_back(arg);
    }
    return invocation;
}

struct TViewCliInvocation {
    CommonCliOptions options;
    std::string mode;
    std::string interface_name;
    std::string pcap_path;
    std::string output_path;
    int port = 0;
    std::size_t packet_count = 10;
    std::size_t seconds = 5;
    std::size_t payload_bytes = 96;
};

struct NeuralMathCliInvocation {
    CommonCliOptions options;
    std::string mode;
    std::string dataset;
    std::string route_input_path;
    std::string route_output_path;
    bool route = false;
    std::size_t epochs = 32;
    double learning_rate = 0.2;
};

NeuralMathCliInvocation parse_neural_math_invocation(const std::vector<std::string>& args) {
    NeuralMathCliInvocation invocation;
    if (args.size() >= 5 && args[2] == "route" && args[3] == "tview") {
        invocation.route = true;
        invocation.mode = "tview";
        invocation.route_input_path = args[4];
        for (std::size_t index = 5; index < args.size(); ++index) {
            const std::string& arg = args[index];
            if (arg == "--out") {
                if (index + 1 >= args.size()) {
                    throw std::runtime_error("--out requires a value.");
                }
                invocation.route_output_path = args[++index];
                invocation.options.output_path = invocation.route_output_path;
            } else if (arg == "--memory-root") {
                if (index + 1 >= args.size()) {
                    throw std::runtime_error("--memory-root requires a value.");
                }
                invocation.options.memory_root_path = args[++index];
            } else if (arg == "--source-map") {
                if (index + 1 >= args.size()) {
                    throw std::runtime_error("--source-map requires a value.");
                }
                invocation.options.source_map_path = args[++index];
            } else if (arg == "--verbose") {
                invocation.options.output_mode = OutputMode::Verbose;
            } else if (arg == "--compact") {
                invocation.options.output_mode = OutputMode::Compact;
            } else {
                throw std::runtime_error("Unknown nn route option `" + arg + "`.");
            }
        }
        return invocation;
    }
    if (args.size() < 4 || args[2] != "math" || args[3] != "perceptron") {
        throw std::runtime_error("nn supports `nn math perceptron --dataset or|and|xor` and `nn route tview <file.jsonl>`.");
    }
    invocation.mode = "perceptron";
    for (std::size_t index = 4; index < args.size(); ++index) {
        const std::string& arg = args[index];
        if (arg == "--dataset") {
            if (index + 1 >= args.size()) {
                throw std::runtime_error("--dataset requires a value.");
            }
            invocation.dataset = args[++index];
        } else if (arg == "--epochs") {
            if (index + 1 >= args.size()) {
                throw std::runtime_error("--epochs requires a value.");
            }
            invocation.epochs = static_cast<std::size_t>(std::stoull(args[++index]));
        } else if (arg == "--learning-rate") {
            if (index + 1 >= args.size()) {
                throw std::runtime_error("--learning-rate requires a value.");
            }
            invocation.learning_rate = std::stod(args[++index]);
        } else if (arg == "--memory-root") {
            if (index + 1 >= args.size()) {
                throw std::runtime_error("--memory-root requires a value.");
            }
            invocation.options.memory_root_path = args[++index];
        } else if (arg == "--source-map") {
            if (index + 1 >= args.size()) {
                throw std::runtime_error("--source-map requires a value.");
            }
            invocation.options.source_map_path = args[++index];
        } else if (arg == "--verbose") {
            invocation.options.output_mode = OutputMode::Verbose;
        } else if (arg == "--compact") {
            invocation.options.output_mode = OutputMode::Compact;
        } else {
            throw std::runtime_error("Unknown nn option `" + arg + "`.");
        }
    }
    if (invocation.dataset.empty()) {
        throw std::runtime_error("nn math perceptron requires --dataset or|and|xor.");
    }
    if (invocation.learning_rate <= 0.0) {
        throw std::runtime_error("--learning-rate must be positive.");
    }
    return invocation;
}

TViewCliInvocation parse_tview_invocation(const std::vector<std::string>& args) {
    if (args.size() < 3) {
        throw std::runtime_error("tview requires `port`, `pcap`, or `doctor`.");
    }
    TViewCliInvocation invocation;
    invocation.mode = args[2];
    std::size_t index = 3;
    if (invocation.mode == "port") {
        if (index >= args.size()) {
            throw std::runtime_error("tview port requires a TCP port.");
        }
        invocation.port = std::stoi(args[index++]);
    } else if (invocation.mode == "pcap") {
        if (index >= args.size()) {
            throw std::runtime_error("tview pcap requires a pcap file path.");
        }
        invocation.pcap_path = args[index++];
    } else if (invocation.mode != "doctor") {
        throw std::runtime_error("tview supports only `port`, `pcap`, and `doctor`.");
    }

    for (; index < args.size(); ++index) {
        const std::string& arg = args[index];
        if (arg == "--interface") {
            if (index + 1 >= args.size()) {
                throw std::runtime_error("--interface requires a value.");
            }
            invocation.interface_name = args[++index];
        } else if (arg == "--port") {
            if (index + 1 >= args.size()) {
                throw std::runtime_error("--port requires a value.");
            }
            invocation.port = std::stoi(args[++index]);
        } else if (arg == "--count") {
            if (index + 1 >= args.size()) {
                throw std::runtime_error("--count requires a value.");
            }
            invocation.packet_count = static_cast<std::size_t>(std::stoull(args[++index]));
        } else if (arg == "--seconds") {
            if (index + 1 >= args.size()) {
                throw std::runtime_error("--seconds requires a value.");
            }
            invocation.seconds = static_cast<std::size_t>(std::stoull(args[++index]));
        } else if (arg == "--payload-bytes") {
            if (index + 1 >= args.size()) {
                throw std::runtime_error("--payload-bytes requires a value.");
            }
            invocation.payload_bytes = static_cast<std::size_t>(std::stoull(args[++index]));
        } else if (arg == "--out") {
            if (index + 1 >= args.size()) {
                throw std::runtime_error("--out requires a value.");
            }
            invocation.output_path = args[++index];
        } else if (arg == "--memory-root") {
            if (index + 1 >= args.size()) {
                throw std::runtime_error("--memory-root requires a value.");
            }
            invocation.options.memory_root_path = args[++index];
        } else if (arg == "--source-map") {
            if (index + 1 >= args.size()) {
                throw std::runtime_error("--source-map requires a value.");
            }
            invocation.options.source_map_path = args[++index];
        } else if (arg == "--verbose") {
            invocation.options.output_mode = OutputMode::Verbose;
        } else if (arg == "--compact") {
            invocation.options.output_mode = OutputMode::Compact;
        } else {
            throw std::runtime_error("Unknown tview option `" + arg + "`.");
        }
    }
    if ((invocation.mode == "port" && invocation.port <= 0) ||
        invocation.port < 0 || invocation.port > 65535) {
        throw std::runtime_error("tview port must be between 1 and 65535.");
    }
    return invocation;
}

bool build_like_success(const tze::ProcessingReport& report) {
    if (report.answer_status == "recipe_authored" || report.answer_status == "recipe_authoring_repaired") {
        return true;
    }
    if (report.answer_status == "native_ready") {
        return true;
    }
    if (report.build_execution.has_value()) {
        return report.build_execution->status == "built" || report.build_execution->status == "installed";
    }
    return report.preflight_report.has_value() && report.preflight_report->ready;
}

int run_build_cmake(const std::vector<std::string>& args) {
    std::vector<std::string> positional;
    const CommonCliOptions options = parse_common_options(args, 2, &positional);
    const std::filesystem::path source_dir = positional.empty()
        ? (find_project_root(std::filesystem::current_path()).empty() ? std::filesystem::current_path() : find_project_root(std::filesystem::current_path()))
        : std::filesystem::path(positional.front());

    tze::RequestProfile profile = make_base_profile(options);
    profile.raw_prompt = "Build CMake";
    profile.instruction_slot = "aZ::1";
    profile.resolved_intent = tze::RequestIntent::BuildProject;
    profile.project_reference = source_dir.string();
    profile.execute_build = true;
    profile.perform_install = false;
    profile.build_source_path = source_dir.string();
    if (profile.source_map_path == "res/tze.txt") {
        const std::filesystem::path candidate = optional_source_file(source_dir);
        if (!candidate.empty()) {
            profile.source_map_path = candidate.string();
        } else {
            profile.source_map_path.clear();
        }
    }

    tze::ProcessingEngine engine;
    const tze::ProcessingReport report = engine.process(profile);
    print_processing_report(report, options.output_mode, false);
    return build_like_success(report) ? 0 : 1;
}

int run_build_prompt(const std::vector<std::string>& args) {
    std::vector<std::string> positional;
    const CommonCliOptions options = parse_common_options(args, 2, &positional);
    if (positional.empty()) {
        throw std::runtime_error("build requires a project alias or source path.");
    }

    tze::RequestProfile profile = make_base_profile(options);
    profile.raw_prompt = "Build " + positional.front();
    profile.project_reference = positional.front();
    profile.instruction_slot = "aZ::1";
    profile.resolved_intent = tze::RequestIntent::BuildProject;
    profile.execute_build = true;
    if (profile.source_map_path == "res/tze.txt") {
        const std::filesystem::path candidate = optional_source_file();
        if (!candidate.empty()) {
            profile.source_map_path = candidate.string();
        } else {
            profile.source_map_path.clear();
        }
    }

    tze::ProcessingEngine engine;
    const tze::ProcessingReport report = engine.process(profile);
    print_processing_report(report, options.output_mode, false);
    return build_like_success(report) ? 0 : 1;
}

int run_preflight(const std::vector<std::string>& args) {
    std::vector<std::string> positional;
    const CommonCliOptions options = parse_common_options(args, 2, &positional);
    if (positional.empty()) {
        throw std::runtime_error("preflight requires a project alias or source path.");
    }

    tze::RequestProfile profile = make_base_profile(options);
    profile.raw_prompt = "Build " + positional.front();
    profile.project_reference = positional.front();
    profile.instruction_slot = "aZ::1";
    profile.resolved_intent = tze::RequestIntent::BuildProject;
    profile.execute_build = true;
    profile.preflight_only = true;
    if (profile.source_map_path == "res/tze.txt") {
        const std::filesystem::path candidate = optional_source_file();
        if (!candidate.empty()) {
            profile.source_map_path = candidate.string();
        } else {
            profile.source_map_path.clear();
        }
    }

    tze::ProcessingEngine engine;
    const tze::ProcessingReport report = engine.process(profile);
    print_processing_report(report, options.output_mode, false);
    return build_like_success(report) ? 0 : 1;
}

int run_recipe_author(const std::vector<std::string>& args) {
    std::vector<std::string> positional;
    const CommonCliOptions options = parse_common_options(args, 3, &positional);
    if (positional.empty()) {
        throw std::runtime_error("recipe author requires a local source path.");
    }

    tze::RequestProfile profile = make_base_profile(options);
    profile.raw_prompt = "recipe author " + positional.front();
    profile.project_reference = positional.front();
    profile.build_source_path = positional.front();
    profile.instruction_slot = "aZ::1";
    profile.resolved_intent = tze::RequestIntent::AuthorBuildRecipe;
    if (profile.source_map_path == "res/tze.txt") {
        const std::filesystem::path candidate = optional_source_file();
        if (!candidate.empty()) {
            profile.source_map_path = candidate.string();
        } else {
            profile.source_map_path.clear();
        }
    }

    tze::ProcessingEngine engine;
    const tze::ProcessingReport report = engine.process(profile);
    print_processing_report(report, options.output_mode, false);
    return build_like_success(report) ? 0 : 1;
}

int run_doctor(const std::vector<std::string>& args) {
    std::vector<std::string> positional;
    const CommonCliOptions options = parse_common_options(args, 2, &positional);
    if (positional.empty()) {
        throw std::runtime_error("doctor requires a project alias or source path.");
    }

    tze::RequestProfile profile = make_base_profile(options);
    profile.raw_prompt = "doctor " + positional.front();
    profile.project_reference = positional.front();
    profile.instruction_slot = "aZ::2";
    profile.resolved_intent = tze::RequestIntent::DoctorProject;
    if (profile.source_map_path == "res/tze.txt") {
        const std::filesystem::path candidate = optional_source_file();
        if (!candidate.empty()) {
            profile.source_map_path = candidate.string();
        } else {
            profile.source_map_path.clear();
        }
    }

    tze::ProcessingEngine engine;
    const tze::ProcessingReport report = engine.process(profile);
    print_processing_report(report, options.output_mode, false);
    if (report.answer_status == "native_ready" || report.answer_status == "build_ready") {
        return 0;
    }
    return report.preflight_report.has_value() && report.preflight_report->ready ? 0 : 1;
}

int run_provider(const std::vector<std::string>& args) {
    std::vector<std::string> positional;
    const CommonCliOptions options = parse_common_options(args, 2, &positional);
    if (positional.empty() || positional.front() != "probe") {
        throw std::runtime_error("provider currently supports only `probe`.");
    }

    tze::RequestProfile profile = make_base_profile(options);
    profile.raw_prompt = "provider probe";
    profile.resolved_intent = tze::RequestIntent::ProbeProvider;
    profile.source_map_path.clear();

    tze::ProcessingEngine engine;
    const tze::ProcessingReport report = engine.process(profile);
    print_processing_report(report, options.output_mode, false);
    return report.answer_status == "provider_ready" || report.answer_status == "provider_inactive" ? 0 : 1;
}

std::string env_value(std::string_view key) {
    if (const char* value = std::getenv(std::string(key).c_str()); value != nullptr) {
        return value;
    }
    return {};
}

std::string dot_env_value(std::string_view key) {
    std::ifstream input(std::filesystem::current_path() / ".env");
    if (!input) {
        return {};
    }
    const std::string wanted(key);
    for (std::string line; std::getline(input, line);) {
        line = trim(line);
        if (line.empty() || line.front() == '#') {
            continue;
        }
        const std::size_t separator = line.find('=');
        if (separator == std::string::npos) {
            continue;
        }
        if (trim(line.substr(0, separator)) == wanted) {
            return trim(line.substr(separator + 1));
        }
    }
    return {};
}

std::string config_value(std::string_view primary_key, std::string_view secondary_key = {}) {
    std::string value = env_value(primary_key);
    if (!value.empty()) {
        return value;
    }
    value = dot_env_value(primary_key);
    if (!value.empty()) {
        return value;
    }
    if (!secondary_key.empty()) {
        value = env_value(secondary_key);
        if (!value.empty()) {
            return value;
        }
        return dot_env_value(secondary_key);
    }
    return {};
}

std::string mask_secret(std::string_view value) {
    if (value.empty()) {
        return "(missing)";
    }
    if (value.size() <= 8) {
        return "****";
    }
    return std::string(value.substr(0, 4)) + "..." + std::string(value.substr(value.size() - 4));
}

std::filesystem::path expand_user_path_main(std::string value) {
    if (value == "~") {
        return env_value("HOME").empty() ? std::filesystem::path(value) : std::filesystem::path(env_value("HOME"));
    }
    if (value.rfind("~/", 0) == 0 && !env_value("HOME").empty()) {
        return std::filesystem::path(env_value("HOME")) / value.substr(2);
    }
    return std::filesystem::path(value);
}

std::string prompt_value(std::string_view label, std::string fallback = {}) {
    std::cout << label;
    if (!fallback.empty()) {
        std::cout << " [" << fallback << "]";
    }
    std::cout << ": ";
    std::string value;
    std::getline(std::cin, value);
    value = trim(value);
    return value.empty() ? fallback : value;
}

std::filesystem::path resolve_invoked_binary_path(std::string_view argv0) {
    const std::string invoked(argv0);
    std::error_code error;
    const auto canonical_if_present = [&](const std::filesystem::path& candidate) {
        if (std::filesystem::exists(candidate, error)) {
            return std::filesystem::weakly_canonical(candidate, error);
        }
        return std::filesystem::path{};
    };
    if (invoked.find('/') != std::string::npos) {
        std::filesystem::path resolved = canonical_if_present(std::filesystem::absolute(invoked));
        if (!resolved.empty()) {
            return resolved;
        }
    } else {
        std::istringstream paths(env_value("PATH"));
        for (std::string dir; std::getline(paths, dir, ':');) {
            if (dir.empty()) {
                continue;
            }
            std::filesystem::path resolved = canonical_if_present(std::filesystem::path(dir) / invoked);
            if (!resolved.empty()) {
                return resolved;
            }
        }
    }
    return std::filesystem::absolute("build/omnix");
}

int run_api(const std::vector<std::string>& args) {
    std::vector<std::string> positional;
    const CommonCliOptions options = parse_common_options(args, 2, &positional);
    const std::string command = positional.empty() ? "status" : positional[0];

    if (command == "status" || command == "doctor") {
        const std::string provider = config_value("OMNIX_REASONING_PROVIDER");
        const std::string openai_key = config_value("OMNIX_OPENAI_API_KEY", "OPENAI_API_KEY");
        const std::string openai_model = config_value("OMNIX_OPENAI_MODEL", "OPENAI_MODEL");
        const std::string ollama_model = config_value("OMNIX_OLLAMA_MODEL");
        const std::string ollama_base_url = config_value("OMNIX_OLLAMA_BASE_URL");
        if (options.output_mode == OutputMode::Compact) {
            std::cout << "api_status: provider=" << (provider.empty() ? "(unset)" : provider)
                      << " openai_key=" << mask_secret(openai_key)
                      << " openai_model=" << (openai_model.empty() ? "(missing)" : openai_model)
                      << " ollama_model=" << (ollama_model.empty() ? "(missing)" : ollama_model)
                      << " ollama_base_url=" << (ollama_base_url.empty() ? "(missing)" : ollama_base_url) << "\n";
        } else {
            std::cout << "API status:\n";
            std::cout << " - Provider: " << (provider.empty() ? "(unset)" : provider) << "\n";
            std::cout << " - OpenAI key: " << mask_secret(openai_key) << "\n";
            std::cout << " - OpenAI model: " << (openai_model.empty() ? "(missing)" : openai_model) << "\n";
            std::cout << " - Ollama model: " << (ollama_model.empty() ? "(missing)" : ollama_model) << "\n";
            std::cout << " - Ollama base URL: " << (ollama_base_url.empty() ? "(missing)" : ollama_base_url) << "\n";
            std::cout << " - Next: `omnix api configure openai` or `omnix api configure ollama`.\n";
            if (provider == "ollama") {
                std::cout << " - Ollama server: run `ollama serve`, then `ollama run "
                          << (ollama_model.empty() ? "<model>" : ollama_model) << "`.\n";
            }
        }
        return 0;
    }

    if (command == "template") {
        if (positional.size() < 2) {
            throw std::runtime_error("api template requires openai, ollama, or huggingface.");
        }
        const std::string provider = positional[1];
        if (provider == "openai") {
            std::cout << "curl https://api.openai.com/v1/responses -H 'Authorization: Bearer $OPENAI_API_KEY' -H 'Content-Type: application/json' -d '{\"model\":\"${OPENAI_MODEL:-gpt-4.1-mini}\",\"input\":\"Say ready.\"}'\n";
        } else if (provider == "ollama") {
            std::cout << "curl http://127.0.0.1:11434/api/generate -d '{\"model\":\"${OMNIX_OLLAMA_MODEL:-deepnimsec-omni:latest}\",\"prompt\":\"Say ready.\",\"stream\":false}'\n";
        } else if (provider == "huggingface") {
            std::cout << "curl https://api-inference.huggingface.co/models/$HF_MODEL -H 'Authorization: Bearer $HUGGINGFACE_API_TOKEN' -H 'Content-Type: application/json' -d '{\"inputs\":\"Say ready.\"}'\n";
        } else {
            throw std::runtime_error("api template supports openai, ollama, or huggingface.");
        }
        return 0;
    }

    if (command == "configure") {
        if (positional.size() < 2) {
            throw std::runtime_error("api configure requires openai or ollama.");
        }
        const std::string provider = positional[1];
        const std::filesystem::path env_path = std::filesystem::current_path() / ".env";
        std::string contents;
        if (provider == "openai") {
            const std::string model = prompt_value("OpenAI model", "gpt-4.1-mini");
            const std::string key = prompt_value("OpenAI API key");
            if (key.empty()) {
                throw std::runtime_error("OpenAI API key cannot be empty.");
            }
            contents = "OMNIX_REASONING_PROVIDER=openai\nOPENAI_MODEL=" + model + "\nOPENAI_API_KEY=" + key + "\n";
            std::ofstream out(env_path);
            out << contents;
        } else if (provider == "ollama") {
            const std::string model = prompt_value("Ollama model", "deepnimsec-omni:latest");
            const std::string base_url = prompt_value("Ollama base URL", "http://127.0.0.1:11434");
            contents = "OMNIX_REASONING_PROVIDER=ollama\nOMNIX_OLLAMA_MODEL=" + model + "\nOMNIX_OLLAMA_BASE_URL=" + base_url + "\n";
            std::ofstream out(env_path);
            out << contents;
        } else {
            throw std::runtime_error("api configure supports openai or ollama.");
        }
#if defined(__APPLE__) || defined(__linux__) || defined(__unix__)
        chmod(env_path.c_str(), S_IRUSR | S_IWUSR);
#endif
        std::cout << "api_configured: wrote " << env_path << "\n";
        std::cout << "secret_status: stored locally; value masked in OmniX output.\n";
        return 0;
    }

    throw std::runtime_error("api supports status, doctor, configure, and template.");
}

int run_why(const std::vector<std::string>& args) {
    std::vector<std::string> positional;
    const CommonCliOptions options = parse_common_options(args, 2, &positional);
    const std::string run_ref = positional.empty() ? "latest" : positional.front();
    tze::RequestProfile profile = make_base_profile(options);
    profile.raw_prompt = "why " + run_ref;
    profile.tze_run_reference = run_ref;
    profile.resolved_intent = tze::RequestIntent::RecursiveWhyDiff;
    profile.source_map_path.clear();
    tze::ProcessingEngine engine;
    const tze::ProcessingReport report = engine.process(profile);
    print_processing_report(report, options.output_mode, false);
    return report.answer_status == "recursive_why_diff_complete" ? 0 : 1;
}

int run_link(const std::vector<std::string>& args) {
    std::vector<std::string> positional;
    const CommonCliOptions options = parse_common_options(args, 2, &positional);
    if (positional.empty() || positional.front() == "doctor") {
        std::cout << "link_ready: OmniX can install user-local links and namespace shims.\n";
        std::cout << "next: omnix link install --with-tze --prefix ~/.local/bin\n";
        return 0;
    }
    const std::string mode = positional.front();
    std::filesystem::path prefix = expand_user_path_main("~/.local/bin");
    bool with_tze = false;
    bool with_gg = false;
    bool force = false;
    for (std::size_t index = 1; index < positional.size(); ++index) {
        if (positional[index] == "--prefix") {
            if (index + 1 >= positional.size()) {
                throw std::runtime_error("link --prefix requires a directory.");
            }
            prefix = expand_user_path_main(positional[++index]);
        } else if (positional[index] == "--with-tze") {
            with_tze = true;
        } else if (positional[index] == "--with-gg") {
            with_gg = true;
        } else if (positional[index] == "--force") {
            force = true;
        }
    }
    const std::filesystem::path omnix_bin = resolve_invoked_binary_path(args.empty() ? std::string_view("build/omnix") : std::string_view(args.front()));
    if (mode == "install") {
        const auto run_symlink = [&](std::vector<std::string> tool_args) {
            tze::RequestProfile profile = make_base_profile(options);
            profile.resolved_intent = tze::RequestIntent::ToolAction;
            profile.tool_mode = tze::ToolCommandMode::Run;
            profile.requested_tool_name = "symlink";
            profile.tool_arguments = std::move(tool_args);
            profile.raw_prompt = "link install";
            profile.source_map_path.clear();
            tze::ProcessingEngine engine;
            const tze::ProcessingReport report = engine.process(profile);
            print_processing_report(report, options.output_mode, false);
            return report.tool_invocation_report.has_value() && report.tool_invocation_report->exit_code == 0;
        };
        std::vector<std::string> create = {"create", omnix_bin.string(), (prefix / "omnix").string()};
        if (force) {
            create.push_back("--force");
        }
        bool ok = run_symlink(create);
        if (with_tze) {
            std::vector<std::string> shim = {"shim", "tze", "tze", "--prefix", prefix.string(), "--bin", omnix_bin.string()};
            if (force) {
                shim.push_back("--force");
            }
            ok = run_symlink(shim) && ok;
        }
        if (with_gg) {
            std::vector<std::string> shim = {"shim", "gg", "gg", "--prefix", prefix.string(), "--bin", omnix_bin.string()};
            if (force) {
                shim.push_back("--force");
            }
            ok = run_symlink(shim) && ok;
        }
        return ok ? 0 : 1;
    }
    if (mode == "remove") {
        std::error_code ec;
        std::filesystem::remove(prefix / "omnix", ec);
        std::filesystem::remove(prefix / "tze", ec);
        std::filesystem::remove(prefix / "gg", ec);
        std::cout << "link_removed: removed OmniX user-local links from " << prefix << "\n";
        return 0;
    }
    throw std::runtime_error("link supports install, remove, and doctor.");
}

bool vuplus_success_status(const std::string& status) {
    return status == "vg_ready" ||
        status == "vg_shaped" ||
        status == "vg_explained" ||
        status == "vg_correlated" ||
        status == "vg_compared" ||
        status == "vg_cab_ready";
}

int run_vg(const std::vector<std::string>& args) {
    std::vector<std::string> positional;
    const CommonCliOptions options = parse_common_options(args, 2, &positional);
    if (positional.empty()) {
        throw std::runtime_error("vg requires doctor, shape, explain, correlate, compare, or cab.");
    }

    const std::string mode = positional.front();
    if (mode != "doctor" && mode != "shape" && mode != "explain" && mode != "correlate" && mode != "compare" && mode != "cab") {
        throw std::runtime_error("vg supports doctor, shape, explain, correlate, compare, and cab.");
    }

    std::string input_path;
    std::string dependency_map_path;
    bool learn_shape = false;
    std::vector<std::string> remaining;
    for (std::size_t index = 1; index < positional.size(); ++index) {
        if (positional[index] == "--dependency-map") {
            if (index + 1 >= positional.size()) {
                throw std::runtime_error("vg --dependency-map requires a file path.");
            }
            dependency_map_path = positional[++index];
            continue;
        }
        if (positional[index] == "--learn-shape") {
            learn_shape = true;
            continue;
        }
        remaining.push_back(positional[index]);
    }
    if (mode != "doctor") {
        if (remaining.empty()) {
            throw std::runtime_error("vg " + mode + " requires an ops artifact path.");
        }
        input_path = remaining.front();
    }

    tze::RequestProfile profile = make_base_profile(options);
    profile.raw_prompt = "vg " + mode + (input_path.empty() ? std::string{} : " " + input_path);
    profile.resolved_intent = tze::RequestIntent::VuplusGate;
    profile.vuplus_mode = mode;
    profile.vuplus_input_path = input_path;
    profile.vuplus_dependency_map_path = dependency_map_path;
    profile.vuplus_learn_shape = learn_shape;
    profile.source_map_path.clear();

    tze::ProcessingEngine engine;
    const tze::ProcessingReport report = engine.process(profile);
    if (options.output_mode == OutputMode::Verbose) {
        print_processing_report(report, options.output_mode, false);
    } else if (options.output_mode == OutputMode::Compact) {
        print_processing_report(report, options.output_mode, false);
    } else if (report.vuplus_gate_report.has_value()) {
        std::cout << report.vuplus_gate_report->json << "\n";
    } else {
        print_processing_report(report, OutputMode::Compact, false);
    }

    if (!report.vuplus_gate_report.has_value()) {
        return 1;
    }
    return vuplus_success_status(report.vuplus_gate_report->status) ? 0 : 1;
}

int run_tview(const std::vector<std::string>& args) {
    const TViewCliInvocation invocation = parse_tview_invocation(args);
    tze::RequestProfile profile = make_base_profile(invocation.options);
    profile.raw_prompt = "tview " + invocation.mode;
    profile.resolved_intent = tze::RequestIntent::PacketCapture;
    profile.packet_capture_mode = invocation.mode == "port" ? "live" : invocation.mode;
    profile.packet_interface = invocation.interface_name;
    profile.packet_pcap_path = invocation.pcap_path;
    profile.packet_export_path = invocation.output_path;
    profile.packet_port = invocation.port;
    profile.packet_count = invocation.packet_count;
    profile.packet_seconds = invocation.seconds;
    profile.packet_payload_bytes = invocation.payload_bytes;
    if (profile.source_map_path == "res/tze.txt") {
        const std::filesystem::path candidate = optional_source_file();
        profile.source_map_path = candidate.empty() ? std::string{} : candidate.string();
    }

    tze::ProcessingEngine engine;
    const tze::ProcessingReport report = engine.process(profile);
    print_processing_report(report, invocation.options.output_mode, false);
    if (!report.packet_capture_report.has_value()) {
        return 1;
    }
    return report.packet_capture_report->status == "capture_complete" ||
           report.packet_capture_report->status == "capture_empty" ||
           report.packet_capture_report->status == "capture_ready"
        ? 0
        : 1;
}

int run_persona(const std::vector<std::string>& args) {
    std::vector<std::string> positional;
    const CommonCliOptions options = parse_common_options(args, 2, &positional);
    if (positional.size() < 2 || positional.front() != "mode") {
        throw std::runtime_error("persona currently supports `persona mode <premise|cynic|professional|neutral>`.");
    }
    tze::RequestProfile profile = make_base_profile(options);
    profile.raw_prompt = "persona mode " + positional[1];
    profile.persona_mode = positional[1];
    profile.resolved_intent = tze::RequestIntent::SetPersonaMode;
    profile.source_map_path.clear();

    tze::ProcessingEngine engine;
    const tze::ProcessingReport report = engine.process(profile);
    print_processing_report(report, options.output_mode, false);
    return report.answer_status == "persona_mode_set" ? 0 : 1;
}

int run_nn(const std::vector<std::string>& args) {
    const NeuralMathCliInvocation invocation = parse_neural_math_invocation(args);
    tze::RequestProfile profile = make_base_profile(invocation.options);
    if (invocation.route) {
        profile.raw_prompt = "nn route tview " + invocation.route_input_path;
        profile.resolved_intent = tze::RequestIntent::NeuralRoute;
        profile.neural_route_mode = invocation.mode;
        profile.neural_route_input_path = invocation.route_input_path;
        profile.neural_route_output_path = invocation.route_output_path;
    } else {
        profile.raw_prompt = "nn math perceptron --dataset " + invocation.dataset;
        profile.resolved_intent = tze::RequestIntent::NeuralMath;
        profile.neural_math_mode = invocation.mode;
        profile.neural_dataset = invocation.dataset;
        profile.neural_epochs = invocation.epochs;
        profile.neural_learning_rate = invocation.learning_rate;
    }
    if (profile.source_map_path == "res/tze.txt") {
        const std::filesystem::path candidate = optional_source_file();
        profile.source_map_path = candidate.empty() ? std::string{} : candidate.string();
    }

    tze::ProcessingEngine engine;
    const tze::ProcessingReport report = engine.process(profile);
    print_processing_report(report, invocation.options.output_mode, false);
    if (invocation.route) {
        return report.neural_route_report.has_value() &&
               report.neural_route_report->status == "neural_route_complete"
            ? 0
            : 1;
    }
    return report.neural_math_report.has_value() &&
           (report.neural_math_report->status == "neural_math_complete" ||
            report.neural_math_report->status == "not_linearly_separable")
        ? 0
        : 1;
}

int run_defend(const std::vector<std::string>& args) {
    std::vector<std::string> positional;
    const CommonCliOptions options = parse_common_options(args, 2, &positional);
    if (positional.empty() || (positional.front() != "diag" && positional.front() != "detect")) {
        throw std::runtime_error("defend supports `defend diag <cpu|memory|logs|pid|port>` or `defend detect <env|sessions|persistence|packages|services|logs|eventviewer|all>`.");
    }

    const bool detect = positional.front() == "detect";
    if (positional.size() < 2) {
        throw std::runtime_error(detect
            ? "defend detect requires one of env, sessions, persistence, packages, services, logs, eventviewer, or all."
            : "defend diag requires one of cpu, memory, logs, pid, or port.");
    }
    const std::string mode = positional[1];
    std::string since_window = "24h";
    std::string quiet_hours;
    std::string admin_user;
    std::string channels;
    std::string source_path;
    std::size_t max_lines = 40;
    std::vector<std::string> target_parts;
    for (std::size_t index = 2; index < positional.size(); ++index) {
        const std::string& arg = positional[index];
        if (detect && arg == "--since") {
            if (index + 1 >= positional.size()) {
                throw std::runtime_error("--since requires a value.");
            }
            since_window = positional[++index];
            continue;
        }
        if (detect && arg == "--quiet-hours") {
            if (index + 1 >= positional.size()) {
                throw std::runtime_error("--quiet-hours requires a value.");
            }
            quiet_hours = positional[++index];
            continue;
        }
        if (detect && arg == "--admin-user") {
            if (index + 1 >= positional.size()) {
                throw std::runtime_error("--admin-user requires a value.");
            }
            admin_user = positional[++index];
            continue;
        }
        if (detect && arg == "--channels") {
            if (index + 1 >= positional.size()) {
                throw std::runtime_error("--channels requires a comma-separated value.");
            }
            channels = positional[++index];
            continue;
        }
        if (detect && arg == "--source") {
            if (index + 1 >= positional.size()) {
                throw std::runtime_error("--source requires a local fixture path.");
            }
            source_path = positional[++index];
            continue;
        }
        if (detect && arg == "--max-lines") {
            if (index + 1 >= positional.size()) {
                throw std::runtime_error("--max-lines requires a value.");
            }
            max_lines = static_cast<std::size_t>(std::stoull(positional[++index]));
            continue;
        }
        target_parts.push_back(arg);
    }
    const std::string target = join_positional_arguments(target_parts);

    tze::RequestProfile profile = make_base_profile(options);
    profile.raw_prompt = std::string("defend ") + (detect ? "detect " : "diag ") + mode +
        (target.empty() ? std::string{} : " " + target);
    profile.resolved_intent = detect ? tze::RequestIntent::DefenseDetection
                                     : tze::RequestIntent::DefenseDiagnostic;
    profile.defense_mode = mode;
    profile.defense_target = target;
    profile.defense_since_window = since_window;
    profile.defense_quiet_hours = quiet_hours;
    profile.defense_admin_user = admin_user;
    profile.defense_max_lines = max_lines;
    profile.defense_channels = channels;
    profile.defense_source_path = source_path;
    if (!detect && mode == "port" && !target.empty()) {
        profile.defense_port = std::stoi(target);
    } else if (!detect && mode == "pid" && !target.empty()) {
        profile.defense_pid = std::stoi(target);
    }
    if (detect) {
        const std::vector<std::string> valid = {"env", "sessions", "persistence", "packages", "services", "logs", "eventviewer", "all"};
        if (std::find(valid.begin(), valid.end(), mode) == valid.end()) {
            throw std::runtime_error("defend detect requires one of env, sessions, persistence, packages, services, logs, eventviewer, or all.");
        }
    }
    if (profile.source_map_path == "res/tze.txt") {
        const std::filesystem::path candidate = optional_source_file();
        profile.source_map_path = candidate.empty() ? std::string{} : candidate.string();
    }

    tze::ProcessingEngine engine;
    const tze::ProcessingReport report = engine.process(profile);
    print_processing_report(report, options.output_mode, false);
    if (detect) {
        return report.defense_detection_report.has_value() &&
               (report.defense_detection_report->status == "defense_detection_complete" ||
                report.defense_detection_report->status == "defense_detection_empty")
            ? 0
            : 1;
    }
    return report.defense_diagnostic_report.has_value() &&
               (report.defense_diagnostic_report->status == "defense_diagnostic_complete" ||
                report.defense_diagnostic_report->status == "defense_diagnostic_empty")
        ? 0
        : 1;
}

int run_analyst_command(const std::vector<std::string>& args,
                        tze::RequestIntent intent,
                        const std::string& command_name,
                        const std::string& success_status) {
    std::vector<std::string> positional;
    const CommonCliOptions options = parse_common_options(args, 2, &positional);
    if (positional.empty()) {
        throw std::runtime_error(command_name + " requires a case id, source path, or command reference.");
    }

    const std::string target = join_positional_arguments(positional);
    tze::RequestProfile profile = make_base_profile(options);
    profile.raw_prompt = command_name + " " + target;
    profile.analyst_reference = target;
    profile.project_reference = target;
    profile.instruction_slot = "aZ::99";
    profile.resolved_intent = intent;
    if (profile.source_map_path == "res/tze.txt") {
        const std::filesystem::path candidate = optional_source_file();
        if (!candidate.empty()) {
            profile.source_map_path = candidate.string();
        } else {
            profile.source_map_path.clear();
        }
    }

    tze::ProcessingEngine engine;
    const tze::ProcessingReport report = engine.process(profile);
    print_processing_report(report, options.output_mode, false);
    return report.answer_status == success_status ? 0 : 1;
}

int run_ask(const std::vector<std::string>& args) {
    std::vector<std::string> positional;
    const CommonCliOptions options = parse_common_options(args, 2, &positional);
    if (positional.empty()) {
        throw std::runtime_error("ask requires a prompt.");
    }

    std::ostringstream prompt;
    for (std::size_t index = 0; index < positional.size(); ++index) {
        if (index != 0) {
            prompt << ' ';
        }
        prompt << positional[index];
    }

    tze::RequestProfile profile = make_base_profile(options);
    profile.raw_prompt = prompt.str();
    if (profile.source_map_path == "res/tze.txt") {
        const std::filesystem::path candidate = optional_source_file();
        if (!candidate.empty()) {
            profile.source_map_path = candidate.string();
        } else {
            profile.source_map_path.clear();
        }
    }

    tze::ProcessingEngine engine;
    const tze::ProcessingReport report = engine.process(profile);
    print_processing_report(report, options.output_mode, false);
    if (report.answer_status == "native_ready") {
        return 0;
    }
    return report.build_execution.has_value()
        ? ((report.build_execution->status == "built" || report.build_execution->status == "installed") ? 0 : 1)
        : (report.preflight_report.has_value() && !report.preflight_report->ready ? 1 : 0);
}

int run_ingest(const std::vector<std::string>& args) {
    return run_analyst_command(args, tze::RequestIntent::IngestData, "ingest", "ingested");
}

int run_analyze(const std::vector<std::string>& args) {
    return run_analyst_command(args, tze::RequestIntent::AnalyzeCase, "analyze", "analyzed");
}

int run_decide(const std::vector<std::string>& args) {
    std::vector<std::string> positional;
    const CommonCliOptions options = parse_common_options(args, 2, &positional);
    if (!positional.empty() && positional.front() == "feedback") {
        if (positional.size() < 4) {
            throw std::runtime_error("decide feedback requires <case-id> <decision-id> <helpful|not-helpful>.");
        }
        if (positional[3] != "helpful" && positional[3] != "not-helpful") {
            throw std::runtime_error("decide feedback requires `helpful` or `not-helpful`.");
        }
        tze::RequestProfile profile = make_base_profile(options);
        profile.raw_prompt = "decide feedback " + positional[1] + " " + positional[2] + " " + positional[3];
        profile.analyst_reference = positional[1];
        profile.decision_reference = positional[2];
        profile.feedback_value = positional[3];
        profile.resolved_intent = tze::RequestIntent::MarkDecisionFeedback;
        if (profile.source_map_path == "res/tze.txt") {
            const std::filesystem::path candidate = optional_source_file();
            profile.source_map_path = candidate.empty() ? std::string{} : candidate.string();
        }
        tze::ProcessingEngine engine;
        const tze::ProcessingReport report = engine.process(profile);
        print_processing_report(report, options.output_mode, false);
        return report.answer_status == "decision_feedback_recorded" ? 0 : 1;
    }

    if (!positional.empty() && positional.front() == "outcome") {
        if (positional.size() < 4) {
            throw std::runtime_error("decide outcome requires <case-id> <decision-id> <success|failed|partial>.");
        }
        if (positional[3] != "success" && positional[3] != "failed" && positional[3] != "partial") {
            throw std::runtime_error("decide outcome requires `success`, `failed`, or `partial`.");
        }
        tze::RequestProfile profile = make_base_profile(options);
        profile.raw_prompt = "decide outcome " + positional[1] + " " + positional[2] + " " + positional[3];
        profile.analyst_reference = positional[1];
        profile.decision_reference = positional[2];
        profile.feedback_value = positional[3];
        profile.resolved_intent = tze::RequestIntent::MarkDecisionOutcome;
        if (profile.source_map_path == "res/tze.txt") {
            const std::filesystem::path candidate = optional_source_file();
            profile.source_map_path = candidate.empty() ? std::string{} : candidate.string();
        }
        tze::ProcessingEngine engine;
        const tze::ProcessingReport report = engine.process(profile);
        print_processing_report(report, options.output_mode, false);
        return report.answer_status == "decision_outcome_recorded" ? 0 : 1;
    }

    return run_analyst_command(args, tze::RequestIntent::DecideAction, "decide", "decided");
}

int run_case(const std::vector<std::string>& args) {
    std::vector<std::string> positional;
    const CommonCliOptions options = parse_common_options(args, 2, &positional);
    if (positional.empty()) {
        throw std::runtime_error("case requires an id, `list`, or `search <term>`.");
    }

    tze::RequestProfile profile = make_base_profile(options);
    profile.instruction_slot = "aZ::99";
    profile.resolved_intent = tze::RequestIntent::InspectCase;

    if (positional.front() == "list") {
        profile.analyst_mode = "list";
        profile.analyst_reference = "list";
        profile.raw_prompt = "case list";
    } else if (positional.front() == "search") {
        if (positional.size() < 2) {
            throw std::runtime_error("case search requires a search term.");
        }
        profile.analyst_mode = "search";
        profile.analyst_query = join_positional_arguments(
            std::vector<std::string>(positional.begin() + 1, positional.end()));
        profile.analyst_reference = "search " + profile.analyst_query;
        profile.raw_prompt = profile.analyst_reference;
    } else if (positional.front() == "timeline") {
        if (positional.size() < 2) {
            throw std::runtime_error("case timeline requires a case id or source.");
        }
        profile.resolved_intent = tze::RequestIntent::CaseTimeline;
        profile.analyst_mode = "timeline";
        profile.analyst_reference = join_positional_arguments(std::vector<std::string>(positional.begin() + 1, positional.end()));
        profile.raw_prompt = "case timeline " + profile.analyst_reference;
    } else if (positional.front() == "export") {
        if (positional.size() < 2) {
            throw std::runtime_error("case export requires a case id or source.");
        }
        profile.resolved_intent = tze::RequestIntent::ExportCaseBundle;
        profile.analyst_mode = "export";
        profile.analyst_reference = join_positional_arguments(std::vector<std::string>(positional.begin() + 1, positional.end()));
        profile.raw_prompt = "case export " + profile.analyst_reference;
    } else if (positional.front() == "import") {
        if (positional.size() < 2) {
            throw std::runtime_error("case import requires a bundle path.");
        }
        profile.resolved_intent = tze::RequestIntent::ImportCaseBundle;
        profile.analyst_mode = "import";
        profile.analyst_reference = positional[1];
        profile.raw_prompt = "case import " + positional[1];
    } else {
        const std::string target = join_positional_arguments(positional);
        profile.analyst_mode = "inspect";
        profile.analyst_reference = target;
        profile.raw_prompt = "case " + target;
    }

    if (profile.source_map_path == "res/tze.txt") {
        const std::filesystem::path candidate = optional_source_file();
        if (!candidate.empty()) {
            profile.source_map_path = candidate.string();
        } else {
            profile.source_map_path.clear();
        }
    }

    tze::ProcessingEngine engine;
    const tze::ProcessingReport report = engine.process(profile);
    print_processing_report(report, options.output_mode, false);
    return report.answer_status == "case_loaded" || report.answer_status == "case_listed" ||
               report.answer_status == "case_search_results" || report.answer_status == "case_search_empty" ||
               report.answer_status == "case_timeline" || report.answer_status == "case_bundle_written" ||
               report.answer_status == "case_bundle_imported"
        ? 0
        : 1;
}

int run_incident(const std::vector<std::string>& args) {
    std::vector<std::string> positional;
    const CommonCliOptions options = parse_common_options(args, 2, &positional);
    if (positional.empty()) {
        throw std::runtime_error("incident requires an id, `list`, or `report <id>`.");
    }

    tze::RequestProfile profile = make_base_profile(options);
    profile.source_map_path.clear();

    if (positional.front() == "list") {
        profile.raw_prompt = "incident list";
        profile.resolved_intent = tze::RequestIntent::ListIncidents;
    } else if (positional.front() == "report") {
        if (positional.size() < 2) {
            throw std::runtime_error("incident report requires an incident id.");
        }
        profile.raw_prompt = "incident report " + positional[1];
        profile.incident_reference = positional[1];
        profile.resolved_intent = tze::RequestIntent::ReportIncident;
    } else {
        profile.raw_prompt = "incident " + positional.front();
        profile.incident_reference = positional.front();
        profile.resolved_intent = tze::RequestIntent::InspectIncident;
    }

    tze::ProcessingEngine engine;
    const tze::ProcessingReport report = engine.process(profile);
    print_processing_report(report, options.output_mode, false);
    return report.answer_status == "incident_listed" || report.answer_status == "incident_loaded" ||
               report.answer_status == "incident_report_written"
        ? 0
        : 1;
}

int run_define(const std::vector<std::string>& args) {
    std::vector<std::string> positional;
    const CommonCliOptions options = parse_common_options(args, 2, &positional);
    if (positional.empty()) {
        throw std::runtime_error("define requires a symbol or term.");
    }

    tze::RequestProfile profile = make_base_profile(options);
    profile.raw_prompt = "define " + positional.front();
    profile.instruction_slot = "aZ::99";
    profile.resolved_intent = tze::RequestIntent::DefineSymbol;
    profile.definition_concept = positional.front();
    if (positional.size() >= 2 && options.source_map_path.empty()) {
        profile.source_map_path = positional[1];
    } else if (profile.source_map_path == "res/tze.txt") {
        const std::filesystem::path candidate = optional_source_file();
        if (!candidate.empty()) {
            profile.source_map_path = candidate.string();
        } else {
            profile.source_map_path.clear();
        }
    }

    tze::ProcessingEngine engine;
    const tze::ProcessingReport report = engine.process(profile);
    print_processing_report(report, options.output_mode, true);
    return report.definition_answer.has_value() && report.definition_answer->found ? 0 : 1;
}

int run_explain(const std::vector<std::string>& args) {
    std::vector<std::string> positional;
    const CommonCliOptions options = parse_common_options(args, 2, &positional);
    if (positional.empty()) {
        throw std::runtime_error("explain requires a command or symbol.");
    }

    tze::RequestProfile profile = make_base_profile(options);
    profile.raw_prompt = "explain " + positional.front();
    profile.instruction_slot = "aZ::99";
    profile.resolved_intent = tze::RequestIntent::ExplainCommand;
    if (positional.size() >= 2 && options.source_map_path.empty()) {
        profile.source_map_path = positional[1];
    } else if (profile.source_map_path == "res/tze.txt") {
        const std::filesystem::path candidate = optional_source_file();
        if (!candidate.empty()) {
            profile.source_map_path = candidate.string();
        } else {
            profile.source_map_path.clear();
        }
    }

    tze::ProcessingEngine engine;
    const tze::ProcessingReport report = engine.process(profile);
    print_processing_report(report, options.output_mode, true);
    return report.definition_answer.has_value() && report.definition_answer->found ? 0 : 1;
}

int reset_runtime_context(const CommonCliOptions& options) {
    tze::MemoryStore memory_store;
    tze::MemorySnapshot memory = memory_store.load(options.memory_root_path);
    const std::size_t definitions_count = memory.definitions.size();
    const std::size_t history_count = memory.history.size();
    const std::size_t language_count = memory.language_contexts.size();
    const std::size_t uac_count = memory.uac_states.size();
    const std::size_t assist_count =
        memory.assist_outcomes.size() + memory.assist_corrections.size() + memory.assist_learning.size();

    memory.definitions.clear();
    memory.history.clear();
    memory.language_contexts.clear();
    memory.uac_states.clear();
    memory.assist_outcomes.clear();
    memory.assist_corrections.clear();
    memory.assist_learning.clear();
    memory_store.persist_snapshot(memory);

    if (options.output_mode == OutputMode::Compact) {
        std::cout << "context_reset: cleared volatile learned/runtime caches without deleting glossary, cases, TZE ledger, recipes, tools, or persona.\n";
    } else {
        std::cout << "Context reset complete:\n";
        std::cout << " - definitions cleared: " << definitions_count << "\n";
        std::cout << " - history entries cleared: " << history_count << "\n";
        std::cout << " - language contexts cleared: " << language_count << "\n";
        std::cout << " - uAC states cleared: " << uac_count << "\n";
        std::cout << " - assist cache entries cleared: " << assist_count << "\n";
        std::cout << " - retained: local glossary, TZE runs, cases, recipes, native tools, security audits, persona.\n";
        std::cout << "Next: rerun `omnix ask \"what is apple\"` to rebuild context from source truth.\n";
    }
    return 0;
}

int run_context(const std::vector<std::string>& args) {
    std::vector<std::string> positional;
    const CommonCliOptions options = parse_common_options(args, 2, &positional);
    if (positional.empty() || positional.front() != "reset") {
        throw std::runtime_error("context currently supports `context reset`.");
    }
    return reset_runtime_context(options);
}

int run_identity(const std::vector<std::string>& args) {
    std::vector<std::string> positional;
    const CommonCliOptions options = parse_common_options(args, 2, &positional);
    (void)positional;
    const tze::InstanceIdentityReport report = tze::resolve_instance_identity(options.memory_root_path);
    if (options.output_mode == OutputMode::Compact) {
        std::cout << "instance_identity_ready: " << report.instance_id << "\n";
        std::cout << "fingerprint: " << report.fingerprint << "\n";
        std::cout << "arch: " << report.architecture << "\n";
        std::cout << "mode: " << report.mode << "\n";
        std::cout << "warning: " << report.warning << "\n";
        return 0;
    }

    std::cout << "OmniX Instance Identity\n";
    std::cout << " - Status: " << report.status << "\n";
    std::cout << " - Instance ID: " << report.instance_id << "\n";
    std::cout << " - Fingerprint: " << report.fingerprint << "\n";
    std::cout << " - Architecture: " << report.architecture << "\n";
    std::cout << " - Platform: " << report.platform << "\n";
    std::cout << " - Host hint hash: " << report.host_hint_hash << "\n";
    std::cout << " - Mode: " << report.mode << "\n";
    std::cout << " - Salt path: " << report.salt_path << "\n";
    std::cout << " - Generated new salt: " << (report.generated_new_salt ? "yes" : "no") << "\n";
    std::cout << " - Components:\n";
    for (const std::string& component : report.components) {
        std::cout << "   * " << component << "\n";
    }
    std::cout << " - Warning: " << report.warning << "\n";
    return 0;
}

std::map<std::string, std::string> parse_flat_json_vars(const std::filesystem::path& path) {
    const std::string text = read_text_file_local(path);
    std::map<std::string, std::string> vars;
    std::size_t index = 0;
    while ((index = text.find('"', index)) != std::string::npos) {
        const std::size_t key_end = text.find('"', index + 1);
        if (key_end == std::string::npos) {
            break;
        }
        const std::string key = text.substr(index + 1, key_end - index - 1);
        const std::size_t colon = text.find(':', key_end + 1);
        if (colon == std::string::npos) {
            break;
        }
        std::size_t value_start = colon + 1;
        while (value_start < text.size() && std::isspace(static_cast<unsigned char>(text[value_start]))) {
            ++value_start;
        }
        std::string value;
        if (value_start < text.size() && text[value_start] == '"') {
            const std::size_t value_end = text.find('"', value_start + 1);
            if (value_end == std::string::npos) {
                break;
            }
            value = text.substr(value_start + 1, value_end - value_start - 1);
            index = value_end + 1;
        } else {
            std::size_t value_end = value_start;
            while (value_end < text.size() && text[value_end] != ',' && text[value_end] != '}') {
                ++value_end;
            }
            value = trim_cli(text.substr(value_start, value_end - value_start));
            index = value_end;
        }
        if (!key.empty()) {
            vars[key] = value;
        }
    }
    return vars;
}

std::vector<std::string> extract_jinja_variables(std::string_view text) {
    std::vector<std::string> variables;
    std::size_t index = 0;
    while ((index = text.find("{{", index)) != std::string::npos) {
        const std::size_t end = text.find("}}", index + 2);
        if (end == std::string::npos) {
            break;
        }
        std::string expr = trim_cli(text.substr(index + 2, end - index - 2));
        if (const std::size_t pipe = expr.find('|'); pipe != std::string::npos) {
            expr = trim_cli(expr.substr(0, pipe));
        }
        if (!expr.empty() && std::find(variables.begin(), variables.end(), expr) == variables.end()) {
            variables.push_back(expr);
        }
        index = end + 2;
    }
    return variables;
}

std::vector<std::string> detect_jinja_risks(std::string_view text) {
    std::vector<std::string> risks;
    const std::string lowered = lowercase_copy(std::string(text));
    if (lowered.find("{%") != std::string::npos) {
        risks.push_back("control_blocks_present");
    }
    if (lowered.find("salt[") != std::string::npos || lowered.find("__salt__") != std::string::npos) {
        risks.push_back("salt_execution_reference");
    }
    if (lowered.find("cmd.run") != std::string::npos || lowered.find("cmd.run_all") != std::string::npos) {
        risks.push_back("command_execution_reference");
    }
    if (lowered.find("sudo ") != std::string::npos || lowered.find("systemctl ") != std::string::npos) {
        risks.push_back("rendered_shell_surface");
    }
    return risks;
}

std::string render_jinja_passthrough(std::string text,
                                     const std::map<std::string, std::string>& vars,
                                     std::vector<std::string>* unresolved) {
    std::size_t index = 0;
    while ((index = text.find("{{", index)) != std::string::npos) {
        const std::size_t end = text.find("}}", index + 2);
        if (end == std::string::npos) {
            break;
        }
        const std::string original = text.substr(index, end - index + 2);
        std::string expr = trim_cli(text.substr(index + 2, end - index - 2));
        if (const std::size_t pipe = expr.find('|'); pipe != std::string::npos) {
            expr = trim_cli(expr.substr(0, pipe));
        }
        auto found = vars.find(expr);
        if (found == vars.end() && expr.rfind("grains.", 0) == 0) {
            found = vars.find(expr.substr(7));
        }
        if (found == vars.end()) {
            if (unresolved != nullptr) {
                unresolved->push_back(expr);
            }
            index = end + 2;
            continue;
        }
        text.replace(index, original.size(), found->second);
        index += found->second.size();
    }
    return text;
}

std::string classify_jinja_plan(std::string_view rendered) {
    const std::string lowered = lowercase_copy(std::string(rendered));
    if (lowered.find("sudo ") != std::string::npos || lowered.find("systemctl ") != std::string::npos ||
        lowered.find("curl ") != std::string::npos || lowered.find("wget ") != std::string::npos) {
        return "shell_plan";
    }
    if (lowered.find("runbook") != std::string::npos || lowered.find("validation") != std::string::npos) {
        return "runbook_plan";
    }
    if (rendered.find(':') != std::string::npos && rendered.find('\n') != std::string::npos) {
        return "config_plan";
    }
    return rendered.empty() ? "unknown_manual_review" : "text_artifact";
}

int run_jinja(const std::vector<std::string>& args) {
    std::vector<std::string> positional;
    CommonCliOptions options = parse_common_options(args, 2, &positional);
    if (positional.size() < 2) {
        throw std::runtime_error("jinja requires inspect|render|plan|execute <file.j2>.");
    }
    const std::string command = positional[0];
    const std::filesystem::path template_path = positional[1];
    std::filesystem::path vars_path;
    bool confirm = false;
    for (std::size_t index = 2; index < positional.size(); ++index) {
        if (positional[index] == "--vars") {
            if (index + 1 >= positional.size()) {
                throw std::runtime_error("--vars requires a JSON file.");
            }
            vars_path = positional[++index];
        } else if (positional[index] == "--confirm") {
            confirm = true;
        } else {
            throw std::runtime_error("unexpected jinja argument `" + positional[index] + "`.");
        }
    }
    const std::string source = read_text_file_local(template_path);
    const std::vector<std::string> variables = extract_jinja_variables(source);
    const std::vector<std::string> risks = detect_jinja_risks(source);
    const std::map<std::string, std::string> vars = vars_path.empty() ? std::map<std::string, std::string>{}
                                                                     : parse_flat_json_vars(vars_path);
    std::vector<std::string> unresolved;
    const std::string rendered = render_jinja_passthrough(source, vars, &unresolved);
    const std::string plan_type = classify_jinja_plan(rendered);
    const bool safe = risks.empty() && unresolved.empty();

    if (command == "execute") {
        std::cout << "jinja_execute_rejected: Jinja execution is disabled for arbitrary rendered text in this phase.\n";
        std::cout << "plan-type: " << plan_type << "\n";
        std::cout << "confirm-present: " << (confirm ? "yes" : "no") << "\n";
        std::cout << "next: Use `omnix jinja plan <file.j2> --vars <vars.json>` and convert safe output into an allowlisted runbook.\n";
        return 1;
    }

    if (command == "render" || command == "plan") {
        if (!options.output_path.empty()) {
            write_text_file_local(options.output_path, rendered);
        }
    }

    if (options.output_mode == OutputMode::Compact) {
        if (command == "inspect") {
            std::cout << "jinja_inspected: " << variables.size() << " variable(s), " << risks.size() << " risk marker(s).\n";
        } else if (command == "render") {
            std::cout << "jinja_rendered: rendered with built-in passthrough renderer.\n";
        } else if (command == "plan") {
            std::cout << "jinja_planned: " << plan_type << "\n";
        } else {
            throw std::runtime_error("jinja supports inspect, render, plan, and execute.");
        }
        std::cout << "template: " << template_path.string() << "\n";
        if (!options.output_path.empty()) {
            std::cout << "artifact: " << options.output_path << "\n";
        }
        std::cout << "status: " << (safe ? "safe" : "manual_review") << "\n";
        return safe ? 0 : 0;
    }

    std::cout << "OmniX Jinja Passthrough\n";
    std::cout << " - Status: " << (safe ? "safe" : "manual_review") << "\n";
    std::cout << " - Template: " << template_path << "\n";
    std::cout << " - Renderer: built_in_passthrough\n";
    std::cout << " - Plan type: " << plan_type << "\n";
    std::cout << " - Variables:\n";
    for (const std::string& variable : variables) {
        std::cout << "   * " << variable << "\n";
    }
    if (!unresolved.empty()) {
        std::cout << " - Unresolved variables:\n";
        for (const std::string& variable : unresolved) {
            std::cout << "   * " << variable << "\n";
        }
    }
    if (!risks.empty()) {
        std::cout << " - Risk markers:\n";
        for (const std::string& risk : risks) {
            std::cout << "   * " << risk << "\n";
        }
    }
    if (command == "render" || command == "plan") {
        std::cout << " - Rendered preview:\n" << rendered << "\n";
    }
    if (!options.output_path.empty()) {
        std::cout << " - Artifact: " << options.output_path << "\n";
    }
    return 0;
}

std::string render_node_heartbeat_json(const tze::InstanceIdentityReport& identity) {
    std::ostringstream out;
    out << "{\"event_type\":\"omnix.node.heartbeat.v1\""
        << ",\"timestamp\":\"" << json_escape(now_timestamp_cli()) << "\""
        << ",\"nodeId\":\"" << json_escape(identity.instance_id) << "\""
        << ",\"fingerprint\":\"" << json_escape(identity.fingerprint) << "\""
        << ",\"trustStatus\":\"local_unenrolled\""
        << ",\"grains\":{"
        << "\"platform\":\"" << json_escape(identity.platform) << "\","
        << "\"architecture\":\"" << json_escape(identity.architecture) << "\","
        << "\"omnixVersion\":\"" << json_escape(OMNIX_VERSION) << "\","
        << "\"tview\":true,"
        << "\"defendDetect\":true,"
        << "\"gg\":" << (command_available("ghostline_cli") || std::filesystem::exists("build/vendor/ghostline-gate/ghostline_cli") ? "true" : "false") << ","
        << "\"vi\":" << (command_available("vi") ? "true" : "false") << ","
        << "\"vim\":" << (command_available("vim") ? "true" : "false") << ","
        << "\"nvim\":" << (command_available("nvim") ? "true" : "false") << ","
        << "\"brew\":" << (command_available("brew") ? "true" : "false") << ","
        << "\"aptGet\":" << (command_available("apt-get") ? "true" : "false")
        << "},\"healthSummary\":\"local node profile ready; no daemon or remote execution active\"}";
    return out.str();
}

int run_node(const std::vector<std::string>& args) {
    std::vector<std::string> positional;
    const CommonCliOptions options = parse_common_options(args, 2, &positional);
    if (positional.empty()) {
        throw std::runtime_error("node requires doctor, id, status, heartbeat, or enroll.");
    }
    const std::string command = positional.front();
    const tze::InstanceIdentityReport identity = tze::resolve_instance_identity(options.memory_root_path);
    const std::filesystem::path root = salt_control_root_for(options);

    if (command == "doctor") {
        std::cout << "node_ready: OmniX node profile support is available without a daemon.\n";
        std::cout << "node-id: " << identity.instance_id << "\n";
        std::cout << "next: Run `omnix node heartbeat --out <file>` or `omnix node enroll --out <file>`.\n";
        return 0;
    }
    if (command == "id") {
        std::cout << "node_identity_ready: " << identity.instance_id << "\n";
        std::cout << "fingerprint: " << identity.fingerprint << "\n";
        return 0;
    }
    if (command == "status") {
        std::cout << "node_status: local_unenrolled\n";
        std::cout << "node-id: " << identity.instance_id << "\n";
        std::cout << "master: not_configured\n";
        std::cout << "daemon: not_required_v1\n";
        return 0;
    }
    if (command == "heartbeat") {
        const std::string heartbeat = render_node_heartbeat_json(identity);
        const std::filesystem::path out_path = options.output_path.empty()
            ? (root / "node-heartbeat.json")
            : std::filesystem::path(options.output_path);
        write_text_file_local(out_path, heartbeat + "\n");
        std::cout << "node_heartbeat_written: " << out_path.string() << "\n";
        std::cout << "node-id: " << identity.instance_id << "\n";
        return 0;
    }
    if (command == "enroll") {
        const std::filesystem::path out_path = options.output_path.empty()
            ? (root / "enrollment-request.json")
            : std::filesystem::path(options.output_path);
        std::ostringstream out;
        out << "{\"event_type\":\"omnix.node.enrollment.v1\",\"timestamp\":\"" << json_escape(now_timestamp_cli())
            << "\",\"nodeId\":\"" << json_escape(identity.instance_id)
            << "\",\"fingerprint\":\"" << json_escape(identity.fingerprint)
            << "\",\"status\":\"pending_master_approval\"}\n";
        write_text_file_local(out_path, out.str());
        std::cout << "node_enrollment_written: " << out_path.string() << "\n";
        std::cout << "fingerprint: " << identity.fingerprint << "\n";
        return 0;
    }
    throw std::runtime_error("node supports doctor, id, status, heartbeat, and enroll.");
}

std::string job_id_for(std::string_view job_type, std::string_view target) {
    return "job-" + std::to_string(std::hash<std::string>{}(std::string(job_type) + "|" + std::string(target) + "|" + now_timestamp_cli()));
}

int run_master(const std::vector<std::string>& args) {
    std::vector<std::string> positional;
    const CommonCliOptions options = parse_common_options(args, 2, &positional);
    if (positional.empty()) {
        throw std::runtime_error("master requires doctor, init, node, or job.");
    }
    const std::filesystem::path root = salt_control_root_for(options);
    const std::filesystem::path nodes_path = root / "nodes.jsonl";
    const std::filesystem::path jobs_dir = root / "jobs";
    const std::string command = positional[0];

    if (command == "doctor") {
        std::cout << "master_ready: file-spool master design is available; network transport is deferred.\n";
        std::cout << "root: " << root.string() << "\n";
        std::cout << "next: Run `omnix master init`, then approve explicit node fingerprints.\n";
        return 0;
    }
    if (command == "init") {
        std::filesystem::create_directories(jobs_dir);
        write_text_file_local(root / "master.json",
                              "{\"event_type\":\"omnix.master.config.v1\",\"mode\":\"file_spool\",\"remoteShell\":false}\n");
        std::cout << "master_initialized: " << root.string() << "\n";
        return 0;
    }
    if (command == "node") {
        if (positional.size() < 2) {
            throw std::runtime_error("master node requires list or approve <fingerprint>.");
        }
        if (positional[1] == "list") {
            std::cout << "master_nodes: " << nodes_path.string() << "\n";
            std::ifstream input(nodes_path);
            std::string line;
            bool any = false;
            while (std::getline(input, line)) {
                if (!line.empty()) {
                    any = true;
                    std::cout << line << "\n";
                }
            }
            if (!any) {
                std::cout << "node_list_empty: no approved nodes yet.\n";
            }
            return 0;
        }
        if (positional[1] == "approve") {
            if (positional.size() < 3) {
                throw std::runtime_error("master node approve requires a fingerprint.");
            }
            std::filesystem::create_directories(root);
            std::ofstream output(nodes_path, std::ios::app);
            output << "{\"event_type\":\"omnix.master.node.v1\",\"fingerprint\":\"" << json_escape(positional[2])
                   << "\",\"trustStatus\":\"approved\",\"approvedAt\":\"" << json_escape(now_timestamp_cli()) << "\"}\n";
            std::cout << "node_approved: " << positional[2] << "\n";
            return 0;
        }
        throw std::runtime_error("master node supports list and approve.");
    }
    if (command == "job") {
        if (positional.size() < 2) {
            throw std::runtime_error("master job requires plan, dispatch, or status.");
        }
        if (positional[1] == "plan") {
            if (positional.size() < 3) {
                throw std::runtime_error("master job plan requires a job type.");
            }
            std::string target = options.build_target.empty() ? "local" : options.build_target;
            for (std::size_t index = 3; index < positional.size(); ++index) {
                if (positional[index] == "--target") {
                    if (index + 1 >= positional.size()) {
                        throw std::runtime_error("--target requires a node id.");
                    }
                    target = positional[++index];
                }
            }
            const std::string job_type = positional[2];
            const std::vector<std::string> allowed = {
                "defend.detect", "tview.doctor", "gg.search", "thresholds.evaluate", "jinja.render", "jinja.plan"
            };
            if (std::find(allowed.begin(), allowed.end(), job_type) == allowed.end()) {
                throw std::runtime_error("master job plan supports only read-only/planning job types in this phase.");
            }
            const std::string id = job_id_for(job_type, target);
            const std::filesystem::path job_path = options.output_path.empty() ? (jobs_dir / (id + ".json"))
                                                                               : std::filesystem::path(options.output_path);
            std::ostringstream out;
            out << "{\"event_type\":\"omnix.master.job.v1\",\"jobId\":\"" << json_escape(id)
                << "\",\"jobType\":\"" << json_escape(job_type)
                << "\",\"target\":\"" << json_escape(target)
                << "\",\"status\":\"planned_not_dispatched\",\"createdAt\":\"" << json_escape(now_timestamp_cli())
                << "\",\"remoteMutation\":false}\n";
            write_text_file_local(job_path, out.str());
            std::cout << "master_job_planned: " << id << "\n";
            std::cout << "artifact: " << job_path.string() << "\n";
            return 0;
        }
        if (positional[1] == "dispatch") {
            std::cout << "master_dispatch_disabled: network dispatch is deferred; file-spool job plans are created but not remotely executed.\n";
            return 1;
        }
        if (positional[1] == "status") {
            std::cout << "master_job_status: file-spool\n";
            if (std::filesystem::exists(jobs_dir)) {
                for (const auto& entry : std::filesystem::directory_iterator(jobs_dir)) {
                    if (entry.is_regular_file()) {
                        std::cout << "job: " << entry.path().string() << "\n";
                    }
                }
            }
            return 0;
        }
        throw std::runtime_error("master job supports plan, dispatch, and status.");
    }
    throw std::runtime_error("master supports doctor, init, node, and job.");
}

int run_next(const std::vector<std::string>& args) {
    std::vector<std::string> positional;
    const CommonCliOptions options = parse_common_options(args, 2, &positional);
    const std::string reference = positional.empty() ? "latest" : positional.front();
    tze::MemoryStore memory_store;
    const tze::MemorySnapshot memory = memory_store.load(options.memory_root_path);
    const std::string run_id = memory_store.resolve_tze_run_id(memory, reference);
    if (run_id.empty()) {
        if (options.output_mode == OutputMode::Compact) {
            std::cout << "next_missing: No TZE run was available for `" << reference << "`.\n";
        } else {
            std::cout << "No TZE run was available for `" << reference << "`.\n";
        }
        return 1;
    }
    const tze::TzeRunRecord* run = memory_store.find_tze_run(memory, run_id);
    if (run == nullptr) {
        throw std::runtime_error("next could not resolve the requested TZE run.");
    }
    const std::string next_action = !run->next_action.empty()
        ? run->next_action
        : "Run `omnix why " + run_id + "` to derive the next safe action from the stored run.";
    if (options.output_mode == OutputMode::Compact) {
        std::cout << "next_action: " << next_action << "\n";
        std::cout << "source-run: " << run_id << "\n";
        std::cout << "status: " << (run->status.empty() ? "unknown" : run->status) << "\n";
        if (!run->prompt.empty()) {
            std::cout << "prompt: " << run->prompt << "\n";
        }
        return 0;
    }

    std::cout << "Next action for `" << run_id << "`:\n";
    std::cout << " - Action: " << next_action << "\n";
    std::cout << " - Status: " << (run->status.empty() ? "unknown" : run->status) << "\n";
    if (!run->intent.empty()) {
        std::cout << " - Intent: " << run->intent << "\n";
    }
    if (!run->prompt.empty()) {
        std::cout << " - Prompt: " << run->prompt << "\n";
    }
    std::cout << " - Rationale: `next` reads the compact TZE ledger and avoids replaying the full chain.\n";
    return 0;
}

int run_review_command(const std::vector<std::string>& args, bool patch_mode) {
    std::vector<std::string> positional;
    const CommonCliOptions options = parse_common_options(args, 2, &positional);
    if (positional.empty()) {
        throw std::runtime_error(std::string(patch_mode ? "patch-proposal" : "review") + " requires a path or module target.");
    }

    const std::string target = join_positional_arguments(positional);
    tze::RequestProfile profile = make_base_profile(options);
    profile.raw_prompt = std::string(patch_mode ? "patch-proposal " : "review ") + target;
    profile.review_target = target;
    profile.resolved_intent = patch_mode ? tze::RequestIntent::PatchProposal : tze::RequestIntent::ReviewModule;
    if (profile.source_map_path == "res/tze.txt") {
        const std::filesystem::path candidate = optional_source_file();
        profile.source_map_path = candidate.empty() ? std::string{} : candidate.string();
    }

    tze::ProcessingEngine engine;
    const tze::ProcessingReport report = engine.process(profile);
    print_processing_report(report, options.output_mode, true);
    return (patch_mode ? report.answer_status == "patch_proposal_ready" : report.answer_status == "review_ready") ? 0 : 1;
}

int run_memory(const std::vector<std::string>& args) {
    std::vector<std::string> positional;
    const CommonCliOptions options = parse_common_options(args, 2, &positional);
    if (!positional.empty() && positional.front() == "reset-context") {
        return reset_runtime_context(options);
    }
    if (!positional.empty() && positional.front() == "prune-expired") {
        tze::MemoryStore memory_store;
        tze::MemorySnapshot memory = memory_store.load(options.memory_root_path);
        const std::string summary = memory_store.prune_expired(memory);
        memory_store.persist_snapshot(memory);
        if (options.output_mode == OutputMode::Compact) {
            std::cout << "memory_pruned_expired: " << summary << "\n";
        } else {
            std::cout << summary << "\n";
        }
        return 0;
    }

    tze::RequestProfile profile = make_base_profile(options);
    if (!positional.empty() && positional.front() == "prune") {
        profile.raw_prompt = "memory prune";
        profile.resolved_intent = tze::RequestIntent::PruneMemory;
    } else {
        profile.raw_prompt = positional.empty() ? "memory history" : "memory " + positional.front();
        profile.memory_view = positional.empty() ? "history" : positional.front();
        profile.resolved_intent = tze::RequestIntent::ShowMemory;
    }
    profile.source_map_path.clear();

    tze::ProcessingEngine engine;
    const tze::ProcessingReport report = engine.process(profile);
    print_processing_report(report, options.output_mode, true);
    return 0;
}

int run_tze(const std::vector<std::string>& args) {
    std::vector<std::string> positional;
    const CommonCliOptions options = parse_common_options(args, 2, &positional);
    if (positional.empty()) {
        throw std::runtime_error("tze requires `runs`, `latest`, `replay`, `chain`, `diff`, `diff-latest`, `explain-change`, `explain-change-latest`, `report`, `diff-report`, `export`, `import`, `prune`, or `mark`.");
    }

    auto run_profile = [&](tze::RequestProfile profile, std::string_view success_status) -> int {
        tze::ProcessingEngine engine;
        const tze::ProcessingReport report = engine.process(profile);
        print_processing_report(report, options.output_mode, true);
        return report.answer_status == success_status ? 0 : 1;
    };

    if (positional.front() == "runs") {
        tze::RequestProfile profile = make_base_profile(options);
        profile.raw_prompt = "memory runs";
        profile.memory_view = "runs";
        profile.resolved_intent = tze::RequestIntent::ShowMemory;
        profile.source_map_path.clear();

        return run_profile(profile, "memory");
    }

    if (positional.front() == "latest") {
        tze::RequestProfile profile = make_base_profile(options);
        profile.raw_prompt = "tze latest";
        profile.tze_run_reference = options.important_only ? "latest-important" : "latest";
        profile.resolved_intent = tze::RequestIntent::ReplayTzeRun;
        profile.source_map_path.clear();
        return run_profile(profile, "tze_run_replayed");
    }

    if (positional.front() == "replay") {
        if (positional.size() < 2) {
            throw std::runtime_error("tze replay requires a run id.");
        }
        tze::RequestProfile profile = make_base_profile(options);
        profile.raw_prompt = "tze replay " + positional[1];
        profile.tze_run_reference = positional[1];
        profile.resolved_intent = tze::RequestIntent::ReplayTzeRun;
        profile.source_map_path.clear();
        return run_profile(profile, "tze_run_replayed");
    }

    if (positional.front() == "chain") {
        if (positional.size() < 2) {
            throw std::runtime_error("tze chain requires a run id or `latest`.");
        }
        tze::RequestProfile profile = make_base_profile(options);
        profile.raw_prompt = "tze chain " + positional[1];
        profile.tze_run_reference = positional[1];
        profile.resolved_intent = tze::RequestIntent::ChainTzeRun;
        profile.source_map_path.clear();
        return run_profile(profile, "tze_chain_rendered");
    }

    if (positional.front() == "diff") {
        if (positional.size() < 3) {
            throw std::runtime_error("tze diff requires two run ids.");
        }
        tze::RequestProfile profile = make_base_profile(options);
        profile.raw_prompt = "tze diff " + positional[1] + " " + positional[2];
        profile.tze_run_reference = positional[1];
        profile.tze_compare_reference = positional[2];
        profile.resolved_intent = tze::RequestIntent::DiffTzeRuns;
        profile.source_map_path.clear();
        return run_profile(profile, "tze_run_diffed");
    }

    if (positional.front() == "diff-latest") {
        tze::RequestProfile profile = make_base_profile(options);
        profile.raw_prompt = "tze diff-latest";
        profile.tze_run_reference = options.important_only ? "latest-important" : "latest";
        profile.tze_compare_reference = options.important_only ? "previous-important" : "previous";
        profile.resolved_intent = tze::RequestIntent::DiffTzeRuns;
        profile.source_map_path.clear();
        return run_profile(profile, "tze_run_diffed");
    }

    if (positional.front() == "explain-change") {
        if (positional.size() < 3) {
            throw std::runtime_error("tze explain-change requires two run ids.");
        }
        tze::RequestProfile profile = make_base_profile(options);
        profile.raw_prompt = "tze explain-change " + positional[1] + " " + positional[2];
        profile.tze_run_reference = positional[1];
        profile.tze_compare_reference = positional[2];
        profile.resolved_intent = tze::RequestIntent::ExplainTzeChange;
        profile.source_map_path.clear();
        return run_profile(profile, "tze_change_explained");
    }

    if (positional.front() == "explain-change-latest") {
        tze::RequestProfile profile = make_base_profile(options);
        profile.raw_prompt = "tze explain-change-latest";
        profile.tze_run_reference = options.important_only ? "latest-important" : "latest";
        profile.tze_compare_reference = options.important_only ? "previous-important" : "previous";
        profile.resolved_intent = tze::RequestIntent::ExplainTzeChange;
        profile.source_map_path.clear();
        return run_profile(profile, "tze_change_explained");
    }

    if (positional.front() == "report") {
        if (positional.size() < 2) {
            throw std::runtime_error("tze report requires a run id.");
        }
        tze::RequestProfile profile = make_base_profile(options);
        profile.raw_prompt = "tze report " + positional[1];
        profile.tze_run_reference = positional[1];
        profile.resolved_intent = tze::RequestIntent::ReportTzeRun;
        profile.source_map_path.clear();
        return run_profile(profile, "tze_report_written");
    }

    if (positional.front() == "diff-report") {
        if (positional.size() < 3) {
            throw std::runtime_error("tze diff-report requires two run ids.");
        }
        tze::RequestProfile profile = make_base_profile(options);
        profile.raw_prompt = "tze diff-report " + positional[1] + " " + positional[2];
        profile.tze_run_reference = positional[1];
        profile.tze_compare_reference = positional[2];
        profile.resolved_intent = tze::RequestIntent::DiffReportTzeRuns;
        profile.source_map_path.clear();
        return run_profile(profile, "tze_diff_report_written");
    }

    if (positional.front() == "export") {
        if (positional.size() < 2) {
            throw std::runtime_error("tze export requires a run id or `latest`.");
        }
        tze::RequestProfile profile = make_base_profile(options);
        profile.raw_prompt = "tze export " + positional[1];
        profile.tze_run_reference = positional[1];
        profile.resolved_intent = tze::RequestIntent::ExportTzeBundle;
        profile.source_map_path.clear();
        return run_profile(profile, "tze_bundle_written");
    }

    if (positional.front() == "import") {
        if (positional.size() < 2) {
            throw std::runtime_error("tze import requires a bundle path.");
        }
        tze::RequestProfile profile = make_base_profile(options);
        profile.raw_prompt = "tze import " + positional[1];
        profile.tze_run_reference = positional[1];
        profile.resolved_intent = tze::RequestIntent::ImportTzeBundle;
        profile.source_map_path.clear();
        return run_profile(profile, "tze_bundle_imported");
    }

    if (positional.front() == "prune") {
        tze::RequestProfile profile = make_base_profile(options);
        profile.raw_prompt = "tze prune";
        profile.resolved_intent = tze::RequestIntent::PruneTzeRuns;
        profile.source_map_path.clear();
        return run_profile(profile, "tze_pruned");
    }

    if (positional.front() == "mark") {
        if (positional.size() < 3) {
            throw std::runtime_error("tze mark requires a run id and either `helpful` or `not-helpful`.");
        }
        if (positional[2] != "helpful" && positional[2] != "not-helpful") {
            throw std::runtime_error("tze mark requires `helpful` or `not-helpful` as the feedback value.");
        }
        tze::RequestProfile profile = make_base_profile(options);
        profile.raw_prompt = "tze mark " + positional[1] + " " + positional[2];
        profile.tze_run_reference = positional[1];
        profile.feedback_value = positional[2];
        profile.resolved_intent = tze::RequestIntent::MarkTzeRunOutcome;
        profile.source_map_path.clear();
        return run_profile(profile, "tze_feedback_recorded");
    }

    throw std::runtime_error("tze requires `runs`, `latest`, `replay`, `chain`, `diff`, `diff-latest`, `explain-change`, `explain-change-latest`, `report`, `diff-report`, `export`, `import`, `prune`, or `mark`.");
}

struct ShellState {
    CommonCliOptions options;
    bool assist_enabled = false;
    bool full_tool_output = true;
    std::string current_case_id;
    std::string current_incident_id;
    std::string current_run_id;
    std::optional<tze::ProcessingReport> last_report;
};

void reset_shell_context(ShellState& state) {
    state.current_case_id.clear();
    state.current_incident_id.clear();
    state.current_run_id.clear();
    state.last_report.reset();
}

void print_shell_help() {
    std::cout << "OmniX shell commands:\n";
    std::cout << " - /help\n";
    std::cout << " - /status\n";
    std::cout << " - /provider\n";
    std::cout << " - /api [status|openai|ollama|template huggingface]\n";
    std::cout << " - /assist on|off\n";
    std::cout << " - /verbose on|off\n";
    std::cout << " - /why\n";
    std::cout << " - /next\n";
    std::cout << " - /reset [memory]\n";
    std::cout << " - /case <id>\n";
    std::cout << " - /incident <id>\n";
    std::cout << " - /replay [run-id|latest]\n";
    std::cout << " - /report [run-id|latest]\n";
    std::cout << " - /diff <left-run-id> <right-run-id>\n";
    std::cout << " - /quit\n";
    std::cout << "Persona shortcuts: premise mode, cynic mode, professional mode, neutral mode.\n";
    std::cout << "Plain input is routed through the normal OmniX intent resolver.\n";
}

std::string shell_prompt(const ShellState& state) {
    std::ostringstream prompt;
    prompt << "omnix";
    if (!state.current_case_id.empty()) {
        prompt << "[case:" << state.current_case_id << "]";
    }
    if (state.assist_enabled) {
        prompt << "[assist]";
    }
    prompt << "> ";
    return prompt.str();
}

std::string expand_shell_input(std::string line, const ShellState& state) {
    line = trim(line);
    if (line.rfind("./build/omnix ", 0) == 0) {
        line = trim(line.substr(std::string("./build/omnix ").size()));
    } else if (line.rfind("build/omnix ", 0) == 0) {
        line = trim(line.substr(std::string("build/omnix ").size()));
    } else if (line.rfind("omnix ", 0) == 0) {
        line = trim(line.substr(std::string("omnix ").size()));
    }

    std::string lowered;
    lowered.reserve(line.size());
    for (char c : line) {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    if (lowered.rfind("ollama,", 0) == 0) {
        line = trim(line.substr(std::string("ollama,").size()));
        lowered = lowered.substr(std::string("ollama,").size());
        lowered = trim(lowered);
    } else if (lowered.rfind("ollama ", 0) == 0) {
        line = trim(line.substr(std::string("ollama ").size()));
        lowered = lowered.substr(std::string("ollama ").size());
        lowered = trim(lowered);
    }

    if (lowered == "provider") {
        return "provider probe";
    }
    if (lowered.rfind("ask ", 0) == 0) {
        line = trim(line.substr(std::string("ask ").size()));
        lowered = trim(lowered.substr(std::string("ask ").size()));
    }
    if (line == "case" && !state.current_case_id.empty()) {
        return "case " + state.current_case_id;
    }
    if (line == "decide" && !state.current_case_id.empty()) {
        return "decide " + state.current_case_id;
    }
    if (line == "incident" && !state.current_incident_id.empty()) {
        return "incident " + state.current_incident_id;
    }
    return line;
}

std::string normalized_shell_input(std::string line,
                                   ShellState& state,
                                   const tze::MemorySnapshot& memory,
                                   std::optional<tze::ShellLexiconEntry>* matched_entry = nullptr) {
    line = expand_shell_input(std::move(line), state);
    const std::string original_line = line;
    tze::ShellLexicon lexicon;
    const std::optional<tze::ShellLexiconEntry> entry = lexicon.normalize(line, memory, true);
    if (!entry.has_value()) {
        if (matched_entry != nullptr) {
            matched_entry->reset();
        }
        return line;
    }
    if (matched_entry != nullptr) {
        *matched_entry = entry;
    }
    if (entry->category == "verbosity_full") {
        state.full_tool_output = true;
        return "__shell_pref_full__";
    }
    if (entry->category == "verbosity_compact") {
        state.full_tool_output = false;
        return "__shell_pref_compact__";
    }
    if (entry->category == "general_definition_query") {
        const std::string lowered = lowercase(original_line);
        if (lowered.rfind("what is ", 0) == 0 ||
            lowered.rfind("define ", 0) == 0 ||
            lowered.find("meaning of ") != std::string::npos ||
            lowered.find("what does ") != std::string::npos) {
            return original_line;
        }

        std::string domain_hint;
        for (const std::string& note : entry->correction_notes) {
            if (note.rfind("domain_hint=", 0) == 0) {
                domain_hint = note.substr(std::string("domain_hint=").size());
                break;
            }
        }

        std::string explicit_prompt = "What is " + entry->canonical;
        if (!domain_hint.empty()) {
            explicit_prompt += " in terms of " + domain_hint;
        }
        return explicit_prompt;
    }
    return entry->canonical;
}

bool shell_requests_last_results(std::string_view line, const ShellState& state) {
    const std::string trimmed = trim(line);
    std::string lowered;
    lowered.reserve(trimmed.size());
    for (char c : trimmed) {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }

    if (lowered == "results" || lowered == "last results") {
        return true;
    }
    if (lowered.find("run ") != std::string::npos || lowered.find("use ") != std::string::npos ||
        lowered.find("scan") != std::string::npos) {
        return false;
    }
    if (!state.last_report.has_value()) {
        return false;
    }

    if (!state.last_report->resolved_project.empty()) {
        std::string project = state.last_report->resolved_project;
        std::transform(project.begin(), project.end(), project.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (lowered == project + " results") {
            return true;
        }
        if (lowered.find(project) != std::string::npos && lowered.find("results") != std::string::npos) {
            return true;
        }
    }
    if (state.last_report->tool_invocation_report.has_value() &&
        !state.last_report->tool_invocation_report->logical_name.empty()) {
        std::string logical_name = state.last_report->tool_invocation_report->logical_name;
        std::transform(logical_name.begin(), logical_name.end(), logical_name.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (lowered == logical_name + " results") {
            return true;
        }
        if (lowered.find(logical_name) != std::string::npos && lowered.find("results") != std::string::npos) {
            return true;
        }
    }
    if (lowered.find("where are my") != std::string::npos && lowered.find("results") != std::string::npos) {
        return true;
    }
    return false;
}

void print_shell_last_results(const ShellState& state) {
    if (!state.last_report.has_value()) {
        std::cout << "No prior results are available in this shell.\n";
        return;
    }

    const tze::ProcessingReport& report = *state.last_report;
    if (report.tool_invocation_report.has_value()) {
        const tze::ToolInvocationReport& invocation = *report.tool_invocation_report;
        std::cout << "Last results:\n";
        if (!invocation.command_line.empty()) {
            std::cout << " - command: " << invocation.command_line << "\n";
        }
        if (!invocation.output_excerpt.empty()) {
            for (const std::string& line : invocation.output_excerpt) {
                std::cout << " - " << line << "\n";
            }
        } else {
            std::cout << " - No tool output was captured.\n";
        }
        return;
    }

    print_processing_report(report, OutputMode::Verbose, true);
}

void print_shell_status(const ShellState& state) {
    std::cout << "Shell status:\n";
    std::cout << " - Assist mode: " << (state.assist_enabled ? "on" : "off") << "\n";
    std::cout << " - Output mode: "
              << (state.options.output_mode == OutputMode::Verbose
                      ? "verbose"
                      : (state.options.output_mode == OutputMode::Compact ? "compact" : "auto"))
              << "\n";
    std::cout << " - Tool results: " << (state.full_tool_output ? "full" : "compact") << "\n";
    std::cout << " - Memory root: "
              << (state.options.memory_root_path.empty() ? std::string("(default)") : state.options.memory_root_path) << "\n";
    std::cout << " - Source map: "
              << (state.options.source_map_path.empty() ? std::string("(auto)") : state.options.source_map_path) << "\n";
    if (!state.current_case_id.empty()) {
        std::cout << " - Current case: " << state.current_case_id << "\n";
    }
    if (!state.current_incident_id.empty()) {
        std::cout << " - Current incident: " << state.current_incident_id << "\n";
    }
    if (!state.current_run_id.empty()) {
        std::cout << " - Current run: " << state.current_run_id << "\n";
    }
}

void update_shell_state(ShellState& state, const tze::ProcessingReport& report, std::string_view expanded_input) {
    state.last_report = report;
    if (!report.tze_run_id.empty()) {
        state.current_run_id = report.tze_run_id;
    }
    if (report.case_record.has_value()) {
        state.current_case_id = report.case_record->id;
    }
    const std::string line = trim(expanded_input);
    if (line.rfind("incident ", 0) == 0 && line != "incident list" && line.rfind("incident report ", 0) != 0) {
        state.current_incident_id = trim(line.substr(std::string("incident ").size()));
    }
}

void print_shell_followup(const ShellState& state, std::string_view request) {
    const std::string lowered = trim(request);
    if (!state.last_report.has_value()) {
        if (lowered == "fix this") {
            std::cout << "Nothing to fix yet. Run a command first and I’ll help repair it.\n";
        } else {
            std::cout << "No prior command is available yet. Try `help`, `Run NMAP`, or `secure my system`.\n";
        }
        return;
    }

    const tze::ProcessingReport& report = *state.last_report;
    if (lowered == "fix this") {
        if (report.answer_status == "unknown_intent") {
            std::cout << "I didn’t parse the last request cleanly.\n";
            if (!report.next_action.empty()) {
                std::cout << "Try this instead: " << report.next_action << "\n";
            }
            return;
        }
        std::cout << "The last command did not fail in a way that needs repair. ";
        std::cout << (!report.next_action.empty() ? report.next_action : "Try `Next?` for a follow-up.") << "\n";
        return;
    }

    if (!report.next_action.empty()) {
        std::cout << report.next_action << "\n";
        return;
    }
    std::cout << "Review the last result with `/why` or run another guarded command.\n";
}

bool shell_validate_next_step_plan(const tze::NextStepAssistPlan& plan) {
    return !plan.suggested_next_step.empty() && plan.confidence >= 0.0 && plan.confidence <= 1.0;
}

void print_shell_assist_followup(const ShellState& state,
                                 std::string_view request,
                                 tze::MemoryStore& memory_store,
                                 tze::MemorySnapshot memory) {
    if (!state.assist_enabled || !state.last_report.has_value()) {
        return;
    }
    std::unique_ptr<tze::ReasoningProvider> provider = tze::make_reasoning_provider_from_env();
    if (provider == nullptr) {
        return;
    }
    const std::string deterministic_guidance = state.last_report->next_action.empty()
        ? state.last_report->answer_explanation
        : state.last_report->next_action;
    const std::optional<tze::NextStepAssistPlan> proposed =
        provider->propose_next_step(request, deterministic_guidance);
    if (!proposed.has_value() || !shell_validate_next_step_plan(*proposed)) {
        return;
    }
    std::cout << "Assist next: " << proposed->suggested_next_step << "\n";
    if (!proposed->safer_alternative.empty()) {
        std::cout << "Assist safer alternative: " << proposed->safer_alternative << "\n";
    }
    tze::AssistOutcomeRecord outcome;
    outcome.id = "assist-outcome-shell-" + std::to_string(std::hash<std::string>{}(std::string(request) + deterministic_guidance));
    outcome.task_type = "shell_followup";
    outcome.plan_type = "next_step";
    outcome.provider_id = proposed->provider_id;
    outcome.model = proposed->model;
    outcome.status = "assist_used";
    outcome.target_label = state.current_run_id;
    outcome.canonical_value = proposed->suggested_next_step;
    outcome.host_platform =
#if defined(__APPLE__)
        "macos";
#elif defined(__linux__)
        "linux";
#else
        "unknown";
#endif
    outcome.persisted_at = "shell-session";
    memory_store.remember_assist_outcome(memory, outcome);
    memory_store.persist_snapshot(memory);
}

int run_shell(const std::vector<std::string>& args) {
    std::vector<std::string> positional;
    ShellState state;
    state.options = parse_common_options(args, 2, &positional);
    state.assist_enabled = state.options.assist;

    tze::ProcessingEngine engine;
    tze::MemoryStore memory_store;
    std::cout << "OmniX shell started. Type /help for commands.\n";

    for (std::string line; std::cout << shell_prompt(state), std::getline(std::cin, line);) {
        line = trim(line);
        if (line.empty()) {
            continue;
        }
        const std::string original_line = line;

        const tze::MemorySnapshot shell_memory = memory_store.load(state.options.memory_root_path);
        std::optional<tze::ShellLexiconEntry> lexicon_entry;
        line = normalized_shell_input(line, state, shell_memory, &lexicon_entry);

        if (line == "__shell_pref_full__") {
            std::cout << "Full shell tool results enabled.\n";
            continue;
        }
        if (line == "__shell_pref_compact__") {
            std::cout << "Compact shell tool results enabled.\n";
            continue;
        }
        if (line == "next" && lowercase(original_line) == "next?" && state.assist_enabled && state.last_report.has_value()) {
            print_shell_followup(state, line);
            print_shell_assist_followup(state, line, memory_store, shell_memory);
            continue;
        }
        if (line == "next" || line == "next?") {
            std::vector<std::string> next_args = {"omnix", "next"};
            if (state.options.output_mode == OutputMode::Compact) {
                next_args.push_back("--compact");
            } else if (state.options.output_mode == OutputMode::Verbose) {
                next_args.push_back("--verbose");
            }
            if (!state.options.memory_root_path.empty()) {
                next_args.push_back("--memory-root");
                next_args.push_back(state.options.memory_root_path);
            }
            next_args.push_back(state.current_run_id.empty() ? "latest" : state.current_run_id);
            run_next(next_args);
            continue;
        }
        if (line == "fix this") {
            print_shell_followup(state, line);
            print_shell_assist_followup(state, line, memory_store, shell_memory);
            continue;
        }

        if (line.front() == '/') {
            const std::vector<std::string> tokens = split_whitespace(line);
            const std::string command = tokens.front();
            if (command == "/quit" || command == "/exit") {
                std::cout << "Closing OmniX shell.\n";
                return 0;
            }
            if (command == "/help") {
                print_shell_help();
                continue;
            }
            if (command == "/status") {
                print_shell_status(state);
                continue;
            }
            if (command == "/reset") {
                if (tokens.size() >= 2 && tokens[1] == "memory") {
                    std::vector<std::string> memory_args = {"omnix", "memory"};
                    if (state.options.output_mode == OutputMode::Compact) {
                        memory_args.push_back("--compact");
                    } else if (state.options.output_mode == OutputMode::Verbose) {
                        memory_args.push_back("--verbose");
                    }
                    if (!state.options.memory_root_path.empty()) {
                        memory_args.push_back("--memory-root");
                        memory_args.push_back(state.options.memory_root_path);
                    }
                    memory_args.push_back("reset-context");
                    run_memory(memory_args);
                }
                reset_shell_context(state);
                std::cout << "Shell context reset. Persistent learned/runtime cache "
                          << (tokens.size() >= 2 && tokens[1] == "memory" ? "was also reset.\n" : "was not changed.\n");
                continue;
            }
            if (command == "/assist") {
                if (tokens.size() < 2) {
                    std::cout << "Assist mode is " << (state.assist_enabled ? "on" : "off") << ".\n";
                } else if (tokens[1] == "on") {
                    state.assist_enabled = true;
                    std::cout << "Assist mode enabled for guarded tasks.\n";
                } else if (tokens[1] == "off") {
                    state.assist_enabled = false;
                    std::cout << "Assist mode disabled; OmniX will stay deterministic-only.\n";
                } else {
                    std::cout << "Use `/assist on` or `/assist off`.\n";
                }
                continue;
            }
            if (command == "/verbose") {
                if (tokens.size() < 2) {
                    std::cout << "Output mode is "
                              << (state.options.output_mode == OutputMode::Verbose ? "verbose" : "compact") << ".\n";
                } else if (tokens[1] == "on") {
                    state.options.output_mode = OutputMode::Verbose;
                    std::cout << "Verbose output enabled.\n";
                } else if (tokens[1] == "off") {
                    state.options.output_mode = OutputMode::Compact;
                    std::cout << "Compact output enabled.\n";
                } else {
                    std::cout << "Use `/verbose on` or `/verbose off`.\n";
                }
                continue;
            }
            if (command == "/api") {
                std::vector<std::string> api_args = {"omnix", "api"};
                if (state.options.output_mode == OutputMode::Compact) {
                    api_args.push_back("--compact");
                } else if (state.options.output_mode == OutputMode::Verbose) {
                    api_args.push_back("--verbose");
                }
                if (tokens.size() == 1) {
                    api_args.push_back("status");
                } else if (tokens[1] == "openai" || tokens[1] == "ollama") {
                    api_args.push_back("configure");
                    api_args.push_back(tokens[1]);
                } else {
                    for (std::size_t index = 1; index < tokens.size(); ++index) {
                        api_args.push_back(tokens[index]);
                    }
                }
                run_api(api_args);
                continue;
            }
            if (command == "/case") {
                if (tokens.size() < 2) {
                    if (state.current_case_id.empty()) {
                        std::cout << "No current case is set.\n";
                    } else {
                        std::cout << "Current case: " << state.current_case_id << "\n";
                    }
                } else {
                    state.current_case_id = tokens[1];
                    std::cout << "Current case set to " << state.current_case_id << ".\n";
                }
                continue;
            }
            if (command == "/incident") {
                if (tokens.size() < 2) {
                    if (state.current_incident_id.empty()) {
                        std::cout << "No current incident is set.\n";
                    } else {
                        std::cout << "Current incident: " << state.current_incident_id << "\n";
                    }
                } else {
                    state.current_incident_id = tokens[1];
                    std::cout << "Current incident set to " << state.current_incident_id << ".\n";
                }
                continue;
            }
            if (command == "/why") {
                line = "why " + (state.current_run_id.empty() ? std::string("latest") : state.current_run_id);
            } else if (command == "/next") {
                std::vector<std::string> next_args = {"omnix", "next"};
                if (state.options.output_mode == OutputMode::Compact) {
                    next_args.push_back("--compact");
                } else if (state.options.output_mode == OutputMode::Verbose) {
                    next_args.push_back("--verbose");
                }
                if (!state.options.memory_root_path.empty()) {
                    next_args.push_back("--memory-root");
                    next_args.push_back(state.options.memory_root_path);
                }
                next_args.push_back(tokens.size() >= 2 ? tokens[1] : (state.current_run_id.empty() ? std::string("latest") : state.current_run_id));
                run_next(next_args);
                continue;
            } else if (command == "/provider") {
                line = "provider probe";
            } else if (command == "/replay") {
                line = "tze replay " + (tokens.size() >= 2 ? tokens[1] : std::string("latest"));
            } else if (command == "/report") {
                line = "tze report " + (tokens.size() >= 2 ? tokens[1] : std::string("latest"));
            } else if (command == "/diff") {
                if (tokens.size() < 3) {
                    std::cout << "Use `/diff <left-run-id> <right-run-id>`.\n";
                    continue;
                }
                line = "tze diff " + tokens[1] + " " + tokens[2];
            } else {
                std::cout << "Unknown shell command. Type /help for commands.\n";
                continue;
            }
        }

        const std::string expanded = expand_shell_input(line, state);
        if (shell_requests_last_results(expanded, state)) {
            print_shell_last_results(state);
            continue;
        }
        tze::RequestProfile profile = make_base_profile(state.options);
        profile.assist_requested = state.assist_enabled;
        profile.raw_prompt = expanded;
        if (expanded == "tensor" || expanded.rfind("tensor ", 0) == 0) {
            std::vector<std::string> tensor_tokens = split_whitespace(expanded);
            if (!tensor_tokens.empty()) {
                tensor_tokens.erase(tensor_tokens.begin());
            }
            profile.resolved_intent = tze::RequestIntent::TensorAction;
            profile.tool_mode = tze::ToolCommandMode::Run;
            profile.requested_tool_name = "tensor";
            profile.tool_arguments = std::move(tensor_tokens);
            profile.verbose_output = state.options.output_mode == OutputMode::Verbose;
        }
        if (profile.source_map_path == "res/tze.txt") {
            const std::filesystem::path candidate = optional_source_file();
            if (!candidate.empty()) {
                profile.source_map_path = candidate.string();
            } else {
                profile.source_map_path.clear();
            }
        }

        if (expanded.rfind("case ", 0) == 0 && expanded != "case list") {
            profile.analyst_reference = trim(expanded.substr(std::string("case ").size()));
        } else if (expanded.rfind("decide ", 0) == 0) {
            profile.analyst_reference = trim(expanded.substr(std::string("decide ").size()));
        } else if (expanded.rfind("incident ", 0) == 0 && expanded != "incident list" &&
                   expanded.rfind("incident report ", 0) != 0) {
            profile.incident_reference = trim(expanded.substr(std::string("incident ").size()));
        }
        if (lexicon_entry.has_value()) {
            profile.shell_correction_note = lexicon_entry->correction_notes.empty()
                ? std::string{}
                : lexicon_entry->correction_notes.front();
        }

        const tze::ProcessingReport report = engine.process(profile);
        print_processing_report(report, state.options.output_mode, false);
        if (state.full_tool_output && report.tool_invocation_report.has_value()) {
            const tze::ToolInvocationReport& invocation = *report.tool_invocation_report;
            if (invocation.output_excerpt.size() > 1) {
                std::cout << "output:\n";
                for (const std::string& output_line : invocation.output_excerpt) {
                    std::cout << " - " << output_line << "\n";
                }
            }
        }
        update_shell_state(state, report, expanded);
    }

    std::cout << "Closing OmniX shell.\n";
    return 0;
}

int run_tool(const std::vector<std::string>& args) {
    const ToolCliInvocation invocation = parse_tool_invocation(args);
    tze::RequestProfile profile = make_base_profile(invocation.options);
    profile.resolved_intent = tze::RequestIntent::ToolAction;
    profile.tool_mode = invocation.mode;
    profile.requested_tool_name = invocation.tool_name;
    profile.tool_arguments = invocation.tool_arguments;
    profile.verbose_output = invocation.options.output_mode == OutputMode::Verbose;
    profile.raw_prompt = invocation.mode == tze::ToolCommandMode::List
        ? "tool list"
        : ("tool " + invocation.tool_name);
    if (profile.source_map_path == "res/tze.txt") {
        const std::filesystem::path candidate = optional_source_file();
        if (!candidate.empty()) {
            profile.source_map_path = candidate.string();
        } else {
            profile.source_map_path.clear();
        }
    }

    tze::ProcessingEngine engine;
    const tze::ProcessingReport report = engine.process(profile);
    print_processing_report(report, invocation.options.output_mode, false);
    if (invocation.mode == tze::ToolCommandMode::List) {
        return 0;
    }
    if (invocation.mode == tze::ToolCommandMode::Locate) {
        return report.tool_resolution.has_value() && report.tool_resolution->found ? 0 : 1;
    }
    if (invocation.mode == tze::ToolCommandMode::Doctor) {
        return report.tool_doctor_report.has_value() &&
                   (report.tool_doctor_report->status == "native_ready" ||
                    report.tool_doctor_report->status == "builtin_ready")
            ? 0
            : 1;
    }
    return report.tool_invocation_report.has_value() && report.tool_invocation_report->status == "ok" ? 0 : 1;
}

int run_tensor(const std::vector<std::string>& args) {
    std::vector<std::string> tensor_arguments;
    const CommonCliOptions options = parse_common_options(args, 2, &tensor_arguments);
    tze::RequestProfile profile = make_base_profile(options);
    profile.resolved_intent = tze::RequestIntent::TensorAction;
    profile.tool_mode = tze::ToolCommandMode::Run;
    profile.requested_tool_name = "tensor";
    profile.tool_arguments = tensor_arguments;
    profile.verbose_output = options.output_mode == OutputMode::Verbose;
    profile.raw_prompt = "tensor";
    if (!tensor_arguments.empty()) {
        profile.raw_prompt += " " + join_positional_arguments(tensor_arguments);
    }
    if (profile.source_map_path == "res/tze.txt") {
        const std::filesystem::path candidate = optional_source_file();
        if (!candidate.empty()) {
            profile.source_map_path = candidate.string();
        } else {
            profile.source_map_path.clear();
        }
    }

    tze::ProcessingEngine engine;
    const tze::ProcessingReport report = engine.process(profile);
    print_processing_report(report, options.output_mode, false);
    return report.tool_invocation_report.has_value() && report.tool_invocation_report->status == "ok" ? 0 : 1;
}

int run_gg(const std::vector<std::string>& args) {
    CommonCliOptions options;
    std::vector<std::string> gg_arguments;
    for (std::size_t index = 2; index < args.size(); ++index) {
        const std::string& arg = args[index];
        if (arg == "--compact") {
            options.output_mode = OutputMode::Compact;
            continue;
        }
        if (arg == "--verbose") {
            options.output_mode = OutputMode::Verbose;
            continue;
        }
        if (arg == "--memory-root") {
            if (index + 1 >= args.size()) {
                throw std::runtime_error("--memory-root requires a value.");
            }
            options.memory_root_path = args[++index];
            continue;
        }
        gg_arguments.push_back(arg);
    }

    tze::RequestProfile profile = make_base_profile(options);
    profile.resolved_intent = tze::RequestIntent::ToolAction;
    profile.requested_tool_name = "gg";
    profile.verbose_output = options.output_mode == OutputMode::Verbose;
    profile.raw_prompt = "gg " + join_positional_arguments(gg_arguments);
    if (gg_arguments.empty() || gg_arguments.front() == "doctor") {
        profile.tool_mode = tze::ToolCommandMode::Doctor;
    } else {
        profile.tool_mode = tze::ToolCommandMode::Run;
        profile.tool_arguments = gg_arguments;
    }
    if (profile.source_map_path == "res/tze.txt") {
        const std::filesystem::path candidate = optional_source_file();
        if (!candidate.empty()) {
            profile.source_map_path = candidate.string();
        } else {
            profile.source_map_path.clear();
        }
    }

    tze::ProcessingEngine engine;
    const tze::ProcessingReport report = engine.process(profile);
    print_processing_report(report, options.output_mode, false);
    if (profile.tool_mode == tze::ToolCommandMode::Doctor) {
        return report.tool_doctor_report.has_value() &&
                   report.tool_doctor_report->status == "builtin_ready"
            ? 0
            : 1;
    }
    if (report.tool_invocation_report.has_value()) {
        return report.tool_invocation_report->exit_code == 0 ? 0 : 1;
    }
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 2) {
            print_usage();
            return 1;
        }

        const std::vector<std::string> args(argv, argv + argc);
        const std::string command = args[1];
        if (command == "--version" || command == "version") {
            std::cout << "omnix " << OMNIX_VERSION << "\n";
            return 0;
        }
        if (command == "map") {
            if (argc < 3) {
                throw std::runtime_error("map requires a source file path.");
            }
            run_map(args[2]);
            return 0;
        }

        if (command == "search") {
            if (argc < 3) {
                throw std::runtime_error("search requires a symbol query.");
            }
            const std::filesystem::path source_file = argc >= 4 ? std::filesystem::path(args[3]) : default_source_file();
            run_search(args[2], source_file);
            return 0;
        }

        if (command == "legacy") {
            if (argc < 3) {
                throw std::runtime_error("legacy requires a subcommand: coverage or report.");
            }
            const std::string legacy_command = args[2];
            const std::filesystem::path source_file =
                argc >= 4 ? std::filesystem::path(args[3]) : default_legacy_source_file();
            if (legacy_command == "coverage") {
                run_legacy_coverage(source_file);
                return 0;
            }
            if (legacy_command == "report") {
                run_legacy_report(source_file);
                return 0;
            }
            throw std::runtime_error("unknown legacy subcommand: " + legacy_command);
        }

        if (command == "emit-cpp") {
            if (argc < 3) {
                throw std::runtime_error("emit-cpp requires a source file path.");
            }
            const std::filesystem::path source_file = args[2];
            const std::filesystem::path output_dir = argc >= 4 ? std::filesystem::path(args[3]) : default_emit_dir(source_file);
            run_emit(source_file, output_dir);
            return 0;
        }

        if (command == "ask") {
            return run_ask(args);
        }

        if (command == "ingest") {
            return run_ingest(args);
        }

        if (command == "analyze") {
            return run_analyze(args);
        }

        if (command == "decide") {
            return run_decide(args);
        }

        if (command == "case") {
            return run_case(args);
        }

        if (command == "incident") {
            return run_incident(args);
        }

        if (command == "define") {
            return run_define(args);
        }

        if (command == "explain") {
            return run_explain(args);
        }

        if (command == "review") {
            return run_review_command(args, false);
        }

        if (command == "patch-proposal") {
            return run_review_command(args, true);
        }

        if (command == "build") {
            return run_build_prompt(args);
        }

        if (command == "preflight") {
            return run_preflight(args);
        }

        if (command == "recipe") {
            return run_recipe_author(args);
        }

        if (command == "doctor") {
            return run_doctor(args);
        }

        if (command == "provider") {
            return run_provider(args);
        }

        if (command == "id") {
            return run_identity(args);
        }

        if (command == "api") {
            return run_api(args);
        }

        if (command == "jinja") {
            return run_jinja(args);
        }

        if (command == "node") {
            return run_node(args);
        }

        if (command == "master") {
            return run_master(args);
        }

        if (command == "why") {
            return run_why(args);
        }

        if (command == "next") {
            return run_next(args);
        }

        if (command == "context") {
            return run_context(args);
        }

        if (command == "link") {
            return run_link(args);
        }

        if (command == "vg") {
            return run_vg(args);
        }

        if (command == "tview") {
            return run_tview(args);
        }

        if (command == "persona") {
            return run_persona(args);
        }

        if (command == "gg") {
            return run_gg(args);
        }

        if (command == "tensor") {
            return run_tensor(args);
        }

        if (command == "nn") {
            return run_nn(args);
        }

        if (command == "defend") {
            return run_defend(args);
        }

        if (command == "shell") {
            return run_shell(args);
        }

        if (command == "memory") {
            return run_memory(args);
        }

        if (command == "tool") {
            return run_tool(args);
        }

        if (command == "tze") {
            return run_tze(args);
        }

        if (command == "build-cmake") {
            return run_build_cmake(args);
        }

        print_usage();
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << "\n";
        return 1;
    }
}
