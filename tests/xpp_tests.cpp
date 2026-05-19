#include "tze/build_executor.hpp"
#include "tze/definition_engine.hpp"
#include "tze/intent_resolver.hpp"
#include "tze/language_engine.hpp"
#include "tze/memory_store.hpp"
#include "tze/native_tool_registry.hpp"
#include "tze/packet_capture_engine.hpp"
#include "tze/processing_engine.hpp"
#include "tze/preprocessor_runtime.hpp"
#include "tze/query_runtime.hpp"
#include "tze/project_alias_registry.hpp"
#include "tze/security_manager.hpp"
#include "tze/unix_evidence_parser.hpp"
#include "xpp/emitter.hpp"
#include "xpp/index.hpp"
#include "xpp/parser.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <vector>

namespace {

const std::filesystem::path kSourceRoot = OMNIX_SOURCE_DIR;
const std::filesystem::path kBinaryRoot = OMNIX_BINARY_DIR;
const std::string kGenericFallbackMessage = "Unmapped X++ symbol captured by the parser for later refinement.";

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void safe_remove_all(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::remove_all(path, error);
}

class ScopedEnvVar {
public:
    ScopedEnvVar(std::string key, std::string value) : key_(std::move(key)) {
        const char* existing = std::getenv(key_.c_str());
        if (existing != nullptr) {
            previous_ = existing;
            had_previous_ = true;
        }
#if defined(__APPLE__) || defined(__unix__)
        setenv(key_.c_str(), value.c_str(), 1);
#endif
    }

    ~ScopedEnvVar() {
#if defined(__APPLE__) || defined(__unix__)
        if (had_previous_) {
            setenv(key_.c_str(), previous_.c_str(), 1);
        } else {
            unsetenv(key_.c_str());
        }
#endif
    }

private:
    std::string key_;
    std::string previous_;
    bool had_previous_ = false;
};

class ScopedCurrentPath {
public:
    explicit ScopedCurrentPath(const std::filesystem::path& path) : previous_(std::filesystem::current_path()) {
        std::filesystem::current_path(path);
    }

    ~ScopedCurrentPath() {
        std::error_code error;
        std::filesystem::current_path(previous_, error);
    }

private:
    std::filesystem::path previous_;
};

std::string load_main_source() {
    return xpp::read_text_file(kSourceRoot / "res" / "tze.txt");
}

std::string shell_quote(std::string_view value) {
    std::string quoted = "'";
    for (char c : value) {
        if (c == '\'') {
            quoted += "'\\''";
        } else {
            quoted.push_back(c);
        }
    }
    quoted.push_back('\'');
    return quoted;
}

std::size_t count_substring(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) {
        return 0;
    }
    std::size_t count = 0;
    std::size_t offset = 0;
    while (true) {
        const std::size_t found = haystack.find(needle, offset);
        if (found == std::string::npos) {
            return count;
        }
        ++count;
        offset = found + needle.size();
    }
}

std::string current_platform_name() {
#if defined(__APPLE__)
    return "macos";
#elif defined(__linux__)
    return "linux";
#else
    return "unsupported";
#endif
}

struct CommandCapture {
    int exit_code = 0;
    std::string output;
};

CommandCapture run_command_capture(const std::string& command) {
    CommandCapture capture;
    FILE* pipe = popen((command + " 2>&1").c_str(), "r");
    if (pipe == nullptr) {
        capture.exit_code = -1;
        capture.output = "Unable to launch command.";
        return capture;
    }

    std::array<char, 4096> buffer{};
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        capture.output.append(buffer.data());
    }

    const int status = pclose(pipe);
    if (status == -1) {
        capture.exit_code = -1;
    } else if (WIFEXITED(status)) {
        capture.exit_code = WEXITSTATUS(status);
    } else {
        capture.exit_code = status;
    }
    return capture;
}

std::string extract_line_value(std::string_view text, std::string_view prefix) {
    const std::size_t start = text.find(prefix);
    if (start == std::string::npos) {
        return {};
    }
    const std::size_t value_start = start + prefix.size();
    const std::size_t line_end = text.find('\n', value_start);
    return std::string(text.substr(value_start, line_end == std::string::npos ? text.size() - value_start : line_end - value_start));
}

const xpp::SectionNode* find_section(const xpp::MappingUnit& unit, const std::string& title) {
    for (const xpp::SectionNode& section : unit.sections) {
        if (section.title == title) {
            return &section;
        }
    }
    return nullptr;
}

const xpp::StageGraph* find_stage_graph(const xpp::MappingUnit& unit, const std::string& graph_id) {
    for (const xpp::StageGraph& graph : unit.stage_graphs) {
        if (graph.graph_id == graph_id) {
            return &graph;
        }
    }
    return nullptr;
}

const xpp::StageNode* find_stage_node(const xpp::StageGraph& graph, const std::string& stage_id) {
    const auto match = std::find_if(graph.stages.begin(), graph.stages.end(), [&stage_id](const xpp::StageNode& stage) {
        return stage.stage_id == stage_id;
    });
    if (match == graph.stages.end()) {
        return nullptr;
    }
    return &(*match);
}

const tze::TzeStageRecord* find_runtime_stage(const tze::ProcessingReport& report, const std::string& stage_id) {
    const auto match = std::find_if(report.tze_stages.begin(), report.tze_stages.end(), [&stage_id](const tze::TzeStageRecord& stage) {
        return stage.stage_id == stage_id;
    });
    if (match == report.tze_stages.end()) {
        return nullptr;
    }
    return &(*match);
}

struct GeneratedSectionCoverage {
    std::size_t mapped = 0;
    std::size_t abstracted = 0;
    std::size_t unsupported = 0;
    std::size_t stubbed = 0;
};

GeneratedSectionCoverage generated_section_coverage(const std::filesystem::path& path) {
    const std::string text = xpp::read_text_file(path);
    return {
        count_substring(text, "mapped_symbol("),
        count_substring(text, "abstracted_symbol("),
        count_substring(text, "unsupported_symbol("),
        count_substring(text, "stubbed_symbol("),
    };
}

const xpp::SymbolMapping& require_mapping(const xpp::SymbolIndex& index, const std::string& symbol) {
    const xpp::SymbolMapping* mapping = xpp::find_mapping(index, symbol);
    require(mapping != nullptr, "Expected symbol to be indexed: " + symbol);
    return *mapping;
}

void test_parser_discovers_top_level_sections() {
    const xpp::MappingUnit unit = xpp::parse_xpp(load_main_source(), (kSourceRoot / "res" / "tze.txt").string());
    require(unit.dialect == xpp::PseudoDialect::XppCppLike, "Expected parse_xpp to use the X++ C++-like dialect.");
    require(unit.sections.size() >= 4, "Expected the parser to discover the major X++ sections.");
    require(find_section(unit, "Build CMake") != nullptr, "Expected Build CMake section.");
    require(find_section(unit, "X++ Language Engine") != nullptr, "Expected X++ Language Engine section.");
    require(find_section(unit, "X++ Security Engine") != nullptr, "Expected X++ Security Engine section.");
    require(find_section(unit, "X++ Self Pre-Processor Engine") != nullptr,
            "Expected X++ Self Pre-Processor Engine section.");
}

void test_parser_preserves_raw_and_operator_lines() {
    const std::string sample =
        "X$:Test Section\n"
        "bogus @@ line\n"
        "match(alpha <~> beta)\n"
        "if(flag)\n"
        "{\n"
        "}\n";

    const xpp::MappingUnit unit = xpp::parse_xpp(sample, "sample");
    require(unit.sections.size() == 1, "Expected one synthetic test section.");
    require(unit.sections.front().nodes.size() >= 3, "Expected the parser to retain non-empty lines.");
    require(unit.sections.front().nodes.front().kind == xpp::NodeKind::RawStatement,
            "Malformed lines should survive as raw statements.");

    bool found_operator = false;
    for (const xpp::PseudoNode& node : unit.sections.front().nodes) {
        if (std::find(node.symbols.begin(), node.symbols.end(), "<~>") != node.symbols.end()) {
            found_operator = true;
        }
    }
    require(found_operator, "Expected odd operators like <~> to be indexed from raw lines.");
}

void test_parser_extracts_build_cmake_stage_graph() {
    const xpp::MappingUnit unit = xpp::parse_xpp(load_main_source(), (kSourceRoot / "res" / "tze.txt").string());
    const xpp::StageGraph* graph = find_stage_graph(unit, "build_cmake_stage_graph");
    require(graph != nullptr, "Expected the parser to emit the Build CMake stage graph.");
    require(graph->stages.size() >= 5, "Expected the stage graph to include the stable TZE backbone.");

    const std::array<std::string, 5> expected = {
        "xProcessingCache",
        "x.Define.Low",
        "x.DisplayPriorityProcessingGate",
        "x.DisplayFeedBackLoop",
        "x.Store",
    };

    std::size_t previous_line = 0;
    for (const std::string& stage_id : expected) {
        const auto match = std::find_if(graph->stages.begin(), graph->stages.end(), [&stage_id](const xpp::StageNode& stage) {
            return stage.stage_id == stage_id;
        });
        require(match != graph->stages.end(), "Expected stage graph entry for " + stage_id + ".");
        require(match->line != 0, "Expected a non-zero source line for " + stage_id + ".");
        require(match->line > previous_line, "Expected stage graph lines to preserve source order.");
        require(!match->source_excerpt.empty(), "Expected a source excerpt for " + stage_id + ".");
        previous_line = match->line;
    }
}

void test_parser_supports_python_like_fixture() {
    const std::string sample =
        "# Section: Python Query Flow\n"
        "def x_seek(item):\n"
        "    x.index(cache)\n"
        "    if x.match(item):\n"
        "        xout(\"hit\")\n"
        "    return GENx\n";

    const xpp::MappingUnit unit = xpp::parse_pseudo(sample, "python-fixture", xpp::PseudoDialect::PythonLike);
    require(unit.dialect == xpp::PseudoDialect::PythonLike, "Expected the parser to retain the selected dialect.");
    require(unit.sections.size() == 1, "Expected one python fixture section.");
    require(unit.sections.front().title == "Python Query Flow", "Expected python section title to be parsed from comments.");
    require(unit.sections.front().nodes.size() >= 4, "Expected python fixture lines to produce nodes.");
    require(unit.sections.front().nodes.front().kind == xpp::NodeKind::FunctionDeclaration,
            "Expected python def lines to be classified as function declarations.");
    const xpp::SymbolIndex index = xpp::build_symbol_index(unit);
    const xpp::SymbolMapping& seek = require_mapping(index, "x.index");
    require(seek.family == xpp::SemanticFamily::Query, "Expected python-like query symbols to share the common query family.");
}

void test_query_runtime_tracks_stateful_session() {
    tze::QueryRuntime runtime;
    tze::QuerySessionRecord session = runtime.open_session("Investigate", "define xProcessingCache");
    runtime.index_values(session, "context", {"Investigate", "define", "xProcessingCache", "Build CMake"});

    const std::vector<tze::KnowledgeReference> references = {
        {"Wikipedia", "Generic build overview.", 2},
        {"Build CMake", "Source-backed stage graph for xProcessingCache.", 1},
    };
    const std::vector<tze::KnowledgeReference> ranked = runtime.rank_references(session, "knowledge", references);
    require(!ranked.empty(), "Expected ranked references from the query session.");
    require(ranked.front().source == "Build CMake",
            "Expected source-aware query ranking to prefer the closer matching reference.");

    std::vector<tze::DecisionCandidate> decisions;
    tze::DecisionCandidate inspect;
    inspect.id = "decision-inspect";
    inspect.title = "Inspect build context";
    inspect.recommended_command = "omnix tool inspect-build -- .";
    inspect.rationale = "Build signals were detected.";
    inspect.score = 68;
    inspect.probability_likelihood = 0.64;
    inspect.confidence = 0.70;
    inspect.supporting_signals = {"build", "cmake"};
    decisions.push_back(inspect);

    tze::DecisionCandidate report;
    report.id = "decision-report";
    report.title = "Report the case";
    report.recommended_command = "omnix tool report-case -- case-1";
    report.rationale = "Persist the summary.";
    report.score = 68;
    report.probability_likelihood = 0.62;
    report.confidence = 0.69;
    report.supporting_signals = {"report"};
    decisions.push_back(report);

    runtime.rank_decisions(session, "decision-ranking", decisions, {"build", "cmake"});
    require(!decisions.empty() && decisions.front().id == "decision-inspect",
            "Expected query-driven decision ranking to prefer the build-aligned action.");
    require(std::find_if(decisions.front().score_trace.begin(),
                         decisions.front().score_trace.end(),
                         [](const std::string& trace) { return trace.find("query_rank=") != std::string::npos; }) !=
                decisions.front().score_trace.end(),
            "Expected ranked decisions to expose query-derived trace output.");
    require(!session.operations.empty(), "Expected the query session to retain operation history.");
    require(!session.final_results.empty(), "Expected the query session to retain final results.");
}

void test_language_engine_resolves_native_context() {
    const std::filesystem::path memory_root = kBinaryRoot / "language-engine-memory";
    safe_remove_all(memory_root);

    tze::MemoryStore store;
    tze::MemorySnapshot snapshot = store.load(memory_root);
    tze::QueryRuntime runtime;
    tze::QuerySessionRecord session = runtime.open_session("Investigate", "define x.determineOSLanguage");

    const tze::LanguageResolutionRecord record = tze::LanguageEngine::resolve_context(
        "x.determineOSLanguage",
        (kSourceRoot / "res" / "tze.txt").string(),
        "auto",
        snapshot,
        &session);

    require(!record.selected_os.empty(), "Expected language resolution to select an operating system.");
    require(!record.selected_language.empty(), "Expected language resolution to select a language.");
    require(record.passes >= 1, "Expected language resolution to perform at least one probability pass.");
    require(record.confidence > 0.0, "Expected language resolution to produce a nonzero confidence.");
    require(!record.os_candidates.empty(), "Expected language resolution to emit OS candidates.");
    require(!record.language_candidates.empty(), "Expected language resolution to emit language candidates.");
    require(record.decompression_candidates.empty(),
            "Expected ordinary language resolution to avoid legacy decompression candidates.");
    require(!session.operations.empty(), "Expected language resolution to index evidence through the query session.");
}

void test_language_engine_recovers_legacy_decompression_ladder() {
    const std::filesystem::path memory_root = kBinaryRoot / "legacy-language-engine-memory";
    safe_remove_all(memory_root);

    tze::MemoryStore store;
    tze::MemorySnapshot snapshot = store.load(memory_root);
    tze::QueryRuntime runtime;
    tze::QuerySessionRecord session = runtime.open_session("Investigate", "define BinaryDecompresser");

    const tze::LanguageResolutionRecord record = tze::LanguageEngine::resolve_context(
        "BinaryDecompresser TernaryDecompression Base4Decompression file coherence",
        "/Volumes/CoE/Tzu.cpp",
        "auto",
        snapshot,
        &session);

    require(!record.decompression_candidates.empty(),
            "Expected the legacy language path to emit decompression candidates.");
    require(record.decompression_candidates.front().label == "native-compression-algorithms",
            "Expected the decompression ladder to prefer native deterministic handling first.");
    require(std::find_if(record.decompression_candidates.begin(),
                         record.decompression_candidates.end(),
                         [](const tze::DecompressionCandidate& candidate) {
                             return candidate.label == "ternary-decompression" &&
                                 candidate.status == "research-only";
                         }) != record.decompression_candidates.end(),
            "Expected ternary decompression to remain a research-only legacy branch.");
    require(!record.research_notes.empty(),
            "Expected the legacy decompression ladder to preserve research notes.");
}

void test_preprocessor_runtime_resolves_uac_state() {
    const std::filesystem::path memory_root = kBinaryRoot / "uac-runtime-memory";
    safe_remove_all(memory_root);

    tze::MemoryStore store;
    tze::MemorySnapshot snapshot = store.load(memory_root);
    tze::QueryRuntime runtime;
    tze::QuerySessionRecord session = runtime.open_session("Investigate", "define x.reGENx");

    const tze::UacStateRecord state = tze::PreprocessorRuntime::resolve_uac_state(
        "x.reGENx",
        snapshot,
        &session);

    require(!state.epoch_marker.empty(), "Expected uAC state resolution to emit an epoch marker.");
    require(!state.machine_identifier.empty(), "Expected uAC state resolution to emit a machine identifier.");
    require(!state.store_namespace.empty(), "Expected uAC state resolution to emit a persistent namespace.");
    require(!state.indexed_traits.empty(), "Expected uAC state resolution to emit indexed traits.");
    require(!state.recovery_hints.empty(), "Expected uAC state resolution to emit recovery hints.");
    require(!state.chapter_series_label.empty(), "Expected uAC recovery to emit a chapter/series label.");
    require(!state.epoch_tier_label.empty(), "Expected uAC recovery to emit an epoch-tier label.");
    require(!state.deletion_discrepancies.empty(), "Expected uAC recovery to track deletion discrepancies.");
    require(!state.search_context_habits.empty(), "Expected uAC recovery to track search context habits.");
    require(!state.time_on_site_traits.empty(), "Expected uAC recovery to track time-on-site traits.");
    require(!session.operations.empty(), "Expected uAC resolution to index evidence through the query session.");
}

void test_cli_legacy_coverage_and_report() {
    const std::filesystem::path binary = kBinaryRoot / "omnix";
    const std::filesystem::path fixture = kBinaryRoot / "legacy-tzu-fixture.cpp";

    std::ofstream output(fixture);
    output
        << "X$:Build CMake\n"
        << "xProcessingCache(findStorage);\n"
        << "x.Define.Low(Investigate);\n"
        << "x.DisplayPriorityProcessingGate(UseSources);\n"
        << "x.DisplayFeedBackLoop(UseFeedback);\n"
        << "x.Store(runLedger);\n"
        << "X$:X++ Language Engine\n"
        << "BinaryDecompresser(data);\n"
        << "TernaryDecompression(data);\n"
        << "Base4Decompression(data);\n"
        << "X$:X++ Security Engine\n"
        << "xXOmni::Premit(RSRD);\n"
        << "X$:X++ Self Pre-Processor Engine\n"
        << "x.reGENx(uAC);\n";
    output.close();

    const CommandCapture coverage = run_command_capture(
        shell_quote(binary.string()) + " legacy coverage " + shell_quote(fixture.string()));
    require(coverage.exit_code == 0, "Expected `omnix legacy coverage` to succeed.");
    require(coverage.output.find("Structured merge status") != std::string::npos,
            "Expected legacy coverage to print the structured merge summary.");
    require(coverage.output.find("xProcessingCache [implemented]") != std::string::npos,
            "Expected legacy coverage to classify the core spine as implemented.");
    require(coverage.output.find("xXOmni::Premit [partial]") != std::string::npos,
            "Expected legacy coverage to report the xXOmni bridge as partially recovered.");

    const CommandCapture report = run_command_capture(
        shell_quote(binary.string()) + " legacy report " + shell_quote(fixture.string()));
    require(report.exit_code == 0, "Expected `omnix legacy report` to succeed.");
    require(report.output.find("Legacy archaeology report") != std::string::npos,
            "Expected legacy report to print the archaeology heading.");
    require(report.output.find("language/decompression ladder: partial") != std::string::npos,
            "Expected legacy report to summarize the decompression recovery track.");
}

void make_executable(const std::filesystem::path& path, const std::string& body) {
    std::ofstream output(path);
    output << body;
    output.close();
    std::filesystem::permissions(
        path,
        std::filesystem::perms::owner_exec | std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
            std::filesystem::perms::group_exec | std::filesystem::perms::group_read |
            std::filesystem::perms::others_exec | std::filesystem::perms::others_read,
        std::filesystem::perm_options::replace);
}

void remember_tool_resolution(tze::MemoryStore& store,
                              tze::MemorySnapshot& snapshot,
                              const tze::ToolResolution& resolution) {
    require(resolution.found, "Expected a found tool resolution before storing it.");
    tze::NativeToolRecord record;
    record.logical_name = resolution.logical_name;
    record.provider_type = resolution.provider_type;
    record.executable_path = resolution.executable_path;
    record.applet_name = resolution.applet_name;
    record.version_fingerprint = resolution.version_fingerprint;
    record.capability_flags = resolution.capability_flags;
    record.environment_signature = resolution.environment_signature;
    record.discovery_origin = resolution.cache_origin;
    record.last_verified = "2026-03-31T00:00:00";
    std::error_code ec;
    const auto size = std::filesystem::file_size(resolution.executable_path, ec);
    record.size_bytes = ec ? 0 : size;
    ec.clear();
    const auto write_time = std::filesystem::last_write_time(resolution.executable_path, ec);
    record.modified_timestamp = ec
        ? 0
        : static_cast<long long>(
              std::chrono::duration_cast<std::chrono::seconds>(write_time.time_since_epoch()).count());
    store.remember_native_tool(snapshot, record);
}

void test_build_executor_probes_modules() {
    const std::filesystem::path root = kBinaryRoot / "fake-toolchain";
    safe_remove_all(root);
    std::filesystem::create_directories(root);

    make_executable(root / "gcc", "#!/bin/sh\nif [ \"$1\" = \"--version\" ]; then echo 'gcc test 14.1.0'; exit 0; fi\nexit 0\n");
    make_executable(root / "g++", "#!/bin/sh\nif [ \"$1\" = \"--version\" ]; then echo 'g++ test 14.1.0'; exit 0; fi\nexit 0\n");
    make_executable(root / "git", "#!/bin/sh\nif [ \"$1\" = \"--version\" ]; then echo 'git version test'; exit 0; fi\nexit 0\n");
    make_executable(root / "make", "#!/bin/sh\nif [ \"$1\" = \"--version\" ] || [ \"$1\" = \"-v\" ]; then echo 'GNU Make test'; exit 0; fi\nexit 0\n");
    make_executable(root / "cmake", "#!/bin/sh\nif [ \"$1\" = \"--version\" ]; then echo 'cmake version 3.30.0'; exit 0; fi\nexit 0\n");

    ScopedEnvVar path_override("PATH", root.string());
    tze::BuildExecutor executor;
    const auto modules = executor.probe_modules();

    bool saw_cmake = false;
    bool saw_gxx = false;
    bool saw_git = false;
    for (const auto& module : modules) {
        if (module.id == "cmake") {
            saw_cmake = module.available && module.version == "cmake version 3.30.0";
        }
        if (module.id == "gxx") {
            saw_gxx = module.available && module.version == "g++ test 14.1.0";
        }
        if (module.id == "git") {
            saw_git = module.available && module.version == "git version test";
        }
    }
    require(saw_cmake, "Expected fake cmake probe to succeed.");
    require(saw_gxx, "Expected fake g++ probe to succeed.");
    require(saw_git, "Expected fake git probe to succeed.");
}

void test_native_tool_registry_caches_path_hit() {
    const std::filesystem::path root = kBinaryRoot / "native-tool-registry-cache";
    const std::filesystem::path bin_dir = root / "bin";
    const std::filesystem::path memory_root = root / "memory";
    safe_remove_all(root);
    std::filesystem::create_directories(bin_dir);

    make_executable(bin_dir / "grep",
                    "#!/bin/sh\n"
                    "if [ \"$1\" = \"--version\" ]; then echo 'grep test 1.0'; exit 0; fi\n"
                    "exec /usr/bin/grep \"$@\"\n");

    ScopedEnvVar path_override("PATH", bin_dir.string());
    tze::MemoryStore store;
    tze::MemorySnapshot snapshot = store.load(memory_root);
    tze::NativeToolRegistry registry;

    const tze::ToolResolution first = registry.resolve("grep", snapshot, false);
    require(first.found, "Expected grep to resolve from the fake PATH.");
    require(first.cache_origin == "path_lookup", "Expected first lookup to come from PATH.");

    remember_tool_resolution(store, snapshot, first);
    store.persist_snapshot(snapshot);

    tze::MemorySnapshot reloaded = store.load(memory_root);
    const tze::ToolResolution second = registry.resolve("grep", reloaded, false);
    require(second.found, "Expected cached grep resolution to stay valid.");
    require(second.cache_origin == "cache_hit", "Expected second lookup to reuse the cache.");
}

void test_native_tool_registry_deep_scan_and_busybox_applets() {
    const std::filesystem::path root = kBinaryRoot / "native-tool-registry-deep";
    const std::filesystem::path nested_bin = root / "search" / "opt" / "native" / "bin";
    const std::filesystem::path memory_root = root / "memory";
    safe_remove_all(root);
    std::filesystem::create_directories(nested_bin);

    make_executable(nested_bin / "nmap",
                    "#!/bin/sh\n"
                    "if [ \"$1\" = \"-V\" ] || [ \"$1\" = \"--version\" ]; then echo 'Nmap version 9.99 test'; exit 0; fi\n"
                    "echo 'nmap fake'\n");
    make_executable(nested_bin / "busybox",
                    "#!/bin/sh\n"
                    "if [ \"$1\" = \"--version\" ]; then echo 'BusyBox vtest'; exit 0; fi\n"
                    "if [ \"$1\" = \"--list\" ]; then printf 'grep\\nsed\\nawk\\n'; exit 0; fi\n"
                    "applet=\"$1\"\n"
                    "shift\n"
                    "case \"$applet\" in\n"
                    "  grep) exec /usr/bin/grep \"$@\" ;;\n"
                    "  sed) exec /usr/bin/sed \"$@\" ;;\n"
                    "  awk) exec /usr/bin/awk \"$@\" ;;\n"
                    "esac\n"
                    "exit 1\n");

    ScopedEnvVar path_override("PATH", "");
    ScopedEnvVar roots_override("OMNIX_NATIVE_SEARCH_ROOTS", (root / "search").string());
    ScopedEnvVar timeout_override("OMNIX_NATIVE_HUNT_TIMEOUT_SECONDS", "1");
    tze::MemoryStore store;
    tze::MemorySnapshot snapshot = store.load(memory_root);
    tze::NativeToolRegistry registry;

    const tze::ToolResolution nmap = registry.resolve("nmap", snapshot, true);
    require(nmap.found, "Expected nmap to resolve via deep scan.");
    require(nmap.cache_origin == "deep_scan", "Expected deep scan origin for the nested nmap binary.");

    const tze::ToolResolution grep = registry.resolve("grep", snapshot, true);
    require(grep.found, "Expected grep to resolve through the BusyBox applet.");
    require(grep.provider_type == "busybox_applet", "Expected BusyBox applet provider for grep.");
}

void test_build_executor_inspects_and_builds_project() {
    const std::filesystem::path root = kBinaryRoot / "fake-builder";
    const std::filesystem::path bin_dir = root / "bin";
    const std::filesystem::path source_dir = root / "sample-project";
    const std::filesystem::path invoke_log = root / "cmake-invocations.log";
    safe_remove_all(root);
    std::filesystem::create_directories(bin_dir);
    std::filesystem::create_directories(source_dir);

    make_executable(bin_dir / "gcc", "#!/bin/sh\nif [ \"$1\" = \"--version\" ]; then echo 'gcc test 14.1.0'; exit 0; fi\nexit 0\n");
    make_executable(bin_dir / "g++", "#!/bin/sh\nif [ \"$1\" = \"--version\" ]; then echo 'g++ test 14.1.0'; exit 0; fi\nexit 0\n");
    make_executable(
        bin_dir / "cmake",
        "#!/bin/sh\n"
        "if [ \"$1\" = \"--version\" ]; then echo 'cmake version 3.30.0'; exit 0; fi\n"
        "echo \"$@\" >> \"" + invoke_log.string() + "\"\n"
        "if [ \"$1\" = \"--build\" ]; then shift; /bin/mkdir -p \"$1\"; : > \"$1/omnix\"; exit 0; fi\n"
        "build_dir=''\n"
        "for arg in \"$@\"; do\n"
        "  if [ \"$prev\" = \"-B\" ]; then build_dir=\"$arg\"; fi\n"
        "  prev=\"$arg\"\n"
        "done\n"
        "if [ -n \"$build_dir\" ]; then /bin/mkdir -p \"$build_dir\"; fi\n"
        "exit 0\n");

    {
        std::ofstream cmake_lists(source_dir / "CMakeLists.txt");
        cmake_lists << "cmake_minimum_required(VERSION 3.20)\nproject(Sample LANGUAGES CXX)\n";
    }

    ScopedEnvVar path_override("PATH", bin_dir.string());
    tze::BuildExecutor executor;
    const tze::SourceInspection inspection = executor.inspect_source(source_dir);
    require(inspection.build_system == "cmake", "Expected fake project to be detected as cmake.");
    require(inspection.ready, "Expected fake project to be build-ready.");

    tze::RequestProfile profile;
    profile.build_source_path = source_dir.string();
    profile.build_target = "omnix";
    profile.memory_root_path = (root / "home").string();
    profile.clean_build = true;
    profile.build_type = "Release";
    profile.perform_install = false;
    const tze::BuildExecution build = executor.build_source(profile);

    require(build.built, "Expected fake project build to succeed.");
    require(build.status == "built", "Expected fake project build status to be 'built'.");
    require(!build.build_dir.empty() && std::filesystem::exists(build.build_dir), "Expected build directory to exist.");
    require(!build.log_path.empty() && std::filesystem::exists(build.log_path), "Expected build log to exist.");

    const std::string log_text = xpp::read_text_file(invoke_log);
    require(log_text.find("--build") != std::string::npos, "Expected fake cmake to receive the build command.");
}

void test_build_executor_builds_configure_project() {
    const std::filesystem::path root = kBinaryRoot / "fake-configure-builder";
    const std::filesystem::path bin_dir = root / "bin";
    const std::filesystem::path source_dir = root / "sample-project";
    safe_remove_all(root);
    std::filesystem::create_directories(bin_dir);
    std::filesystem::create_directories(source_dir);

    make_executable(bin_dir / "gcc", "#!/bin/sh\nif [ \"$1\" = \"--version\" ]; then echo 'gcc test 14.1.0'; exit 0; fi\nexit 0\n");
    make_executable(bin_dir / "g++", "#!/bin/sh\nif [ \"$1\" = \"--version\" ]; then echo 'g++ test 14.1.0'; exit 0; fi\nexit 0\n");
    make_executable(bin_dir / "make",
                    "#!/bin/sh\nif [ \"$1\" = \"--version\" ] || [ \"$1\" = \"-v\" ]; then echo 'GNU Make test'; exit 0; fi\n: > nmap\nexit 0\n");
    make_executable(source_dir / "configure", "#!/bin/sh\nexit 0\n");

    ScopedEnvVar path_override("PATH", bin_dir.string());
    tze::BuildExecutor executor;
    const tze::SourceInspection inspection = executor.inspect_source(source_dir);
    require(inspection.build_system == "configure", "Expected configure project to be detected.");
    require(inspection.ready, "Expected configure project to be build-ready.");

    tze::RequestProfile profile;
    profile.build_source_path = source_dir.string();
    profile.build_target = "nmap";
    profile.memory_root_path = (root / "home").string();
    profile.perform_install = false;
    const tze::BuildExecution build = executor.build_source(profile);
    require(build.built, "Expected configure project build to succeed.");
    require(build.status == "built", "Expected configure project to succeed without staged install.");
    require(build.artifact_hint.find("nmap") != std::string::npos, "Expected configure build artifact to mention nmap.");
}

void test_build_executor_builds_make_project() {
    const std::filesystem::path root = kBinaryRoot / "fake-make-builder";
    const std::filesystem::path bin_dir = root / "bin";
    const std::filesystem::path source_dir = root / "sample-project";
    safe_remove_all(root);
    std::filesystem::create_directories(bin_dir);
    std::filesystem::create_directories(source_dir);

    make_executable(bin_dir / "gcc", "#!/bin/sh\nif [ \"$1\" = \"--version\" ]; then echo 'gcc test 14.1.0'; exit 0; fi\nexit 0\n");
    make_executable(bin_dir / "g++", "#!/bin/sh\nif [ \"$1\" = \"--version\" ]; then echo 'g++ test 14.1.0'; exit 0; fi\nexit 0\n");
    make_executable(bin_dir / "make",
                    "#!/bin/sh\nif [ \"$1\" = \"--version\" ] || [ \"$1\" = \"-v\" ]; then echo 'GNU Make test'; exit 0; fi\n: > custom-target\nexit 0\n");

    {
        std::ofstream makefile(source_dir / "Makefile");
        makefile << "all:\n\t@echo ok\n";
    }

    ScopedEnvVar path_override("PATH", bin_dir.string());
    tze::BuildExecutor executor;
    const tze::SourceInspection inspection = executor.inspect_source(source_dir);
    require(inspection.build_system == "make", "Expected make project to be detected.");
    require(inspection.ready, "Expected make project to be build-ready.");

    tze::RequestProfile profile;
    profile.build_source_path = source_dir.string();
    profile.build_target = "custom-target";
    profile.memory_root_path = (root / "home").string();
    profile.perform_install = false;
    const tze::BuildExecution build = executor.build_source(profile);
    require(build.built, "Expected make project build to succeed.");
    require(build.status == "built", "Expected make project to succeed without staged install.");
    require(build.artifact_hint.find("custom-target") != std::string::npos,
            "Expected make build artifact to mention the custom target.");
}

void test_build_executor_preflights_nmap_alias() {
    const std::filesystem::path root = kBinaryRoot / "fake-preflight";
    safe_remove_all(root);
    std::filesystem::create_directories(root);

    make_executable(root / "git", "#!/bin/sh\nif [ \"$1\" = \"--version\" ]; then echo 'git version test'; exit 0; fi\nexit 0\n");
    make_executable(root / "gcc", "#!/bin/sh\nif [ \"$1\" = \"--version\" ]; then echo 'gcc test'; exit 0; fi\nexit 0\n");
    make_executable(root / "g++", "#!/bin/sh\nif [ \"$1\" = \"--version\" ]; then echo 'g++ test'; exit 0; fi\nexit 0\n");
    make_executable(root / "make", "#!/bin/sh\nif [ \"$1\" = \"--version\" ] || [ \"$1\" = \"-v\" ]; then echo 'GNU Make test'; exit 0; fi\nexit 0\n");

    ScopedEnvVar path_override("PATH", root.string());
    tze::ProjectAliasRegistry aliases;
    const std::optional<tze::ProjectAlias> alias = aliases.find("nmap");
    require(alias.has_value(), "Expected bundled nmap alias.");

    tze::RequestProfile profile;
    profile.project_reference = "nmap";
    profile.memory_root_path = (kBinaryRoot / "preflight-memory").string();

    tze::BuildExecutor executor;
    const tze::PreflightReport preflight = executor.preflight(profile, alias);
    require(preflight.ready, "Expected nmap preflight to be ready with the fake toolchain.");
    require(preflight.recipe_id == "nmap-configure", "Expected nmap preflight to select the configure recipe.");
    require(preflight.will_acquire, "Expected nmap preflight to plan source acquisition when no local source is present.");
    require(!preflight.dependency_hints.empty(), "Expected alias-specific dependency hints during preflight.");
}

void test_build_executor_preflights_tshark_alias() {
    const std::filesystem::path root = kBinaryRoot / "fake-preflight-tshark";
    safe_remove_all(root);
    std::filesystem::create_directories(root);

    make_executable(root / "git", "#!/bin/sh\nif [ \"$1\" = \"--version\" ]; then echo 'git version test'; exit 0; fi\nexit 0\n");
    make_executable(root / "gcc", "#!/bin/sh\nif [ \"$1\" = \"--version\" ]; then echo 'gcc test'; exit 0; fi\nexit 0\n");
    make_executable(root / "g++", "#!/bin/sh\nif [ \"$1\" = \"--version\" ]; then echo 'g++ test'; exit 0; fi\nexit 0\n");
    make_executable(root / "cmake", "#!/bin/sh\nif [ \"$1\" = \"--version\" ]; then echo 'cmake version 3.30.0'; exit 0; fi\nexit 0\n");

    ScopedEnvVar path_override("PATH", root.string());
    tze::ProjectAliasRegistry aliases;
    const std::optional<tze::ProjectAlias> alias = aliases.find("tshark");
    require(alias.has_value(), "Expected bundled tshark alias.");

    tze::RequestProfile profile;
    profile.project_reference = "tshark";
    profile.memory_root_path = (kBinaryRoot / "preflight-tshark-memory").string();

    tze::BuildExecutor executor;
    const tze::PreflightReport preflight = executor.preflight(profile, alias);
    require(preflight.ready, "Expected tshark preflight to be ready with the fake toolchain.");
    require(preflight.recipe_id == "tshark-cmake", "Expected tshark preflight to select the cmake recipe.");
    require(preflight.will_acquire, "Expected tshark preflight to plan source acquisition when no local source is present.");
    require(std::find(preflight.expected_artifacts.begin(), preflight.expected_artifacts.end(), "bin/tshark") !=
                preflight.expected_artifacts.end(),
            "Expected tshark preflight to verify the staged tshark binary.");
}

void test_build_executor_doctor_reports_package_guidance() {
    const std::filesystem::path root = kBinaryRoot / "fake-doctor";
    safe_remove_all(root);
    std::filesystem::create_directories(root);

    make_executable(root / "git", "#!/bin/sh\nif [ \"$1\" = \"--version\" ]; then echo 'git version test'; exit 0; fi\nexit 0\n");
    make_executable(root / "gcc", "#!/bin/sh\nif [ \"$1\" = \"--version\" ]; then echo 'gcc test'; exit 0; fi\nexit 0\n");
    make_executable(root / "g++", "#!/bin/sh\nif [ \"$1\" = \"--version\" ]; then echo 'g++ test'; exit 0; fi\nexit 0\n");
    make_executable(root / "make", "#!/bin/sh\nif [ \"$1\" = \"--version\" ] || [ \"$1\" = \"-v\" ]; then echo 'GNU Make test'; exit 0; fi\nexit 0\n");
    make_executable(root / "pkg-config",
                    "#!/bin/sh\n"
                    "if [ \"$1\" = \"--exists\" ]; then\n"
                    "  if [ \"$2\" = \"libpcap\" ] || [ \"$2\" = \"openssl\" ] || [ \"$2\" = \"libssl\" ]; then exit 0; fi\n"
                    "  exit 1\n"
                    "fi\n"
                    "echo 'pkg-config test'\n");
    for (std::string_view tool : {"brew", "apt-get", "dnf", "yum", "pacman", "rpm", "pkg", "curl", "wget"}) {
        make_executable(root / tool, "#!/bin/sh\nexit 0\n");
    }

    ScopedEnvVar path_override("PATH", root.string());
    tze::ProjectAliasRegistry aliases;
    const std::optional<tze::ProjectAlias> alias = aliases.find("nmap");
    require(alias.has_value(), "Expected bundled nmap alias.");

    tze::RequestProfile profile;
    profile.project_reference = "nmap";
    profile.memory_root_path = (root / "home").string();

    tze::BuildExecutor executor;
    const tze::DoctorReport doctor = executor.doctor(profile, alias);
    require(!doctor.package_guidance.empty(), "Expected package-manager guidance blocks.");
    require(!doctor.detected_platform.empty(), "Expected doctor report to include the detected platform.");
    require(!doctor.dependency_checks.empty(), "Expected alias-aware dependency checks.");

    std::vector<std::string> guidance_ids;
    bool saw_primary = false;
    for (const tze::PackageManagerGuidance& guidance : doctor.package_guidance) {
        guidance_ids.push_back(guidance.id);
        saw_primary = saw_primary || guidance.primary;
    }

    for (std::string_view required : {"brew", "apt-get", "dnf", "yum", "pacman", "rpm", "pkg", "curl", "wget"}) {
        require(std::find(guidance_ids.begin(), guidance_ids.end(), required) != guidance_ids.end(),
                "Expected doctor guidance block for " + std::string(required));
    }
    require(saw_primary, "Expected one package-manager guidance block to be marked as primary.");
    require(std::find_if(doctor.dependency_checks.begin(),
                         doctor.dependency_checks.end(),
                         [](const std::string& line) { return line.find("libpcap") != std::string::npos; }) !=
                doctor.dependency_checks.end(),
            "Expected nmap doctor checks to mention libpcap.");
    require(!doctor.build_guidance.empty(), "Expected doctor to include universal build guidance.");
    require(!doctor.next_steps.empty(), "Expected doctor to include next-step guidance.");
}

void test_build_executor_doctor_reports_universal_cmake_guidance() {
    const std::filesystem::path root = kBinaryRoot / "fake-doctor-cmake";
    const std::filesystem::path bin_dir = root / "bin";
    const std::filesystem::path source_dir = root / "sample-project";
    safe_remove_all(root);
    std::filesystem::create_directories(bin_dir);
    std::filesystem::create_directories(source_dir);

    make_executable(bin_dir / "gcc", "#!/bin/sh\nif [ \"$1\" = \"--version\" ]; then echo 'gcc test'; exit 0; fi\nexit 0\n");
    make_executable(bin_dir / "g++", "#!/bin/sh\nif [ \"$1\" = \"--version\" ]; then echo 'g++ test'; exit 0; fi\nexit 0\n");
    make_executable(bin_dir / "cmake", "#!/bin/sh\nif [ \"$1\" = \"--version\" ]; then echo 'cmake version 3.30.0'; exit 0; fi\nexit 0\n");
    make_executable(bin_dir / "pkg-config", "#!/bin/sh\nif [ \"$1\" = \"--version\" ]; then echo 'pkg-config test'; exit 0; fi\nexit 0\n");

    std::ofstream cmakelists(source_dir / "CMakeLists.txt");
    cmakelists << "cmake_minimum_required(VERSION 3.15)\nproject(sample CXX)\nadd_executable(sample main.cpp)\n";
    std::ofstream main_cpp(source_dir / "main.cpp");
    main_cpp << "int main(){return 0;}\n";

    ScopedEnvVar path_override("PATH", bin_dir.string());
    tze::RequestProfile profile;
    profile.project_reference = source_dir.string();
    profile.memory_root_path = (root / "home").string();

    tze::BuildExecutor executor;
    const tze::DoctorReport doctor = executor.doctor(profile, std::nullopt, source_dir);
    require(doctor.build_system == "cmake", "Expected doctor to report the detected CMake build system.");
    require(std::find_if(doctor.dependency_checks.begin(),
                         doctor.dependency_checks.end(),
                         [](const std::string& line) { return line.find("cmake: available") != std::string::npos; }) !=
                doctor.dependency_checks.end(),
            "Expected universal CMake doctor checks.");
    require(std::find_if(doctor.configure_flags.begin(),
                         doctor.configure_flags.end(),
                         [](const std::string& line) { return line.find("-DCMAKE_BUILD_TYPE=Release") != std::string::npos; }) !=
                doctor.configure_flags.end(),
            "Expected doctor to explain injected CMake flags.");
    require(std::find_if(doctor.build_guidance.begin(),
                         doctor.build_guidance.end(),
                         [](const std::string& line) { return line.find("omnix preflight") != std::string::npos; }) !=
                doctor.build_guidance.end(),
            "Expected doctor to include a universal preflight command.");
}

void test_build_executor_doctor_guides_tshark_dependencies() {
    const std::filesystem::path root = kBinaryRoot / "fake-doctor-tshark";
    safe_remove_all(root);
    std::filesystem::create_directories(root);

    make_executable(root / "git", "#!/bin/sh\nif [ \"$1\" = \"--version\" ]; then echo 'git version test'; exit 0; fi\nexit 0\n");
    make_executable(root / "gcc", "#!/bin/sh\nif [ \"$1\" = \"--version\" ]; then echo 'gcc test'; exit 0; fi\nexit 0\n");
    make_executable(root / "g++", "#!/bin/sh\nif [ \"$1\" = \"--version\" ]; then echo 'g++ test'; exit 0; fi\nexit 0\n");
    make_executable(root / "cmake", "#!/bin/sh\nif [ \"$1\" = \"--version\" ]; then echo 'cmake version 3.30.0'; exit 0; fi\nexit 0\n");
    make_executable(root / "pkg-config", "#!/bin/sh\nif [ \"$1\" = \"--exists\" ]; then exit 1; fi\necho 'pkg-config test'\n");
    make_executable(root / "brew", "#!/bin/sh\nexit 0\n");

    ScopedEnvVar path_override("PATH", root.string());
    tze::ProjectAliasRegistry aliases;
    const std::optional<tze::ProjectAlias> alias = aliases.find("tshark");
    require(alias.has_value(), "Expected bundled tshark alias.");

    tze::RequestProfile profile;
    profile.project_reference = "tshark";
    profile.memory_root_path = (root / "home").string();

    tze::BuildExecutor executor;
    const tze::DoctorReport doctor = executor.doctor(profile, alias);
    require(doctor.status == "doctor_attention_needed",
            "Expected tshark doctor to flag missing project-specific dependencies.");
    require(std::find_if(doctor.dependency_checks.begin(),
                         doctor.dependency_checks.end(),
                         [](const std::string& line) { return line.find("glib-2.0") != std::string::npos; }) !=
                doctor.dependency_checks.end(),
            "Expected tshark doctor checks to mention GLib.");
    require(std::find_if(doctor.configure_flags.begin(),
                         doctor.configure_flags.end(),
                         [](const std::string& line) { return line.find("-DBUILD_tshark=ON") != std::string::npos; }) !=
                doctor.configure_flags.end(),
            "Expected tshark doctor to surface recipe CMake flags.");
    require(std::find_if(doctor.package_guidance.begin(),
                         doctor.package_guidance.end(),
                         [](const tze::PackageManagerGuidance& guidance) {
                             return guidance.id == "brew" &&
                                    std::find_if(guidance.commands.begin(),
                                                 guidance.commands.end(),
                                                 [](const std::string& command) {
                                                     return command.find("flex") != std::string::npos &&
                                                            command.find("gnutls") != std::string::npos;
                                                 }) != guidance.commands.end();
                         }) != doctor.package_guidance.end(),
            "Expected tshark doctor to include richer package guidance.");
}

std::vector<unsigned char> sample_tcp_packet(const std::vector<unsigned char>& payload) {
    std::vector<unsigned char> packet(14 + 20 + 20 + payload.size(), 0);
    packet[12] = 0x08;
    packet[13] = 0x00;
    const std::size_t ip = 14;
    packet[ip] = 0x45;
    const std::uint16_t total = static_cast<std::uint16_t>(20 + 20 + payload.size());
    packet[ip + 2] = static_cast<unsigned char>(total >> 8);
    packet[ip + 3] = static_cast<unsigned char>(total & 0xff);
    packet[ip + 8] = 64;
    packet[ip + 9] = 6;
    packet[ip + 12] = 127;
    packet[ip + 15] = 1;
    packet[ip + 16] = 127;
    packet[ip + 19] = 1;
    const std::size_t tcp = ip + 20;
    packet[tcp] = 0x13;
    packet[tcp + 1] = 0x88;
    packet[tcp + 2] = 0xef;
    packet[tcp + 3] = 0x32;
    packet[tcp + 12] = 0x50;
    packet[tcp + 13] = 0x18;
    std::copy(payload.begin(), payload.end(), packet.begin() + static_cast<std::ptrdiff_t>(tcp + 20));
    return packet;
}

void write_u32_le(std::ofstream& out, std::uint32_t value) {
    const std::array<unsigned char, 4> bytes = {
        static_cast<unsigned char>(value & 0xff),
        static_cast<unsigned char>((value >> 8) & 0xff),
        static_cast<unsigned char>((value >> 16) & 0xff),
        static_cast<unsigned char>((value >> 24) & 0xff),
    };
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void write_u16_le(std::ofstream& out, std::uint16_t value) {
    const std::array<unsigned char, 2> bytes = {
        static_cast<unsigned char>(value & 0xff),
        static_cast<unsigned char>((value >> 8) & 0xff),
    };
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void write_pcap_fixture(const std::filesystem::path& path, const std::vector<unsigned char>& packet) {
    std::ofstream out(path, std::ios::binary);
    write_u32_le(out, 0xa1b2c3d4);
    write_u16_le(out, 2);
    write_u16_le(out, 4);
    write_u32_le(out, 0);
    write_u32_le(out, 0);
    write_u32_le(out, 65535);
    write_u32_le(out, 1);
    write_u32_le(out, 1);
    write_u32_le(out, 42);
    write_u32_le(out, static_cast<std::uint32_t>(packet.size()));
    write_u32_le(out, static_cast<std::uint32_t>(packet.size()));
    out.write(reinterpret_cast<const char*>(packet.data()), static_cast<std::streamsize>(packet.size()));
}

void test_packet_capture_parser_decodes_http_payload() {
    const std::string request = "GET /health HTTP/1.1\r\nHost: localhost:5000\r\n\r\n";
    const std::vector<unsigned char> payload(request.begin(), request.end());
    const std::vector<unsigned char> packet = sample_tcp_packet(payload);
    const std::optional<tze::PacketRecord> record =
        tze::PacketCaptureEngine::parse_packet_bytes(packet.data(), packet.size(), 1, 32, "fixture");
    require(record.has_value(), "Expected packet parser to decode an Ethernet IPv4/TCP packet.");
    require(record->src_port == 5000, "Expected source port 5000.");
    require(record->dst_port == 61234, "Expected destination port 61234.");
    require(record->classification == "plaintext_http", "Expected plaintext HTTP classification.");
    require(record->analysis_code == "NET.TCP.HTTP_PLAINTEXT", "Expected HTTP Simplex analysis code.");
    require(record->payload_text_status.find("text_utf8") != std::string::npos,
            "Expected HTTP payload to expose a UTF-8 text readout.");
    require(!record->plaintext_decode.empty() &&
                record->plaintext_decode.front().find("GET /health") != std::string::npos,
            "Expected HTTP request line decode.");
}

void test_packet_capture_parser_labels_tls_payload() {
    const std::vector<unsigned char> packet = sample_tcp_packet({0x16, 0x03, 0x03, 0x00, 0x2a, 0x01});
    const std::optional<tze::PacketRecord> record =
        tze::PacketCaptureEngine::parse_packet_bytes(packet.data(), packet.size(), 1, 32, "fixture");
    require(record.has_value(), "Expected packet parser to decode TLS-like packet.");
    require(record->classification == "tls_or_encrypted", "Expected TLS-like packet to be labeled encrypted.");
    require(record->analysis_code == "NET.TCP.TLS_OPAQUE", "Expected TLS-like packet to use the opaque TLS code.");
}

void test_packet_capture_parser_classifies_plain_utf8_payload() {
    const std::string text = "hello from plaintext service\n";
    const std::vector<unsigned char> packet = sample_tcp_packet(std::vector<unsigned char>(text.begin(), text.end()));
    const std::optional<tze::PacketRecord> record =
        tze::PacketCaptureEngine::parse_packet_bytes(packet.data(), packet.size(), 1, 64, "fixture");
    require(record.has_value(), "Expected packet parser to decode plain UTF-8 packet.");
    require(record->classification == "text_utf8", "Expected non-HTTP readable payload to classify as text_utf8.");
    require(record->analysis_code == "NET.TCP.TEXT_UTF8", "Expected readable payload Simplex code.");
    require(record->payload_text_utf8.find("hello from plaintext service") != std::string::npos,
            "Expected UTF-8 readout to include plaintext payload.");
}

void test_cli_tview_pcap_fixture_decodes_http() {
    const std::filesystem::path root = kBinaryRoot / "tview-pcap-fixture";
    safe_remove_all(root);
    std::filesystem::create_directories(root);
    const std::string request = "GET /health HTTP/1.1\r\nHost: localhost:5000\r\n\r\n";
    const std::vector<unsigned char> payload(request.begin(), request.end());
    const std::filesystem::path fixture = root / "http-5000.pcap";
    write_pcap_fixture(fixture, sample_tcp_packet(payload));

    const std::filesystem::path binary = kBinaryRoot / "omnix";
    const CommandCapture run = run_command_capture(
        shell_quote(binary.string()) + " tview pcap " + shell_quote(fixture.string()) +
        " --port 5000 --payload-bytes 24 --memory-root " + shell_quote((root / "memory").string()) +
        " --verbose");
    require(run.exit_code == 0, "Expected tview pcap fixture command to succeed.");
    require(run.output.find("OmniXTView: capture_complete") != std::string::npos,
            "Expected tview fixture output to report capture_complete.");
    require(run.output.find("decode: GET /health HTTP/1.1") != std::string::npos,
            "Expected tview fixture output to decode plaintext HTTP.");
}

void test_cli_tview_pcap_fixture_exports_jsonl() {
    const std::filesystem::path root = kBinaryRoot / "tview-jsonl-fixture";
    safe_remove_all(root);
    std::filesystem::create_directories(root);
    const std::string request = "GET /health HTTP/1.1\r\nHost: localhost:5000\r\n\r\n";
    const std::vector<unsigned char> payload(request.begin(), request.end());
    const std::filesystem::path fixture = root / "http-5000.pcap";
    const std::filesystem::path jsonl = root / "packets.jsonl";
    write_pcap_fixture(fixture, sample_tcp_packet(payload));

    const std::filesystem::path binary = kBinaryRoot / "omnix";
    const CommandCapture run = run_command_capture(
        shell_quote(binary.string()) + " tview pcap " + shell_quote(fixture.string()) +
        " --port 5000 --payload-bytes 32 --out " + shell_quote(jsonl.string()) +
        " --memory-root " + shell_quote((root / "memory").string()) + " --verbose");
    require(run.exit_code == 0, "Expected tview pcap JSONL export command to succeed.");
    const std::string exported = xpp::read_text_file(jsonl);
    require(exported.find("\"event_type\":\"omnix.tview.packet.v1\"") != std::string::npos,
            "Expected JSONL export to include versioned TView packet events.");
    require(exported.find("\"analysis_code\":\"NET.TCP.HTTP_PLAINTEXT\"") != std::string::npos,
            "Expected JSONL export to include the Simplex analysis code.");
    require(exported.find("\"payload_text_utf8\"") != std::string::npos,
            "Expected JSONL export to include UTF-8 payload readout field.");
}

void test_unix_evidence_parser_parses_tview_jsonl() {
    tze::ObservationRecord observation;
    observation.id = "obs-tview";
    observation.case_id = "case-tview";
    observation.source_kind = "file";
    observation.source_ref = "packets.jsonl";
    observation.raw_content =
        "{\"event_type\":\"omnix.tview.packet.v1\",\"src_ip\":\"127.0.0.1\",\"src_port\":5000,"
        "\"dst_ip\":\"127.0.0.1\",\"dst_port\":61234,\"payload_length\":48,"
        "\"analysis_code\":\"NET.TCP.HTTP_PLAINTEXT\"}\n";

    const tze::UnixEvidenceParser parser;
    const std::vector<tze::NormalizedObject> objects = parser.parse(observation);
    require(std::find_if(objects.begin(), objects.end(), [](const tze::NormalizedObject& object) {
                return object.object_type == "packet_capture_summary";
            }) != objects.end(),
            "Expected TView JSONL to produce a packet capture summary.");
    require(std::find_if(objects.begin(), objects.end(), [](const tze::NormalizedObject& object) {
                return object.object_type == "packet_flow_summary";
            }) != objects.end(),
            "Expected TView JSONL to produce packet flow evidence.");
    require(std::find_if(objects.begin(), objects.end(), [](const tze::NormalizedObject& object) {
                return object.object_type == "packet_payload_observation";
            }) != objects.end(),
            "Expected TView JSONL to produce packet payload evidence.");
}

void test_intent_resolver_routes_port_investigation_to_tview() {
    tze::IntentResolver resolver;
    const tze::IntentResolution resolution = resolver.resolve("Investigate port 5000");
    require(resolution.intent == tze::RequestIntent::PacketCapture,
            "Expected port investigation to route to packet capture.");
    require(resolution.primary_target == "5000", "Expected port 5000 as packet-capture target.");
}

void test_defense_diagnostic_routes_and_stays_non_destructive() {
    tze::IntentResolver resolver;
    const tze::IntentResolution resolution = resolver.resolve("show CPU hogs");
    require(resolution.intent == tze::RequestIntent::DefenseDiagnostic,
            "Expected CPU diagnostic prompt to route to defense diagnostics.");

    const std::filesystem::path root = kBinaryRoot / "defense-diagnostic";
    safe_remove_all(root);
    std::filesystem::create_directories(root);
    const std::filesystem::path binary = kBinaryRoot / "omnix";
    const CommandCapture run = run_command_capture(
        shell_quote(binary.string()) + " defend diag cpu --memory-root " +
        shell_quote((root / "memory").string()) + " --verbose");
    require(run.exit_code == 0, "Expected defense diagnostic command to succeed.");
    require(run.output.find("Defense diagnostic: defense_diagnostic_complete") != std::string::npos ||
                run.output.find("Defense diagnostic: defense_diagnostic_empty") != std::string::npos,
            "Expected defense diagnostic status in output.");
    require(run.output.find("Diagnostic-only mode") != std::string::npos,
            "Expected defense diagnostic to declare non-destructive behavior.");
}

void test_defense_detection_reports_local_environment_changes() {
    tze::IntentResolver resolver;
    const tze::IntentResolution resolution = resolver.resolve("detect environment changes");
    require(resolution.intent == tze::RequestIntent::DefenseDetection,
            "Expected environmental change prompt to route to defense detection.");

    const std::filesystem::path root = kBinaryRoot / "defense-detection";
    safe_remove_all(root);
    std::filesystem::create_directories(root / "home");
    std::filesystem::create_directories(root / "bin");
    {
        std::ofstream profile(root / "home" / ".zshrc");
        profile << "alias ll='ls -la'\n";
        profile << "omni_helper() { echo ok; }\n";
    }
    const std::filesystem::path evidence = root / "evidence.json";
    const std::filesystem::path binary = kBinaryRoot / "omnix";
    ScopedEnvVar home_override("HOME", (root / "home").string());
    ScopedEnvVar path_override("PATH", (root / "bin").string());
    const CommandCapture run = run_command_capture(
        shell_quote(binary.string()) +
        " defend detect env --max-lines 12 --out " + shell_quote(evidence.string()) +
        " --memory-root " + shell_quote((root / "memory").string()) + " --assist --verbose");
    require(run.exit_code == 0, "Expected defense detection command to succeed.");
    require(run.output.find("Defense detection: defense_detection_complete") != std::string::npos,
            "Expected defense detection status in verbose output.");
    require(run.output.find("x.Defense.EnvironmentDetect") != std::string::npos,
            "Expected defense detection TZE stage to be recorded.");
    require(run.output.find("Detection-only mode") != std::string::npos,
            "Expected defense detection to declare non-mutating behavior.");
    require(run.output.find("local_defense_only") != std::string::npos,
            "Expected defense detection to bypass API/provider assist even when --assist is supplied.");
    require(run.output.find("profile_alias=") != std::string::npos,
            "Expected shell profile alias evidence without dumping arbitrary profile content.");
    const std::string exported = xpp::read_text_file(evidence);
    require(exported.find("omnix.defense.detection.v1") != std::string::npos,
            "Expected defense detection JSON evidence artifact.");
    require(exported.find("\"mode\":\"env\"") != std::string::npos,
            "Expected defense detection artifact to preserve mode.");

    const std::filesystem::path eventviewer_low =
        kSourceRoot / "res" / "ops" / "windows-eventviewer-retention-low.json";
    const CommandCapture eventviewer = run_command_capture(
        shell_quote(binary.string()) +
        " defend detect eventviewer --source " + shell_quote(eventviewer_low.string()) +
        " --compact --memory-root " + shell_quote((root / "memory-eventviewer").string()));
    require(eventviewer.exit_code == 0, "Expected Event Viewer fixture detection to succeed.");
    require(eventviewer.output.find("eventviewer.retention.below_1gb") != std::string::npos,
            "Expected Event Viewer fixture to flag retention below 1GB.");
    require(eventviewer.output.find("alarm-cab: OMNIX-EVENTVIEWER-RETENTION-Security") != std::string::npos,
            "Expected Event Viewer fixture to emit an Alarm CAB recommendation.");

    const CommandCapture eventviewer_live = run_command_capture(
        shell_quote(binary.string()) +
        " defend detect eventviewer --compact --memory-root " +
        shell_quote((root / "memory-eventviewer-live").string()));
    require(eventviewer_live.exit_code == 0,
            "Expected Event Viewer live detection to return a clean status on non-Windows hosts.");
    require(eventviewer_live.output.find("eventviewer.unsupported_or_missing") != std::string::npos ||
                eventviewer_live.output.find("eventviewer.retention") != std::string::npos,
            "Expected Event Viewer live detection to report unsupported/missing or retention evidence.");
}

void test_preflight_validates_recipe_overrides() {
    tze::ProjectAliasRegistry aliases;
    const std::optional<tze::ProjectAlias> alias = aliases.find("nmap");
    require(alias.has_value(), "Expected bundled nmap alias.");

    tze::BuildExecutor executor;
    tze::RequestProfile valid_profile;
    valid_profile.project_reference = "nmap";
    valid_profile.selected_recipe_id = "nmap-configure";
    const tze::PreflightReport valid = executor.preflight(valid_profile, alias);
    require(valid.recipe_id == "nmap-configure", "Expected valid recipe override to be preserved.");
    require(valid.recipe_selection_reason == "manual_override", "Expected manual override selection reason.");

    tze::RequestProfile invalid_profile;
    invalid_profile.project_reference = "nmap";
    invalid_profile.selected_recipe_id = "not-a-real-recipe";
    const tze::PreflightReport invalid = executor.preflight(invalid_profile, alias);
    require(invalid.status == "preflight_failed", "Expected invalid recipe override to fail deterministically.");
    require(std::find(invalid.available_recipe_ids.begin(), invalid.available_recipe_ids.end(), "nmap-configure") !=
                invalid.available_recipe_ids.end(),
            "Expected valid recipe suggestions when a recipe override is invalid.");
}

void test_build_executor_stages_cmake_install() {
    const std::filesystem::path root = kBinaryRoot / "fake-cmake-install";
    const std::filesystem::path bin_dir = root / "bin";
    const std::filesystem::path source_dir = root / "sample-project";
    safe_remove_all(root);
    std::filesystem::create_directories(bin_dir);
    std::filesystem::create_directories(source_dir);

    make_executable(bin_dir / "gcc", "#!/bin/sh\nif [ \"$1\" = \"--version\" ]; then echo 'gcc test'; exit 0; fi\nexit 0\n");
    make_executable(bin_dir / "g++", "#!/bin/sh\nif [ \"$1\" = \"--version\" ]; then echo 'g++ test'; exit 0; fi\nexit 0\n");
    make_executable(
        bin_dir / "cmake",
        "#!/bin/sh\n"
        "if [ \"$1\" = \"--version\" ]; then echo 'cmake version 3.30.0'; exit 0; fi\n"
        "if [ \"$1\" = \"--build\" ]; then shift; build_dir=\"$1\"; /bin/mkdir -p \"$build_dir\"; : > \"$build_dir/omnix\"; exit 0; fi\n"
        "if [ \"$1\" = \"--install\" ]; then shift; build_dir=\"$1\"; prefix=$(/bin/cat \"$build_dir/.install-prefix\"); /bin/mkdir -p \"$prefix/bin\"; : > \"$prefix/bin/omnix\"; exit 0; fi\n"
        "build_dir=''\n"
        "for arg in \"$@\"; do\n"
        "  case \"$arg\" in\n"
        "    -DCMAKE_INSTALL_PREFIX=*) prefix=${arg#*=} ;;\n"
        "  esac\n"
        "  if [ \"$prev\" = \"-B\" ]; then build_dir=\"$arg\"; fi\n"
        "  prev=\"$arg\"\n"
        "done\n"
        "if [ -n \"$build_dir\" ]; then /bin/mkdir -p \"$build_dir\"; echo \"$prefix\" > \"$build_dir/.install-prefix\"; fi\n"
        "exit 0\n");

    {
        std::ofstream cmake_lists(source_dir / "CMakeLists.txt");
        cmake_lists << "cmake_minimum_required(VERSION 3.20)\nproject(Sample LANGUAGES CXX)\n";
    }

    ScopedEnvVar path_override("PATH", bin_dir.string());
    tze::BuildExecutor executor;

    tze::RequestProfile profile;
    profile.build_source_path = source_dir.string();
    profile.build_target = "omnix";
    profile.memory_root_path = (root / "home").string();
    profile.clean_build = true;
    const tze::BuildExecution build = executor.build_source(profile);
    require(build.status == "installed", "Expected staged CMake install to complete.");
    require(!build.install_prefix.empty() && std::filesystem::exists(build.install_prefix),
            "Expected staged install prefix to exist.");
    require(std::find_if(build.verified_install_outputs.begin(),
                         build.verified_install_outputs.end(),
                         [](const std::string& path) { return path.find("bin/omnix") != std::string::npos; }) !=
                build.verified_install_outputs.end(),
            "Expected staged CMake install to verify bin/omnix.");
}

void test_build_executor_stages_lua_recipe_install() {
    const std::filesystem::path root = kBinaryRoot / "fake-lua-install";
    const std::filesystem::path bin_dir = root / "bin";
    const std::filesystem::path source_dir = root / "sample-project";
    safe_remove_all(root);
    std::filesystem::create_directories(bin_dir);
    std::filesystem::create_directories(source_dir);

    make_executable(bin_dir / "gcc", "#!/bin/sh\nif [ \"$1\" = \"--version\" ]; then echo 'gcc test'; exit 0; fi\nexit 0\n");
    make_executable(bin_dir / "g++", "#!/bin/sh\nif [ \"$1\" = \"--version\" ]; then echo 'g++ test'; exit 0; fi\nexit 0\n");
    make_executable(bin_dir / "make",
                    "#!/bin/sh\n"
                    "if [ \"$1\" = \"--version\" ] || [ \"$1\" = \"-v\" ]; then echo 'GNU Make test'; exit 0; fi\n"
                    "/bin/mkdir -p src\n"
                    ": > src/lua\n"
                    ": > src/liblua.a\n"
                    "exit 0\n");

    {
        std::ofstream makefile(source_dir / "Makefile");
        makefile << "all:\n\t@echo ok\n";
    }

    ScopedEnvVar path_override("PATH", bin_dir.string());
    tze::ProjectAliasRegistry aliases;
    const std::optional<tze::ProjectAlias> alias = aliases.find("lua");
    require(alias.has_value(), "Expected bundled lua alias.");

    tze::BuildExecutor executor;
    tze::RequestProfile profile;
    profile.build_source_path = source_dir.string();
    profile.memory_root_path = (root / "home").string();
    const tze::BuildExecution build = executor.build_source(profile, alias);
    require(build.status == "installed", "Expected Lua recipe to stage install by copying verified artifacts.");
    require(std::find_if(build.verified_install_outputs.begin(),
                         build.verified_install_outputs.end(),
                         [](const std::string& path) { return path.find("bin/lua") != std::string::npos; }) !=
                build.verified_install_outputs.end(),
            "Expected Lua staged install to produce bin/lua.");
}

void test_symbol_index_maps_known_symbols() {
    const xpp::MappingUnit unit = xpp::parse_xpp(load_main_source(), (kSourceRoot / "res" / "tze.txt").string());
    const xpp::SymbolIndex index = xpp::build_symbol_index(unit);

    const xpp::SymbolMapping& cache = require_mapping(index, "xProcessingCache");
    require(cache.status == xpp::MappingStatus::Mapped, "xProcessingCache should map to the tze runtime.");
    require(cache.family == xpp::SemanticFamily::Storage, "xProcessingCache should be classified as storage.");

    const xpp::SymbolMapping& operator_symbol = require_mapping(index, "<~>");
    require(operator_symbol.normalized_symbol == "contextual_match", "<~> should normalize consistently.");
    require(!operator_symbol.occurrences.empty(), "<~> should have recorded occurrences.");

    const xpp::SymbolMapping& genx = require_mapping(index, "GENx");
    require(genx.status == xpp::MappingStatus::Mapped, "GENx should now map into the preprocessor runtime.");
    require(genx.family == xpp::SemanticFamily::Preprocessor, "GENx should be classified as preprocessor work.");

    const xpp::SymbolMapping& kill_all = require_mapping(index, "xX_Kill.All");
    require(kill_all.status == xpp::MappingStatus::Unsupported,
            "Destructive branches should be marked unsupported.");

    const xpp::SymbolMapping& native_os = require_mapping(index, "x.readNativeOS");
    require(native_os.status == xpp::MappingStatus::Mapped, "x.readNativeOS should now map into LanguageEngine.");
    require(native_os.mapped_cpp_target == "tze::LanguageEngine::read_native_os",
            "x.readNativeOS should point at the native language engine.");

    const xpp::SymbolMapping& omni_map = require_mapping(index, "xXOmni::Map");
    require(omni_map.status == xpp::MappingStatus::Mapped, "xXOmni::Map should now map into OmniBridge.");

    const xpp::SymbolMapping& search_engine = require_mapping(index, "xccess.SearchEngine");
    require(search_engine.status == xpp::MappingStatus::Mapped,
            "xccess.SearchEngine should now map into OmniBridge.");

    const xpp::SymbolMapping& secure_tunnel = require_mapping(index, "xXOmni::RequestSecureKeyTunnel");
    require(secure_tunnel.status == xpp::MappingStatus::Unsupported,
            "Tunnel-establishment branches should stay explicitly unsupported.");
}

void test_symbol_index_finishes_translation_ready_families() {
    const xpp::MappingUnit unit = xpp::parse_xpp(load_main_source(), (kSourceRoot / "res" / "tze.txt").string());
    const xpp::SymbolIndex index = xpp::build_symbol_index(unit);

    const struct Expectation {
        const char* symbol;
        xpp::MappingStatus status;
        xpp::SemanticFamily family;
        const char* target;
    } expectations[] = {
        {"x.index", xpp::MappingStatus::Mapped, xpp::SemanticFamily::Query, "tze::QueryRuntime::index_value"},
        {"x.seek", xpp::MappingStatus::Mapped, xpp::SemanticFamily::Query, "tze::QueryRuntime::seek_value"},
        {"x.find", xpp::MappingStatus::Mapped, xpp::SemanticFamily::Query, "tze::QueryRuntime::find_matches"},
        {"x.determine", xpp::MappingStatus::Mapped, xpp::SemanticFamily::Query, "tze::QueryRuntime::determine_value"},
        {"x.read", xpp::MappingStatus::Mapped, xpp::SemanticFamily::Query, "tze::QueryRuntime::read_value"},
        {"xin", xpp::MappingStatus::Mapped, xpp::SemanticFamily::Io, "tze::IoRuntime::read_input"},
        {"xout", xpp::MappingStatus::Mapped, xpp::SemanticFamily::Io, "tze::IoRuntime::write_output"},
        {"x.DNLIO", xpp::MappingStatus::Mapped, xpp::SemanticFamily::Language, "tze::LanguageEngine::native_language_io"},
    };

    for (const Expectation& expectation : expectations) {
        const xpp::SymbolMapping& mapping = require_mapping(index, expectation.symbol);
        require(mapping.status == expectation.status, "Unexpected status for " + std::string(expectation.symbol));
        require(mapping.family == expectation.family, "Unexpected semantic family for " + std::string(expectation.symbol));
        require(mapping.mapped_cpp_target == expectation.target, "Unexpected target for " + std::string(expectation.symbol));
        require(mapping.inferred_meaning.find(kGenericFallbackMessage) == std::string::npos,
                "Core blocker symbol should no longer use the generic fallback message: " + std::string(expectation.symbol));
    }
}

void test_security_symbols_are_classified_safely() {
    const xpp::MappingUnit unit = xpp::parse_xpp(load_main_source(), (kSourceRoot / "res" / "tze.txt").string());
    const xpp::SymbolIndex index = xpp::build_symbol_index(unit);

    const xpp::SymbolMapping& classify = require_mapping(index, "x.classify");
    require(classify.status == xpp::MappingStatus::Abstracted || classify.status == xpp::MappingStatus::Mapped,
            "Defensive security symbols should resolve to abstracted or mapped semantics.");
    require(classify.family == xpp::SemanticFamily::SecuritySafe,
            "Defensive security symbols should be marked as safe security semantics.");

    const xpp::SymbolMapping& penetration = require_mapping(index, "x.Penetration");
    require(penetration.status == xpp::MappingStatus::Unsupported,
            "Risky security symbols should remain unsupported.");
    require(penetration.family == xpp::SemanticFamily::SecurityBlocked,
            "Risky security symbols should be marked as blocked security semantics.");
}

void test_emitter_writes_manifest_and_section_files() {
    const xpp::MappingUnit unit = xpp::parse_xpp(load_main_source(), (kSourceRoot / "res" / "tze.txt").string());
    const xpp::SymbolIndex index = xpp::build_symbol_index(unit);
    const std::filesystem::path output_dir = kBinaryRoot / "tests-generated" / "xpp";

    safe_remove_all(output_dir);
    const xpp::EmitReport report = xpp::emit_cpp(unit, index, {output_dir, "generated::xpp", true});

    require(std::filesystem::exists(output_dir / "xpp_generated_support.hpp"),
            "Emitter should create the shared support header.");
    require(std::filesystem::exists(report.manifest_path), "Emitter should create a manifest.");
    require(report.artifacts.size() >= 4, "Emitter should produce one artifact per top-level section.");

    const std::string support_header = xpp::read_text_file(output_dir / "xpp_generated_support.hpp");
    require(support_header.find("abstracted_symbol") != std::string::npos,
            "Emitter support header should include the abstracted symbol helper.");

    const std::vector<std::filesystem::path> safe_sections = {
        output_dir / "build_cmake.cpp",
        output_dir / "x_language_engine.cpp",
        output_dir / "x_self_pre_processor_engine.cpp",
    };
    for (const std::filesystem::path& section_file : safe_sections) {
        const std::string generated = xpp::read_text_file(section_file);
        require(generated.find(kGenericFallbackMessage) == std::string::npos,
                "Safe generated sections should no longer contain the generic fallback message.");
        require(generated.find("Pseudo-language map namespace with no concrete runtime binding yet.") == std::string::npos,
                "Safe generated sections should no longer contain the generic namespace fallback message.");
    }
}

void test_generated_sections_meet_tze_conformance_thresholds() {
    const xpp::MappingUnit unit = xpp::parse_xpp(load_main_source(), (kSourceRoot / "res" / "tze.txt").string());
    const xpp::SymbolIndex index = xpp::build_symbol_index(unit);
    const std::filesystem::path output_dir = kBinaryRoot / "tests-generated-conformance" / "xpp";

    safe_remove_all(output_dir);
    const xpp::EmitReport report = xpp::emit_cpp(unit, index, {output_dir, "generated::xpp", true});
    require(report.artifacts.size() >= 4, "Expected one generated artifact per top-level TZE section.");

    const GeneratedSectionCoverage build = generated_section_coverage(output_dir / "build_cmake.cpp");
    require(build.mapped >= 70, "Expected Build CMake generated coverage to retain at least 70 mapped symbols.");
    require(build.abstracted >= 80, "Expected Build CMake generated coverage to retain at least 80 abstracted symbols.");
    require(build.unsupported >= 5, "Expected Build CMake generated coverage to retain inert unsupported branches.");

    const GeneratedSectionCoverage language = generated_section_coverage(output_dir / "x_language_engine.cpp");
    require(language.mapped >= 80, "Expected Language Engine generated coverage to retain at least 80 mapped symbols.");
    require(language.abstracted >= 4, "Expected Language Engine generated coverage to retain abstracted helper branches.");
    require(language.unsupported >= 4, "Expected Language Engine generated coverage to retain inert unsupported branches.");

    const GeneratedSectionCoverage security = generated_section_coverage(output_dir / "x_security_engine.cpp");
    require(security.mapped >= 8, "Expected Security Engine generated coverage to retain mapped safe symbols.");
    require(security.abstracted >= 27, "Expected Security Engine generated coverage to retain abstracted safe symbols.");
    require(security.unsupported >= 5, "Expected Security Engine generated coverage to retain blocked inert branches.");

    const GeneratedSectionCoverage preprocessor = generated_section_coverage(output_dir / "x_self_pre_processor_engine.cpp");
    require(preprocessor.mapped >= 10, "Expected Self Pre-Processor generated coverage to retain mapped symbols.");
    require(preprocessor.abstracted >= 5, "Expected Self Pre-Processor generated coverage to retain abstracted helpers.");
    require(preprocessor.unsupported == 0, "Expected Self Pre-Processor generated coverage to avoid unsupported branches.");
}

void test_intent_resolver_and_definition_engine() {
    tze::IntentResolver resolver;
    const tze::IntentResolution build = resolver.resolve("Build NMAP");
    require(build.intent == tze::RequestIntent::BuildProject, "Expected Build NMAP to resolve to a build intent.");
    require(build.primary_target == "NMAP", "Expected Build NMAP to preserve the project token.");

    const tze::IntentResolution greeting = resolver.resolve("Hello");
    require(greeting.intent == tze::RequestIntent::Conversation,
            "Expected Hello to resolve to the conversational intent.");

    const tze::IntentResolution identity = resolver.resolve("Who are you");
    require(identity.intent == tze::RequestIntent::Conversation,
            "Expected identity questions to resolve to the conversational intent.");

    const tze::MemoryStore store;
    const std::filesystem::path memory_root = kBinaryRoot / "definition-engine-memory";
    safe_remove_all(memory_root);
    const tze::MemorySnapshot memory = store.load(memory_root);

    tze::DefinitionEngine definitions;
    const tze::DefinitionAnswer answer =
        definitions.lookup("xProcessingCache", (kSourceRoot / "res" / "tze.txt").string(), memory);
    require(answer.found, "Expected definition lookup to resolve xProcessingCache.");
    require(answer.mapped_cpp_target == "tze::CacheCoordinator::prepare",
            "Expected definition lookup to return the mapped runtime target.");
    require(std::find(answer.sources.begin(), answer.sources.end(), "operand_catalogue") != answer.sources.end(),
            "Expected definition lookup to mention the operand catalogue.");
}

void test_memory_store_persists_and_renders_history() {
    const std::filesystem::path memory_root = kBinaryRoot / "memory-store";
    safe_remove_all(memory_root);

    tze::MemoryStore store;
    tze::MemorySnapshot snapshot = store.load(memory_root);
    tze::ProcessingReport report;
    report.raw_prompt = "define xProcessingCache";
    report.resolved_intent = "define_symbol";
    report.answer_status = "defined";
    report.answer_explanation = "Prepare cache context for a new request.";
    store.record_interaction(snapshot, report);

    tze::DefinitionAnswer answer;
    answer.query = "xProcessingCache";
    answer.found = true;
    answer.summary = "Prepare cache context for a new request.";
    answer.mapped_cpp_target = "tze::CacheCoordinator::prepare";
    answer.semantic_family = "storage";
    answer.sources = {"source_map"};
    store.remember_definition(snapshot, answer);
    store.persist_snapshot(snapshot);

    const tze::MemorySnapshot reloaded = store.load(memory_root);
    require(!reloaded.history.empty(), "Expected memory history to persist across reloads.");
    require(reloaded.history.back().prompt == "define xProcessingCache", "Expected persisted history prompt to round-trip.");
    require(!reloaded.definitions.empty(), "Expected stored definitions to persist across reloads.");
    require(store.render_view(reloaded, "history").find("define xProcessingCache") != std::string::npos,
            "Expected memory history rendering to include the stored interaction.");
}

void test_memory_store_compacts_definition_history_summary() {
    const std::filesystem::path memory_root = kBinaryRoot / "memory-store-definition-history";
    safe_remove_all(memory_root);

    tze::MemoryStore store;
    tze::MemorySnapshot snapshot = store.load(memory_root);
    tze::ProcessingReport report;
    report.raw_prompt = "What is the Sun";
    report.resolved_intent = "general_definition_query";
    report.answer_status = "defined";
    report.answer_explanation =
        "The Sun is the star at the center of the Solar System.\n"
        "Definition source: local_glossary\n"
        "Recent feedback: 2026-05-08T21:19:57 | defined | The Sun is the star around which Earth orbits.\n"
        "Suggestions: What is sun?";

    tze::DefinitionAnswer answer;
    answer.query = "sun";
    answer.found = true;
    answer.summary = "The Sun is the star at the center of the Solar System.";
    answer.selected_source_type = "local_glossary";
    report.definition_answer = answer;

    store.record_interaction(snapshot, report);
    store.persist_snapshot(snapshot);

    const tze::MemorySnapshot reloaded = store.load(memory_root);
    require(!reloaded.history.empty(), "Expected compact definition history to persist.");
    require(reloaded.history.back().summary == "The Sun is the star at the center of the Solar System. [source=local_glossary]",
            "Expected definition history to store only the compact final artifact summary.");
    require(reloaded.history.back().summary.find("Recent feedback:") == std::string::npos,
            "Expected definition history to avoid storing the recursive feedback chain.");
}

void test_memory_store_renders_operator_persona_view() {
    const std::filesystem::path memory_root = kBinaryRoot / "memory-persona";
    safe_remove_all(memory_root);

    tze::MemoryStore store;
    tze::MemorySnapshot snapshot = store.load(memory_root);
    tze::OperatorPersonaRecord persona;
    persona.preferred_label = "Premise";
    persona.role_label = "Operator";
    persona.local_username = "premise";
    persona.host_identifier = "MacBookPro";
    persona.last_source_map = "res/tze.txt";
    persona.last_memory_root = memory_root.string();
    persona.self_description = "Primary local analyst.";
    persona.persona_mode = "premise";
    persona.tone_profile = "warm_playful_local_truth";
    persona.interaction_style = "pairing_persistent_momentum";
    persona.safety_posture = "display_only_safety_bounded";
    persona.preferred_next_action_style = "concrete_next_step_with_context";
    persona.custom_phrases = {"who am i", "my persona"};
    store.remember_operator_persona(snapshot, persona);
    store.persist_snapshot(snapshot);

    const tze::MemorySnapshot reloaded = store.load(memory_root);
    require(reloaded.operator_persona.has_value(), "Expected operator persona to persist.");
    require(store.render_view(reloaded, "persona").find("Premise") != std::string::npos,
            "Expected `memory persona` view to include the preferred label.");
    require(store.render_view(reloaded, "operator").find("Primary local analyst.") != std::string::npos,
            "Expected `memory operator` view to include the persona description.");
    require(store.render_view(reloaded, "persona").find("Mode: premise") != std::string::npos,
            "Expected `memory persona` view to include the active persona mode.");
    require(store.render_view(reloaded, "persona").find("Safety posture: display_only_safety_bounded") != std::string::npos,
            "Expected persona modes to render as display-only and safety-bounded.");
}

void test_memory_store_persists_learned_recipes() {
    const std::filesystem::path memory_root = kBinaryRoot / "memory-recipes";
    safe_remove_all(memory_root);

    tze::MemoryStore store;
    tze::MemorySnapshot snapshot = store.load(memory_root);
    snapshot.learned_recipes.push_back({
        "nmap",
        "nmap-configure",
        "macos-arm64|configure",
        "configure",
        2,
        1,
        "2026-03-30T10:00:00",
        "2026-03-29T10:00:00",
        "installed",
        "/tmp/nmap",
        "/tmp/install",
        75,
    });
    store.persist_snapshot(snapshot);

    const tze::MemorySnapshot reloaded = store.load(memory_root);
    require(!reloaded.learned_recipes.empty(), "Expected learned recipe records to persist.");
    require(reloaded.learned_recipes.front().recipe_id == "nmap-configure",
            "Expected learned recipe id to round-trip.");
    require(store.render_view(reloaded, "prefs").find("nmap-configure") != std::string::npos,
            "Expected prefs view to include learned recipe summaries.");
    require(store.render_view(reloaded, "prefs").find("artifact=/tmp/nmap") != std::string::npos,
            "Expected prefs view to include the last artifact hint.");
}

void test_general_definition_routing_prefers_definition_over_context_summary() {
    tze::IntentResolver resolver;

    const tze::IntentResolution sun = resolver.resolve("What is the Sun");
    require(sun.intent == tze::RequestIntent::GeneralDefinitionQuery,
            "Expected `What is the Sun` to resolve as a general definition query.");
    require(sun.primary_target == "Sun" || sun.primary_target == "sun",
            "Expected `What is the Sun` to preserve the concept target.");

    const tze::IntentResolution matter = resolver.resolve("What is matter in terms of science");
    require(matter.intent == tze::RequestIntent::GeneralDefinitionQuery,
            "Expected science-domain matter prompts to resolve as definition queries.");
    require(matter.definition_domain_hint == "science",
            "Expected the domain hint extractor to retain `science`.");

    const tze::IntentResolution apple = resolver.resolve("What is Apple in terms of technology");
    require(apple.intent == tze::RequestIntent::GeneralDefinitionQuery,
            "Expected technology-domain Apple prompts to resolve as definition queries.");
    require(apple.definition_domain_hint == "technology",
            "Expected technology prompts to retain the `technology` domain hint.");
    require(apple.primary_target == "Apple",
            "Expected domain qualifiers to be stripped from the Apple concept target.");

    const tze::IntentResolution steve_jobs = resolver.resolve("Who is Steve Jobs");
    require(steve_jobs.intent == tze::RequestIntent::GeneralDefinitionQuery,
            "Expected `Who is Steve Jobs` to resolve as a general definition query.");
    require(steve_jobs.primary_target == "Steve Jobs",
            "Expected `Who is Steve Jobs` to preserve the person target.");

    const tze::IntentResolution output_turning_scale = resolver.resolve("Output your Turning Scale");
    require(output_turning_scale.intent == tze::RequestIntent::GeneralDefinitionQuery,
            "Expected `Output your Turning Scale` to resolve as a local concept retrieval query.");
    require(output_turning_scale.primary_target == "Turning Scale",
            "Expected local concept retrieval verbs to strip operator-facing possessives.");

    const tze::IntentResolution locate_turning_scale =
        resolver.resolve("Locate your Turning Scale inside your root directory");
    require(locate_turning_scale.intent == tze::RequestIntent::GeneralDefinitionQuery,
            "Expected `Locate your Turning Scale ...` to prefer local concept retrieval before file/tool routing.");
    require(locate_turning_scale.primary_target == "Turning Scale",
            "Expected local concept retrieval to strip lookup context suffixes.");

    const tze::IntentResolution what_matters = resolver.resolve("What matters");
    require(what_matters.intent == tze::RequestIntent::Conversation,
            "Expected `What matters` to remain a contextual summary prompt.");

    const tze::IntentResolution recipe_author = resolver.resolve("Create a build recipe for /tmp/sample-project");
    require(recipe_author.intent == tze::RequestIntent::AuthorBuildRecipe,
            "Expected `Create a build recipe for ...` to resolve as a recipe-authoring intent.");
    require(recipe_author.primary_target == "/tmp/sample-project",
            "Expected recipe authoring to preserve the source-path target.");
}

void test_cli_recipe_author_authors_and_reuses_local_recipe() {
    const std::filesystem::path root = kBinaryRoot / "cli-recipe-author";
    const std::filesystem::path memory_root = root / "memory";
    const std::filesystem::path bin_dir = root / "bin";
    const std::filesystem::path source_dir = root / "sample-project";
    const std::filesystem::path fixture = root / "recipe-plan.json";
    safe_remove_all(root);
    std::filesystem::create_directories(bin_dir);
    std::filesystem::create_directories(source_dir);

    make_executable(bin_dir / "gcc", "#!/bin/sh\nif [ \"$1\" = \"--version\" ]; then echo 'gcc test 14.1.0'; exit 0; fi\nexit 0\n");
    make_executable(bin_dir / "g++", "#!/bin/sh\nif [ \"$1\" = \"--version\" ]; then echo 'g++ test 14.1.0'; exit 0; fi\nexit 0\n");
    make_executable(
        bin_dir / "cmake",
        "#!/bin/sh\n"
        "if [ \"$1\" = \"--version\" ]; then echo 'cmake version 3.30.0'; exit 0; fi\n"
        "if [ \"$1\" = \"--build\" ]; then build_dir=\"$2\"; /bin/mkdir -p \"$build_dir\"; : > \"$build_dir/omnix-app\"; exit 0; fi\n"
        "build_dir=''\n"
        "prev=''\n"
        "for arg in \"$@\"; do\n"
        "  if [ \"$prev\" = \"-B\" ]; then build_dir=\"$arg\"; fi\n"
        "  prev=\"$arg\"\n"
        "done\n"
        "if [ -n \"$build_dir\" ]; then /bin/mkdir -p \"$build_dir\"; fi\n"
        "exit 0\n");

    {
        std::ofstream cmake_lists(source_dir / "CMakeLists.txt");
        cmake_lists << "cmake_minimum_required(VERSION 3.20)\nproject(Sample LANGUAGES CXX)\n";
    }

    {
        std::ofstream output(fixture);
        output << "{\n"
               << "  \"recipe\": {\n"
               << "    \"id\": \"local-sample-cmake\",\n"
               << "    \"acquisition_method\": \"local\",\n"
               << "    \"build_system\": \"cmake\",\n"
               << "    \"supported_platforms\": [\"macos\", \"linux\"],\n"
               << "    \"default_target\": \"omnix-app\",\n"
               << "    \"install_target\": \"install\",\n"
               << "    \"artifact_patterns\": [\"omnix-app\"],\n"
               << "    \"install_output_patterns\": [],\n"
               << "    \"fallback_stage_patterns\": [],\n"
               << "    \"dependency_hints\": [\"Use the local CMake and compiler toolchain.\"],\n"
               << "    \"configure_arguments\": [],\n"
               << "    \"supports_install\": true,\n"
               << "    \"copy_artifacts_on_install\": false\n"
               << "  },\n"
               << "  \"rationale\": \"CMakeLists.txt at the source root makes the local CMake recipe the right fit.\",\n"
               << "  \"evidence_references\": [\"file:CMakeLists.txt\", \"detected_build_system:cmake\"],\n"
               << "  \"confidence\": 0.94,\n"
               << "  \"warnings\": [\"Recipe stays local to the inspected path.\"]\n"
               << "}\n";
    }

    const std::filesystem::path binary = kBinaryRoot / "omnix";
    const std::string author_command =
        "PATH=" + shell_quote(bin_dir.string()) + " "
        "OMNIX_REASONING_PROVIDER=ollama "
        "OMNIX_OLLAMA_MODEL=fixture "
        "OMNIX_OLLAMA_RECIPE_PLAN_FILE=" + shell_quote(fixture.string()) + " " +
        shell_quote(binary.string()) + " recipe author " + shell_quote(source_dir.string()) +
        " --no-install --memory-root " + shell_quote(memory_root.string()) +
        " --source-map " + shell_quote((kSourceRoot / "res" / "tze.txt").string());
    const CommandCapture author = run_command_capture(author_command);
    require(author.exit_code == 0, "Expected fixture-backed recipe authoring to succeed.");
    require(author.output.find("Verdict: recipe_authored") != std::string::npos,
            "Expected recipe authoring to report the authored verdict.");
    require(author.output.find("Recipe authoring status: validated_active") != std::string::npos,
            "Expected recipe authoring to report an active validated recipe.");
    require(author.output.find("Recipe artifact id: local-sample-cmake") != std::string::npos,
            "Expected recipe authoring to surface the generated recipe id.");
    require(author.output.find("RecipeAuthoring.SourceIntake") != std::string::npos,
            "Expected recipe authoring reports to use the HumanReadable source-intake stage name.");
    require(author.output.find("RecipeAuthoring.EvidenceRanking") != std::string::npos,
            "Expected recipe authoring reports to use the HumanReadable evidence-ranking stage name.");
    require(author.output.find("RecipeAuthoring.RecipeDraft") != std::string::npos,
            "Expected recipe authoring reports to use the HumanReadable recipe-draft stage name.");
    require(author.output.find("RecipeAuthoring.ValidateRepairStore") != std::string::npos,
            "Expected recipe authoring reports to use the HumanReadable validation/storage stage name.");

    const CommandCapture preflight = run_command_capture(
        "PATH=" + shell_quote(bin_dir.string()) + " " +
        shell_quote(binary.string()) + " preflight " + shell_quote(source_dir.string()) +
        " --memory-root " + shell_quote(memory_root.string()) +
        " --source-map " + shell_quote((kSourceRoot / "res" / "tze.txt").string()));
    require(preflight.exit_code == 0, "Expected authored recipe reuse to make preflight succeed.");
    require(preflight.output.find("Recipe: local-sample-cmake") != std::string::npos,
            "Expected preflight to reuse the authored recipe id for the exact local path.");
    require(preflight.output.find("Recipe selection: authored_recipe(path_match)") != std::string::npos,
            "Expected preflight to prefer the exact-path authored recipe over the generic fallback.");
}

void test_cli_recipe_author_repairs_failed_first_attempt() {
    const std::filesystem::path root = kBinaryRoot / "cli-recipe-author-repair";
    const std::filesystem::path memory_root = root / "memory";
    const std::filesystem::path bin_dir = root / "bin";
    const std::filesystem::path source_dir = root / "sample-project";
    const std::filesystem::path first_fixture = root / "recipe-plan-first.json";
    const std::filesystem::path repair_fixture = root / "recipe-plan-repair.json";
    safe_remove_all(root);
    std::filesystem::create_directories(bin_dir);
    std::filesystem::create_directories(source_dir);

    make_executable(bin_dir / "gcc", "#!/bin/sh\nif [ \"$1\" = \"--version\" ]; then echo 'gcc test 14.1.0'; exit 0; fi\nexit 0\n");
    make_executable(bin_dir / "g++", "#!/bin/sh\nif [ \"$1\" = \"--version\" ]; then echo 'g++ test 14.1.0'; exit 0; fi\nexit 0\n");
    make_executable(
        bin_dir / "cmake",
        "#!/bin/sh\n"
        "if [ \"$1\" = \"--version\" ]; then echo 'cmake version 3.30.0'; exit 0; fi\n"
        "if [ \"$1\" = \"--build\" ]; then build_dir=\"$2\"; /bin/mkdir -p \"$build_dir\"; : > \"$build_dir/omnix-app\"; exit 0; fi\n"
        "build_dir=''\n"
        "prev=''\n"
        "for arg in \"$@\"; do\n"
        "  if [ \"$prev\" = \"-B\" ]; then build_dir=\"$arg\"; fi\n"
        "  prev=\"$arg\"\n"
        "done\n"
        "if [ -n \"$build_dir\" ]; then /bin/mkdir -p \"$build_dir\"; fi\n"
        "exit 0\n");

    {
        std::ofstream cmake_lists(source_dir / "CMakeLists.txt");
        cmake_lists << "cmake_minimum_required(VERSION 3.20)\nproject(Sample LANGUAGES CXX)\n";
    }

    {
        std::ofstream output(first_fixture);
        output << "{\n"
               << "  \"recipe\": {\n"
               << "    \"id\": \"broken-local-cmake\",\n"
               << "    \"acquisition_method\": \"local\",\n"
               << "    \"build_system\": \"cmake\",\n"
               << "    \"supported_platforms\": [\"macos\", \"linux\"],\n"
               << "    \"default_target\": \"omnix-app\",\n"
               << "    \"install_target\": \"install\",\n"
               << "    \"artifact_patterns\": [\"wrong-artifact\"],\n"
               << "    \"install_output_patterns\": [],\n"
               << "    \"fallback_stage_patterns\": [],\n"
               << "    \"dependency_hints\": [],\n"
               << "    \"configure_arguments\": [],\n"
               << "    \"supports_install\": true,\n"
               << "    \"copy_artifacts_on_install\": false\n"
               << "  },\n"
               << "  \"rationale\": \"Initial guess keeps the project on the local CMake path.\",\n"
               << "  \"evidence_references\": [\"file:CMakeLists.txt\"],\n"
               << "  \"confidence\": 0.62,\n"
               << "  \"warnings\": [\"First attempt may need repair after validation.\"]\n"
               << "}\n";
    }
    {
        std::ofstream output(repair_fixture);
        output << "{\n"
               << "  \"recipe\": {\n"
               << "    \"id\": \"repaired-local-cmake\",\n"
               << "    \"acquisition_method\": \"local\",\n"
               << "    \"build_system\": \"cmake\",\n"
               << "    \"supported_platforms\": [\"macos\", \"linux\"],\n"
               << "    \"default_target\": \"omnix-app\",\n"
               << "    \"install_target\": \"install\",\n"
               << "    \"artifact_patterns\": [\"omnix-app\"],\n"
               << "    \"install_output_patterns\": [],\n"
               << "    \"fallback_stage_patterns\": [],\n"
               << "    \"dependency_hints\": [],\n"
               << "    \"configure_arguments\": [],\n"
               << "    \"supports_install\": true,\n"
               << "    \"copy_artifacts_on_install\": false\n"
               << "  },\n"
               << "  \"rationale\": \"Repair the artifact pattern so OmniX can verify the built output.\",\n"
               << "  \"evidence_references\": [\"file:CMakeLists.txt\", \"build_status=artifact_missing\"],\n"
               << "  \"confidence\": 0.89,\n"
               << "  \"warnings\": [\"Repair uses only local validation feedback.\"]\n"
               << "}\n";
    }

    const std::filesystem::path binary = kBinaryRoot / "omnix";
    const std::string command =
        "PATH=" + shell_quote(bin_dir.string()) + " "
        "OMNIX_REASONING_PROVIDER=ollama "
        "OMNIX_OLLAMA_MODEL=fixture "
        "OMNIX_OLLAMA_RECIPE_PLAN_FILE=" + shell_quote(first_fixture.string()) + " "
        "OMNIX_OLLAMA_RECIPE_PLAN_REPAIR_FILE=" + shell_quote(repair_fixture.string()) + " " +
        shell_quote(binary.string()) + " recipe author " + shell_quote(source_dir.string()) +
        " --no-install --memory-root " + shell_quote(memory_root.string()) +
        " --source-map " + shell_quote((kSourceRoot / "res" / "tze.txt").string());
    const CommandCapture author = run_command_capture(command);
    require(author.exit_code == 0, "Expected repair-backed recipe authoring to succeed.");
    require(author.output.find("Verdict: recipe_authoring_repaired") != std::string::npos,
            "Expected repair-backed recipe authoring to report the repaired verdict.");
    require(author.output.find("Recipe authoring status: validated_active") != std::string::npos,
            "Expected repaired recipe authoring to end in an active validated recipe.");
    require(author.output.find("Recipe artifact id: repaired-local-cmake") != std::string::npos,
            "Expected repaired recipe authoring to surface the repaired recipe id.");
}

void test_cli_general_definition_uses_system_dictionary_fixture() {
    const std::filesystem::path root = kBinaryRoot / "cli-general-definition-system";
    const std::filesystem::path memory_root = root / "memory";
    const std::filesystem::path fixture = root / "dictionary.tsv";
    safe_remove_all(root);
    std::filesystem::create_directories(root);

    {
        std::ofstream output(fixture);
        output << "quasar||A quasar is an extremely luminous active galactic nucleus.\n";
        output << "matter|science|Matter is anything that has mass and occupies space.\n";
    }

    ScopedEnvVar fixture_file("OMNIX_SYSTEM_DICTIONARY_FIXTURE_FILE", fixture.string());
    const std::filesystem::path binary = kBinaryRoot / "omnix";
    const CommandCapture run = run_command_capture(
        shell_quote(binary.string()) + " ask " + shell_quote("What is a quasar") +
        " --memory-root " + shell_quote(memory_root.string()));

    require(run.exit_code == 0, "Expected `omnix ask \"What is a quasar\"` to succeed.");
    require(run.output.find("extremely luminous active galactic nucleus") != std::string::npos,
            "Expected the system dictionary fixture to provide the quasar definition.");
    require(run.output.find("unknown_query") == std::string::npos,
            "Expected the quasar definition query to avoid the unknown_query path.");
}

void test_cli_shell_ask_prefix_preserves_definition_query() {
    const std::filesystem::path root = kBinaryRoot / "cli-shell-ask-prefix-definition";
    const std::filesystem::path memory_root = root / "memory";
    const std::filesystem::path fixture = root / "dictionary.tsv";
    safe_remove_all(root);
    std::filesystem::create_directories(root);

    {
        std::ofstream output(fixture);
        output << "quasar||A quasar is an extremely luminous active galactic nucleus.\n";
    }

    ScopedEnvVar fixture_file("OMNIX_SYSTEM_DICTIONARY_FIXTURE_FILE", fixture.string());
    const std::filesystem::path binary = kBinaryRoot / "omnix";
    const std::string command =
        "printf '%s\\n' 'ask what is a quasar' '/quit' | " +
        shell_quote(binary.string()) + " shell --memory-root " + shell_quote(memory_root.string());

    const CommandCapture shell = run_command_capture(command);
    require(shell.exit_code == 0, "Expected shell `ask what is a quasar` to exit cleanly.");
    require(shell.output.find("extremely luminous active galactic nucleus") != std::string::npos,
            "Expected shell `ask what is a quasar` to preserve the explicit definition query.");
    require(shell.output.find("Verdict: clarify_needed") == std::string::npos,
            "Expected shell `ask what is a quasar` not to collapse into a clarification-only response.");
}

void test_cli_science_definition_beats_what_matters_route() {
    const std::filesystem::path root = kBinaryRoot / "cli-general-definition-science";
    const std::filesystem::path memory_root = root / "memory";
    const std::filesystem::path fixture = root / "dictionary.tsv";
    safe_remove_all(root);
    std::filesystem::create_directories(root);

    {
        std::ofstream output(fixture);
        output << "matter|science|Matter is anything that has mass and occupies space.\n";
    }

    ScopedEnvVar fixture_file("OMNIX_SYSTEM_DICTIONARY_FIXTURE_FILE", fixture.string());
    const std::filesystem::path binary = kBinaryRoot / "omnix";
    const CommandCapture run = run_command_capture(
        shell_quote(binary.string()) + " ask " + shell_quote("What is matter in terms of science") +
        " --memory-root " + shell_quote(memory_root.string()));

    require(run.exit_code == 0, "Expected science-domain matter definition query to succeed.");
    require(run.output.find("In science, matter is physical substance that occupies space and possesses mass.") !=
                std::string::npos ||
            run.output.find("Matter is anything that has mass and occupies space.") != std::string::npos,
            "Expected science-domain matter query to return a definition instead of a contextual summary.");
    require(run.output.find("What matters right now") == std::string::npos,
            "Expected science-domain matter query not to collapse into the context-summary path.");
}

void test_cli_what_matters_stays_contextual() {
    const std::filesystem::path root = kBinaryRoot / "cli-what-matters";
    const std::filesystem::path memory_root = root / "memory";
    safe_remove_all(root);
    std::filesystem::create_directories(root);

    const std::filesystem::path binary = kBinaryRoot / "omnix";
    require(run_command_capture(shell_quote(binary.string()) + " ask " + shell_quote("define xProcessingCache") +
                                " --memory-root " + shell_quote(memory_root.string()) +
                                " --source-map " + shell_quote((kSourceRoot / "res" / "tze.txt").string()))
                .exit_code == 0,
            "Expected a seed definition run to succeed before asking for context summary.");

    const CommandCapture run = run_command_capture(
        shell_quote(binary.string()) + " ask " + shell_quote("What matters") +
        " --memory-root " + shell_quote(memory_root.string()));

    require(run.exit_code == 0, "Expected `What matters` to succeed.");
    require(run.output.find("What matters right now") != std::string::npos,
            "Expected `What matters` to return a contextual summary.");
    require(run.output.find("star at the center of the Solar System") == std::string::npos,
            "Expected `What matters` not to trigger dictionary lookup.");
}

void test_cli_general_definition_glossary_fallback_and_clarification() {
    const std::filesystem::path root = kBinaryRoot / "cli-general-definition-glossary";
    const std::filesystem::path memory_root = root / "memory";
    safe_remove_all(root);
    std::filesystem::create_directories(root);

    ScopedEnvVar disable_dictionary("OMNIX_DISABLE_SYSTEM_DICTIONARY", "1");
    const std::filesystem::path binary = kBinaryRoot / "omnix";

    const CommandCapture glossary = run_command_capture(
        shell_quote(binary.string()) + " ask " + shell_quote("What is matter in terms of science") +
        " --memory-root " + shell_quote(memory_root.string()));
    require(glossary.exit_code == 0, "Expected glossary fallback query to succeed.");
    require(glossary.output.find("In science, matter is physical substance that occupies space and possesses mass.") !=
                std::string::npos ||
            glossary.output.find("anything that has mass and occupies space") != std::string::npos,
            "Expected glossary fallback to answer the science matter definition.");

    const CommandCapture clarify = run_command_capture(
        shell_quote(binary.string()) + " ask " + shell_quote("matter") +
        " --memory-root " + shell_quote(memory_root.string()));
    require(clarify.exit_code == 0,
            "Expected ambiguous single-term prompts to return a handled clarification response.");
    require(clarify.output.find("clarify_needed") != std::string::npos,
            "Expected ambiguous single-term prompts to surface the clarify_needed verdict.");
    require(clarify.output.find("What is matter?") != std::string::npos,
            "Expected ambiguous single-term prompts to suggest the explicit definition form.");
}

void test_cli_general_definition_supports_who_is_and_technology_domain() {
    const std::filesystem::path root = kBinaryRoot / "cli-general-definition-who-is-technology";
    const std::filesystem::path memory_root = root / "memory";
    safe_remove_all(root);
    std::filesystem::create_directories(root);

    ScopedEnvVar disable_dictionary("OMNIX_DISABLE_SYSTEM_DICTIONARY", "1");
    const std::filesystem::path binary = kBinaryRoot / "omnix";

    const CommandCapture apple = run_command_capture(
        shell_quote(binary.string()) + " ask " + shell_quote("What is Apple in terms of technology") +
        " --memory-root " + shell_quote(memory_root.string()));
    require(apple.exit_code == 0, "Expected technology-domain Apple definition query to succeed.");
    require(apple.output.find("The brainchild of Steve Jobs and his team.") != std::string::npos,
            "Expected technology-domain Apple prompts to resolve through the bundled glossary.");

    const CommandCapture steve_jobs = run_command_capture(
        shell_quote(binary.string()) + " ask " + shell_quote("Who is Steve Jobs") +
        " --memory-root " + shell_quote(memory_root.string()));
    require(steve_jobs.exit_code == 0, "Expected `Who is Steve Jobs` to return a handled response.");
    require(steve_jobs.output.find("unknown_intent") == std::string::npos,
            "Expected `Who is Steve Jobs` not to fall through to the unknown intent path.");
}

void test_cli_neuromorphic_backloop_glossary_terms() {
    const std::filesystem::path root = kBinaryRoot / "cli-neuromorphic-glossary";
    const std::filesystem::path memory_root = root / "memory";
    safe_remove_all(root);
    std::filesystem::create_directories(root);

    ScopedEnvVar disable_dictionary("OMNIX_DISABLE_SYSTEM_DICTIONARY", "1");
    const std::filesystem::path binary = kBinaryRoot / "omnix";
    const std::string common = shell_quote(binary.string()) + " define ";
    const std::string memory_arg = " --memory-root " + shell_quote(memory_root.string()) +
        " --source-map " + shell_quote((kSourceRoot / "res" / "tze.txt").string());

    const CommandCapture neuromorphic =
        run_command_capture(common + shell_quote("neuromorphic programming") + memory_arg);
    require(neuromorphic.exit_code == 0, "Expected neuromorphic programming glossary definition to resolve.");
    require(neuromorphic.output.find("brain-inspired") != std::string::npos,
            "Expected neuromorphic programming definition to explain the brain-inspired research track.");

    const CommandCapture backtrace = run_command_capture(common + shell_quote("backtrace") + memory_arg);
    require(backtrace.exit_code == 0, "Expected backtrace glossary definition to resolve.");
    require(backtrace.output.find("source evidence") != std::string::npos,
            "Expected backtrace definition to mention source evidence provenance.");

    const CommandCapture backtest = run_command_capture(common + shell_quote("backtest") + memory_arg);
    require(backtest.exit_code == 0, "Expected backtest glossary definition to resolve.");
    require(backtest.output.find("without mutating memory") != std::string::npos,
            "Expected backtest definition to preserve non-mutating replay semantics.");

    const CommandCapture back_add = run_command_capture(common + shell_quote("back-add") + memory_arg);
    require(back_add.exit_code == 0, "Expected back-add glossary definition to resolve.");
    require(back_add.output.find("without re-ingesting full reasoning chains") != std::string::npos,
            "Expected back-add definition to reject recursive chain ingestion.");
}

void test_cli_neuralnetwork_glossary_terms() {
    const std::filesystem::path root = kBinaryRoot / "cli-neuralnetwork-glossary";
    const std::filesystem::path memory_root = root / "memory";
    safe_remove_all(root);
    std::filesystem::create_directories(root);

    ScopedEnvVar disable_dictionary("OMNIX_DISABLE_SYSTEM_DICTIONARY", "1");
    const std::filesystem::path binary = kBinaryRoot / "omnix";
    const std::string common = shell_quote(binary.string()) + " define ";
    const std::string memory_arg = " --memory-root " + shell_quote(memory_root.string()) +
        " --source-map " + shell_quote((kSourceRoot / "res" / "tze.txt").string());

    const CommandCapture neural_network = run_command_capture(common + shell_quote("neural network") + memory_arg);
    require(neural_network.exit_code == 0, "Expected neural network glossary definition to resolve.");
    require(neural_network.output.find("weights and biases") != std::string::npos,
            "Expected neural network definition to mention weights and biases.");

    const CommandCapture perceptron = run_command_capture(common + shell_quote("perceptron") + memory_arg);
    require(perceptron.exit_code == 0, "Expected perceptron glossary definition to resolve.");
    require(perceptron.output.find("single-layer") != std::string::npos,
            "Expected perceptron definition to mention the single-layer teaching model.");

    const CommandCapture tensorflow = run_command_capture(common + shell_quote("TensorFlow") + memory_arg);
    require(tensorflow.exit_code == 0, "Expected TensorFlow glossary definition to resolve.");
    require(tensorflow.output.find("machine-learning framework") != std::string::npos,
            "Expected TensorFlow definition to describe the ML framework.");

    const CommandCapture simulation = run_command_capture(common + shell_quote("neural simulation") + memory_arg);
    require(simulation.exit_code == 0, "Expected neural simulation glossary definition to resolve.");
    require(simulation.output.find("without requiring specialized neural hardware") != std::string::npos,
            "Expected neural simulation definition to keep simulation independent from specialized hardware.");

    const CommandCapture router = run_command_capture(common + shell_quote("neural signal router") + memory_arg);
    require(router.exit_code == 0, "Expected neural signal router glossary definition to resolve.");
    require(router.output.find("advisory classifier") != std::string::npos,
            "Expected neural signal router definition to keep neural output advisory.");
}

void test_cli_neural_math_perceptron_lab() {
    const std::filesystem::path root = kBinaryRoot / "cli-neural-math";
    const std::filesystem::path memory_root = root / "memory";
    safe_remove_all(root);
    std::filesystem::create_directories(root);
    const std::filesystem::path binary = kBinaryRoot / "omnix";
    const std::string common = shell_quote(binary.string()) +
        " nn math perceptron --epochs 24 --learning-rate 0.2 --memory-root " +
        shell_quote(memory_root.string()) +
        " --source-map " + shell_quote((kSourceRoot / "res" / "tze.txt").string()) +
        " --compact --dataset ";

    const CommandCapture or_run = run_command_capture(common + "or");
    require(or_run.exit_code == 0, "Expected OR perceptron simulation to succeed.");
    require(or_run.output.find("neural_math_complete") != std::string::npos,
            "Expected OR perceptron simulation to complete.");
    require(or_run.output.find("dataset: or") != std::string::npos,
            "Expected OR compact output to include the dataset.");
    require(or_run.output.find("accuracy: 1") != std::string::npos,
            "Expected OR perceptron to converge to full accuracy.");

    const CommandCapture and_run = run_command_capture(common + "and");
    require(and_run.exit_code == 0, "Expected AND perceptron simulation to succeed.");
    require(and_run.output.find("neural_math_complete") != std::string::npos,
            "Expected AND perceptron simulation to complete.");
    require(and_run.output.find("dataset: and") != std::string::npos,
            "Expected AND compact output to include the dataset.");
    require(and_run.output.find("accuracy: 1") != std::string::npos,
            "Expected AND perceptron to converge to full accuracy.");

    const CommandCapture xor_run = run_command_capture(common + "xor");
    require(xor_run.exit_code == 0, "Expected XOR perceptron limitation to fail safely.");
    require(xor_run.output.find("not_linearly_separable") != std::string::npos,
            "Expected XOR to report the single-layer perceptron limitation.");
    require(xor_run.output.find("hidden layer") != std::string::npos,
            "Expected XOR output to explain the MLP requirement.");

    const CommandCapture replay = run_command_capture(
        shell_quote(binary.string()) + " tze replay latest --memory-root " + shell_quote(memory_root.string()));
    require(replay.exit_code == 0, "Expected neural math run to persist into TZE replay.");
    require(replay.output.find("Neural math:") != std::string::npos,
            "Expected replay to include the compact neural math artifact.");
    require(replay.output.find("x.Neural.Math") != std::string::npos,
            "Expected replay to include the neural math TZE stage.");
}

void test_cli_neural_signal_router_tview_jsonl() {
    const std::filesystem::path root = kBinaryRoot / "cli-neural-route";
    const std::filesystem::path memory_root = root / "memory";
    const std::filesystem::path http_jsonl = root / "http.jsonl";
    const std::filesystem::path mixed_http_jsonl = root / "mixed-http.jsonl";
    const std::filesystem::path control_jsonl = root / "control.jsonl";
    const std::filesystem::path route_jsonl = root / "route.jsonl";
    safe_remove_all(root);
    std::filesystem::create_directories(root);

    {
        std::ofstream out(http_jsonl);
        out << "{\"event_type\":\"omnix.tview.packet.v1\",\"timestamp\":\"1\",\"src_ip\":\"127.0.0.1\","
            << "\"src_port\":61234,\"dst_ip\":\"127.0.0.1\",\"dst_port\":5000,\"tcp_flags\":\"AP\","
            << "\"payload_length\":48,\"classification\":\"plaintext_http\","
            << "\"analysis_code\":\"NET.TCP.HTTP_PLAINTEXT\",\"payload_text_utf8\":\"GET / HTTP/1.1\"}\n";
    }
    {
        std::ofstream out(control_jsonl);
        out << "{\"event_type\":\"omnix.tview.packet.v1\",\"timestamp\":\"1\",\"src_ip\":\"127.0.0.1\","
            << "\"src_port\":61234,\"dst_ip\":\"127.0.0.1\",\"dst_port\":5432,\"tcp_flags\":\"S\","
            << "\"payload_length\":0,\"classification\":\"tcp_control\","
            << "\"analysis_code\":\"NET.TCP.CONTROL\"}\n";
    }
    {
        std::ofstream out(mixed_http_jsonl);
        for (int index = 0; index < 16; ++index) {
            out << "{\"event_type\":\"omnix.tview.packet.v1\",\"timestamp\":\"" << index
                << "\",\"src_ip\":\"127.0.0.1\",\"src_port\":61234,\"dst_ip\":\"127.0.0.1\","
                << "\"dst_port\":5000,\"tcp_flags\":\"A\",\"payload_length\":0,\"classification\":\"tcp_control\","
                << "\"analysis_code\":\"NET.TCP.CONTROL\"}\n";
        }
        for (int index = 0; index < 3; ++index) {
            out << "{\"event_type\":\"omnix.tview.packet.v1\",\"timestamp\":\"http" << index
                << "\",\"src_ip\":\"127.0.0.1\",\"src_port\":61234,\"dst_ip\":\"127.0.0.1\","
                << "\"dst_port\":5000,\"tcp_flags\":\"AP\",\"payload_length\":884,\"classification\":\"plaintext_http\","
                << "\"analysis_code\":\"NET.TCP.HTTP_PLAINTEXT\",\"payload_text_utf8\":\"GET / HTTP/1.1\"}\n";
        }
        out << "{\"event_type\":\"omnix.tview.packet.v1\",\"timestamp\":\"utf8\",\"src_ip\":\"127.0.0.1\","
            << "\"src_port\":61234,\"dst_ip\":\"127.0.0.1\",\"dst_port\":5000,\"tcp_flags\":\"AP\","
            << "\"payload_length\":884,\"classification\":\"text_utf8\",\"analysis_code\":\"NET.TCP.TEXT_UTF8\","
            << "\"payload_text_utf8\":\"HTTP response body\"}\n";
    }

    const std::filesystem::path binary = kBinaryRoot / "omnix";
    const std::string route_common = shell_quote(binary.string()) +
        " nn route tview ";
    const std::string common_tail =
        " --memory-root " + shell_quote(memory_root.string()) +
        " --source-map " + shell_quote((kSourceRoot / "res" / "tze.txt").string()) +
        " --compact";

    const CommandCapture http_run = run_command_capture(
        route_common + shell_quote(http_jsonl.string()) + " --out " + shell_quote(route_jsonl.string()) + common_tail);
    require(http_run.exit_code == 0, "Expected HTTP TView route fixture to succeed.");
    require(http_run.output.find("neural_route_complete") != std::string::npos,
            "Expected neural route to complete.");
    require(http_run.output.find("label: plaintext_http") != std::string::npos,
            "Expected HTTP fixture to classify as plaintext_http.");
    require(http_run.output.find("top-factor:") != std::string::npos,
            "Expected compact output to include a top math factor.");
    require(std::filesystem::exists(route_jsonl), "Expected neural route export JSONL to be written.");
    {
        std::ifstream input(route_jsonl);
        std::string exported((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        require(exported.find("\"event_type\":\"omnix.nn.route.v1\"") != std::string::npos,
                "Expected route export to use the versioned neural route event type.");
        require(exported.find("\"label\":\"plaintext_http\"") != std::string::npos,
                "Expected route export to include the plaintext_http label.");
    }

    const CommandCapture replay = run_command_capture(
        shell_quote(binary.string()) + " tze replay latest --memory-root " + shell_quote(memory_root.string()));
    require(replay.exit_code == 0, "Expected neural route run to persist into TZE replay.");
    require(replay.output.find("Neural route:") != std::string::npos,
            "Expected replay to include the neural route report.");
    require(replay.output.find("x.Neural.SignalRouter") != std::string::npos,
            "Expected replay to include the neural signal router stage.");
    require(replay.output.find("Math attributions:") != std::string::npos,
            "Expected replay to backtrace neural route math attributions.");

    const CommandCapture mixed_run = run_command_capture(route_common + shell_quote(mixed_http_jsonl.string()) + common_tail);
    require(mixed_run.exit_code == 0, "Expected mixed control+HTTP TView route fixture to succeed.");
    require(mixed_run.output.find("label: plaintext_http") != std::string::npos,
            "Expected payload-bearing HTTP evidence to outrank control packets in mixed captures.");

    const CommandCapture control_run = run_command_capture(route_common + shell_quote(control_jsonl.string()) + common_tail);
    require(control_run.exit_code == 0, "Expected control-only TView route fixture to succeed.");
    require(control_run.output.find("label: benign_control") != std::string::npos,
            "Expected control-only fixture to classify as benign_control.");
}

void test_definition_flow_records_math_attribution() {
    const std::filesystem::path root = kBinaryRoot / "definition-math-attribution";
    const std::filesystem::path memory_root = root / "memory";
    safe_remove_all(root);
    std::filesystem::create_directories(root);
    ScopedEnvVar disable_dictionary("OMNIX_DISABLE_SYSTEM_DICTIONARY", "1");

    tze::RequestProfile profile;
    profile.raw_prompt = "define Turning Scale";
    profile.definition_concept = "Turning Scale";
    profile.resolved_intent = tze::RequestIntent::DefineSymbol;
    profile.source_map_path = (kSourceRoot / "res" / "tze.txt").string();
    profile.memory_root_path = memory_root.string();

    tze::ProcessingEngine engine;
    const tze::ProcessingReport report = engine.process(profile);
    require(report.definition_answer.has_value() && report.definition_answer->found,
            "Expected Turning Scale to resolve from local sources.");
    require(!report.definition_answer->math_attributions.empty(),
            "Expected definition flow to record math attributions.");
    require(std::find_if(report.definition_answer->math_attributions.begin(),
                         report.definition_answer->math_attributions.end(),
                         [](const tze::MathAttribution& attribution) {
                             return attribution.name == "source_authority";
                         }) != report.definition_answer->math_attributions.end(),
            "Expected definition attribution to include source authority.");
}

void test_tensorflow_env_check_reports_missing_dependencies() {
    const std::filesystem::path script = kSourceRoot / "scripts" / "omnix_tensorflow_env_check.sh";
    const CommandCapture run = run_command_capture(shell_quote(script.string()));
    require(run.output.find("python:") != std::string::npos,
            "Expected TensorFlow env check to report Python status.");
    require(run.output.find("tensorflow:") != std::string::npos,
            "Expected TensorFlow env check to report TensorFlow status.");
    require(run.output.find("omnix_tensorflow_env:") != std::string::npos,
            "Expected TensorFlow env check to emit an overall status.");
}

void test_definition_engine_macos_system_dictionary_bridge() {
#if defined(__APPLE__)
    const std::filesystem::path root = kBinaryRoot / "definition-engine-system-dictionary";
    safe_remove_all(root);
    std::filesystem::create_directories(root);

    ScopedCurrentPath cwd(root);
    tze::DefinitionEngine engine;
    const tze::MemorySnapshot memory;
    const tze::DefinitionAnswer answer = engine.lookup(
        "sun",
        "",
        memory,
        "",
        "Behavioral comparison selected a concept-definition route from the explicit `what is <concept>` phrase.",
        true);

    require(answer.found, "Expected the macOS system dictionary bridge to return a definition.");
    require(answer.selected_source_type == "system_dictionary",
            "Expected the macOS system dictionary bridge to remain a valid live source.");
    require(answer.selected_source_label == "macos_dictionary_services",
            "Expected the macOS system dictionary bridge to report dictionary services provenance.");
    require(!answer.summary.empty(), "Expected the macOS system dictionary bridge to return non-empty text.");
#endif
}

void test_definition_engine_operator_teaching_beats_system_dictionary_fixture() {
    const std::filesystem::path root = kBinaryRoot / "definition-engine-operator-beats-system";
    const std::filesystem::path fixture = root / "dictionary.tsv";
    safe_remove_all(root);
    std::filesystem::create_directories(root / "res");

    {
        std::ofstream glossary(root / "res" / "local_glossary.tsv");
        glossary << "Steve Jobs|Biography|Read his damn book.\n";
    }
    {
        std::ofstream output(fixture);
        output << "Steve Jobs||Jobs, Steven Jobs, Steven | jahbz | (1955-2011), US computer entrepreneur\n";
    }

    ScopedCurrentPath cwd(root);
    ScopedEnvVar fixture_file("OMNIX_SYSTEM_DICTIONARY_FIXTURE_FILE", fixture.string());
    tze::DefinitionEngine engine;
    const tze::MemorySnapshot memory;
    const tze::DefinitionAnswer answer = engine.lookup(
        "Steve Jobs",
        "",
        memory,
        "",
        "Behavioral comparison selected a concept-definition route from the explicit `who is <person>` phrase.",
        true);

    require(answer.found, "Expected operator-taught Steve Jobs definition to resolve.");
    require(answer.selected_source_type == "local_glossary",
            "Expected explicit operator teaching to outrank the system dictionary.");
    require(answer.selected_authority_tier == "operator_override",
            "Expected operator teaching to surface the operator_override authority tier.");
    require(answer.summary == "Read his damn book.",
            "Expected the operator-taught Steve Jobs definition to win on the bare exact match.");
}

void test_definition_engine_operator_domain_ambiguity_clarifies() {
    const std::filesystem::path root = kBinaryRoot / "definition-engine-operator-domain-ambiguity";
    safe_remove_all(root);
    std::filesystem::create_directories(root / "res");

    {
        std::ofstream glossary(root / "res" / "local_glossary.tsv");
        glossary << "Apple|technology|The brainchild of Steve Jobs and his team.\n";
        glossary << "Apple|finance|A public company traded on modern markets.\n";
    }

    ScopedCurrentPath cwd(root);
    ScopedEnvVar disable_dictionary("OMNIX_DISABLE_SYSTEM_DICTIONARY", "1");
    tze::DefinitionEngine engine;
    const tze::MemorySnapshot memory;
    const tze::DefinitionAnswer answer = engine.lookup(
        "Apple",
        "",
        memory,
        "",
        "Behavioral comparison selected a concept-definition route from the explicit `what is <concept>` phrase.",
        true);

    require(!answer.found, "Expected multiple scoped operator meanings without a default to require clarification.");
    require(answer.selected_source_type == "clarification_required",
            "Expected multiple taught domain meanings to return a clarification response.");
    require(answer.summary.find("technology") != std::string::npos &&
                answer.summary.find("finance") != std::string::npos,
            "Expected the clarification response to list the competing taught domains.");
}

void test_definition_engine_reference_cache_does_not_outrank_operator_override() {
    const std::filesystem::path root = kBinaryRoot / "definition-engine-reference-cache-precedence";
    safe_remove_all(root);
    std::filesystem::create_directories(root / "res");

    {
        std::ofstream glossary(root / "res" / "local_glossary.tsv");
        glossary << "Apple|technology|The brainchild of Steve Jobs and his team.\n";
    }

    ScopedCurrentPath cwd(root);
    tze::MemorySnapshot memory;
    memory.definitions.push_back({
        "Apple",
        "apple",
        "technology",
        "A public company that designs consumer electronics.",
        "",
        "general_knowledge",
        "fixture-system-dictionary",
        "system_dictionary",
        "reference_cache",
        0.89,
    });

    tze::DefinitionEngine engine;
    const tze::DefinitionAnswer answer = engine.lookup(
        "Apple",
        "",
        memory,
        "technology",
        "Behavioral comparison selected a concept-definition route from the explicit `what is <concept>` phrase.",
        true);

    require(answer.found, "Expected Apple technology query to resolve.");
    require(answer.selected_source_type == "local_glossary",
            "Expected operator teaching to outrank cached reference definitions.");
    require(answer.summary == "The brainchild of Steve Jobs and his team.",
            "Expected the operator-taught glossary definition to win.");
}

void test_definition_engine_local_retrieval_recovers_close_local_miss() {
    const std::filesystem::path root = kBinaryRoot / "definition-engine-local-retrieval";
    safe_remove_all(root);
    std::filesystem::create_directories(root / "res");

    {
        std::ofstream glossary(root / "res" / "local_glossary.tsv");
        glossary << "Turning Scale|omnix|A local rubric for judging whether OmniX is retrieving, learning, or reasoning from internal memory.\n";
    }

    ScopedCurrentPath cwd(root);
    ScopedEnvVar disable_dictionary("OMNIX_DISABLE_SYSTEM_DICTIONARY", "1");
    tze::DefinitionEngine engine;
    const tze::MemorySnapshot memory;
    const tze::DefinitionAnswer answer = engine.lookup(
        "Turning Scael",
        "",
        memory,
        "omnix",
        "Behavioral comparison selected a concept-definition route from the explicit `what is <concept>` phrase.",
        true);

    require(answer.found, "Expected local retrieval to recover a close glossary miss.");
    require(answer.summary.find("local rubric") != std::string::npos,
            "Expected local retrieval to recover the taught Turning Scale definition.");
    require(answer.comparison_rationale.find("Local retrieval matched the nearest taught concept") != std::string::npos,
            "Expected retrieval-backed matches to expose their provenance.");
}

void test_processing_engine_records_pre_and_post_runtime_stages() {
    const std::filesystem::path root = kBinaryRoot / "processing-pre-post-stages";
    const std::filesystem::path memory_root = root / "memory";
    const std::filesystem::path fixture = root / "dictionary.tsv";
    safe_remove_all(root);
    std::filesystem::create_directories(root);

    {
        std::ofstream output(fixture);
        output << "sun||The Sun is the star at the center of the Solar System.\n";
    }

    ScopedEnvVar fixture_file("OMNIX_SYSTEM_DICTIONARY_FIXTURE_FILE", fixture.string());
    tze::ProcessingEngine engine;
    tze::RequestProfile profile;
    profile.raw_prompt = "What is the Sun";
    profile.resolved_intent = tze::RequestIntent::GeneralDefinitionQuery;
    profile.memory_root_path = memory_root.string();
    profile.source_map_path = (kSourceRoot / "res" / "tze.txt").string();

    const tze::ProcessingReport report = engine.process(profile);
    require(report.uac_state.has_value(), "Expected processing to persist the preprocessor uAC state.");
    require(report.postprocess_record.has_value(), "Expected processing to emit a postprocess record.");
    require(report.postprocess_record->status == "PostSuccess",
            "Expected successful definition lookup to classify as PostSuccess.");

    const auto pre_stage = std::find_if(report.tze_stages.begin(), report.tze_stages.end(),
                                        [](const tze::TzeStageRecord& stage) { return stage.stage_id == "x.Preprocessor"; });
    require(pre_stage != report.tze_stages.end(), "Expected x.Preprocessor to be present in the stage trace.");
    const auto post_stage = std::find_if(report.tze_stages.begin(), report.tze_stages.end(),
                                         [](const tze::TzeStageRecord& stage) { return stage.stage_id == "x.PostProcessor"; });
    require(post_stage != report.tze_stages.end(), "Expected x.PostProcessor to be present in the stage trace.");

    tze::MemoryStore store;
    const tze::MemorySnapshot snapshot = store.load(memory_root);
    require(!snapshot.tze_runs.empty() && snapshot.tze_runs.back().postprocess_record.has_value(),
            "Expected the persisted TZE run to retain its postprocess record.");
    require(store.render_tze_run(snapshot, snapshot.tze_runs.back().id).find("Postprocess:") != std::string::npos,
            "Expected replay output to render the postprocess section.");
}

void test_cli_provider_probe_detects_unusable_ollama_model() {
    const std::filesystem::path root = kBinaryRoot / "cli-provider-probe-ollama-unusable";
    const std::filesystem::path memory_root = root / "memory";
    const std::filesystem::path list_fixture = root / "models.json";
    const std::filesystem::path show_fixture = root / "show.json";
    safe_remove_all(root);
    std::filesystem::create_directories(root);

    {
        std::ofstream output(list_fixture);
        output << "{\n"
               << "  \"models\": [\n"
               << "    {\"name\": \"deepnimsec-omni:latest\"}\n"
               << "  ]\n"
               << "}\n";
    }
    {
        std::ofstream output(show_fixture);
        output << "{\"error\":\"file does not exist\"}\n";
    }

    const std::filesystem::path binary = kBinaryRoot / "omnix";
    const CommandCapture probe = run_command_capture(
        "OMNIX_REASONING_PROVIDER=ollama "
        "OMNIX_OLLAMA_MODEL=deepnimsec-omni:latest "
        "OMNIX_OLLAMA_MODEL_LIST_FILE=" + shell_quote(list_fixture.string()) + " "
        "OMNIX_OLLAMA_MODEL_SHOW_FILE=" + shell_quote(show_fixture.string()) + " " +
        shell_quote(binary.string()) + " provider probe --memory-root " + shell_quote(memory_root.string()));

    require(probe.exit_code != 0, "Expected a listed-but-unusable Ollama model to fail readiness checks.");
    require(probe.output.find("Provider probe: model_unusable") != std::string::npos,
            "Expected provider probe to surface the model_unusable status.");
    require(probe.output.find("./scripts/omnix_deepnimsec.sh --refresh-model") != std::string::npos,
            "Expected DeepNimSec stale-model probes to recommend the refresh-model workflow.");
}

void test_definition_engine_webster_fixture_meta_fallback() {
    const std::filesystem::path root = kBinaryRoot / "definition-engine-webster-meta";
    const std::filesystem::path fixture = root / "webster-meta.html";
    safe_remove_all(root);
    std::filesystem::create_directories(root);

    {
        std::ofstream output(fixture);
        output << "<html><head><meta name=\"description\" content=\"The meaning of AURORA is a natural electrical phenomenon characterized by the appearance of streamers of reddish or greenish light in the sky. How to use aurora in a sentence.\"></head></html>\n";
    }

    ScopedCurrentPath cwd(root);
    ScopedEnvVar disable_dictionary("OMNIX_DISABLE_SYSTEM_DICTIONARY", "1");
    ScopedEnvVar enable_webster("OMNIX_ENABLE_WEBSTER_FALLBACK", "1");
    ScopedEnvVar fixture_file("OMNIX_WEBSTER_FIXTURE_FILE", fixture.string());
    tze::DefinitionEngine engine;
    const tze::MemorySnapshot memory;
    const tze::DefinitionAnswer answer = engine.lookup(
        "aurora",
        "",
        memory,
        "",
        "Behavioral comparison selected a concept-definition route from the explicit `what is <concept>` phrase.",
        true);

    require(answer.found, "Expected the Webster fixture meta fallback to resolve the concept.");
    require(answer.selected_source_type == "webster_fallback",
            "Expected the Webster fixture meta fallback to report Webster provenance.");
    require(answer.selected_source_label == "fixture-merriam-webster",
            "Expected the Webster fixture meta fallback to report the fixture label.");
    require(answer.summary.find("natural electrical phenomenon") != std::string::npos,
            "Expected the Webster fixture meta fallback to retain the extracted definition text.");
}

void test_definition_engine_webster_fixture_dttext_fallback() {
    const std::filesystem::path root = kBinaryRoot / "definition-engine-webster-dttext";
    const std::filesystem::path fixture = root / "webster-dttext.html";
    safe_remove_all(root);
    std::filesystem::create_directories(root);

    {
        std::ofstream output(fixture);
        output << "<html><body><span class=\"dtText\"><strong class=\"mw_t_bc\">: </strong>a small automation guide for OmniX commands</span></body></html>\n";
    }

    ScopedCurrentPath cwd(root);
    ScopedEnvVar disable_dictionary("OMNIX_DISABLE_SYSTEM_DICTIONARY", "1");
    ScopedEnvVar enable_webster("OMNIX_ENABLE_WEBSTER_FALLBACK", "1");
    ScopedEnvVar fixture_file("OMNIX_WEBSTER_FIXTURE_FILE", fixture.string());
    tze::DefinitionEngine engine;
    const tze::MemorySnapshot memory;
    const tze::DefinitionAnswer answer = engine.lookup(
        "omni-guide",
        "",
        memory,
        "",
        "Behavioral comparison selected a concept-definition route from the explicit `what is <concept>` phrase.",
        true);

    require(answer.found, "Expected the Webster fixture body fallback to resolve the concept.");
    require(answer.selected_source_type == "webster_fallback",
            "Expected the Webster fixture body fallback to report Webster provenance.");
    require(answer.summary.find("small automation guide for OmniX commands") != std::string::npos,
            "Expected the Webster fixture body fallback to parse the first visible definition node.");
}

void test_definition_engine_webster_parse_failure_stays_unresolved() {
    const std::filesystem::path root = kBinaryRoot / "definition-engine-webster-invalid";
    const std::filesystem::path fixture = root / "webster-invalid.html";
    safe_remove_all(root);
    std::filesystem::create_directories(root);

    {
        std::ofstream output(fixture);
        output << "<html><body><p>no supported dictionary markup here</p></body></html>\n";
    }

    ScopedCurrentPath cwd(root);
    ScopedEnvVar disable_dictionary("OMNIX_DISABLE_SYSTEM_DICTIONARY", "1");
    ScopedEnvVar enable_webster("OMNIX_ENABLE_WEBSTER_FALLBACK", "1");
    ScopedEnvVar fixture_file("OMNIX_WEBSTER_FIXTURE_FILE", fixture.string());
    tze::DefinitionEngine engine;
    const tze::MemorySnapshot memory;
    const tze::DefinitionAnswer answer = engine.lookup(
        "omni-void",
        "",
        memory,
        "",
        "Behavioral comparison selected a concept-definition route from the explicit `what is <concept>` phrase.",
        true);

    require(!answer.found, "Expected Webster parse failures to fall through to the unresolved response.");
    require(answer.selected_source_type == "unresolved",
            "Expected Webster parse failures not to masquerade as valid dictionary results.");
    require(answer.summary.find("no local definition source answered it") != std::string::npos,
            "Expected Webster parse failures to preserve the unresolved local-definition message.");
}

void test_docs_manpage_and_readme_stay_aligned() {
    const std::string man_markdown = xpp::read_text_file(kSourceRoot / "docs" / "man" / "omnix.1.md");
    const std::string man_roff = xpp::read_text_file(kSourceRoot / "docs" / "man" / "omnix.1");
    const std::string readme = xpp::read_text_file(kSourceRoot / "README.md");
    const std::string roadmap = xpp::read_text_file(kSourceRoot / "docs" / "agile" / "00-roadmap.md");
    const std::string neuromorphic_spec =
        xpp::read_text_file(kSourceRoot / "docs" / "agile" / "101-neuromorphic-backtrace-backtest-backadd.md");

    require(man_markdown.find("build <project-or-path>") != std::string::npos,
            "Expected the Markdown man page to document the guarded canonical build command.");
    require(man_markdown.find("tool <name> -- <args...>") != std::string::npos,
            "Expected the Markdown man page to document the guarded tool command surface.");
    require(man_markdown.find("provider probe") != std::string::npos,
            "Expected the Markdown man page to document provider readiness checks.");
    require(man_markdown.find("persona mode <premise|cynic|professional|neutral>") != std::string::npos,
            "Expected the Markdown man page to document display-only persona modes.");
    require(man_markdown.find("Run NMAP with a local /24 scan") != std::string::npos,
            "Expected the Markdown man page to document the loopback-only Nmap guardrail idiom.");
    require(man_markdown.find("OMNIX_ENABLE_WEBSTER_FALLBACK=1") != std::string::npos,
            "Expected the Markdown man page to document the opt-in Webster fallback flag.");
    require(man_markdown.find("res/local_glossary.tsv") != std::string::npos,
            "Expected the Markdown man page to document glossary authoring in the bundled TSV.");
    require(roadmap.find("101-neuromorphic-backtrace-backtest-backadd.md") != std::string::npos,
            "Expected the roadmap to link the neuromorphic Backtrace/Backtest/Back-add research track.");
    require(neuromorphic_spec.find("Backtrace") != std::string::npos &&
                neuromorphic_spec.find("Backtest") != std::string::npos &&
                neuromorphic_spec.find("Back-add") != std::string::npos,
            "Expected the neuromorphic research spec to define the three back-loop concepts.");

    require(man_roff.find(".TH OMNIX 1") != std::string::npos,
            "Expected the roff man page to declare the OmniX manual header.");
    require(man_roff.find(".SH COMMAND DICTIONARY") != std::string::npos,
            "Expected the roff man page to expose the canonical command dictionary section.");

    require(readme.find("docs/man/omnix.1.md") != std::string::npos,
            "Expected README to link to the Markdown man page mirror.");
    require(readme.find("man -l docs/man/omnix.1") != std::string::npos,
            "Expected README to show how to open the roff man page locally.");
    require(readme.find("command dictionary") != std::string::npos,
            "Expected README to position the man page as the authoritative command dictionary.");
}

void test_unix_evidence_parser_parses_json_logs() {
    tze::ObservationRecord observation;
    observation.id = "obs-json";
    observation.case_id = "case-json";
    observation.source_kind = "file";
    observation.source_ref = "/tmp/events.json";
    observation.summary = "Captured JSON log.";
    observation.raw_content = R"({"event":"login","status":"failed","user":"alice","ip":"10.0.0.5"})";
    observation.content_hash = "json-hash";

    tze::UnixEvidenceParser parser;
    const std::vector<tze::NormalizedObject> objects = parser.parse(observation);
    require(std::find_if(objects.begin(),
                         objects.end(),
                         [](const tze::NormalizedObject& object) { return object.object_type == "json_document"; }) !=
                objects.end(),
            "Expected JSON ingest to preserve the primary JSON document object.");
    const auto summary = std::find_if(objects.begin(),
                                      objects.end(),
                                      [](const tze::NormalizedObject& object) { return object.object_type == "json_field_summary"; });
    require(summary != objects.end(), "Expected JSON ingest to emit a field summary object.");
    require(std::find(summary->attributes.begin(), summary->attributes.end(), "key=event") != summary->attributes.end(),
            "Expected JSON field summary to include extracted keys.");
}

void test_unix_evidence_parser_parses_build_logs() {
    tze::ObservationRecord observation;
    observation.id = "obs-build";
    observation.case_id = "case-build";
    observation.source_kind = "file";
    observation.source_ref = "/tmp/build.log";
    observation.summary = "Captured build log.";
    observation.raw_content =
        "CMake Error at src/main.cpp:12 (message): bad\n"
        "warning: unused variable 'x'\n"
        "Built target omnix\n";
    observation.content_hash = "build-hash";

    tze::UnixEvidenceParser parser;
    const std::vector<tze::NormalizedObject> objects = parser.parse(observation);
    const auto summary = std::find_if(objects.begin(),
                                      objects.end(),
                                      [](const tze::NormalizedObject& object) { return object.object_type == "build_log_summary"; });
    require(summary != objects.end(), "Expected build log ingest to emit a build summary object.");
    require(std::find(summary->attributes.begin(), summary->attributes.end(), "target=omnix") != summary->attributes.end(),
            "Expected build summary to retain built target hints.");
    require(std::find_if(objects.begin(),
                         objects.end(),
                         [](const tze::NormalizedObject& object) { return object.object_type == "build_issue"; }) !=
                objects.end(),
            "Expected build log ingest to emit at least one build issue object.");
}

void test_unix_evidence_parser_parses_ssh_and_tool_output() {
    tze::UnixEvidenceParser parser;

    tze::ObservationRecord auth;
    auth.id = "obs-auth";
    auth.case_id = "case-auth";
    auth.source_kind = "file";
    auth.source_ref = "/tmp/auth.log";
    auth.summary = "Captured auth log.";
    auth.raw_content = "sshd[222]: Failed password for invalid user alice from 10.0.0.5 port 22 ssh2\n";
    auth.content_hash = "auth-hash";

    const std::vector<tze::NormalizedObject> auth_objects = parser.parse(auth);
    const auto auth_event = std::find_if(auth_objects.begin(),
                                         auth_objects.end(),
                                         [](const tze::NormalizedObject& object) { return object.object_type == "ssh_auth_event"; });
    require(auth_event != auth_objects.end(), "Expected auth log ingest to emit an ssh_auth_event object.");
    require(std::find_if(auth_event->attributes.begin(),
                         auth_event->attributes.end(),
                         [](const std::string& attribute) { return attribute.find("user=alice") != std::string::npos; }) !=
                auth_event->attributes.end(),
            "Expected auth event parsing to retain the username.");

    tze::ObservationRecord tool;
    tool.id = "obs-tool";
    tool.case_id = "case-tool";
    tool.source_kind = "command";
    tool.source_ref = "nmap -V";
    tool.summary = "Captured command output from `nmap -V` (exit=0).";
    tool.raw_content = "Nmap version 7.95\nCompiled with: libpcap\n";
    tool.content_hash = "tool-hash";

    const std::vector<tze::NormalizedObject> tool_objects = parser.parse(tool);
    require(std::find_if(tool_objects.begin(),
                         tool_objects.end(),
                         [](const tze::NormalizedObject& object) { return object.object_type == "shell_command_summary"; }) !=
                tool_objects.end(),
            "Expected command ingest to emit a shell command summary.");
    require(std::find_if(tool_objects.begin(),
                         tool_objects.end(),
                         [](const tze::NormalizedObject& object) { return object.object_type == "tool_output_summary"; }) !=
                tool_objects.end(),
            "Expected tool ingest to emit a tool output summary.");
    require(std::find_if(tool_objects.begin(),
                         tool_objects.end(),
                         [](const tze::NormalizedObject& object) { return object.object_type == "nmap_output_summary"; }) !=
                tool_objects.end(),
            "Expected nmap output ingest to emit an nmap summary object.");
}

void test_unix_evidence_parser_parses_host_inventory_report() {
    tze::ObservationRecord host;
    host.id = "obs-host";
    host.case_id = "case-host";
    host.source_kind = "file";
    host.source_ref = "/tmp/host-report.txt";
    host.summary = "Generated host report";
    host.raw_content =
        "OMNIX_HOST_REPORT\n"
        "platform=linux\n"
        "user=root uid=0 gid=0 shell=/bin/bash\n"
        "sudoers_path=/etc/sudoers\n"
        "package_manager=apt-get path=/usr/bin/apt-get\n"
        "log_path=/var/log/auth.log\n"
        "syslog_pattern=auth count=3\n"
        "cron_path=/etc/crontab\n"
        "systemd_unit=sshd.service enabled\n"
        "initrd_path=/boot/initramfs-linux.img\n"
        "native_tool=nmap provider=native_binary path=/usr/bin/nmap\n";
    host.content_hash = "hash-host";

    const tze::UnixEvidenceParser parser;
    const std::vector<tze::NormalizedObject> objects = parser.parse(host);
    require(std::find_if(objects.begin(),
                         objects.end(),
                         [](const tze::NormalizedObject& object) {
                             return object.object_type == "host_inventory_summary";
                         }) != objects.end(),
            "Expected host inspection output to emit a host inventory summary object.");
    require(std::find_if(objects.begin(),
                         objects.end(),
                         [](const tze::NormalizedObject& object) {
                             return object.object_type == "linux_runtime_summary";
                         }) != objects.end(),
            "Expected host inspection output to emit a runtime summary object.");
}

void test_memory_store_persists_cases_and_renders_view() {
    const std::filesystem::path memory_root = kBinaryRoot / "memory-cases";
    safe_remove_all(memory_root);

    tze::MemoryStore store;
    tze::MemorySnapshot snapshot = store.load(memory_root);

    tze::ObservationRecord observation;
    observation.id = "obs-1";
    observation.case_id = "case-1";
    observation.source_kind = "file";
    observation.source_ref = "/tmp/example.log";
    observation.collected_at = "2026-03-31T12:00:00";
    observation.summary = "Captured a sample log.";
    observation.raw_content = "error: sample";
    observation.content_hash = "123";
    store.remember_observation(snapshot, observation);

    tze::NormalizedObject object;
    object.id = "obj-1";
    object.case_id = "case-1";
    object.observation_id = observation.id;
    object.object_type = "log_document";
    object.title = "example.log";
    object.summary = "Normalized sample log.";
    object.attributes = {"error"};
    store.remember_normalized_object(snapshot, object);

    tze::EvidenceLink link;
    link.id = "evidence-1";
    link.case_id = "case-1";
    link.source_observation_id = observation.id;
    link.target_object_id = object.id;
    link.relation = "derived_from";
    link.rationale = "Normalization link.";
    store.remember_evidence_link(snapshot, link);

    tze::AnalystComment comment;
    comment.id = "comment-1";
    comment.case_id = "case-1";
    comment.author = "omnix-system";
    comment.text = "Reviewed the sample case.";
    comment.created_at = "2026-03-31T12:01:00";
    store.remember_analyst_comment(snapshot, comment);

    tze::DecisionCandidate decision;
    decision.id = "decision-1";
    decision.case_id = "case-1";
    decision.title = "Inspect the case";
    decision.rationale = "Safe default.";
    decision.recommended_command = "omnix case case-1";
    decision.status = "recommended";
    decision.score = 60;
    decision.valid = true;
    decision.validity_score = 100;
    decision.evidence_coverage = 35;
    decision.prior_success_score = 50;
    decision.confidence = 0.62;
    decision.probability_likelihood = 0.58;
    decision.supporting_signals = {"fallback"};
    decision.validation_checks = {"case_record_available:pass"};
    decision.score_trace = {"signals=0/0", "validity=1/1", "posterior=0.580", "confidence=0.620"};
    store.remember_decision_candidate(snapshot, decision);

    tze::CaseRecord case_record;
    case_record.id = "case-1";
    case_record.title = "Sample case";
    case_record.primary_source = "/tmp/example.log";
    case_record.status = "analyzed";
    case_record.created_at = "2026-03-31T12:00:00";
    case_record.updated_at = "2026-03-31T12:01:00";
    case_record.observation_ids = {observation.id};
    case_record.object_ids = {object.id};
    case_record.evidence_link_ids = {link.id};
    case_record.comment_ids = {comment.id};
    case_record.decision_ids = {decision.id};
    case_record.latest_summary = "Sample case summary.";
    store.remember_case_record(snapshot, case_record);

    tze::CaseLink case_link;
    case_link.id = "case-link-1";
    case_link.left_case_id = "case-1";
    case_link.right_case_id = "case-2";
    case_link.link_type = "shared_signal";
    case_link.link_value = "error";
    case_link.rationale = "Linked by shared signal `error`.";
    case_link.strength = 60;
    store.remember_case_link(snapshot, case_link);
    store.persist_snapshot(snapshot);

    const tze::MemorySnapshot reloaded = store.load(memory_root);
    require(reloaded.case_records.size() == 1, "Expected persisted case records to round-trip.");
    require(reloaded.observations.size() == 1, "Expected observations to round-trip through the case store.");
    require(reloaded.normalized_objects.size() == 1, "Expected normalized objects to round-trip through the case store.");
    require(reloaded.evidence_links.size() == 1, "Expected evidence links to round-trip through the case store.");
    require(reloaded.analyst_comments.size() == 1, "Expected analyst comments to round-trip through the case store.");
    require(reloaded.decision_candidates.size() == 1, "Expected decision candidates to round-trip through the case store.");
    require(reloaded.case_links.size() == 1, "Expected case links to round-trip through the case store.");
    require(reloaded.decision_candidates.front().valid, "Expected decision validity to round-trip through the case store.");
    require(reloaded.decision_candidates.front().probability_likelihood > 0.5,
            "Expected decision probability likelihood to round-trip through the case store.");
    require(!reloaded.decision_candidates.front().validation_checks.empty(),
            "Expected decision validation checks to round-trip through the case store.");
    require(store.render_view(reloaded, "cases").find("Sample case") != std::string::npos,
            "Expected the cases view to include the stored case title.");
    require(store.render_view(reloaded, "cases").find("links=1") != std::string::npos,
            "Expected the cases view to include case link counts.");
}

void test_processing_engine_uses_source_backed_mappings() {
    tze::ProcessingEngine engine;
    tze::RequestProfile profile;
    profile.instruction_slot = "aZ::1";
    profile.operator_handle = "admin";
    profile.operator_is_admin = true;
    profile.first_run = false;
    profile.persist_on_success = true;
    profile.estimated_size = 4096;
    profile.source_map_path = (kSourceRoot / "res" / "tze.txt").string();
    profile.memory_root_path = (kBinaryRoot / "processing-memory-mappings").string();

    const tze::ProcessingReport report = engine.process(profile);
    require(!report.source_backed_mappings.empty(), "ProcessingReport should expose source-backed symbol mappings.");

    bool found_runtime_mapping = false;
    bool found_query_family = false;
    for (const tze::SourceBackedMapping& mapping : report.source_backed_mappings) {
        if (mapping.symbol == "xProcessingCache" && mapping.mapped_cpp_target == "tze::CacheCoordinator::prepare") {
            found_runtime_mapping = true;
        }
        if (mapping.symbol == "x.index" && mapping.semantic_family == "query") {
            found_query_family = true;
        }
    }
    require(found_runtime_mapping, "Expected xProcessingCache to be exposed through source-backed mappings.");
    require(found_query_family, "Expected query-family mappings to be exposed through source-backed mappings.");
}

void test_runtime_backbone_conforms_to_source_stage_graph() {
    const xpp::MappingUnit unit = xpp::parse_xpp(load_main_source(), (kSourceRoot / "res" / "tze.txt").string());
    const xpp::StageGraph* graph = find_stage_graph(unit, "build_cmake_stage_graph");
    require(graph != nullptr, "Expected the Build CMake stage graph for runtime conformance checks.");

    tze::ProcessingEngine engine;
    tze::RequestProfile profile;
    profile.raw_prompt = "define xProcessingCache";
    profile.instruction_slot = "aZ::99";
    profile.resolved_intent = tze::RequestIntent::DefineSymbol;
    profile.operator_handle = "admin";
    profile.operator_is_admin = true;
    profile.source_map_path = (kSourceRoot / "res" / "tze.txt").string();
    profile.memory_root_path = (kBinaryRoot / "processing-conformance-backbone").string();

    safe_remove_all(profile.memory_root_path);
    const tze::ProcessingReport report = engine.process(profile);

    const std::array<std::string, 5> expected = {
        "xProcessingCache",
        "x.Define.Low",
        "x.DisplayPriorityProcessingGate",
        "x.DisplayFeedBackLoop",
        "x.Store",
    };

    std::vector<std::string> runtime_backbone;
    for (const tze::TzeStageRecord& stage : report.tze_stages) {
        if (std::find(expected.begin(), expected.end(), stage.stage_id) != expected.end()) {
            runtime_backbone.push_back(stage.stage_id);
        }
    }
    require(runtime_backbone.size() == expected.size(),
            "Expected the runtime to preserve the full stable TZE backbone.");
    require(std::equal(runtime_backbone.begin(), runtime_backbone.end(), expected.begin()),
            "Expected the runtime backbone to preserve the source stage order.");

    for (const std::string& stage_id : expected) {
        const xpp::StageNode* source_stage = find_stage_node(*graph, stage_id);
        require(source_stage != nullptr, "Expected source stage metadata for " + stage_id + ".");
        const tze::TzeStageRecord* runtime_stage = find_runtime_stage(report, stage_id);
        require(runtime_stage != nullptr, "Expected runtime stage metadata for " + stage_id + ".");
        require(runtime_stage->source_section == source_stage->section_title,
                "Expected runtime stage section title to match the parsed source stage for " + stage_id + ".");
        require(runtime_stage->source_line == source_stage->line,
                "Expected runtime stage line to match the parsed source stage for " + stage_id + ".");
        require(runtime_stage->source_excerpt == source_stage->source_excerpt,
                "Expected runtime stage excerpt to match the parsed source stage for " + stage_id + ".");
        require(runtime_stage->graph_origin.find("build_cmake_stage_graph") != std::string::npos,
                "Expected runtime stage graph origin to stay tied to the parsed Build CMake stage graph.");
    }

    const tze::MemoryStore store;
    const tze::MemorySnapshot snapshot = store.load(profile.memory_root_path);
    const std::string rendered = store.render_tze_run(snapshot, report.tze_run_id);
    require(rendered.find("Cache.PrepareWorkspace (legacy=xProcessingCache)") != std::string::npos,
            "Expected replay rendering to lead with the HumanReadable cache stage name.");
    require(rendered.find("Intent.DecodeInstruction (legacy=x.Define.Low)") != std::string::npos,
            "Expected replay rendering to lead with the HumanReadable intent stage name.");
    require(rendered.find("Knowledge.EvidenceRanking (legacy=x.DisplayPriorityProcessingGate)") != std::string::npos,
            "Expected replay rendering to lead with the HumanReadable knowledge stage name.");
    require(rendered.find("Memory.FeedbackReview (legacy=x.DisplayFeedBackLoop)") != std::string::npos,
            "Expected replay rendering to lead with the HumanReadable memory feedback stage name.");
    require(rendered.find("Memory.StoreArtifact (legacy=x.Store)") != std::string::npos,
            "Expected replay rendering to lead with the HumanReadable storage stage name.");
    require(rendered.find("Storage.Permanent") != std::string::npos,
            "Expected replay rendering to translate storage namespace vocabulary for operators.");
}

void test_processing_engine_executes_build_flow() {
    const std::filesystem::path root = kBinaryRoot / "fake-processing-build";
    const std::filesystem::path bin_dir = root / "bin";
    const std::filesystem::path source_dir = root / "sample-project";
    safe_remove_all(root);
    std::filesystem::create_directories(bin_dir);
    std::filesystem::create_directories(source_dir);

    make_executable(bin_dir / "gcc", "#!/bin/sh\nif [ \"$1\" = \"--version\" ]; then echo 'gcc test 14.1.0'; exit 0; fi\nexit 0\n");
    make_executable(bin_dir / "g++", "#!/bin/sh\nif [ \"$1\" = \"--version\" ]; then echo 'g++ test 14.1.0'; exit 0; fi\nexit 0\n");
    make_executable(
        bin_dir / "cmake",
        "#!/bin/sh\n"
        "if [ \"$1\" = \"--version\" ]; then echo 'cmake version 3.30.0'; exit 0; fi\n"
        "if [ \"$1\" = \"--build\" ]; then shift; build_dir=\"$1\"; /bin/mkdir -p \"$build_dir\"; : > \"$build_dir/omnix\"; exit 0; fi\n"
        "if [ \"$1\" = \"--install\" ]; then shift; build_dir=\"$1\"; prefix=$(/bin/cat \"$build_dir/.install-prefix\"); /bin/mkdir -p \"$prefix/bin\"; : > \"$prefix/bin/omnix\"; exit 0; fi\n"
        "build_dir=''\n"
        "for arg in \"$@\"; do\n"
        "  case \"$arg\" in\n"
        "    -DCMAKE_INSTALL_PREFIX=*) prefix=${arg#*=} ;;\n"
        "  esac\n"
        "  if [ \"$prev\" = \"-B\" ]; then build_dir=\"$arg\"; fi\n"
        "  prev=\"$arg\"\n"
        "done\n"
        "if [ -n \"$build_dir\" ]; then /bin/mkdir -p \"$build_dir\"; echo \"$prefix\" > \"$build_dir/.install-prefix\"; fi\n"
        "exit 0\n");

    {
        std::ofstream cmake_lists(source_dir / "CMakeLists.txt");
        cmake_lists << "cmake_minimum_required(VERSION 3.20)\nproject(Sample LANGUAGES CXX)\n";
    }

    ScopedEnvVar path_override("PATH", bin_dir.string());
    tze::ProcessingEngine engine;
    tze::RequestProfile profile;
    profile.instruction_slot = "aZ::1";
    profile.operator_handle = "admin";
    profile.operator_is_admin = true;
    profile.execute_build = true;
    profile.build_source_path = source_dir.string();
    profile.build_target = "omnix";
    profile.clean_build = true;
    profile.build_type = "Release";
    profile.source_map_path = (kSourceRoot / "res" / "tze.txt").string();
    profile.memory_root_path = (kBinaryRoot / "processing-build-memory").string();

    const tze::ProcessingReport report = engine.process(profile);
    require(report.preflight_report.has_value(), "Expected build flow reports to include preflight details.");
    require(report.build_execution.has_value(), "Expected build execution details in the processing report.");
    require(report.build_execution->status == "installed", "Expected build flow to complete a staged install.");
    require(!report.build_execution->verified_install_outputs.empty(),
            "Expected build flow to verify staged install outputs.");
    require(find_runtime_stage(report, "xProcessingCache") != nullptr &&
                find_runtime_stage(report, "xProcessingCache")->source_line != 0,
            "Expected build flow to retain source-backed stage provenance.");
}

void test_processing_engine_routes_define_through_feedback_loop() {
    tze::ProcessingEngine engine;
    tze::RequestProfile profile;
    profile.raw_prompt = "define xProcessingCache";
    profile.instruction_slot = "aZ::99";
    profile.resolved_intent = tze::RequestIntent::DefineSymbol;
    profile.source_map_path = (kSourceRoot / "res" / "tze.txt").string();
    profile.memory_root_path = (kBinaryRoot / "processing-define-memory").string();

    safe_remove_all(profile.memory_root_path);
    const tze::ProcessingReport first = engine.process(profile);
    const tze::ProcessingReport second = engine.process(profile);

    require(first.definition_answer.has_value() && first.definition_answer->found,
            "Expected define flow to resolve a known symbol.");
    require(first.query_session.has_value(), "Expected define flow to persist a lightweight query session.");
    require(!first.query_session->operations.empty(), "Expected define flow to record query operations.");
    require(!second.feedback_loop.empty(),
            "Expected repeated define flow to replay prior feedback from memory.");
    require(std::find_if(second.references.begin(),
                         second.references.end(),
                         [](const tze::KnowledgeReference& reference) { return reference.source == "memory"; }) !=
                second.references.end(),
            "Expected define flow to order knowledge sources through the interpreter.");
    require(second.query_session.has_value() && !second.query_session->final_results.empty(),
            "Expected repeated define flow to retain ranked query results.");
}

void test_processing_engine_resolves_language_context_for_define_flow() {
    tze::ProcessingEngine engine;
    tze::RequestProfile profile;
    profile.raw_prompt = "define x.determineOSLanguage";
    profile.instruction_slot = "aZ::99";
    profile.resolved_intent = tze::RequestIntent::DefineSymbol;
    profile.source_map_path = (kSourceRoot / "res" / "tze.txt").string();
    profile.memory_root_path = (kBinaryRoot / "processing-language-memory").string();
    profile.language_confirmation = "yes";

    safe_remove_all(profile.memory_root_path);
    const tze::ProcessingReport report = engine.process(profile);

    require(report.definition_answer.has_value() && report.definition_answer->found,
            "Expected define flow to resolve the language symbol.");
    require(report.language_resolution.has_value(),
            "Expected language-family define flow to resolve a native language context.");
    require(report.language_resolution->manual_confirmation_used,
            "Expected the explicit language confirmation flag to be captured in the resolution record.");
    require(!report.language_resolution->combined_context.empty(),
            "Expected language resolution to emit a combined OS/language context.");
    require(std::find(report.memory_writes.begin(),
                      report.memory_writes.end(),
                      (std::filesystem::path(profile.memory_root_path) / "language_contexts.json").string()) !=
                report.memory_writes.end(),
            "Expected define flow to record the language context memory path as an updated store.");

    tze::MemoryStore store;
    const tze::MemorySnapshot snapshot = store.load(profile.memory_root_path);
    require(!snapshot.language_contexts.empty(), "Expected language context memory to persist across reloads.");
    require(store.render_view(snapshot, "language").find("Language Contexts:") != std::string::npos,
            "Expected memory language view to render stored language contexts.");
    require(!snapshot.tze_runs.empty() && snapshot.tze_runs.back().language_resolution.has_value(),
            "Expected the persisted TZE run to retain its language resolution.");
}

void test_processing_engine_resolves_uac_state_for_define_flow() {
    tze::ProcessingEngine engine;
    tze::RequestProfile profile;
    profile.raw_prompt = "define x.reGENx";
    profile.instruction_slot = "aZ::99";
    profile.resolved_intent = tze::RequestIntent::DefineSymbol;
    profile.source_map_path = (kSourceRoot / "res" / "tze.txt").string();
    profile.memory_root_path = (kBinaryRoot / "processing-uac-memory").string();

    safe_remove_all(profile.memory_root_path);
    const tze::ProcessingReport report = engine.process(profile);

    require(report.definition_answer.has_value() && report.definition_answer->found,
            "Expected define flow to resolve the preprocessor symbol.");
    require(report.uac_state.has_value(),
            "Expected preprocessor-family define flow to resolve a persisted uAC state.");
    require(!report.uac_state->epoch_marker.empty(),
            "Expected the resolved uAC state to expose an epoch marker.");
    require(std::find(report.memory_writes.begin(),
                      report.memory_writes.end(),
                      (std::filesystem::path(profile.memory_root_path) / "uac_states.json").string()) !=
                report.memory_writes.end(),
            "Expected define flow to record the uAC state memory path as an updated store.");

    tze::MemoryStore store;
    const tze::MemorySnapshot snapshot = store.load(profile.memory_root_path);
    require(!snapshot.uac_states.empty(), "Expected uAC state memory to persist across reloads.");
    require(store.render_view(snapshot, "uac").find("uAC States:") != std::string::npos,
            "Expected memory uAC view to render stored preprocessor states.");
    require(!snapshot.tze_runs.empty() && snapshot.tze_runs.back().uac_state.has_value(),
            "Expected the persisted TZE run to retain its uAC state.");
}

void test_security_manager_simulates_safe_and_blocked_branches() {
    tze::MemoryStore store;
    const std::filesystem::path memory_root = kBinaryRoot / "security-manager-memory";
    safe_remove_all(memory_root);
    const tze::MemorySnapshot snapshot = store.load(memory_root.string());

    tze::QueryRuntime runtime;
    tze::QuerySessionRecord session = runtime.open_session("Investigate", "define x.classify");

    tze::SecurityManager manager;
    const tze::SecurityAudit safe = manager.simulate_symbol("x.classify", "security_safe", snapshot, &session);
    require(safe.status == "simulated", "Expected `x.classify` to simulate a safe defensive audit.");
    require(safe.behavior_mode == "simulated", "Expected safe security audit to remain in simulated mode.");
    require(!safe.simulated_actions.empty(), "Expected safe security audit to expose simulated actions.");
    require(safe.blocked_paths.empty(), "Expected safe security audit to avoid blocked path reporting.");

    const tze::SecurityAudit blocked = manager.simulate_symbol("x.Penetration", "security_blocked", snapshot, &session);
    require(blocked.status == "blocked", "Expected `x.Penetration` to remain blocked.");
    require(blocked.behavior_mode == "blocked", "Expected blocked security audit to remain in blocked mode.");
    require(!blocked.blocked_paths.empty(), "Expected blocked security audit to expose blocked paths.");
}

void test_processing_engine_resolves_security_audit_for_define_flow() {
    tze::ProcessingEngine engine;
    tze::RequestProfile profile;
    profile.raw_prompt = "define xXOmni::Detection";
    profile.instruction_slot = "aZ::99";
    profile.resolved_intent = tze::RequestIntent::DefineSymbol;
    profile.source_map_path = (kSourceRoot / "res" / "tze.txt").string();
    profile.memory_root_path = (kBinaryRoot / "processing-security-memory").string();

    safe_remove_all(profile.memory_root_path);
    const tze::ProcessingReport report = engine.process(profile);

    require(report.definition_answer.has_value() && report.definition_answer->found,
            "Expected define flow to resolve the defensive security symbol.");
    require(report.security.status == "simulated",
            "Expected safe security define flow to emit a simulated security audit.");
    require(report.security.behavior_mode == "simulated",
            "Expected safe security define flow to stay in simulated mode.");
    require(!report.security.simulated_actions.empty(),
            "Expected safe security define flow to expose simulated defensive actions.");
    require(std::find(report.memory_writes.begin(),
                      report.memory_writes.end(),
                      (std::filesystem::path(profile.memory_root_path) / "security_audits.json").string()) !=
                report.memory_writes.end(),
            "Expected define flow to record the security audit memory path as an updated store.");

    tze::MemoryStore store;
    const tze::MemorySnapshot snapshot = store.load(profile.memory_root_path);
    require(!snapshot.security_audits.empty(), "Expected security audits to persist across reloads.");
    require(store.render_view(snapshot, "security").find("Security Audits:") != std::string::npos,
            "Expected memory security view to render stored security audits.");
    require(!snapshot.tze_runs.empty() && snapshot.tze_runs.back().security_audit.has_value(),
            "Expected the persisted TZE run to retain its security audit.");
}

void test_source_mapped_runtime_semantics_stay_in_sync() {
    const std::filesystem::path root = kBinaryRoot / "processing-conformance-semantics";
    safe_remove_all(root);

    tze::MemoryStore store;
    const tze::MemorySnapshot cold_memory = store.load((root / "memory").string());
    tze::DefinitionEngine definitions;

    const tze::DefinitionAnswer language_definition =
        definitions.lookup("x.determineOSLanguage", (kSourceRoot / "res" / "tze.txt").string(), cold_memory);
    require(language_definition.found, "Expected x.determineOSLanguage to remain source-backed.");
    require(language_definition.mapped_cpp_target == "tze::LanguageEngine::determine_os_language",
            "Expected x.determineOSLanguage to remain mapped to the language engine.");

    const tze::DefinitionAnswer preprocessor_definition =
        definitions.lookup("x.reGENx", (kSourceRoot / "res" / "tze.txt").string(), cold_memory);
    require(preprocessor_definition.found, "Expected x.reGENx to remain source-backed.");
    require(preprocessor_definition.mapped_cpp_target == "tze::PreprocessorRuntime::regenerate_token",
            "Expected x.reGENx to remain mapped to the preprocessor runtime.");

    const tze::DefinitionAnswer safe_security_definition =
        definitions.lookup("xXOmni::Detection", (kSourceRoot / "res" / "tze.txt").string(), cold_memory);
    require(safe_security_definition.found, "Expected xXOmni::Detection to remain source-backed.");
    require(safe_security_definition.semantic_family == "security_safe",
            "Expected xXOmni::Detection to remain classified as a safe security symbol.");

    const tze::DefinitionAnswer blocked_security_definition =
        definitions.lookup("x.Penetration", (kSourceRoot / "res" / "tze.txt").string(), cold_memory);
    require(blocked_security_definition.found, "Expected x.Penetration to remain indexed for traceability.");
    require(blocked_security_definition.semantic_family == "security_blocked",
            "Expected x.Penetration to remain classified as a blocked security symbol.");

    tze::ProcessingEngine engine;

    tze::RequestProfile language_profile;
    language_profile.raw_prompt = "define x.determineOSLanguage";
    language_profile.instruction_slot = "aZ::99";
    language_profile.resolved_intent = tze::RequestIntent::DefineSymbol;
    language_profile.operator_handle = "admin";
    language_profile.operator_is_admin = true;
    language_profile.language_confirmation = "yes";
    language_profile.source_map_path = (kSourceRoot / "res" / "tze.txt").string();
    language_profile.memory_root_path = (root / "memory").string();
    const tze::ProcessingReport language_report = engine.process(language_profile);
    require(language_report.language_resolution.has_value(),
            "Expected language-family runtime flow to retain a resolved language context.");

    tze::RequestProfile uac_profile = language_profile;
    uac_profile.raw_prompt = "define x.reGENx";
    const tze::ProcessingReport uac_report = engine.process(uac_profile);
    require(uac_report.uac_state.has_value(),
            "Expected preprocessor-family runtime flow to retain a resolved uAC state.");

    tze::RequestProfile safe_security_profile = language_profile;
    safe_security_profile.raw_prompt = "define xXOmni::Detection";
    const tze::ProcessingReport safe_security_report = engine.process(safe_security_profile);
    require(safe_security_report.security.behavior_mode == "simulated",
            "Expected safe security runtime flow to remain simulated.");
    require(!safe_security_report.security.simulated_actions.empty(),
            "Expected safe security runtime flow to retain simulated actions.");

    tze::RequestProfile blocked_security_profile = language_profile;
    blocked_security_profile.raw_prompt = "define x.Penetration";
    const tze::ProcessingReport blocked_security_report = engine.process(blocked_security_profile);
    require(blocked_security_report.security.behavior_mode == "blocked",
            "Expected blocked security runtime flow to remain blocked.");
    require(!blocked_security_report.security.blocked_paths.empty(),
            "Expected blocked security runtime flow to retain blocked paths.");
}

void test_processing_engine_build_nmap_prefers_native_tool() {
    const std::filesystem::path root = kBinaryRoot / "native-first-nmap";
    const std::filesystem::path bin_dir = root / "bin";
    const std::filesystem::path memory_root = root / "memory";
    safe_remove_all(root);
    std::filesystem::create_directories(bin_dir);

    make_executable(bin_dir / "nmap",
                    "#!/bin/sh\n"
                    "if [ \"$1\" = \"-V\" ] || [ \"$1\" = \"--version\" ]; then echo 'Nmap version native-first'; exit 0; fi\n"
                    "echo 'native nmap'\n");

    ScopedEnvVar path_override("PATH", bin_dir.string());
    tze::RequestProfile profile;
    profile.raw_prompt = "Build NMAP";
    profile.project_reference = "nmap";
    profile.instruction_slot = "aZ::1";
    profile.resolved_intent = tze::RequestIntent::BuildProject;
    profile.execute_build = true;
    profile.source_map_path = (kSourceRoot / "res" / "tze.txt").string();
    profile.memory_root_path = memory_root.string();

    tze::ProcessingEngine engine;
    const tze::ProcessingReport report = engine.process(profile);
    require(report.answer_status == "native_ready", "Expected native nmap to short-circuit the build flow.");
    require(report.tool_resolution.has_value() && report.tool_resolution->found,
            "Expected native nmap resolution details in the processing report.");
    require(!report.preflight_report.has_value() || report.preflight_report->status == "native_ready",
            "Expected native nmap preflight status.");
    require(!report.produced_artifact.empty(), "Expected the native nmap path to be reported as the produced artifact.");
}

void test_processing_engine_build_tshark_prefers_native_tool() {
    const std::filesystem::path root = kBinaryRoot / "native-first-tshark";
    const std::filesystem::path bin_dir = root / "bin";
    const std::filesystem::path memory_root = root / "memory";
    safe_remove_all(root);
    std::filesystem::create_directories(bin_dir);

    make_executable(bin_dir / "tshark",
                    "#!/bin/sh\n"
                    "if [ \"$1\" = \"--version\" ] || [ \"$1\" = \"-v\" ]; then echo 'TShark native-first'; exit 0; fi\n"
                    "echo 'native tshark'\n");

    ScopedEnvVar path_override("PATH", bin_dir.string());
    tze::RequestProfile profile;
    profile.raw_prompt = "Build TShark";
    profile.project_reference = "tshark";
    profile.instruction_slot = "aZ::1";
    profile.resolved_intent = tze::RequestIntent::BuildProject;
    profile.execute_build = true;
    profile.source_map_path = (kSourceRoot / "res" / "tze.txt").string();
    profile.memory_root_path = memory_root.string();

    tze::ProcessingEngine engine;
    const tze::ProcessingReport report = engine.process(profile);
    require(report.answer_status == "native_ready", "Expected native tshark to short-circuit the build flow.");
    require(report.resolved_project == "tshark", "Expected the tshark build flow to resolve the canonical tshark project.");
    require(report.tool_resolution.has_value() && report.tool_resolution->found,
            "Expected native tshark resolution details in the processing report.");
    require(report.tool_resolution->logical_name == "tshark",
            "Expected native tshark resolution to preserve the logical tool name.");
    require(!report.preflight_report.has_value() || report.preflight_report->status == "native_ready",
            "Expected native tshark preflight status.");
    require(!report.produced_artifact.empty(), "Expected the native tshark path to be reported as the produced artifact.");
}

void test_processing_engine_keeps_ollama_dormant_when_configured() {
    const std::filesystem::path memory_root = kBinaryRoot / "processing-ollama-dormant";
    safe_remove_all(memory_root);

    ScopedEnvVar provider("OMNIX_REASONING_PROVIDER", "ollama");
    ScopedEnvVar base_url("OMNIX_OLLAMA_BASE_URL", "http://127.0.0.1:9");
    ScopedEnvVar model("OMNIX_OLLAMA_MODEL", "qwen3:8b");

    tze::RequestProfile profile;
    profile.raw_prompt = "define xProcessingCache";
    profile.instruction_slot = "aZ::99";
    profile.resolved_intent = tze::RequestIntent::DefineSymbol;
    profile.source_map_path = (kSourceRoot / "res" / "tze.txt").string();
    profile.memory_root_path = memory_root.string();

    tze::ProcessingEngine engine;
    const tze::ProcessingReport report = engine.process(profile);
    require(report.reasoning_provider == "ollama",
            "Expected configured Ollama provider selection to be visible in the processing report.");
    require(report.answer_status == "defined",
            "Expected the deterministic define flow to remain functional with a dormant Ollama provider.");
    const tze::TzeStageRecord* provider_stage = find_runtime_stage(report, "x.Assist.Provider");
    require(provider_stage != nullptr, "Expected the runtime to retain the provider gate stage.");
    require(provider_stage->status == "configured_idle",
            "Expected the provider gate to show that Ollama is selected but assist is off for this request.");
}

void test_processing_engine_runs_analyst_case_flow() {
    const std::filesystem::path root = kBinaryRoot / "processing-analyst";
    const std::filesystem::path memory_root = root / "memory";
    const std::filesystem::path sample = root / "sample.log";
    safe_remove_all(root);
    std::filesystem::create_directories(root);

    {
        std::ofstream output(sample);
        output << "error: ssh auth failure\n";
        output << "warning: nmap scan requested\n";
    }

    tze::ProcessingEngine engine;
    tze::RequestProfile ingest;
    ingest.raw_prompt = "ingest " + sample.string();
    ingest.analyst_reference = sample.string();
    ingest.resolved_intent = tze::RequestIntent::IngestData;
    ingest.instruction_slot = "aZ::99";
    ingest.source_map_path = (kSourceRoot / "res" / "tze.txt").string();
    ingest.memory_root_path = memory_root.string();

    const tze::ProcessingReport ingested = engine.process(ingest);
    require(ingested.answer_status == "ingested", "Expected analyst ingest flow to create a case.");
    require(ingested.case_record.has_value(), "Expected ingest flow to return a case record.");
    require(ingested.case_record->created_by_run_id == ingested.tze_run_id,
            "Expected ingest flow to link the created case back to the TZE run that created it.");
    require(!ingested.observations.empty(), "Expected ingest flow to persist an observation.");
    require(!ingested.normalized_objects.empty(), "Expected ingest flow to create normalized objects.");
    require(!ingested.evidence_links.empty(), "Expected ingest flow to create evidence links.");
    require(std::find(ingested.storage_writes.begin(),
                      ingested.storage_writes.end(),
                      "x.Store(observation -> " + ingested.observations.front().id + ")") !=
                ingested.storage_writes.end(),
            "Expected ingest flow to record the TZE-aligned observation store write.");

    tze::RequestProfile analyze;
    analyze.raw_prompt = "analyze " + ingested.case_record->id;
    analyze.analyst_reference = ingested.case_record->id;
    analyze.resolved_intent = tze::RequestIntent::AnalyzeCase;
    analyze.instruction_slot = "aZ::99";
    analyze.source_map_path = (kSourceRoot / "res" / "tze.txt").string();
    analyze.memory_root_path = memory_root.string();

    const tze::ProcessingReport analyzed = engine.process(analyze);
    require(analyzed.answer_status == "analyzed", "Expected analyst analyze flow to succeed.");
    require(analyzed.case_record.has_value() && analyzed.case_record->analyzed_by_run_id == analyzed.tze_run_id,
            "Expected analyze flow to link the case back to the TZE run that analyzed it.");
    require(!analyzed.feedback_loop.empty(), "Expected analyze flow to read prior case history.");
    require(!analyzed.analyst_comments.empty(), "Expected analyze flow to add a system analyst comment.");
    require(std::find(analyzed.storage_writes.begin(),
                      analyzed.storage_writes.end(),
                      "x.Store(case.summary -> " + analyzed.case_record->id + ")") != analyzed.storage_writes.end(),
            "Expected analyze flow to record the TZE-aligned case summary store write.");

    tze::RequestProfile decide;
    decide.raw_prompt = "decide " + ingested.case_record->id;
    decide.analyst_reference = ingested.case_record->id;
    decide.resolved_intent = tze::RequestIntent::DecideAction;
    decide.instruction_slot = "aZ::99";
    decide.source_map_path = (kSourceRoot / "res" / "tze.txt").string();
    decide.memory_root_path = memory_root.string();

    const tze::ProcessingReport decided = engine.process(decide);
    require(decided.answer_status == "decided", "Expected analyst decide flow to succeed.");
    require(decided.case_record.has_value() && decided.case_record->decided_by_run_id == decided.tze_run_id,
            "Expected decide flow to link the case back to the TZE run that scored next actions.");
    require(!decided.decision_candidates.empty(), "Expected decide flow to produce decision candidates.");
    require(decided.decision_candidates.front().probability_likelihood > 0.4,
            "Expected the planner to compute a nontrivial probability likelihood.");
    require(decided.decision_candidates.front().confidence > 0.4,
            "Expected the planner to compute a nontrivial confidence score.");
    require(!decided.decision_candidates.front().validation_checks.empty(),
            "Expected the planner to emit validity checks for the top action.");
    require(!decided.decision_candidates.front().score_trace.empty(),
            "Expected the planner to emit a scoring trace for the top action.");
    require(!decided.decision_candidates.front().math_attributions.empty(),
            "Expected the planner to emit weighted math attributions for the top action.");
    require(decided.decision_candidates.front().recommended_command.find("omnix tool") != std::string::npos ||
                decided.decision_candidates.front().recommended_command.find("omnix doctor") != std::string::npos ||
                decided.decision_candidates.front().recommended_command.find("omnix preflight") != std::string::npos,
            "Expected decide flow to recommend safe Omni commands.");

    tze::RequestProfile inspect;
    inspect.raw_prompt = "case " + ingested.case_record->id;
    inspect.analyst_reference = ingested.case_record->id;
    inspect.resolved_intent = tze::RequestIntent::InspectCase;
    inspect.instruction_slot = "aZ::99";
    inspect.source_map_path = (kSourceRoot / "res" / "tze.txt").string();
    inspect.memory_root_path = memory_root.string();

    const tze::ProcessingReport inspected = engine.process(inspect);
    require(inspected.answer_status == "case_loaded", "Expected case inspection flow to succeed.");
    require(inspected.case_record.has_value() && inspected.case_record->id == ingested.case_record->id,
            "Expected case inspection to reload the same case.");
    require(inspected.case_record->created_by_run_id == ingested.tze_run_id,
            "Expected the persisted case record to retain the creation run id.");
    require(inspected.case_record->analyzed_by_run_id == analyzed.tze_run_id,
            "Expected the persisted case record to retain the analyze run id.");
    require(inspected.case_record->decided_by_run_id == decided.tze_run_id,
            "Expected the persisted case record to retain the decide run id.");
    require(!inspected.normalized_objects.empty(), "Expected case inspection to expose persisted normalized objects.");
}

void test_processing_engine_decide_uses_prior_success_history() {
    const std::filesystem::path root = kBinaryRoot / "processing-analyst-history";
    const std::filesystem::path memory_root = root / "memory";
    const std::filesystem::path sample = root / "auth.log";
    safe_remove_all(root);
    std::filesystem::create_directories(root);

    {
        std::ofstream output(sample);
        output << "auth warning detected\n";
        output << "ssh login failure observed\n";
    }

    tze::MemoryStore store;
    tze::MemorySnapshot snapshot = store.load(memory_root);
    tze::ProcessingReport prior;
    prior.raw_prompt = "tool ssh -- -V";
    prior.resolved_intent = "tool_action";
    prior.answer_status = "ok";
    prior.answer_explanation = "SSH client check succeeded.";
    prior.resolved_project = "ssh";
    store.record_interaction(snapshot, prior);
    store.persist_snapshot(snapshot);

    tze::ProcessingEngine engine;
    tze::RequestProfile decide;
    decide.raw_prompt = "decide " + sample.string();
    decide.analyst_reference = sample.string();
    decide.resolved_intent = tze::RequestIntent::DecideAction;
    decide.instruction_slot = "aZ::99";
    decide.source_map_path = (kSourceRoot / "res" / "tze.txt").string();
    decide.memory_root_path = memory_root.string();

    const tze::ProcessingReport report = engine.process(decide);
    require(report.query_session.has_value(), "Expected decide flow to expose the stateful query session.");
    require(std::find_if(report.query_session->operations.begin(),
                         report.query_session->operations.end(),
                         [](const tze::QueryOperation& operation) { return operation.label == "decision-ranking"; }) !=
                report.query_session->operations.end(),
            "Expected decide flow to record decision ranking through the query session.");
    const auto ssh = std::find_if(report.decision_candidates.begin(),
                                  report.decision_candidates.end(),
                                  [](const tze::DecisionCandidate& candidate) {
                                      return candidate.recommended_command.find("omnix tool ssh -- -V") != std::string::npos;
                                  });
    require(ssh != report.decision_candidates.end(), "Expected decide flow to include an SSH validation action.");
    require(ssh->prior_success_score > 50, "Expected prior successful SSH history to improve the prior success score.");
    require(ssh->probability_likelihood > 0.5,
            "Expected prior successful SSH history to help push the action likelihood upward.");
}

void test_processing_engine_decide_uses_helpful_tze_feedback() {
    const std::filesystem::path root = kBinaryRoot / "processing-analyst-feedback";
    const std::filesystem::path memory_root = root / "memory";
    const std::filesystem::path sample = root / "auth.log";
    safe_remove_all(root);
    std::filesystem::create_directories(root);

    {
        std::ofstream output(sample);
        output << "auth warning detected\n";
        output << "ssh login failure observed\n";
    }

    tze::MemoryStore store;
    tze::MemorySnapshot snapshot = store.load(memory_root);
    tze::TzeRunRecord helpful_run;
    helpful_run.id = "tze-run-helpful-ssh";
    helpful_run.timestamp = "2026-04-02T10:00:00";
    helpful_run.intent = "tool_action";
    helpful_run.prompt = "tool ssh -- -V";
    helpful_run.target = "ssh";
    helpful_run.status = "ok";
    helpful_run.reasoning_provider = "null";
    helpful_run.next_action = "Reuse this tool directly.";
    helpful_run.feedback_status = "helpful";
    helpful_run.feedback_note = "Validated the SSH path and it helped the investigation.";
    helpful_run.feedback_timestamp = "2026-04-02T10:05:00";
    store.remember_tze_run(snapshot, helpful_run);
    store.persist_snapshot(snapshot);

    tze::ProcessingEngine engine;
    tze::RequestProfile decide;
    decide.raw_prompt = "decide " + sample.string();
    decide.analyst_reference = sample.string();
    decide.resolved_intent = tze::RequestIntent::DecideAction;
    decide.instruction_slot = "aZ::99";
    decide.source_map_path = (kSourceRoot / "res" / "tze.txt").string();
    decide.memory_root_path = memory_root.string();

    const tze::ProcessingReport report = engine.process(decide);
    const auto ssh = std::find_if(report.decision_candidates.begin(),
                                  report.decision_candidates.end(),
                                  [](const tze::DecisionCandidate& candidate) {
                                      return candidate.recommended_command.find("omnix tool ssh -- -V") != std::string::npos;
                                  });
    require(ssh != report.decision_candidates.end(), "Expected decide flow to include an SSH validation action.");
    require(ssh->prior_success_score > 60,
            "Expected helpful TZE run feedback to improve the prior success score.");
    require(std::find_if(ssh->score_trace.begin(),
                         ssh->score_trace.end(),
                         [](const std::string& line) {
                             return line.find("history_success=") != std::string::npos;
                         }) != ssh->score_trace.end(),
            "Expected the scoring trace to reflect the feedback-adjusted history counts.");
}

void test_processing_engine_lists_searches_and_links_cases() {
    const std::filesystem::path root = kBinaryRoot / "processing-case-links";
    const std::filesystem::path memory_root = root / "memory";
    const std::filesystem::path auth_one = root / "auth-one.log";
    const std::filesystem::path auth_two = root / "auth-two.log";
    safe_remove_all(root);
    std::filesystem::create_directories(root);

    {
        std::ofstream output(auth_one);
        output << "sshd[1]: Failed password for invalid user alice from 10.0.0.5 port 22 ssh2\n";
        output << "warning: ssh auth failure\n";
    }
    {
        std::ofstream output(auth_two);
        output << "sshd[2]: Failed password for invalid user bob from 10.0.0.5 port 22 ssh2\n";
        output << "warning: ssh auth failure\n";
    }

    tze::ProcessingEngine engine;
    auto ingest_and_analyze = [&](const std::filesystem::path& path) -> tze::CaseRecord {
        tze::RequestProfile analyze;
        analyze.raw_prompt = "analyze " + path.string();
        analyze.analyst_reference = path.string();
        analyze.resolved_intent = tze::RequestIntent::AnalyzeCase;
        analyze.instruction_slot = "aZ::99";
        analyze.source_map_path = (kSourceRoot / "res" / "tze.txt").string();
        analyze.memory_root_path = memory_root.string();
        const tze::ProcessingReport report = engine.process(analyze);
        require(report.answer_status == "analyzed", "Expected analyze flow to ingest and analyze the source.");
        require(report.case_record.has_value(), "Expected analyzed flow to return a case record.");
        return *report.case_record;
    };

    const tze::CaseRecord case_one = ingest_and_analyze(auth_one);
    const tze::CaseRecord case_two = ingest_and_analyze(auth_two);

    tze::RequestProfile list;
    list.raw_prompt = "case list";
    list.analyst_reference = "list";
    list.analyst_mode = "list";
    list.resolved_intent = tze::RequestIntent::InspectCase;
    list.instruction_slot = "aZ::99";
    list.source_map_path = (kSourceRoot / "res" / "tze.txt").string();
    list.memory_root_path = memory_root.string();
    const tze::ProcessingReport listed = engine.process(list);
    require(listed.answer_status == "case_listed", "Expected case list flow to succeed.");
    require(listed.case_matches.size() >= 2, "Expected case list flow to return both cases.");
    require(!listed.case_links.empty(), "Expected case list flow to surface derived cross-case links.");
    require(!listed.case_clusters.empty(), "Expected case list flow to surface derived case clusters.");
    require(listed.case_clusters.front().cluster_type == "incident_cluster",
            "Expected two linked cases to form an incident cluster.");

    tze::RequestProfile search;
    search.raw_prompt = "case search 10.0.0.5";
    search.analyst_reference = "search 10.0.0.5";
    search.analyst_mode = "search";
    search.analyst_query = "10.0.0.5";
    search.resolved_intent = tze::RequestIntent::InspectCase;
    search.instruction_slot = "aZ::99";
    search.source_map_path = (kSourceRoot / "res" / "tze.txt").string();
    search.memory_root_path = memory_root.string();
    const tze::ProcessingReport searched = engine.process(search);
    require(searched.answer_status == "case_search_results", "Expected case search flow to find matching cases.");
    require(searched.case_matches.size() >= 2, "Expected case search to match both linked auth cases.");
    require(!searched.case_clusters.empty(), "Expected case search to include matching clusters.");

    tze::RequestProfile inspect;
    inspect.raw_prompt = "case " + case_one.id;
    inspect.analyst_reference = case_one.id;
    inspect.resolved_intent = tze::RequestIntent::InspectCase;
    inspect.instruction_slot = "aZ::99";
    inspect.source_map_path = (kSourceRoot / "res" / "tze.txt").string();
    inspect.memory_root_path = memory_root.string();
    const tze::ProcessingReport inspected = engine.process(inspect);
    require(inspected.answer_status == "case_loaded", "Expected case inspect flow to succeed.");
    require(std::find_if(inspected.case_links.begin(),
                         inspected.case_links.end(),
                         [&case_two](const tze::CaseLink& link) {
                             return link.left_case_id == case_two.id || link.right_case_id == case_two.id;
                         }) != inspected.case_links.end(),
            "Expected case inspection to include links to the related case.");
    require(!inspected.case_clusters.empty(), "Expected case inspection to include the related cluster.");
}

void test_processing_engine_derives_campaign_clusters() {
    const std::filesystem::path root = kBinaryRoot / "processing-case-clusters";
    const std::filesystem::path memory_root = root / "memory";
    safe_remove_all(root);
    std::filesystem::create_directories(root);

    const std::vector<std::filesystem::path> samples = {
        root / "ssh-one.log",
        root / "ssh-two.log",
        root / "ssh-three.log",
    };
    const std::vector<std::string> contents = {
        "sshd[1]: Failed password for invalid user alice from 172.16.0.9 port 22 ssh2\nwarning: ssh auth failure\n",
        "sshd[2]: Failed password for invalid user bob from 172.16.0.9 port 22 ssh2\nwarning: ssh auth failure\n",
        "sshd[3]: Accepted password for carol from 172.16.0.9 port 22 ssh2\nwarning: ssh auth failure\n",
    };

    for (std::size_t index = 0; index < samples.size(); ++index) {
        std::ofstream output(samples[index]);
        output << contents[index];
    }

    tze::ProcessingEngine engine;
    for (const std::filesystem::path& sample : samples) {
        tze::RequestProfile analyze;
        analyze.raw_prompt = "analyze " + sample.string();
        analyze.analyst_reference = sample.string();
        analyze.resolved_intent = tze::RequestIntent::AnalyzeCase;
        analyze.instruction_slot = "aZ::99";
        analyze.source_map_path = (kSourceRoot / "res" / "tze.txt").string();
        analyze.memory_root_path = memory_root.string();
        const tze::ProcessingReport report = engine.process(analyze);
        require(report.answer_status == "analyzed", "Expected clustered sample to analyze successfully.");
    }

    tze::RequestProfile list;
    list.raw_prompt = "case list";
    list.analyst_reference = "list";
    list.analyst_mode = "list";
    list.resolved_intent = tze::RequestIntent::InspectCase;
    list.instruction_slot = "aZ::99";
    list.source_map_path = (kSourceRoot / "res" / "tze.txt").string();
    list.memory_root_path = memory_root.string();
    const tze::ProcessingReport listed = engine.process(list);
    require(!listed.case_clusters.empty(), "Expected campaign clustering to produce at least one cluster.");
    require(std::find_if(listed.case_clusters.begin(),
                         listed.case_clusters.end(),
                         [](const tze::CaseCluster& cluster) {
                             return cluster.cluster_type == "campaign_cluster" &&
                                 cluster.case_count >= 3 &&
                                 cluster.correlation_score > 60;
                         }) != listed.case_clusters.end(),
            "Expected three linked cases to form a scored campaign cluster.");
}

void test_processing_engine_decide_surfaces_safe_modules() {
    const std::filesystem::path root = kBinaryRoot / "processing-analyst-safe-modules";
    const std::filesystem::path memory_root = root / "memory";
    const std::filesystem::path sample = root / "combined.log";
    safe_remove_all(root);
    std::filesystem::create_directories(root);

    {
        std::ofstream output(sample);
        output << "warning: build failed for target omnix\n";
        output << "sshd[1]: Failed password for invalid user alice from 10.1.2.3 port 22 ssh2\n";
        output << "nmap requested for host survey\n";
    }

    tze::ProcessingEngine engine;
    tze::RequestProfile decide;
    decide.raw_prompt = "decide " + sample.string();
    decide.analyst_reference = sample.string();
    decide.resolved_intent = tze::RequestIntent::DecideAction;
    decide.instruction_slot = "aZ::99";
    decide.source_map_path = (kSourceRoot / "res" / "tze.txt").string();
    decide.memory_root_path = memory_root.string();

    const tze::ProcessingReport report = engine.process(decide);
    require(report.answer_status == "decided", "Expected decide flow to succeed.");
    require(std::find_if(report.decision_candidates.begin(),
                         report.decision_candidates.end(),
                         [](const tze::DecisionCandidate& candidate) {
                             return candidate.recommended_command.find("omnix tool inspect-log --") != std::string::npos;
                         }) != report.decision_candidates.end(),
            "Expected decide flow to recommend structured log inspection.");
    require(std::find_if(report.decision_candidates.begin(),
                         report.decision_candidates.end(),
                         [](const tze::DecisionCandidate& candidate) {
                             return candidate.recommended_command.find("omnix tool inspect-build --") != std::string::npos;
                         }) != report.decision_candidates.end(),
            "Expected decide flow to recommend build inspection.");
    require(std::find_if(report.decision_candidates.begin(),
                         report.decision_candidates.end(),
                         [](const tze::DecisionCandidate& candidate) {
                             return candidate.recommended_command.find("omnix tool report-case --") != std::string::npos;
                         }) != report.decision_candidates.end(),
            "Expected decide flow to recommend case report generation.");
    require(std::find_if(report.decision_candidates.begin(),
                         report.decision_candidates.end(),
                         [](const tze::DecisionCandidate& candidate) {
                             return candidate.recommended_command.find("omnix tool text-pipeline --") != std::string::npos;
                         }) != report.decision_candidates.end(),
            "Expected decide flow to recommend the safe text pipeline module.");
    require(std::find_if(report.decision_candidates.begin(),
                         report.decision_candidates.end(),
                         [](const tze::DecisionCandidate& candidate) {
                             return candidate.recommended_command.find("omnix tool inspect-host -- --linux") != std::string::npos;
                         }) != report.decision_candidates.end(),
            "Expected decide flow to recommend host inspection for host-relevant evidence.");
}

void test_cli_define_and_memory_history() {
    const std::filesystem::path memory_root = kBinaryRoot / "cli-memory-define";
    safe_remove_all(memory_root);
    const std::filesystem::path binary = kBinaryRoot / "omnix";

    const CommandCapture define = run_command_capture(
        shell_quote(binary.string()) + " define xProcessingCache " +
        shell_quote((kSourceRoot / "res" / "tze.txt").string()) + " --memory-root " + shell_quote(memory_root.string()));
    require(define.exit_code == 0, "Expected `omnix define` to succeed.");
    require(define.output.find("TZE run:") != std::string::npos,
            "Expected `omnix define` output to include the persisted TZE run identifier.");
    require(define.output.find("TZE stages:") != std::string::npos,
            "Expected `omnix define` output to include the TZE stage trace.");
    require(define.output.find("x.Define.Low") != std::string::npos,
            "Expected `omnix define` output to include the TZE intent stage.");
    require(define.output.find("Mapped target: tze::CacheCoordinator::prepare") != std::string::npos,
            "Expected `omnix define` output to include the mapped target.");

    const CommandCapture history = run_command_capture(
        shell_quote(binary.string()) + " memory history --memory-root " + shell_quote(memory_root.string()));
    require(history.exit_code == 0, "Expected `omnix memory history` to succeed.");
    require(history.output.find("define xProcessingCache") != std::string::npos,
            "Expected memory history output to include the prior definition lookup.");

    const CommandCapture runs = run_command_capture(
        shell_quote(binary.string()) + " memory runs --memory-root " + shell_quote(memory_root.string()));
    require(runs.exit_code == 0, "Expected `omnix memory runs` to succeed.");
    require(runs.output.find("define_symbol") != std::string::npos,
            "Expected TZE runs output to include the prior definition intent.");
    require(runs.output.find("x.Store") != std::string::npos,
            "Expected TZE runs output to include the persisted store stage.");
}

void test_cli_language_resolution_and_memory_view() {
    const std::filesystem::path memory_root = kBinaryRoot / "cli-memory-language";
    safe_remove_all(memory_root);
    const std::filesystem::path binary = kBinaryRoot / "omnix";
    const std::string source_map = shell_quote((kSourceRoot / "res" / "tze.txt").string());

    const CommandCapture define = run_command_capture(
        shell_quote(binary.string()) + " define x.determineOSLanguage " + source_map +
        " --lang-confirm yes --memory-root " + shell_quote(memory_root.string()));
    require(define.exit_code == 0, "Expected `omnix define x.determineOSLanguage` to succeed.");
    require(define.output.find("Language context:") != std::string::npos,
            "Expected CLI define output to include the resolved language context.");
    const std::string run_id = extract_line_value(define.output, "TZE run: ");
    require(!run_id.empty(), "Expected language define output to expose a TZE run id.");

    const CommandCapture replay = run_command_capture(
        shell_quote(binary.string()) + " tze replay " + shell_quote(run_id) +
        " --memory-root " + shell_quote(memory_root.string()));
    require(replay.exit_code == 0, "Expected `omnix tze replay` for the language run to succeed.");
    require(replay.output.find("Language resolution:") != std::string::npos,
            "Expected replay output to include the persisted language resolution block.");

    const CommandCapture language_view = run_command_capture(
        shell_quote(binary.string()) + " memory language --memory-root " + shell_quote(memory_root.string()));
    require(language_view.exit_code == 0, "Expected `omnix memory language` to succeed.");
    require(language_view.output.find("Language Contexts:") != std::string::npos,
            "Expected `omnix memory language` to expose the persisted language contexts.");
    require(language_view.output.find("query=x.determineOSLanguage") != std::string::npos,
            "Expected `omnix memory language` to render the stored language context entry.");
}

void test_cli_uac_state_and_memory_view() {
    const std::filesystem::path memory_root = kBinaryRoot / "cli-memory-uac";
    safe_remove_all(memory_root);
    const std::filesystem::path binary = kBinaryRoot / "omnix";
    const std::string source_map = shell_quote((kSourceRoot / "res" / "tze.txt").string());

    const CommandCapture define = run_command_capture(
        shell_quote(binary.string()) + " define x.reGENx " + source_map +
        " --memory-root " + shell_quote(memory_root.string()));
    require(define.exit_code == 0, "Expected `omnix define x.reGENx` to succeed.");
    require(define.output.find("uAC epoch:") != std::string::npos,
            "Expected CLI define output to include the resolved uAC state.");
    const std::string run_id = extract_line_value(define.output, "TZE run: ");
    require(!run_id.empty(), "Expected uAC define output to expose a TZE run id.");

    const CommandCapture replay = run_command_capture(
        shell_quote(binary.string()) + " tze replay " + shell_quote(run_id) +
        " --memory-root " + shell_quote(memory_root.string()));
    require(replay.exit_code == 0, "Expected `omnix tze replay` for the uAC run to succeed.");
    require(replay.output.find("uAC state:") != std::string::npos,
            "Expected replay output to include the persisted uAC state block.");

    const CommandCapture uac_view = run_command_capture(
        shell_quote(binary.string()) + " memory uac --memory-root " + shell_quote(memory_root.string()));
    require(uac_view.exit_code == 0, "Expected `omnix memory uac` to succeed.");
    require(uac_view.output.find("uAC States:") != std::string::npos,
            "Expected `omnix memory uac` to expose the persisted uAC states.");
    require(uac_view.output.find("query=x.reGENx") != std::string::npos,
            "Expected `omnix memory uac` to render the stored uAC state entry.");
}

void test_cli_security_audit_and_memory_view() {
    const std::filesystem::path memory_root = kBinaryRoot / "cli-memory-security";
    safe_remove_all(memory_root);
    const std::filesystem::path binary = kBinaryRoot / "omnix";
    const std::string source_map = shell_quote((kSourceRoot / "res" / "tze.txt").string());

    const CommandCapture define = run_command_capture(
        shell_quote(binary.string()) + " define xXOmni::Detection " + source_map +
        " --memory-root " + shell_quote(memory_root.string()));
    require(define.exit_code == 0, "Expected `omnix define xXOmni::Detection` to succeed.");
    require(define.output.find("Security status: simulated") != std::string::npos,
            "Expected CLI define output to include the simulated security status.");
    require(define.output.find("Simulated actions:") != std::string::npos,
            "Expected CLI define output to include simulated defensive actions.");
    const std::string run_id = extract_line_value(define.output, "TZE run: ");
    require(!run_id.empty(), "Expected security define output to expose a TZE run id.");

    const CommandCapture replay = run_command_capture(
        shell_quote(binary.string()) + " tze replay " + shell_quote(run_id) +
        " --memory-root " + shell_quote(memory_root.string()));
    require(replay.exit_code == 0, "Expected `omnix tze replay` for the security run to succeed.");
    require(replay.output.find("Security audit:") != std::string::npos,
            "Expected replay output to include the persisted security audit block.");

    const CommandCapture security_view = run_command_capture(
        shell_quote(binary.string()) + " memory security --memory-root " + shell_quote(memory_root.string()));
    require(security_view.exit_code == 0, "Expected `omnix memory security` to succeed.");
    require(security_view.output.find("Security Audits:") != std::string::npos,
            "Expected `omnix memory security` to expose the persisted security audits.");
    require(security_view.output.find("query=xXOmni::Detection") != std::string::npos,
            "Expected `omnix memory security` to render the stored security audit entry.");
}

void test_cli_provider_probe_modes() {
    const std::filesystem::path root = kBinaryRoot / "cli-provider-probe";
    const std::filesystem::path inactive_memory = root / "inactive";
    const std::filesystem::path incomplete_memory = root / "incomplete";
    const std::filesystem::path unreachable_memory = root / "unreachable";
    safe_remove_all(root);
    const std::filesystem::path binary = kBinaryRoot / "omnix";

    const CommandCapture inactive = run_command_capture(
        shell_quote(binary.string()) + " provider probe --memory-root " + shell_quote(inactive_memory.string()));
    require(inactive.exit_code == 0, "Expected inactive provider probe to succeed as a diagnostic.");
    require(inactive.output.find("Reasoning provider: null") != std::string::npos,
            "Expected inactive provider probe output to report the null provider.");
    require(inactive.output.find("Provider probe: inactive") != std::string::npos,
            "Expected inactive provider probe output to report the inactive status.");

    const CommandCapture replay_inactive = run_command_capture(
        shell_quote(binary.string()) + " tze latest --memory-root " + shell_quote(inactive_memory.string()));
    require(replay_inactive.exit_code == 0, "Expected provider probe run to persist into the TZE ledger.");
    require(replay_inactive.output.find("Provider probe:") != std::string::npos,
            "Expected replaying the latest probe run to include the provider probe block.");

    const CommandCapture incomplete = run_command_capture(
        "OMNIX_REASONING_PROVIDER=ollama " +
        shell_quote(binary.string()) + " provider probe --memory-root " + shell_quote(incomplete_memory.string()));
    require(incomplete.exit_code != 0, "Expected incomplete Ollama provider configuration to fail readiness checks.");
    require(incomplete.output.find("Reasoning provider: ollama") != std::string::npos,
            "Expected incomplete probe output to report the selected Ollama provider.");
    require(incomplete.output.find("Provider probe: config_incomplete") != std::string::npos,
            "Expected incomplete probe output to expose the config_incomplete status.");

    const CommandCapture unreachable = run_command_capture(
        "OMNIX_REASONING_PROVIDER=ollama "
        "OMNIX_OLLAMA_BASE_URL=http://127.0.0.1:9 "
        "OMNIX_OLLAMA_MODEL=qwen3:8b " +
        shell_quote(binary.string()) + " provider probe --memory-root " + shell_quote(unreachable_memory.string()));
    require(unreachable.exit_code != 0, "Expected unreachable Ollama endpoint to fail readiness checks.");
    require(unreachable.output.find("Provider probe: endpoint_unreachable") != std::string::npos,
            "Expected unreachable probe output to expose the endpoint_unreachable status.");
    require(unreachable.output.find("Provider base URL: http://127.0.0.1:9") != std::string::npos,
            "Expected unreachable probe output to echo the configured base URL.");
    require(unreachable.output.find("Provider model: qwen3:8b") != std::string::npos,
            "Expected unreachable probe output to echo the requested model.");
}

void test_cli_instance_identity_is_stable_and_private() {
    const std::filesystem::path root = kBinaryRoot / "cli-instance-identity";
    const std::filesystem::path memory_root = root / "memory";
    safe_remove_all(root);
    std::filesystem::create_directories(memory_root);
    const std::filesystem::path binary = kBinaryRoot / "omnix";

    const std::string command =
        shell_quote(binary.string()) + " id --compact --memory-root " + shell_quote(memory_root.string());
    const CommandCapture first = run_command_capture(command);
    const CommandCapture second = run_command_capture(command);
    require(first.exit_code == 0 && second.exit_code == 0, "Expected `omnix id` to succeed.");
    require(first.output.find("instance_identity_ready: omnixid-v1-") != std::string::npos,
            "Expected compact identity output to include the OmniX instance id.");
    require(first.output.find("fingerprint: sha256:") != std::string::npos,
            "Expected compact identity output to include a SHA-256 fingerprint.");
    require(first.output.find("not Intel SGX") != std::string::npos,
            "Expected identity output to clarify this is not hardware attestation.");
    const std::string first_id_line = first.output.substr(0, first.output.find('\n'));
    const std::string second_id_line = second.output.substr(0, second.output.find('\n'));
    require(first_id_line == second_id_line,
            "Expected OmniX instance identity to stay stable for the same memory root.");

    const std::string salt = xpp::read_text_file(memory_root / "instance_salt");
    require(!salt.empty(), "Expected `omnix id` to persist a local instance salt.");
    require(first.output.find(salt) == std::string::npos,
            "Expected compact identity output not to leak the raw local salt.");
    require(std::filesystem::exists(memory_root / "instance_identity.json"),
            "Expected `omnix id` to write a compact instance identity artifact.");

    const CommandCapture verbose = run_command_capture(
        shell_quote(binary.string()) + " id --verbose --memory-root " + shell_quote(memory_root.string()));
    require(verbose.exit_code == 0, "Expected verbose identity output to succeed.");
    require(verbose.output.find("Host hint hash: sha256:") != std::string::npos,
            "Expected verbose identity output to hash the host hint.");
}

void test_cli_provider_probe_openai_fixture() {
    const std::filesystem::path root = kBinaryRoot / "cli-provider-probe-openai";
    const std::filesystem::path memory_root = root / "memory";
    const std::filesystem::path fixture = root / "models.json";
    safe_remove_all(root);
    std::filesystem::create_directories(root);

    {
        std::ofstream output(fixture);
        output << "{\n"
               << "  \"object\": \"list\",\n"
               << "  \"data\": [\n"
               << "    {\"id\": \"gpt-4.1-mini\"},\n"
               << "    {\"id\": \"gpt-4.1\"}\n"
               << "  ]\n"
               << "}\n";
    }

    const std::filesystem::path binary = kBinaryRoot / "omnix";
    const CommandCapture probe = run_command_capture(
        "OMNIX_REASONING_PROVIDER=openai "
        "OMNIX_OPENAI_API_KEY=dummy-key "
        "OMNIX_OPENAI_MODEL=gpt-4.1-mini "
        "OMNIX_OPENAI_MODEL_LIST_FILE=" + shell_quote(fixture.string()) + " " +
        shell_quote(binary.string()) + " provider probe --memory-root " + shell_quote(memory_root.string()));
    require(probe.exit_code == 0, "Expected fixture-backed OpenAI provider probe to succeed.");
    require(probe.output.find("Reasoning provider: openai") != std::string::npos,
            "Expected OpenAI provider probe output to report the selected provider.");
    require(probe.output.find("Provider probe: ready") != std::string::npos,
            "Expected fixture-backed OpenAI provider probe to report the ready status.");
}

void test_cli_guarded_assist_falls_back_deterministically() {
    const std::filesystem::path root = kBinaryRoot / "cli-guarded-assist";
    const std::filesystem::path memory_root = root / "memory";
    safe_remove_all(root);
    std::filesystem::create_directories(root);
    const std::filesystem::path binary = kBinaryRoot / "omnix";
    const std::string source_map = shell_quote((kSourceRoot / "res" / "tze.txt").string());

    const CommandCapture define_run = run_command_capture(
        shell_quote(binary.string()) + " define xProcessingCache " + source_map +
        " --memory-root " + shell_quote(memory_root.string()));
    require(define_run.exit_code == 0, "Expected define to succeed before guarded assist tests.");

    const CommandCapture explain_run = run_command_capture(
        shell_quote(binary.string()) + " explain x.DisplayFeedBackLoop " + source_map +
        " --memory-root " + shell_quote(memory_root.string()));
    require(explain_run.exit_code == 0, "Expected explain to succeed before guarded assist tests.");

    const std::string env_prefix =
        "OMNIX_REASONING_PROVIDER=ollama "
        "OMNIX_OLLAMA_BASE_URL=http://127.0.0.1:9 "
        "OMNIX_OLLAMA_MODEL=qwen3:8b ";

    const CommandCapture explain_change = run_command_capture(
        env_prefix + shell_quote(binary.string()) + " tze explain-change-latest --assist --memory-root " +
        shell_quote(memory_root.string()));
    require(explain_change.exit_code == 0,
            "Expected guarded assist explain-change to preserve deterministic success when Ollama is unreachable.");
    require(explain_change.output.find("Verdict: tze_change_explained") != std::string::npos,
            "Expected explain-change to keep the deterministic explained verdict.");
    require(explain_change.output.find("Assist: assist_bypassed") != std::string::npos,
            "Expected explain-change to report guarded assist fallback.");

    const std::filesystem::path report_path = root / "assist-report.txt";
    const CommandCapture report_run = run_command_capture(
        env_prefix + shell_quote(binary.string()) + " tze report latest --assist --out " +
        shell_quote(report_path.string()) + " --memory-root " + shell_quote(memory_root.string()));
    require(report_run.exit_code == 0,
            "Expected guarded assist report generation to preserve deterministic success when Ollama is unreachable.");
    require(std::filesystem::exists(report_path), "Expected guarded assist report command to write the requested artifact.");
    const std::string report_text = xpp::read_text_file(report_path);
    require(report_text.find("## Guarded Assist") != std::string::npos,
            "Expected guarded assist report output to include the guarded assist section.");
    require(report_text.find("assist_bypassed") != std::string::npos,
            "Expected guarded assist report output to record the bypassed assist status.");
}

void test_cli_assist_tool_plan_executes_allowlisted_builtin() {
    const std::filesystem::path root = kBinaryRoot / "cli-assist-tool-plan";
    const std::filesystem::path memory_root = root / "memory";
    const std::filesystem::path sample = root / "assist.log";
    const std::filesystem::path fixture = root / "tool-plan.json";
    safe_remove_all(root);
    std::filesystem::create_directories(root);

    {
        std::ofstream output(sample);
        output << "warning: ssh auth failure\n";
        output << "build ready\n";
    }
    {
        std::ofstream output(fixture);
        output << "{\n"
               << "  \"tool_name\": \"inspect-log\",\n"
               << "  \"arguments\": [\"" << sample.string() << "\"],\n"
               << "  \"rationale\": \"Use the built-in structured log inspector on the provided local file.\",\n"
               << "  \"safety_notes\": [\"Read-only local file inspection only.\"]\n"
               << "}\n";
    }

    const std::filesystem::path binary = kBinaryRoot / "omnix";
    const std::string command =
        "OMNIX_REASONING_PROVIDER=ollama "
        "OMNIX_OLLAMA_MODEL=fixture "
        "OMNIX_OLLAMA_TOOL_PLAN_FILE=" + shell_quote(fixture.string()) + " " +
        shell_quote(binary.string()) + " ask --assist " +
        shell_quote("take a look at the local log and tell me what matters") +
        " --memory-root " + shell_quote(memory_root.string()) +
        " --source-map " + shell_quote((kSourceRoot / "res" / "tze.txt").string());
    const CommandCapture ask = run_command_capture(command);
    require(ask.exit_code == 0, "Expected assist-backed tool planning through `ask --assist` to succeed.");
    require(ask.output.find("Assist: assist_used") != std::string::npos,
            "Expected assist-backed tool planning to report assist_used.");
    require(ask.output.find("Assist tool plan: inspect-log") != std::string::npos,
            "Expected assist-backed tool planning to expose the validated tool plan.");
    require(ask.output.find("Tool invocation: ok") != std::string::npos,
            "Expected assist-backed tool planning to execute the built-in tool successfully.");

    const CommandCapture replay = run_command_capture(
        shell_quote(binary.string()) + " tze replay latest --memory-root " + shell_quote(memory_root.string()));
    require(replay.exit_code == 0, "Expected assist-backed tool run to persist into the TZE ledger.");
    require(replay.output.find("Tool assist plan:") != std::string::npos,
            "Expected replay output to include the persisted tool assist plan.");
}

void test_cli_assist_build_plan_selects_allowlisted_recipe() {
    const std::filesystem::path root = kBinaryRoot / "cli-assist-build-plan";
    const std::filesystem::path memory_root = root / "memory";
    const std::filesystem::path fixture = root / "build-plan.json";
    safe_remove_all(root);
    std::filesystem::create_directories(root);

    {
        std::ofstream output(fixture);
        output << "{\n"
               << "  \"selected_recipe_id\": \"fmt-cmake\",\n"
               << "  \"fallback_recipe_id\": \"\",\n"
               << "  \"rationale\": \"Use the approved CMake recipe for fmt on this host.\",\n"
               << "  \"confidence\": 0.91,\n"
               << "  \"safety_notes\": [\"Recipe stays inside OmniX's allowlisted alias registry.\"]\n"
               << "}\n";
    }

    const std::filesystem::path binary = kBinaryRoot / "omnix";
    const std::string command =
        "OMNIX_REASONING_PROVIDER=ollama "
        "OMNIX_OLLAMA_MODEL=fixture "
        "OMNIX_OLLAMA_BUILD_PLAN_FILE=" + shell_quote(fixture.string()) + " " +
        shell_quote(binary.string()) + " preflight fmt --assist --offline " +
        " --memory-root " + shell_quote(memory_root.string()) +
        " --source-map " + shell_quote((kSourceRoot / "res" / "tze.txt").string());
    const CommandCapture preflight = run_command_capture(command);
    require(preflight.exit_code == 0, "Expected assist-backed build recipe selection to succeed.");
    require(preflight.output.find("Assist: assist_used") != std::string::npos,
            "Expected assist-backed build selection to report assist_used.");
    require(preflight.output.find("Assist build plan: fmt-cmake") != std::string::npos,
            "Expected assist-backed build selection to expose the validated build plan.");
    require(preflight.output.find("Recipe selection: assist_selected(fmt-cmake") != std::string::npos,
            "Expected preflight output to show the assist-selected recipe reason.");

    const CommandCapture replay = run_command_capture(
        shell_quote(binary.string()) + " tze replay latest --memory-root " + shell_quote(memory_root.string()));
    require(replay.exit_code == 0, "Expected assist-backed build preflight to persist into the TZE ledger.");
    require(replay.output.find("Build assist plan:") != std::string::npos,
            "Expected replay output to include the persisted build assist plan.");
}

void test_cli_assist_command_plan_routes_ask_to_preflight() {
    const std::filesystem::path root = kBinaryRoot / "cli-assist-command-plan";
    const std::filesystem::path memory_root = root / "memory";
    const std::filesystem::path fixture = root / "command-plan.json";
    safe_remove_all(root);
    std::filesystem::create_directories(root);

    {
        std::ofstream output(fixture);
        output << "{\n"
               << "  \"canonical_command\": \"preflight fmt\",\n"
               << "  \"command_family\": \"build_project\",\n"
               << "  \"rationale\": \"The request is asking whether fmt can be built on this host, which maps to preflight.\",\n"
               << "  \"confidence\": 0.94,\n"
               << "  \"requires_confirmation\": false,\n"
               << "  \"safety_notes\": [\"Only allowlisted Omni commands may be routed.\", \"Preflight remains deterministic.\"]\n"
               << "}\n";
    }

    const std::filesystem::path binary = kBinaryRoot / "omnix";
    const std::string command =
        "OMNIX_REASONING_PROVIDER=ollama "
        "OMNIX_OLLAMA_MODEL=fixture "
        "OMNIX_OLLAMA_COMMAND_PLAN_FILE=" + shell_quote(fixture.string()) + " " +
        shell_quote(binary.string()) + " ask --assist " +
        shell_quote("check fmt build readiness") +
        " --offline --memory-root " + shell_quote(memory_root.string()) +
        " --source-map " + shell_quote((kSourceRoot / "res" / "tze.txt").string());
    const CommandCapture ask = run_command_capture(command);
    require(ask.exit_code == 0, "Expected assist-backed command routing through `ask --assist` to succeed.");
    require(ask.output.find("Assist: assist_used") != std::string::npos,
            "Expected assist-backed command routing to report assist_used.");
    require(ask.output.find("Assist command plan: preflight fmt") != std::string::npos,
            "Expected assist-backed command routing to expose the validated command plan.");
    require(ask.output.find("Project: fmt") != std::string::npos,
            "Expected routed command execution to resolve the fmt project.");
    require(ask.output.find("Preflight:") != std::string::npos,
            "Expected routed command execution to run the preflight path.");

    const CommandCapture replay = run_command_capture(
        shell_quote(binary.string()) + " tze replay latest --memory-root " + shell_quote(memory_root.string()));
    require(replay.exit_code == 0, "Expected assist-backed command routing to persist into the TZE ledger.");
    require(replay.output.find("Command assist plan:") != std::string::npos,
            "Expected replay output to include the persisted command assist plan.");
}

void test_cli_run_nmap_routes_to_safe_tool_flow() {
    const std::filesystem::path root = kBinaryRoot / "cli-run-nmap";
    const std::filesystem::path bin_dir = root / "bin";
    const std::filesystem::path memory_root = root / "memory";
    const std::filesystem::path search_root = root / "search";
    safe_remove_all(root);
    std::filesystem::create_directories(bin_dir);
    std::filesystem::create_directories(memory_root);
    std::filesystem::create_directories(search_root);

    make_executable(bin_dir / "nmap",
                    "#!/bin/sh\n"
                    "if [ \"$1\" = \"-V\" ] || [ \"$1\" = \"--version\" ]; then echo 'Nmap 7.95'; exit 0; fi\n"
                    "if [ \"$1\" = \"-F\" ] && [ \"$2\" = \"127.0.0.1\" ]; then echo 'Nmap scan report for 127.0.0.1'; exit 0; fi\n"
                    "exit 1\n");

    const std::filesystem::path binary = kBinaryRoot / "omnix";
    const std::string prefix =
        "PATH=" + shell_quote(bin_dir.string()) + " " +
        "OMNIX_HOME=" + shell_quote(memory_root.string()) + " " +
        "OMNIX_NATIVE_SEARCH_ROOTS=" + shell_quote(search_root.string()) + " " +
        shell_quote(binary.string()) + " ask ";
    const std::string suffix =
        " --memory-root " + shell_quote(memory_root.string()) +
        " --source-map " + shell_quote((kSourceRoot / "res" / "tze.txt").string());

    const CommandCapture run = run_command_capture(prefix + shell_quote("Run NMAP") + suffix);
    require(run.exit_code == 0, "Expected `omnix ask \"Run NMAP\"` to route into the safe Nmap tool flow.");
    require(run.output.find("Verdict: ok") != std::string::npos,
            "Expected `Run NMAP` to report a successful tool invocation.");
    require(run.output.find("Tool: nmap") != std::string::npos,
            "Expected `Run NMAP` to resolve the nmap tool.");
    require(run.output.find("-V") != std::string::npos,
            "Expected `Run NMAP` to default to the safe version probe.");

    const CommandCapture scan = run_command_capture(prefix + shell_quote("Run NMAP Scan") + suffix);
    require(scan.exit_code == 0, "Expected `omnix ask \"Run NMAP Scan\"` to route into the guarded Nmap scan flow.");
    require(scan.output.find("Verdict: ok") != std::string::npos,
            "Expected `Run NMAP Scan` to report a successful tool invocation.");
    require(scan.output.find("-F") != std::string::npos && scan.output.find("127.0.0.1") != std::string::npos,
            "Expected `Run NMAP Scan` to default to a safe loopback fast scan.");
}

void test_cli_assist_command_plan_routes_ask_to_tool_nmap() {
    const std::filesystem::path root = kBinaryRoot / "cli-assist-command-tool-nmap";
    const std::filesystem::path bin_dir = root / "bin";
    const std::filesystem::path memory_root = root / "memory";
    const std::filesystem::path search_root = root / "search";
    const std::filesystem::path fixture = root / "command-plan.json";
    safe_remove_all(root);
    std::filesystem::create_directories(bin_dir);
    std::filesystem::create_directories(memory_root);
    std::filesystem::create_directories(search_root);

    make_executable(bin_dir / "nmap",
                    "#!/bin/sh\n"
                    "if [ \"$1\" = \"-V\" ] || [ \"$1\" = \"--version\" ]; then echo 'Nmap 7.95'; exit 0; fi\n"
                    "exit 1\n");

    {
        std::ofstream output(fixture);
        output << "{\n"
               << "  \"canonical_command\": \"tool nmap -- -V\",\n"
               << "  \"command_family\": \"tool_action\",\n"
               << "  \"rationale\": \"The request is asking to run a safe Nmap probe, which maps to the guarded tool flow.\",\n"
               << "  \"confidence\": 0.93,\n"
               << "  \"requires_confirmation\": false,\n"
               << "  \"safety_notes\": [\"Only guarded tool runs are allowed.\", \"Nmap is constrained to the version probe.\"]\n"
               << "}\n";
    }

    const std::filesystem::path binary = kBinaryRoot / "omnix";
    const std::string prompt = "please verify the network mapper tool";
    const std::string command =
        "PATH=" + shell_quote(bin_dir.string()) + " " +
        "OMNIX_HOME=" + shell_quote(memory_root.string()) + " " +
        "OMNIX_NATIVE_SEARCH_ROOTS=" + shell_quote(search_root.string()) + " " +
        "OMNIX_REASONING_PROVIDER=ollama "
        "OMNIX_OLLAMA_MODEL=fixture "
        "OMNIX_OLLAMA_COMMAND_PLAN_FILE=" + shell_quote(fixture.string()) + " " +
        shell_quote(binary.string()) + " ask --assist " + shell_quote(prompt) +
        " --memory-root " + shell_quote(memory_root.string()) +
        " --source-map " + shell_quote((kSourceRoot / "res" / "tze.txt").string());

    const CommandCapture ask = run_command_capture(command);
    require(ask.exit_code == 0, "Expected assist-backed guarded Nmap routing to succeed.");
    require(ask.output.find("Assist: assist_used") != std::string::npos,
            "Expected assist-backed Nmap routing to report assist_used.");
    require(ask.output.find("Assist command plan: tool nmap -- -V") != std::string::npos,
            "Expected assist-backed Nmap routing to expose the validated tool command plan.");
    require(ask.output.find("Tool: nmap") != std::string::npos,
            "Expected assist-backed Nmap routing to execute the nmap tool.");
    require(ask.output.find("Tool invocation: ok") != std::string::npos,
            "Expected assist-backed Nmap routing to succeed through the guarded tool flow.");
}

void test_cli_assist_scan_request_rejects_explicit_external_target() {
    const std::filesystem::path root = kBinaryRoot / "cli-assist-command-tool-nmap-external-target";
    const std::filesystem::path bin_dir = root / "bin";
    const std::filesystem::path memory_root = root / "memory";
    const std::filesystem::path search_root = root / "search";
    safe_remove_all(root);
    std::filesystem::create_directories(bin_dir);
    std::filesystem::create_directories(memory_root);
    std::filesystem::create_directories(search_root);

    make_executable(bin_dir / "nmap",
                    "#!/bin/sh\n"
                    "echo 'unexpected nmap execution'\n"
                    "exit 0\n");

    const std::filesystem::path binary = kBinaryRoot / "omnix";
    const std::string prompt = "Run NMAP against 192.168.1.1-254 and output results of the portscan";
    const std::string command =
        "PATH=" + shell_quote(bin_dir.string()) + " " +
        "OMNIX_HOME=" + shell_quote(memory_root.string()) + " " +
        "OMNIX_NATIVE_SEARCH_ROOTS=" + shell_quote(search_root.string()) + " " +
        shell_quote(binary.string()) + " ask --assist " + shell_quote(prompt) +
        " --memory-root " + shell_quote(memory_root.string()) +
        " --source-map " + shell_quote((kSourceRoot / "res" / "tze.txt").string());

    const CommandCapture ask = run_command_capture(command);
    require(ask.exit_code == 0, "Expected guarded external-target Nmap request to return a diagnostic response.");
    require(ask.output.find("Verdict: guardrail_blocked") != std::string::npos,
            "Expected explicit external-target Nmap requests to be blocked by guardrails.");
    require(ask.output.find("192.168.1.1-254") != std::string::npos,
            "Expected the blocked response to surface the requested external target.");
    require(ask.output.find("Command line:") == std::string::npos,
            "Expected blocked external-target Nmap requests not to execute a guarded localhost command.");
    require(ask.output.find("Tool invocation: ok") == std::string::npos,
            "Expected blocked external-target Nmap requests not to report a successful tool execution.");
    require(ask.output.find("unexpected nmap execution") == std::string::npos,
            "Expected blocked external-target Nmap requests not to invoke the nmap binary at all.");
}

void test_cli_assist_command_plan_routes_ask_to_tool_nmap_openai_fixture() {
    const std::filesystem::path root = kBinaryRoot / "cli-assist-command-tool-nmap-openai";
    const std::filesystem::path bin_dir = root / "bin";
    const std::filesystem::path memory_root = root / "memory";
    const std::filesystem::path search_root = root / "search";
    const std::filesystem::path models_fixture = root / "models.json";
    const std::filesystem::path plan_fixture = root / "command-plan.json";
    safe_remove_all(root);
    std::filesystem::create_directories(bin_dir);
    std::filesystem::create_directories(memory_root);
    std::filesystem::create_directories(search_root);

    make_executable(bin_dir / "nmap",
                    "#!/bin/sh\n"
                    "if [ \"$1\" = \"-V\" ] || [ \"$1\" = \"--version\" ]; then echo 'Nmap 7.95'; exit 0; fi\n"
                    "exit 1\n");

    {
        std::ofstream output(models_fixture);
        output << "{\n"
               << "  \"object\": \"list\",\n"
               << "  \"data\": [\n"
               << "    {\"id\": \"gpt-4.1-mini\"}\n"
               << "  ]\n"
               << "}\n";
    }
    {
        std::ofstream output(plan_fixture);
        output << "{\n"
               << "  \"canonical_command\": \"tool nmap -- -V\",\n"
               << "  \"command_family\": \"tool_action\",\n"
               << "  \"rationale\": \"The request is asking to run a safe Nmap probe, which maps to the guarded tool flow.\",\n"
               << "  \"confidence\": 0.93,\n"
               << "  \"requires_confirmation\": false,\n"
               << "  \"safety_notes\": [\"Only guarded tool runs are allowed.\", \"Nmap is constrained to the version probe.\"]\n"
               << "}\n";
    }

    const std::filesystem::path binary = kBinaryRoot / "omnix";
    const std::string prompt = "please verify the network mapper tool";
    const std::string command =
        "PATH=" + shell_quote(bin_dir.string()) + " " +
        "OMNIX_HOME=" + shell_quote(memory_root.string()) + " " +
        "OMNIX_NATIVE_SEARCH_ROOTS=" + shell_quote(search_root.string()) + " " +
        "OMNIX_REASONING_PROVIDER=openai "
        "OMNIX_OPENAI_API_KEY=dummy-key "
        "OMNIX_OPENAI_MODEL=gpt-4.1-mini "
        "OMNIX_OPENAI_MODEL_LIST_FILE=" + shell_quote(models_fixture.string()) + " "
        "OMNIX_OPENAI_COMMAND_PLAN_FILE=" + shell_quote(plan_fixture.string()) + " " +
        shell_quote(binary.string()) + " ask --assist " + shell_quote(prompt) +
        " --memory-root " + shell_quote(memory_root.string()) +
        " --source-map " + shell_quote((kSourceRoot / "res" / "tze.txt").string());

    const CommandCapture ask = run_command_capture(command);
    require(ask.exit_code == 0, "Expected OpenAI fixture-backed guarded Nmap routing to succeed.");
    require(ask.output.find("Reasoning provider: openai") != std::string::npos,
            "Expected OpenAI fixture-backed Nmap routing to report the OpenAI provider.");
    require(ask.output.find("Assist: assist_used") != std::string::npos,
            "Expected OpenAI fixture-backed Nmap routing to report assist_used.");
    require(ask.output.find("Assist command plan: tool nmap -- -V") != std::string::npos,
            "Expected OpenAI fixture-backed Nmap routing to expose the validated tool command plan.");
    require(ask.output.find("Tool invocation: ok") != std::string::npos,
            "Expected OpenAI fixture-backed Nmap routing to succeed through the guarded tool flow.");
}

void test_cli_openai_freeform_answers_after_local_miss() {
    const std::filesystem::path root = kBinaryRoot / "cli-openai-freeform-math";
    const std::filesystem::path memory_root = root / "memory";
    const std::filesystem::path models_fixture = root / "models.json";
    const std::filesystem::path empty_plan = root / "empty-plan.json";
    const std::filesystem::path freeform_fixture = root / "freeform.json";
    safe_remove_all(root);
    std::filesystem::create_directories(root);

    {
        std::ofstream output(models_fixture);
        output << "{\"data\":[{\"id\":\"gpt-4.1-mini\"}]}\n";
    }
    {
        std::ofstream output(empty_plan);
        output << "{}\n";
    }
    {
        std::ofstream output(freeform_fixture);
        output << "{\n"
               << "  \"answer\": \"For a right triangle with legs 7 in and 7 in, the hypotenuse is \\u221a98 \\u2248 9.90 inches.\",\n"
               << "  \"rationale\": \"Apply the Pythagorean theorem after local OmniX sources miss.\",\n"
               << "  \"confidence\": 0.99,\n"
               << "  \"suggested_commands\": [],\n"
               << "  \"safety_warnings\": [],\n"
               << "  \"used_context\": [\"openai_freeform\", \"math\"]\n"
               << "}\n";
    }

    const std::filesystem::path binary = kBinaryRoot / "omnix";
    const std::string command =
        "OMNIX_REASONING_PROVIDER=openai "
        "OMNIX_OPENAI_API_KEY=dummy-key "
        "OMNIX_OPENAI_MODEL=gpt-4.1-mini "
        "OMNIX_OPENAI_MODEL_LIST_FILE=" + shell_quote(models_fixture.string()) + " "
        "OMNIX_OPENAI_COMMAND_PLAN_FILE=" + shell_quote(empty_plan.string()) + " "
        "OMNIX_OPENAI_TOOL_PLAN_FILE=" + shell_quote(empty_plan.string()) + " "
        "OMNIX_OPENAI_FREEFORM_FILE=" + shell_quote(freeform_fixture.string()) + " " +
        shell_quote(binary.string()) + " ask --assist " +
        shell_quote("calculate hypotenuse of triangle whose sides are 7in and 7in") +
        " --memory-root " + shell_quote(memory_root.string()) +
        " --source-map " + shell_quote((kSourceRoot / "res" / "tze.txt").string());

    const CommandCapture ask = run_command_capture(command);
    require(ask.exit_code == 0, "Expected OpenAI freeform fallback to return successfully.");
    require(ask.output.find("Verdict: assist_freeform") != std::string::npos,
            "Expected freeform fallback to use the assist_freeform verdict.");
    require(ask.output.find("9.90 inches") != std::string::npos,
            "Expected freeform fallback to print the math answer.");
    require(ask.output.find("u221a") == std::string::npos,
            "Expected freeform fallback to decode JSON unicode escapes instead of printing `u221a`.");
    require(ask.output.find("x.Assist.Freeform") != std::string::npos,
            "Expected freeform fallback to record a TZE freeform stage.");

    const CommandCapture definition_shaped = run_command_capture(
        "OMNIX_REASONING_PROVIDER=openai "
        "OMNIX_OPENAI_API_KEY=dummy-key "
        "OMNIX_OPENAI_MODEL=gpt-4.1-mini "
        "OMNIX_OPENAI_MODEL_LIST_FILE=" + shell_quote(models_fixture.string()) + " "
        "OMNIX_OPENAI_COMMAND_PLAN_FILE=" + shell_quote(empty_plan.string()) + " "
        "OMNIX_OPENAI_TOOL_PLAN_FILE=" + shell_quote(empty_plan.string()) + " "
        "OMNIX_OPENAI_FREEFORM_FILE=" + shell_quote(freeform_fixture.string()) + " " +
        shell_quote(binary.string()) + " ask --assist " + shell_quote("what is 1+1") +
        " --memory-root " + shell_quote(memory_root.string()) +
        " --source-map " + shell_quote((kSourceRoot / "res" / "tze.txt").string()));
    require(definition_shaped.exit_code == 0,
            "Expected OpenAI freeform fallback to cover unresolved definition-shaped asks.");
    require(definition_shaped.output.find("Verdict: assist_freeform") != std::string::npos,
            "Expected unresolved definition-shaped asks to use the assist_freeform verdict.");

    const CommandCapture replay = run_command_capture(
        shell_quote(binary.string()) + " tze replay latest --memory-root " + shell_quote(memory_root.string()));
    require(replay.exit_code == 0, "Expected freeform run to persist into the TZE ledger.");
    require(replay.output.find("Freeform assist answer:") != std::string::npos,
            "Expected replay output to include the persisted freeform answer.");
}

void test_cli_openai_freeform_security_guidance_is_non_executing() {
    const std::filesystem::path root = kBinaryRoot / "cli-openai-freeform-security";
    const std::filesystem::path memory_root = root / "memory";
    const std::filesystem::path models_fixture = root / "models.json";
    const std::filesystem::path command_plan = root / "command-plan.json";
    const std::filesystem::path empty_plan = root / "empty-plan.json";
    const std::filesystem::path freeform_fixture = root / "freeform-security.json";
    safe_remove_all(root);
    std::filesystem::create_directories(root);

    {
        std::ofstream output(models_fixture);
        output << "{\"data\":[{\"id\":\"gpt-4.1-mini\"}]}\n";
    }
    {
        std::ofstream output(command_plan);
        output << "{\n"
               << "  \"canonical_command\": \"doctor .\",\n"
               << "  \"command_family\": \"doctor_project\",\n"
               << "  \"rationale\": \"A deliberately bad fixture: broad security guidance must not route to build doctor.\",\n"
               << "  \"confidence\": 0.95,\n"
               << "  \"requires_confirmation\": false,\n"
               << "  \"safety_notes\": []\n"
               << "}\n";
    }
    {
        std::ofstream output(empty_plan);
        output << "{}\n";
    }
    {
        std::ofstream output(freeform_fixture);
        output << "{\n"
               << "  \"answer\": \"I cannot say whether you are secure without current local evidence. Start with read-only diagnostics, then inspect concrete ports before drawing a conclusion.\",\n"
               << "  \"rationale\": \"Security claims require deterministic evidence; this phase proposes commands only.\",\n"
               << "  \"confidence\": 0.88,\n"
               << "  \"suggested_commands\": [\"omnix defend diag cpu\", \"omnix defend diag memory\", \"omnix defend diag port 5000\", \"omnix ask --assist run NMAP against 127.0.0.1 verbose\", \"omnix tview port 5000\"],\n"
               << "  \"safety_warnings\": [\"No scan or packet capture was executed by this answer.\", \"Do not kill PIDs or close ports until evidence is validated.\"],\n"
               << "  \"used_context\": [\"openai_freeform\", \"security_guidance\"]\n"
               << "}\n";
    }

    const std::filesystem::path binary = kBinaryRoot / "omnix";
    const std::string command =
        "OMNIX_REASONING_PROVIDER=openai "
        "OMNIX_OPENAI_API_KEY=dummy-key "
        "OMNIX_OPENAI_MODEL=gpt-4.1-mini "
        "OMNIX_OPENAI_MODEL_LIST_FILE=" + shell_quote(models_fixture.string()) + " "
        "OMNIX_OPENAI_COMMAND_PLAN_FILE=" + shell_quote(command_plan.string()) + " "
        "OMNIX_OPENAI_TOOL_PLAN_FILE=" + shell_quote(empty_plan.string()) + " "
        "OMNIX_OPENAI_FREEFORM_FILE=" + shell_quote(freeform_fixture.string()) + " " +
        shell_quote(binary.string()) + " ask --assist " + shell_quote("am I secure") +
        " --memory-root " + shell_quote(memory_root.string()) +
        " --source-map " + shell_quote((kSourceRoot / "res" / "tze.txt").string());

    const CommandCapture ask = run_command_capture(command);
    require(ask.exit_code == 0, "Expected OpenAI security guidance fallback to return successfully.");
    require(ask.output.find("Verdict: assist_freeform") != std::string::npos,
            "Expected security guidance to use the assist_freeform verdict.");
    require(ask.output.find("I cannot say whether you are secure") != std::string::npos,
            "Expected security guidance to avoid unsupported secure/insecure claims.");
    require(ask.output.find("Tool invocation: ok") == std::string::npos,
            "Expected broad security guidance not to execute local tools automatically.");
    require(ask.output.find("Command line:") == std::string::npos,
            "Expected broad security guidance not to execute Nmap or packet capture automatically.");
    require(ask.output.find("doctor .") == std::string::npos,
            "Expected broad security guidance not to accept a build-doctor command plan.");
    require(ask.output.find("omnix tview port 5000") != std::string::npos,
            "Expected security guidance to propose TView only as an explicit next command.");
}

void test_cli_openai_freeform_does_not_override_command_route() {
    const std::filesystem::path root = kBinaryRoot / "cli-openai-freeform-command-route";
    const std::filesystem::path memory_root = root / "memory";
    const std::filesystem::path models_fixture = root / "models.json";
    const std::filesystem::path freeform_fixture = root / "freeform.json";
    safe_remove_all(root);
    std::filesystem::create_directories(root);

    {
        std::ofstream output(models_fixture);
        output << "{\"data\":[{\"id\":\"gpt-4.1-mini\"}]}\n";
    }
    {
        std::ofstream output(freeform_fixture);
        output << "{\n"
               << "  \"answer\": \"This fixture should not be used for command-shaped prompts.\",\n"
               << "  \"rationale\": \"Command routes must outrank freeform fallback.\",\n"
               << "  \"confidence\": 0.5,\n"
               << "  \"suggested_commands\": [],\n"
               << "  \"safety_warnings\": [],\n"
               << "  \"used_context\": [\"openai_freeform\"]\n"
               << "}\n";
    }

    const std::filesystem::path binary = kBinaryRoot / "omnix";
    const std::string command =
        "OMNIX_REASONING_PROVIDER=openai "
        "OMNIX_OPENAI_API_KEY=dummy-key "
        "OMNIX_OPENAI_MODEL=gpt-4.1-mini "
        "OMNIX_OPENAI_MODEL_LIST_FILE=" + shell_quote(models_fixture.string()) + " "
        "OMNIX_OPENAI_FREEFORM_FILE=" + shell_quote(freeform_fixture.string()) + " " +
        shell_quote(binary.string()) + " ask --assist " + shell_quote("omnix tview port 5000") +
        " --memory-root " + shell_quote(memory_root.string());

    const CommandCapture ask = run_command_capture(command);
    require(ask.exit_code == 0, "Expected command-shaped ask to route deterministically.");
    require(ask.output.find("OmniXTView") != std::string::npos ||
                ask.output.find("capture_") != std::string::npos,
            "Expected command-shaped ask to route to TView rather than freeform.");
    require(ask.output.find("This fixture should not be used") == std::string::npos,
            "Expected command-shaped ask not to consume the freeform fixture.");
    require(ask.output.find("x.Assist.Freeform") == std::string::npos,
            "Expected command-shaped ask not to record a freeform assist stage.");
}

void test_cli_shell_provider_and_context() {
    const std::filesystem::path root = kBinaryRoot / "cli-shell";
    const std::filesystem::path memory_root = root / "memory";
    safe_remove_all(root);
    std::filesystem::create_directories(root);
    const std::filesystem::path binary = kBinaryRoot / "omnix";
    const std::string command =
        "printf '%s\\n' '/status' '/assist on' '/provider' 'define xProcessingCache' '/quit' | " +
        shell_quote(binary.string()) + " shell --memory-root " + shell_quote(memory_root.string()) +
        " --source-map " + shell_quote((kSourceRoot / "res" / "tze.txt").string());

    const CommandCapture shell = run_command_capture(command);
    require(shell.exit_code == 0, "Expected `omnix shell` to exit cleanly.");
    require(shell.output.find("OmniX shell started.") != std::string::npos,
            "Expected shell output to include the startup banner.");
    require(shell.output.find("Shell status:") != std::string::npos,
            "Expected shell output to include the shell status block.");
    require(shell.output.find("Assist mode enabled for guarded tasks.") != std::string::npos,
            "Expected shell output to acknowledge assist mode changes.");
    require(shell.output.find("Provider probe:") != std::string::npos,
            "Expected shell output to route `/provider` through the provider probe.");
    require(shell.output.find("Definition query: xProcessingCache") != std::string::npos,
            "Expected shell output to execute plain OmniX commands through the shared engine.");
}

void test_cli_recursive_why_api_and_link_ux() {
    const std::filesystem::path root = kBinaryRoot / "cli-recursive-api-link";
    const std::filesystem::path memory_root = root / "memory";
    const std::filesystem::path api_root = root / "api";
    const std::filesystem::path link_prefix = root / "bin";
    safe_remove_all(root);
    std::filesystem::create_directories(memory_root);
    std::filesystem::create_directories(api_root);
    std::filesystem::create_directories(link_prefix);

    const std::filesystem::path binary = kBinaryRoot / "omnix";
    const std::string common = shell_quote(binary.string()) + " ";
    const std::string memory_args = " --memory-root " + shell_quote(memory_root.string());

    const CommandCapture probe =
        run_command_capture(common + "provider probe --compact" + memory_args);
    require(probe.exit_code == 0, "Expected provider probe to create a TZE run for Recursive Why/Diff.");

    const CommandCapture why =
        run_command_capture(common + "why latest --compact" + memory_args);
    require(why.exit_code == 0, "Expected `omnix why latest` to complete.");
    require(why.output.find("Current State:") != std::string::npos,
            "Expected compact Recursive Why/Diff output to include the Current State section.");
    require(why.output.find("Successful Path Pattern:") != std::string::npos,
            "Expected compact Recursive Why/Diff output to include the success path section.");
    require(why.output.find("Why This Matters:") != std::string::npos,
            "Expected compact Recursive Why/Diff output to include why the diff matters.");

    const CommandCapture replay =
        run_command_capture(common + "tze replay latest" + memory_args);
    require(replay.exit_code == 0, "Expected replay of Recursive Why/Diff run to succeed.");
    require(replay.output.find("recursive_why_diff_complete") != std::string::npos ||
                replay.output.find("Recursive Why/Diff") != std::string::npos,
            "Expected replay to retain compact Recursive Why/Diff metadata.");

    const std::string shell_command =
        "printf '%s\\n' '/provider' '/why' 'next' '/next' '/api' '/quit' | " +
        common + "shell --compact" + memory_args;
    const CommandCapture shell = run_command_capture(shell_command);
    require(shell.exit_code == 0, "Expected shell `/why` and `/api` shortcuts to complete.");
    require(shell.output.find("Current State:") != std::string::npos,
            "Expected shell `/why` to route through Recursive Why/Diff.");
    require(shell.output.find("api_status:") != std::string::npos,
            "Expected shell `/api` to route through API status.");
    require(shell.output.find("next_action:") != std::string::npos,
            "Expected shell next aliases to route through top-level next.");

    const CommandCapture api_template =
        run_command_capture(common + "api template huggingface --compact");
    require(api_template.exit_code == 0, "Expected HuggingFace API template to render.");
    require(api_template.output.find("HUGGINGFACE_API_TOKEN") != std::string::npos &&
                api_template.output.find("HF_MODEL") != std::string::npos,
            "Expected HuggingFace template to stay placeholder-based.");

    const std::string configure_command =
        "cd " + shell_quote(api_root.string()) +
        " && printf '%s\\n' 'gpt-test-mini' 'sk-test-secret' | " +
        shell_quote(binary.string()) + " api configure openai --compact";
    const CommandCapture configure = run_command_capture(configure_command);
    require(configure.exit_code == 0, "Expected OpenAI API configure to write repo-local .env.");
    require(configure.output.find("sk-test-secret") == std::string::npos,
            "Expected API configure output not to print the secret.");
    require(std::filesystem::exists(api_root / ".env"), "Expected API configure to write .env.");

    const CommandCapture api_status =
        run_command_capture("cd " + shell_quote(api_root.string()) +
                            " && " + shell_quote(binary.string()) + " api status --compact");
    require(api_status.exit_code == 0, "Expected API status to read repo-local .env.");
    require(api_status.output.find("provider=openai") != std::string::npos,
            "Expected API status to report configured OpenAI provider.");
    require(api_status.output.find("sk-test-secret") == std::string::npos,
            "Expected API status to mask the OpenAI key.");
    require(api_status.output.find("sk-t...cret") != std::string::npos,
            "Expected API status to show a masked key hint.");

    const CommandCapture link_install =
        run_command_capture("cd " + shell_quote(kSourceRoot.string()) + " && " +
                            shell_quote(binary.string()) + " link install --prefix " +
                            shell_quote(link_prefix.string()) + " --with-tze --force --compact" + memory_args);
    require(link_install.exit_code == 0, "Expected top-level link install to create user-local shims.");
    require(std::filesystem::is_symlink(link_prefix / "omnix"),
            "Expected top-level link install to create an omnix symlink.");
    require(std::filesystem::exists(link_prefix / "tze"),
            "Expected top-level link install to create the tze shim.");

    const CommandCapture run_tze_shim =
        run_command_capture(shell_quote((link_prefix / "tze").string()) + " latest --compact" + memory_args);
    require(run_tze_shim.exit_code == 0, "Expected top-level installed tze shim to execute.");

    const CommandCapture link_remove =
        run_command_capture(shell_quote(binary.string()) + " link remove --prefix " +
                            shell_quote(link_prefix.string()) + " --compact");
    require(link_remove.exit_code == 0, "Expected top-level link remove to succeed.");
    require(!std::filesystem::exists(link_prefix / "omnix") && !std::filesystem::exists(link_prefix / "tze"),
            "Expected top-level link remove to remove OmniX-managed link names.");
}

void test_cli_salt_style_jinja_node_master() {
    const std::filesystem::path root = kBinaryRoot / "cli-salt-style-jinja-node-master";
    const std::filesystem::path memory_root = root / "memory";
    const std::filesystem::path template_path = root / "service-plan.j2";
    const std::filesystem::path vars_path = root / "vars.json";
    const std::filesystem::path rendered_path = root / "rendered.txt";
    const std::filesystem::path heartbeat_path = root / "heartbeat.json";
    const std::filesystem::path job_path = root / "job.json";
    safe_remove_all(root);
    std::filesystem::create_directories(root);
    std::filesystem::create_directories(memory_root);

    {
        std::ofstream template_file(template_path);
        template_file << "service: {{ service }}\n"
                      << "owner: {{ owner }}\n"
                      << "validation: confirm status after plan\n";
    }
    {
        std::ofstream vars_file(vars_path);
        vars_file << "{\"service\":\"abc-worker.service\",\"owner\":\"Engineer Infra\"}\n";
    }

    const std::filesystem::path binary = kBinaryRoot / "omnix";
    const std::string common = shell_quote(binary.string()) + " ";
    const std::string memory_args = " --memory-root " + shell_quote(memory_root.string());

    const CommandCapture inspect = run_command_capture(
        common + "jinja inspect " + shell_quote(template_path.string()) +
        " --vars " + shell_quote(vars_path.string()) + " --compact" + memory_args);
    require(inspect.exit_code == 0, "Expected `omnix jinja inspect` to succeed.");
    require(inspect.output.find("jinja_inspected") != std::string::npos,
            "Expected Jinja inspect to report inspected status.");
    require(inspect.output.find("status: safe") != std::string::npos,
            "Expected simple Jinja inspect fixture to be safe.");

    const CommandCapture render = run_command_capture(
        common + "jinja render " + shell_quote(template_path.string()) +
        " --vars " + shell_quote(vars_path.string()) +
        " --out " + shell_quote(rendered_path.string()) + " --compact" + memory_args);
    require(render.exit_code == 0, "Expected `omnix jinja render` to succeed.");
    require(std::filesystem::exists(rendered_path), "Expected Jinja render to write the requested artifact.");
    const std::string rendered = xpp::read_text_file(rendered_path);
    require(rendered.find("abc-worker.service") != std::string::npos &&
                rendered.find("Engineer Infra") != std::string::npos,
            "Expected Jinja passthrough renderer to replace fixture variables.");

    const CommandCapture plan = run_command_capture(
        common + "jinja plan " + shell_quote(template_path.string()) +
        " --vars " + shell_quote(vars_path.string()) + " --compact" + memory_args);
    require(plan.exit_code == 0, "Expected `omnix jinja plan` to succeed.");
    require(plan.output.find("jinja_planned: runbook_plan") != std::string::npos ||
                plan.output.find("jinja_planned: config_plan") != std::string::npos,
            "Expected Jinja plan to classify the rendered artifact.");

    const CommandCapture execute = run_command_capture(
        common + "jinja execute " + shell_quote(template_path.string()) +
        " --vars " + shell_quote(vars_path.string()) + " --confirm --compact" + memory_args);
    require(execute.exit_code != 0, "Expected `omnix jinja execute` to reject arbitrary execution.");
    require(execute.output.find("jinja_execute_rejected") != std::string::npos,
            "Expected Jinja execute to refuse arbitrary rendered shell text.");

    const CommandCapture node_heartbeat = run_command_capture(
        common + "node heartbeat --out " + shell_quote(heartbeat_path.string()) + " --compact" + memory_args);
    require(node_heartbeat.exit_code == 0, "Expected `omnix node heartbeat` to succeed.");
    require(std::filesystem::exists(heartbeat_path), "Expected node heartbeat artifact to be written.");
    const std::string heartbeat = xpp::read_text_file(heartbeat_path);
    require(heartbeat.find("omnix.node.heartbeat.v1") != std::string::npos,
            "Expected node heartbeat artifact to include its event type.");
    require(heartbeat.find("fingerprint") != std::string::npos &&
                heartbeat.find("grains") != std::string::npos,
            "Expected node heartbeat to include fingerprint and grains.");

    const CommandCapture master_init =
        run_command_capture(common + "master init --compact" + memory_args);
    require(master_init.exit_code == 0, "Expected `omnix master init` to succeed.");

    const CommandCapture approve =
        run_command_capture(common + "master node approve sha256:test-node --compact" + memory_args);
    require(approve.exit_code == 0, "Expected master node approval to succeed.");
    require(approve.output.find("node_approved") != std::string::npos,
            "Expected master node approval to report the approved fingerprint.");

    const CommandCapture job_plan = run_command_capture(
        common + "master job plan defend.detect --target edge-1 --out " + shell_quote(job_path.string()) +
        " --compact" + memory_args);
    require(job_plan.exit_code == 0, "Expected master job plan to succeed for allowlisted jobs.");
    require(std::filesystem::exists(job_path), "Expected master job plan to write a job artifact.");
    const std::string job = xpp::read_text_file(job_path);
    require(job.find("omnix.master.job.v1") != std::string::npos &&
                job.find("\"jobType\":\"defend.detect\"") != std::string::npos &&
                job.find("\"target\":\"edge-1\"") != std::string::npos,
            "Expected master job plan artifact to preserve job type and target.");

    const CommandCapture job_status =
        run_command_capture(common + "master job status --compact" + memory_args);
    require(job_status.exit_code == 0, "Expected master job status to succeed.");
    require(job_status.output.find("master_job_status") != std::string::npos,
            "Expected master job status to report file-spool mode.");

    const CommandCapture dispatch =
        run_command_capture(common + "master job dispatch " + shell_quote(job_path.string()) + " --compact" + memory_args);
    require(dispatch.exit_code != 0, "Expected master dispatch to remain disabled in this phase.");
    require(dispatch.output.find("master_dispatch_disabled") != std::string::npos,
            "Expected master dispatch to explain that network dispatch is deferred.");
}

void test_cli_recursive_why_mines_learned_definition_route() {
    const std::filesystem::path root = kBinaryRoot / "cli-recursive-learned-definition-route";
    const std::filesystem::path memory_root = root / "memory";
    safe_remove_all(root);
    std::filesystem::create_directories(memory_root);

    ScopedEnvVar disable_dictionary("OMNIX_DISABLE_SYSTEM_DICTIONARY", "1");
    const std::filesystem::path binary = kBinaryRoot / "omnix";
    const std::string common = shell_quote(binary.string()) + " ";
    const std::string args = " --memory-root " + shell_quote(memory_root.string()) +
        " --source-map " + shell_quote((kSourceRoot / "res" / "tze.txt").string());

    const CommandCapture apple_ask =
        run_command_capture(common + "ask " + shell_quote("what is Apple in technology") + args + " --compact");
    require(apple_ask.exit_code == 0, "Expected Apple technology ask to be handled.");
    require(apple_ask.output.find("defined: The brainchild of Steve Jobs and his team.") != std::string::npos,
            "Expected normal ask to consume Recursive Route Learning before returning unknown_query.");
    require(apple_ask.output.find("definition-source: recursive_route_learning") != std::string::npos,
            "Expected compact ask output to show Recursive Route Learning provenance.");

    const CommandCapture apple_why =
        run_command_capture(common + "why latest --compact --memory-root " + shell_quote(memory_root.string()));
    require(apple_why.exit_code == 0, "Expected Recursive Why/Diff to explain the Apple route use.");
    require(apple_why.output.find("Recursive Route Learning: recursive_route_learning_used") != std::string::npos,
            "Expected Recursive Route Learning to be first-class in compact why output.");
    require(apple_why.output.find("Ask used the learned route") != std::string::npos,
            "Expected Recursive Why/Diff to report the route was used, not merely discovered after failure.");
    require(apple_why.output.find("route-learning: memory-search:") != std::string::npos,
            "Expected Recursive Why/Diff to include learned-memory search evidence.");
    require(apple_why.output.find("local_glossary:apple") != std::string::npos ||
                apple_why.output.find("prior_tze_definition:Apple[technology]") != std::string::npos,
            "Expected Recursive Why/Diff to mine the Apple technology glossary route.");
    require(apple_why.output.find("The brainchild of Steve Jobs and his team.") != std::string::npos,
            "Expected Recursive Why/Diff to surface the learned Apple answer.");

    const CommandCapture next =
        run_command_capture(common + "next latest --compact --memory-root " + shell_quote(memory_root.string()));
    require(next.exit_code == 0, "Expected `omnix next` to report the latest next action.");
    require(next.output.find("next_action:") != std::string::npos,
            "Expected `omnix next` compact output to include the next action only.");

    const CommandCapture jobs_ask =
        run_command_capture(common + "ask " + shell_quote("who is Steve Jobs") + args + " --compact");
    require(jobs_ask.exit_code == 0, "Expected Steve Jobs ask to be handled.");
    require(jobs_ask.output.find("defined: Read his damn book.") != std::string::npos,
            "Expected single-domain Biography ambiguity to auto-select instead of asking for clarification.");

    const CommandCapture jobs_why =
        run_command_capture(common + "why latest --compact --memory-root " + shell_quote(memory_root.string()));
    require(jobs_why.exit_code == 0, "Expected Recursive Why/Diff to explain the Steve Jobs route miss.");
    require(jobs_why.output.find("local_glossary:Steve Jobs[Biography]") != std::string::npos ||
                jobs_why.output.find("local_glossary matched Steve Jobs domain=Biography") != std::string::npos,
            "Expected Recursive Why/Diff to mine the Steve Jobs biography glossary route.");
    require(jobs_why.output.find("Recursive Route Learning: recursive_route_learning_observed") != std::string::npos,
            "Expected Recursive Why/Diff to observe the successful Steve Jobs route without treating it as a miss.");

    const CommandCapture replay =
        run_command_capture(common + "tze replay latest --memory-root " + shell_quote(memory_root.string()));
    require(replay.exit_code == 0, "Expected replay of route-learning run to succeed.");
    require(replay.output.find("x.Recursive.RouteLearning") != std::string::npos,
            "Expected TZE replay to include the Recursive Route Learning stage.");
}

void test_cli_context_reset_clears_volatile_definition_cache() {
    const std::filesystem::path root = kBinaryRoot / "cli-context-reset";
    const std::filesystem::path memory_root = root / "memory";
    safe_remove_all(root);
    std::filesystem::create_directories(memory_root);

    ScopedEnvVar disable_dictionary("OMNIX_DISABLE_SYSTEM_DICTIONARY", "1");
    const std::filesystem::path binary = kBinaryRoot / "omnix";
    const std::string common = shell_quote(binary.string()) + " ";
    const std::string memory_args = " --memory-root " + shell_quote(memory_root.string()) +
        " --source-map " + shell_quote((kSourceRoot / "res" / "tze.txt").string());

    tze::MemoryStore store;
    tze::MemorySnapshot snapshot = store.load(memory_root);
    tze::DefinitionAnswer answer;
    answer.query = "Apple";
    answer.normalized_concept = "apple";
    answer.domain_hint = "technology";
    answer.found = true;
    answer.summary = "The brainchild of Steve Jobs and his team.";
    answer.semantic_family = "general_knowledge";
    answer.selected_source_type = "memory";
    answer.selected_source_label = "test-runtime-cache";
    answer.selected_authority_tier = "operator_override";
    answer.confidence = 0.82;
    store.remember_definition(snapshot, answer);
    store.persist_snapshot(snapshot);

    const std::string seeded_definitions = xpp::read_text_file(memory_root / "definitions.json");
    require(seeded_definitions.find("Steve Jobs") != std::string::npos,
            "Expected seeded runtime definitions cache to contain the temporary Apple association.");

    const CommandCapture reset =
        run_command_capture(common + "context reset --compact --memory-root " + shell_quote(memory_root.string()));
    require(reset.exit_code == 0, "Expected top-level context reset to complete.");
    require(reset.output.find("context_reset") != std::string::npos,
            "Expected context reset to report the reset status.");

    const std::string reset_definitions = xpp::read_text_file(memory_root / "definitions.json");
    require(reset_definitions.find("Steve Jobs") == std::string::npos,
            "Expected context reset to clear the volatile runtime definition cache.");

    const CommandCapture apple_default =
        run_command_capture(common + "ask " + shell_quote("what is apple") + memory_args + " --compact");
    require(apple_default.exit_code == 0, "Expected Apple ask to resolve after context reset.");
    require(apple_default.output.find("round fruit") != std::string::npos,
            "Expected source truth to return the default fruit definition after context reset.");
    require(apple_default.output.find("Steve Jobs") == std::string::npos,
            "Expected reset source truth not to stay pinned to the temporary Apple technology association.");

    const CommandCapture memory_reset =
        run_command_capture(common + "memory reset-context --compact --memory-root " + shell_quote(memory_root.string()));
    require(memory_reset.exit_code == 0, "Expected memory reset-context alias to complete.");
    require(memory_reset.output.find("context_reset") != std::string::npos,
            "Expected memory reset-context alias to share the same reset status.");

    const std::string shell_command =
        "printf '%s\\n' '/status' '/reset' '/quit' | " +
        common + "shell --compact --memory-root " + shell_quote(memory_root.string());
    const CommandCapture shell = run_command_capture(shell_command);
    require(shell.exit_code == 0, "Expected shell /reset to complete.");
    require(shell.output.find("Shell context reset.") != std::string::npos,
            "Expected shell /reset to clear observer state.");

    tze::MemorySnapshot expiring = store.load(memory_root);
    expiring.definitions.push_back({
        "Temporary Apple",
        "temporary apple",
        "technology",
        "Expired sticky association.",
        "",
        "general_knowledge",
        "test",
        "memory",
        "memory_artifact",
        0.9,
        "temporary",
        "2000-01-01T00:00:00",
        "2000-01-01T00:00:01",
    });
    expiring.history.push_back({
        "2000-01-01T00:00:00",
        "what is Temporary Apple",
        "general_definition_query",
        "",
        "defined",
        "Expired sticky association.",
        "temporary",
        "2000-01-01T00:00:00",
        "2000-01-01T00:00:01",
    });
    store.persist_snapshot(expiring);
    const CommandCapture prune_expired =
        run_command_capture(common + "memory prune-expired --compact --memory-root " + shell_quote(memory_root.string()));
    require(prune_expired.exit_code == 0, "Expected memory prune-expired to complete.");
    require(prune_expired.output.find("memory_pruned_expired") != std::string::npos,
            "Expected prune-expired to report compact status.");
    const std::string pruned_definitions = xpp::read_text_file(memory_root / "definitions.json");
    const std::string pruned_history = xpp::read_text_file(memory_root / "history.jsonl");
    require(pruned_definitions.find("Expired sticky association") == std::string::npos,
            "Expected prune-expired to remove expired temporary definitions.");
    require(pruned_history.find("Expired sticky association") == std::string::npos,
            "Expected prune-expired to remove expired temporary history.");
}

void test_cli_shell_nmap_results_alias() {
    const std::filesystem::path root = kBinaryRoot / "cli-shell-nmap-results";
    const std::filesystem::path bin_dir = root / "bin";
    const std::filesystem::path memory_root = root / "memory";
    const std::filesystem::path search_root = root / "search";
    safe_remove_all(root);
    std::filesystem::create_directories(bin_dir);
    std::filesystem::create_directories(memory_root);
    std::filesystem::create_directories(search_root);

    make_executable(bin_dir / "nmap",
                    "#!/bin/sh\n"
                    "if [ \"$1\" = \"-V\" ] || [ \"$1\" = \"--version\" ]; then echo 'Nmap 7.95'; exit 0; fi\n"
                    "if [ \"$1\" = \"-F\" ] && [ \"$2\" = \"127.0.0.1\" ]; then echo 'Nmap scan report for 127.0.0.1'; echo 'Host is up.'; exit 0; fi\n"
                    "exit 1\n");

    const std::filesystem::path binary = kBinaryRoot / "omnix";
    const std::string command =
        "printf '%s\\n' 'Run NMAP Scan' 'nmap results' 'ask nmap results' '/quit' | "
        "PATH=" + shell_quote(bin_dir.string()) + " " +
        "OMNIX_HOME=" + shell_quote(memory_root.string()) + " " +
        "OMNIX_NATIVE_SEARCH_ROOTS=" + shell_quote(search_root.string()) + " " +
        shell_quote(binary.string()) + " shell --assist --memory-root " + shell_quote(memory_root.string()) +
        " --source-map " + shell_quote((kSourceRoot / "res" / "tze.txt").string());

    const CommandCapture shell = run_command_capture(command);
    require(shell.exit_code == 0, "Expected shell Nmap result aliases to exit cleanly.");
    require(shell.output.find("Nmap scan report for 127.0.0.1") != std::string::npos,
            "Expected shell scan flow to capture the Nmap output excerpt.");
    require(shell.output.find("Last results:") != std::string::npos,
            "Expected shell `nmap results` handling to print the cached last results block.");
    require(shell.output.find("command: '") != std::string::npos && shell.output.find("'-F' '127.0.0.1'") != std::string::npos,
            "Expected shell `nmap results` handling to show the exact guarded command that ran.");
}

void test_cli_shell_secure_my_system_routes_to_inspect_host() {
    const std::filesystem::path root = kBinaryRoot / "cli-shell-secure-my-system";
    const std::filesystem::path memory_root = root / "memory";
    safe_remove_all(root);
    std::filesystem::create_directories(memory_root);

    const std::filesystem::path binary = kBinaryRoot / "omnix";
    const std::string command =
        "printf '%s\\n' 'Ollama, secure my system' '/quit' | " +
        shell_quote(binary.string()) + " shell --assist --memory-root " + shell_quote(memory_root.string()) +
        " --source-map " + shell_quote((kSourceRoot / "res" / "tze.txt").string());

    const CommandCapture shell = run_command_capture(command);
    require(shell.exit_code == 0, "Expected shell system-security prompt to exit cleanly.");
    require(shell.output.find("Project: inspect-host") != std::string::npos,
            "Expected `secure my system` to route into the inspect-host analyst module.");
    require(shell.output.find("Tool: inspect-host") != std::string::npos,
            "Expected `secure my system` to execute the inspect-host analyst module.");
    require(shell.output.find("host-inspection/") != std::string::npos,
            "Expected `secure my system` to produce a host inspection artifact.");
}

void test_cli_shell_local_slash24_routes_to_safe_nmap_discovery() {
    const std::filesystem::path root = kBinaryRoot / "cli-shell-nmap-local-slash24";
    const std::filesystem::path bin_dir = root / "bin";
    const std::filesystem::path memory_root = root / "memory";
    const std::filesystem::path search_root = root / "search";
    safe_remove_all(root);
    std::filesystem::create_directories(bin_dir);
    std::filesystem::create_directories(memory_root);
    std::filesystem::create_directories(search_root);

    make_executable(bin_dir / "nmap",
                    "#!/bin/sh\n"
                    "if [ \"$1\" = \"-V\" ] || [ \"$1\" = \"--version\" ]; then echo 'Nmap 7.95'; exit 0; fi\n"
                    "if [ \"$1\" = \"-sn\" ] && [ \"$2\" = \"127.0.0.0/24\" ]; then echo 'Nmap scan report for localhost (127.0.0.1)'; echo 'Host is up.'; echo 'Nmap done: 256 IP addresses (1 host up) scanned in 0.10 seconds'; exit 0; fi\n"
                    "exit 1\n");

    const std::filesystem::path binary = kBinaryRoot / "omnix";
    const std::string command =
        "printf '%s\\n' 'run NMAP of local /24 and output all results' 'ollama where are my nmap results' '/quit' | "
        "PATH=" + shell_quote(bin_dir.string()) + " " +
        "OMNIX_HOME=" + shell_quote(memory_root.string()) + " " +
        "OMNIX_NATIVE_SEARCH_ROOTS=" + shell_quote(search_root.string()) + " " +
        shell_quote(binary.string()) + " shell --assist --memory-root " + shell_quote(memory_root.string()) +
        " --source-map " + shell_quote((kSourceRoot / "res" / "tze.txt").string());

    const CommandCapture shell = run_command_capture(command);
    require(shell.exit_code == 0, "Expected shell `/24` Nmap prompt to exit cleanly.");
    require(shell.output.find("'-sn' '127.0.0.0/24'") != std::string::npos,
            "Expected local `/24` phrasing to route into the guarded loopback-subnet host discovery scan.");
    require(shell.output.find("Nmap done: 256 IP addresses (1 host up) scanned in 0.10 seconds") != std::string::npos,
            "Expected the guarded loopback-subnet host discovery scan output to be surfaced.");
    require(shell.output.find("Last results:") != std::string::npos,
            "Expected `where are my nmap results` to print the cached last results block.");
}

void test_cli_shell_handles_greeting_and_identity() {
    const std::filesystem::path root = kBinaryRoot / "cli-shell-conversation";
    const std::filesystem::path memory_root = root / "memory";
    safe_remove_all(root);
    std::filesystem::create_directories(memory_root);

    const std::filesystem::path binary = kBinaryRoot / "omnix";
    const std::string command =
        "printf '%s\\n' 'Hello' 'Who are you' '/quit' | " +
        shell_quote(binary.string()) + " shell --memory-root " + shell_quote(memory_root.string()) +
        " --source-map " + shell_quote((kSourceRoot / "res" / "tze.txt").string());

    const CommandCapture shell = run_command_capture(command);
    require(shell.exit_code == 0, "Expected conversational shell prompts to exit cleanly.");
    require(shell.output.find("Hello. I’m OmniX") != std::string::npos,
            "Expected Hello to return a conversational OmniX greeting.");
    require(shell.output.find("I’m OmniX: a deterministic analyst") != std::string::npos,
            "Expected `Who are you` to return an identity response.");
}

void test_cli_shell_broad_aliases_and_persona_identity() {
    const std::filesystem::path root = kBinaryRoot / "cli-shell-aliases";
    const std::filesystem::path memory_root = root / "memory";
    safe_remove_all(root);
    std::filesystem::create_directories(memory_root);

    tze::MemoryStore store;
    tze::MemorySnapshot snapshot = store.load(memory_root);
    tze::OperatorPersonaRecord persona;
    persona.preferred_label = "Premise";
    persona.role_label = "Local Operator";
    persona.local_username = "premise";
    persona.host_identifier = "MacBookPro";
    persona.self_description = "Trusted local analyst.";
    store.remember_operator_persona(snapshot, persona);
    store.persist_snapshot(snapshot);

    const std::filesystem::path binary = kBinaryRoot / "omnix";
    const std::string command =
        "printf '%s\\n' 'commands' 'history' 'Who am I?' 'quit' | " +
        shell_quote(binary.string()) + " shell --memory-root " + shell_quote(memory_root.string()) +
        " --source-map " + shell_quote((kSourceRoot / "res" / "tze.txt").string());

    const CommandCapture shell = run_command_capture(command);
    require(shell.exit_code == 0, "Expected broad alias shell prompts to exit cleanly.");
    require(shell.output.find("OmniX shell commands:") != std::string::npos,
            "Expected `commands` to normalize to shell help.");
    require(shell.output.find("History:") != std::string::npos,
            "Expected `history` to normalize to `memory history`.");
    require(shell.output.find("You are Premise, acting as Local Operator.") != std::string::npos,
            "Expected `Who am I?` to prefer the stored persona.");
    require(shell.output.find("Closing OmniX shell.") != std::string::npos,
            "Expected `quit` to close the shell through the natural alias.");
}

void test_cli_persona_modes_are_display_only() {
    const std::filesystem::path root = kBinaryRoot / "cli-persona-modes";
    const std::filesystem::path memory_root = root / "memory";
    safe_remove_all(root);
    std::filesystem::create_directories(root);

    const std::filesystem::path binary = kBinaryRoot / "omnix";
    const CommandCapture set_mode = run_command_capture(
        shell_quote(binary.string()) + " persona mode cynic --memory-root " + shell_quote(memory_root.string()));
    require(set_mode.exit_code == 0, "Expected persona mode CLI command to succeed.");
    require(set_mode.output.find("Cynic Mode") != std::string::npos,
            "Expected persona mode command to acknowledge Cynic Mode.");
    require(set_mode.output.find("Safety remains bounded") != std::string::npos,
            "Expected persona mode output to state the safety boundary.");

    const CommandCapture memory = run_command_capture(
        shell_quote(binary.string()) + " memory persona --memory-root " + shell_quote(memory_root.string()));
    require(memory.exit_code == 0, "Expected memory persona view to succeed.");
    require(memory.output.find("Mode: cynic") != std::string::npos,
            "Expected memory persona view to render the active mode.");
    require(memory.output.find("Safety posture: display_only_safety_bounded") != std::string::npos,
            "Expected persona mode to stay display-only in memory.");

    const CommandCapture identity = run_command_capture(
        shell_quote(binary.string()) + " ask " + shell_quote("Who am I?") +
        " --memory-root " + shell_quote(memory_root.string()));
    require(identity.exit_code == 0, "Expected persona-backed identity prompt to succeed.");
    require(identity.output.find("Active mode: cynic") != std::string::npos,
            "Expected operator identity to include the active persona mode.");
}

void test_cli_shell_persona_mode_shortcut() {
    const std::filesystem::path root = kBinaryRoot / "cli-shell-persona-mode";
    const std::filesystem::path memory_root = root / "memory";
    safe_remove_all(root);
    std::filesystem::create_directories(root);

    const std::filesystem::path binary = kBinaryRoot / "omnix";
    const std::string command =
        "printf '%s\\n' 'professional mode' 'Who am I?' 'quit' | " +
        shell_quote(binary.string()) + " shell --memory-root " + shell_quote(memory_root.string()) +
        " --source-map " + shell_quote((kSourceRoot / "res" / "tze.txt").string());

    const CommandCapture shell = run_command_capture(command);
    require(shell.exit_code == 0, "Expected shell persona mode shortcut to exit cleanly.");
    require(shell.output.find("Professional Mode") != std::string::npos,
            "Expected shell shortcut to set Professional Mode.");
    require(shell.output.find("Active mode: professional") != std::string::npos,
            "Expected shell identity to include Professional Mode.");
}

void test_cli_shell_next_step_assist_persists_memory() {
    const std::filesystem::path root = kBinaryRoot / "cli-shell-next-step-assist";
    const std::filesystem::path memory_root = root / "memory";
    const std::filesystem::path fixture = root / "next-step.json";
    safe_remove_all(root);
    std::filesystem::create_directories(root);

    {
        std::ofstream output(fixture);
        output << "{\n"
               << "  \"suggested_next_step\": \"Run nmap results\",\n"
               << "  \"confidence\": 0.88,\n"
               << "  \"rationale\": \"Review the most recent scan output before narrowing scope.\",\n"
               << "  \"safer_alternative\": \"Create a case from the last scan if you want to preserve evidence.\",\n"
               << "  \"warnings\": [\"Keep scans scoped to the local loopback range.\"]\n"
               << "}\n";
    }

    const std::filesystem::path binary = kBinaryRoot / "omnix";
    const std::string command =
        "printf '%s\\n' 'Run NMAP with a local /24 scan' 'Next?' '/quit' | "
        "OMNIX_REASONING_PROVIDER=ollama "
        "OMNIX_OLLAMA_MODEL=fixture "
        "OMNIX_OLLAMA_NEXT_STEP_PLAN_FILE=" + shell_quote(fixture.string()) + " " +
        shell_quote(binary.string()) + " shell --assist --memory-root " + shell_quote(memory_root.string()) +
        " --source-map " + shell_quote((kSourceRoot / "res" / "tze.txt").string());

    const CommandCapture shell = run_command_capture(command);
    require(shell.exit_code == 0, "Expected assisted shell next-step flow to exit cleanly.");
    require(shell.output.find("Assist next: Run nmap results") != std::string::npos,
            "Expected shell `Next?` to surface the validated assist next-step suggestion.");
    require(shell.output.find("Assist safer alternative: Create a case from the last scan if you want to preserve evidence.") != std::string::npos,
            "Expected shell `Next?` to surface the validated safer alternative.");

    const CommandCapture memory = run_command_capture(
        shell_quote(binary.string()) + " memory assist --memory-root " + shell_quote(memory_root.string()));
    require(memory.exit_code == 0, "Expected `memory assist` to succeed after shell next-step assist.");
    require(memory.output.find("Outcomes: 1") != std::string::npos,
            "Expected assist memory to record the shell next-step outcome.");
    require(memory.output.find("[assist_used] next_step :: Run nmap results") != std::string::npos,
            "Expected assist memory to render the persisted next-step outcome.");
}

void test_cli_shell_tolerates_nmap_typos_and_full_results() {
    const std::filesystem::path root = kBinaryRoot / "cli-shell-nmap-lexicon";
    const std::filesystem::path bin_dir = root / "bin";
    const std::filesystem::path memory_root = root / "memory";
    const std::filesystem::path search_root = root / "search";
    safe_remove_all(root);
    std::filesystem::create_directories(bin_dir);
    std::filesystem::create_directories(memory_root);
    std::filesystem::create_directories(search_root);

    make_executable(bin_dir / "nmap",
                    "#!/bin/sh\n"
                    "if [ \"$1\" = \"-sn\" ] && [ \"$2\" = \"127.0.0.0/24\" ]; then echo 'Starting Nmap'; echo 'Nmap scan report for localhost (127.0.0.1)'; echo 'Host is up.'; echo 'Nmap done: 256 IP addresses (1 host up) scanned in 0.10 seconds'; exit 0; fi\n"
                    "if [ \"$1\" = \"-F\" ] && [ \"$2\" = \"127.0.0.1\" ]; then echo 'Starting Nmap'; echo 'Fast scan complete'; exit 0; fi\n"
                    "if [ \"$1\" = \"-V\" ] || [ \"$1\" = \"--version\" ]; then echo 'Nmap 7.95'; exit 0; fi\n"
                    "exit 1\n");

    const std::filesystem::path binary = kBinaryRoot / "omnix";
    const std::string command =
        "printf '%s\\n' 'always -v' 'use namp locally 127.0.0.1' 'run nmap /24 locally with a -v and output results' '/quit' | "
        "PATH=" + shell_quote(bin_dir.string()) + " " +
        "OMNIX_HOME=" + shell_quote(memory_root.string()) + " " +
        "OMNIX_NATIVE_SEARCH_ROOTS=" + shell_quote(search_root.string()) + " " +
        shell_quote(binary.string()) + " shell --assist --memory-root " + shell_quote(memory_root.string()) +
        " --source-map " + shell_quote((kSourceRoot / "res" / "tze.txt").string());

    const CommandCapture shell = run_command_capture(command);
    require(shell.exit_code == 0, "Expected lexicon-driven Nmap prompts to exit cleanly.");
    require(shell.output.find("Full shell tool results enabled.") != std::string::npos,
            "Expected `always -v` to update shell result visibility.");
    require(shell.output.find("'-F' '127.0.0.1'") != std::string::npos,
            "Expected `use namp locally 127.0.0.1` to correct the typo and run the safe local target scan.");
    require(shell.output.find("'-sn' '127.0.0.0/24'") != std::string::npos,
            "Expected `/24` local phrasing to normalize into the guarded loopback subnet scan.");
    require(shell.output.find("output:\n - Starting Nmap\n - Nmap scan report for localhost (127.0.0.1)") != std::string::npos,
            "Expected full shell output mode to print the multi-line scan results.");
}

void test_cli_review_and_patch_proposal_round_trip() {
    const std::filesystem::path root = kBinaryRoot / "cli-review-and-patch";
    const std::filesystem::path memory_root = root / "memory";
    safe_remove_all(root);
    std::filesystem::create_directories(root);

    const std::filesystem::path binary = kBinaryRoot / "omnix";
    const std::string review_target = (kSourceRoot / "src" / "session_coordinator.cpp").string();

    const CommandCapture review = run_command_capture(
        shell_quote(binary.string()) + " review " + shell_quote(review_target) +
        " --memory-root " + shell_quote(memory_root.string()));
    require(review.exit_code == 0, "Expected `omnix review` to succeed.");
    require(review.output.find("Verdict: review_ready") != std::string::npos,
            "Expected review output to expose the review_ready verdict.");
    const std::string review_artifact = extract_line_value(review.output, "Produced artifact: ");
    require(!review_artifact.empty() && std::filesystem::exists(review_artifact),
            "Expected `omnix review` to write a persisted review artifact.");

    const CommandCapture patch = run_command_capture(
        shell_quote(binary.string()) + " patch-proposal " + shell_quote(review_target) +
        " --memory-root " + shell_quote(memory_root.string()));
    require(patch.exit_code == 0, "Expected `omnix patch-proposal` to succeed.");
    require(patch.output.find("Verdict: patch_proposal_ready") != std::string::npos,
            "Expected patch proposal output to expose the patch_proposal_ready verdict.");
    const std::string patch_artifact = extract_line_value(patch.output, "Produced artifact: ");
    require(!patch_artifact.empty() && std::filesystem::exists(patch_artifact),
            "Expected `omnix patch-proposal` to write a persisted patch artifact.");

    const CommandCapture replay = run_command_capture(
        shell_quote(binary.string()) + " tze replay latest --memory-root " + shell_quote(memory_root.string()));
    require(replay.exit_code == 0, "Expected replay after patch proposal to succeed.");
    require(replay.output.find("Review artifact:") != std::string::npos,
            "Expected TZE replay to include the persisted review artifact.");
    require(replay.output.find("Patch proposal artifact:") != std::string::npos,
            "Expected TZE replay to include the persisted patch proposal artifact.");
}

void test_cli_incident_report_assist_summary() {
    const std::filesystem::path root = kBinaryRoot / "cli-incident-assist-summary";
    const std::filesystem::path memory_root = root / "memory";
    const std::filesystem::path fixture = root / "case-summary.json";
    const std::filesystem::path sample_one = root / "a.log";
    const std::filesystem::path sample_two = root / "b.log";
    safe_remove_all(root);
    std::filesystem::create_directories(root);

    {
        std::ofstream output(fixture);
        output << "{\n"
               << "  \"summary_title\": \"Incident executive summary\",\n"
               << "  \"executive_summary\": \"The incident clusters repeated SSH authentication failures and recommends preserving the evidence trail.\",\n"
               << "  \"highlights\": [\"SSH auth failures repeated across the local host.\", \"Preserve the case bundle before remediation.\"],\n"
               << "  \"rationale\": \"Summarize the correlated incident so the operator can move into preservation or containment.\",\n"
               << "  \"confidence\": 0.86,\n"
               << "  \"recommended_followup\": \"Run another incident report export after triage notes are added.\",\n"
               << "  \"warnings\": [\"Summary is advisory only; execution remains deterministic.\"]\n"
               << "}\n";
    }
    {
        std::ofstream output(sample_one);
        output << "Failed password for root from 10.0.0.8\n";
    }
    {
        std::ofstream output(sample_two);
        output << "Failed password for admin from 10.0.0.9\n";
    }

    const std::filesystem::path binary = kBinaryRoot / "omnix";
    require(run_command_capture(shell_quote(binary.string()) + " ingest " + shell_quote(sample_one.string()) +
                                " --memory-root " + shell_quote(memory_root.string())).exit_code == 0,
            "Expected first assist-summary ingest to succeed.");
    require(run_command_capture(shell_quote(binary.string()) + " ingest " + shell_quote(sample_two.string()) +
                                " --memory-root " + shell_quote(memory_root.string())).exit_code == 0,
            "Expected second assist-summary ingest to succeed.");

    const CommandCapture incident_list =
        run_command_capture(shell_quote(binary.string()) + " incident list --memory-root " + shell_quote(memory_root.string()));
    require(incident_list.exit_code == 0, "Expected incident list to succeed before assist-summary reporting.");
    const std::string incident_line = extract_line_value(incident_list.output, " - ");
    require(incident_line.find("incident-") != std::string::npos,
            "Expected incident list output to include an incident id.");
    const std::string incident_id = incident_line.substr(0, incident_line.find(" | "));

    const std::string report_command =
        "OMNIX_REASONING_PROVIDER=ollama "
        "OMNIX_OLLAMA_MODEL=fixture "
        "OMNIX_OLLAMA_CASE_SUMMARY_PLAN_FILE=" + shell_quote(fixture.string()) + " " +
        shell_quote(binary.string()) + " incident report " + shell_quote(incident_id) +
        " --assist --memory-root " + shell_quote(memory_root.string());
    const CommandCapture report = run_command_capture(report_command);
    require(report.exit_code == 0, "Expected assist-backed incident report generation to succeed.");
    require(report.output.find("Assist: assist_used") != std::string::npos,
            "Expected incident report summary to report assist_used.");
    require(report.output.find("Assist summary title: Incident executive summary") != std::string::npos,
            "Expected incident report summary to expose the validated title.");
    const std::string report_artifact = extract_line_value(report.output, "Produced artifact: ");
    require(!report_artifact.empty() && std::filesystem::exists(report_artifact),
            "Expected assist-backed incident report to write a persisted artifact.");
    require(xpp::read_text_file(report_artifact).find("## Guarded Assist Summary") != std::string::npos,
            "Expected persisted incident report artifact to include the guarded assist summary block.");
}

void test_cli_doctor_and_recipe_override_output() {
    const std::filesystem::path memory_root = kBinaryRoot / "cli-memory-doctor";
    safe_remove_all(memory_root);
    const std::filesystem::path binary = kBinaryRoot / "omnix";

    const CommandCapture version =
        run_command_capture(shell_quote(binary.string()) + " --version");
    require(version.exit_code == 0, "Expected `omnix --version` to succeed.");
    require(version.output.find("omnix ") != std::string::npos,
            "Expected `omnix --version` output to include the version banner.");

    const CommandCapture doctor = run_command_capture(
        shell_quote(binary.string()) + " doctor nmap --offline --memory-root " + shell_quote(memory_root.string()) +
        " --source-map " + shell_quote((kSourceRoot / "res" / "tze.txt").string()));
    require(doctor.output.find("Primary guidance") != std::string::npos,
            "Expected doctor output to include package-manager guidance.");
    require(doctor.output.find("brew install") != std::string::npos,
            "Expected doctor output to include Homebrew guidance.");
    require(doctor.output.find("apt-get install") != std::string::npos,
            "Expected doctor output to include APT guidance.");
    require(doctor.output.find("pacman -S") != std::string::npos,
            "Expected doctor output to include Pacman guidance.");
    require(doctor.output.find("rpm -q") != std::string::npos,
            "Expected doctor output to include RPM verification guidance.");
    require(doctor.output.find("curl -L") != std::string::npos,
            "Expected doctor output to include curl bootstrap guidance.");
    require(doctor.output.find("wget -O") != std::string::npos,
            "Expected doctor output to include wget bootstrap guidance.");

    const CommandCapture invalid_recipe = run_command_capture(
        shell_quote(binary.string()) + " preflight nmap --recipe invalid-recipe --offline --memory-root " +
        shell_quote(memory_root.string()) + " --source-map " +
        shell_quote((kSourceRoot / "res" / "tze.txt").string()));
    require(invalid_recipe.exit_code != 0, "Expected invalid recipe override to fail.");
    require(invalid_recipe.output.find("Available recipes") != std::string::npos,
            "Expected invalid recipe output to include available recipe suggestions.");
}

void test_cli_tze_replay_and_diff() {
    const std::filesystem::path memory_root = kBinaryRoot / "cli-tze-runs";
    safe_remove_all(memory_root);
    const std::filesystem::path binary = kBinaryRoot / "omnix";
    const std::string source_map = shell_quote((kSourceRoot / "res" / "tze.txt").string());

    const CommandCapture define = run_command_capture(
        shell_quote(binary.string()) + " define xProcessingCache " + source_map +
        " --memory-root " + shell_quote(memory_root.string()));
    require(define.exit_code == 0, "Expected first TZE-producing define run to succeed.");
    require(define.output.find("Cache.PrepareWorkspace (legacy=xProcessingCache)") != std::string::npos,
            "Expected live CLI output to lead with HumanReadable stage names and preserve legacy provenance.");
    const std::string define_run = extract_line_value(define.output, "TZE run: ");
    require(!define_run.empty(), "Expected define output to expose a TZE run id.");

    const CommandCapture explain = run_command_capture(
        shell_quote(binary.string()) + " explain xProcessingCache " + source_map +
        " --memory-root " + shell_quote(memory_root.string()));
    require(explain.exit_code == 0, "Expected second TZE-producing explain run to succeed.");
    const std::string explain_run = extract_line_value(explain.output, "TZE run: ");
    require(!explain_run.empty(), "Expected explain output to expose a TZE run id.");

    const CommandCapture runs = run_command_capture(
        shell_quote(binary.string()) + " tze runs --memory-root " + shell_quote(memory_root.string()));
    require(runs.exit_code == 0, "Expected `omnix tze runs` to succeed.");
    require(runs.output.find(define_run) != std::string::npos,
            "Expected `omnix tze runs` to include the define run id.");
    require(runs.output.find(explain_run) != std::string::npos,
            "Expected `omnix tze runs` to include the explain run id.");

    const CommandCapture replay = run_command_capture(
        shell_quote(binary.string()) + " tze replay " + shell_quote(define_run) +
        " --memory-root " + shell_quote(memory_root.string()));
    require(replay.exit_code == 0, "Expected `omnix tze replay` to succeed.");
    require(replay.output.find("TZE Run Replay") != std::string::npos,
            "Expected replay output to include the replay header.");
    require(replay.output.find("Query session:") != std::string::npos,
            "Expected replay output to include the persisted query session summary.");
    require(replay.output.find("Stages:") != std::string::npos,
            "Expected replay output to include the stored stage list.");
    require(replay.output.find("x.Dispatch") != std::string::npos,
            "Expected replay output to include the dispatch stage.");
    require(replay.output.find("source: Build CMake:") != std::string::npos,
            "Expected replay output to include source-backed stage provenance.");
    require(replay.output.find("xProcessingCache(findStorage);") != std::string::npos,
            "Expected replay output to include a source excerpt from the parsed stage graph.");

    const CommandCapture diff = run_command_capture(
        shell_quote(binary.string()) + " tze diff " + shell_quote(define_run) + " " + shell_quote(explain_run) +
        " --memory-root " + shell_quote(memory_root.string()));
    require(diff.exit_code == 0, "Expected `omnix tze diff` to succeed.");
    require(diff.output.find("TZE Run Diff") != std::string::npos,
            "Expected diff output to include the diff header.");
    require(diff.output.find("Changed fields:") != std::string::npos,
            "Expected diff output to explain the changed run fields.");
    require(diff.output.find("Stage changes:") != std::string::npos,
            "Expected diff output to explain the stage-level differences.");
}

void test_cli_tze_latest_prune_report_and_feedback() {
    const std::filesystem::path root = kBinaryRoot / "cli-tze-latest";
    const std::filesystem::path memory_root = root / "memory";
    const std::filesystem::path report_root = root / "reports";
    safe_remove_all(root);
    std::filesystem::create_directories(report_root);
    const std::filesystem::path binary = kBinaryRoot / "omnix";
    const std::string source_map = shell_quote((kSourceRoot / "res" / "tze.txt").string());

    const CommandCapture define = run_command_capture(
        shell_quote(binary.string()) + " define xProcessingCache " + source_map +
        " --memory-root " + shell_quote(memory_root.string()));
    require(define.exit_code == 0, "Expected first TZE-producing define run to succeed.");
    const std::string define_run = extract_line_value(define.output, "TZE run: ");
    require(!define_run.empty(), "Expected define output to expose a TZE run id.");

    const CommandCapture explain = run_command_capture(
        shell_quote(binary.string()) + " explain x.DisplayFeedBackLoop " + source_map +
        " --memory-root " + shell_quote(memory_root.string()));
    require(explain.exit_code == 0, "Expected second TZE-producing explain run to succeed.");
    const std::string explain_run = extract_line_value(explain.output, "TZE run: ");
    require(!explain_run.empty(), "Expected explain output to expose a TZE run id.");

    const CommandCapture latest = run_command_capture(
        shell_quote(binary.string()) + " tze latest --memory-root " + shell_quote(memory_root.string()));
    require(latest.exit_code == 0, "Expected `omnix tze latest` to succeed.");
    require(latest.output.find(explain_run) != std::string::npos,
            "Expected `omnix tze latest` to replay the most recent run.");

    const CommandCapture diff_latest = run_command_capture(
        shell_quote(binary.string()) + " tze diff-latest --memory-root " + shell_quote(memory_root.string()));
    require(diff_latest.exit_code == 0, "Expected `omnix tze diff-latest` to succeed.");
    require(diff_latest.output.find("TZE Run Diff") != std::string::npos,
            "Expected diff-latest output to include the diff header.");

    const CommandCapture explain_change = run_command_capture(
        shell_quote(binary.string()) + " tze explain-change-latest --memory-root " + shell_quote(memory_root.string()));
    require(explain_change.exit_code == 0, "Expected `omnix tze explain-change-latest` to succeed.");
    require(explain_change.output.find("TZE Change Explanation") != std::string::npos,
            "Expected explain-change output to include the explanation header.");
    require(explain_change.output.find("Stage interpretation:") != std::string::npos,
            "Expected explain-change output to include the stage interpretation section.");
    require(explain_change.output.find("Memory.FeedbackReview") != std::string::npos ||
                explain_change.output.find("Knowledge.EvidenceRanking") != std::string::npos ||
                explain_change.output.find("Memory.StoreArtifact") != std::string::npos ||
                explain_change.output.find("Cache.PrepareWorkspace") != std::string::npos,
            "Expected explain-change output to use HumanReadable stage interpretation labels.");

    const std::filesystem::path report_path = report_root / "latest-run.txt";
    const CommandCapture report = run_command_capture(
        shell_quote(binary.string()) + " tze report latest --out " + shell_quote(report_path.string()) +
        " --memory-root " + shell_quote(memory_root.string()));
    require(report.exit_code == 0, "Expected `omnix tze report latest` to succeed.");
    require(std::filesystem::exists(report_path), "Expected the TZE run report artifact to be written.");
    {
        std::ifstream input(report_path);
        std::ostringstream text;
        text << input.rdbuf();
        require(text.str().find("# OmniX TZE Run Report") != std::string::npos,
                "Expected the TZE run report artifact to include the report header.");
        require(text.str().find("Memory.StoreArtifact (legacy=x.Store)") != std::string::npos,
                "Expected the TZE run report artifact to include HumanReadable storage stage labels.");
    }

    const std::filesystem::path diff_report_path = report_root / "latest-diff.txt";
    const CommandCapture diff_report = run_command_capture(
        shell_quote(binary.string()) + " tze diff-report latest previous --out " + shell_quote(diff_report_path.string()) +
        " --memory-root " + shell_quote(memory_root.string()));
    require(diff_report.exit_code == 0, "Expected `omnix tze diff-report latest previous` to succeed.");
    require(std::filesystem::exists(diff_report_path), "Expected the TZE diff report artifact to be written.");

    const CommandCapture mark = run_command_capture(
        shell_quote(binary.string()) + " tze mark " + shell_quote(explain_run) + " helpful --note " +
        shell_quote("Validated the replay and it helped.") +
        " --memory-root " + shell_quote(memory_root.string()));
    require(mark.exit_code == 0, "Expected `omnix tze mark` to succeed.");
    require(mark.output.find("Verdict: tze_feedback_recorded") != std::string::npos,
            "Expected feedback recording to report success.");

    const CommandCapture replay_marked = run_command_capture(
        shell_quote(binary.string()) + " tze replay " + shell_quote(explain_run) +
        " --memory-root " + shell_quote(memory_root.string()));
    require(replay_marked.exit_code == 0, "Expected replaying the marked run to succeed.");
    require(replay_marked.output.find("Operator feedback: helpful") != std::string::npos,
            "Expected replay output to include the recorded operator feedback.");

    const CommandCapture prune_runs = run_command_capture(
        shell_quote(binary.string()) + " tze prune --keep 1 --memory-root " + shell_quote(memory_root.string()));
    require(prune_runs.exit_code == 0, "Expected `omnix tze prune` to succeed.");
    require(prune_runs.output.find("Verdict: tze_pruned") != std::string::npos,
            "Expected TZE prune output to report the prune verdict.");

    const CommandCapture prune_memory = run_command_capture(
        shell_quote(binary.string()) + " memory prune --keep 1 --memory-root " + shell_quote(memory_root.string()));
    require(prune_memory.exit_code == 0, "Expected `omnix memory prune` to succeed.");
    require(prune_memory.output.find("Verdict: memory_pruned") != std::string::npos,
            "Expected memory prune output to report the prune verdict.");
}

void test_cli_ask_build_nmap_uses_alias_flow() {
    const std::filesystem::path root = kBinaryRoot / "cli-nmap";
    const std::filesystem::path bin_dir = root / "bin";
    const std::filesystem::path memory_root = root / "home";
    const std::filesystem::path search_root = root / "search";
    safe_remove_all(root);
    std::filesystem::create_directories(bin_dir);
    std::filesystem::create_directories(memory_root);
    std::filesystem::create_directories(search_root);

    make_executable(bin_dir / "git",
                    "#!/bin/sh\n"
                    "if [ \"$1\" = \"--version\" ]; then echo 'git version test'; exit 0; fi\n"
                    "if [ \"$1\" = \"clone\" ]; then\n"
                    "  for arg in \"$@\"; do target=\"$arg\"; done\n"
                    "  /bin/mkdir -p \"$target\"\n"
                    "  printf '#!/bin/sh\\nexit 0\\n' > \"$target/configure\"\n"
                    "  /bin/chmod +x \"$target/configure\"\n"
                    "  exit 0\n"
                    "fi\n"
                    "exit 0\n");
    make_executable(bin_dir / "make",
                    "#!/bin/sh\n"
                    "if [ \"$1\" = \"--version\" ] || [ \"$1\" = \"-v\" ]; then echo 'GNU Make test'; exit 0; fi\n"
                    ": > nmap\n"
                    "exit 0\n");
    make_executable(bin_dir / "gcc", "#!/bin/sh\nif [ \"$1\" = \"--version\" ]; then echo 'gcc test'; exit 0; fi\nexit 0\n");
    make_executable(bin_dir / "g++", "#!/bin/sh\nif [ \"$1\" = \"--version\" ]; then echo 'g++ test'; exit 0; fi\nexit 0\n");

    const std::filesystem::path binary = kBinaryRoot / "omnix";
    const std::string command =
        "PATH=" + shell_quote(bin_dir.string()) + " " +
        "OMNIX_HOME=" + shell_quote(memory_root.string()) + " " +
        "OMNIX_NATIVE_SEARCH_ROOTS=" + shell_quote(search_root.string()) + " " +
        "OMNIX_NATIVE_HUNT_TIMEOUT_SECONDS='1' " +
        shell_quote(binary.string()) + " ask " + shell_quote("Build NMAP") +
        " --memory-root " + shell_quote(memory_root.string()) +
        " --source-map " + shell_quote((kSourceRoot / "res" / "tze.txt").string());

    const CommandCapture ask = run_command_capture(command);
    require(ask.exit_code == 0, "Expected `omnix ask \"Build NMAP\"` to succeed in the fake toolchain.");
    require(ask.output.find("Project: nmap") != std::string::npos,
            "Expected alias-based build output to resolve the nmap project.");
    require(ask.output.find("Preflight: ready") != std::string::npos,
            "Expected alias-based build output to include preflight details.");
    require(ask.output.find("Recipe: nmap-configure") != std::string::npos,
            "Expected alias-based build output to report the selected recipe.");
    require(ask.output.find("Build status: installed") != std::string::npos ||
                ask.output.find("Build status: built") != std::string::npos,
            "Expected alias-based build output to report a successful portable build.");
    require(ask.output.find("Acquisition: acquired") != std::string::npos ||
                ask.output.find("Acquisition: reused_workspace") != std::string::npos,
            "Expected alias-based build output to mention acquisition or workspace reuse.");

    const CommandCapture history = run_command_capture(
        "PATH=" + shell_quote(bin_dir.string()) + " " +
        "OMNIX_HOME=" + shell_quote(memory_root.string()) + " " +
        "OMNIX_NATIVE_SEARCH_ROOTS=" + shell_quote(search_root.string()) + " " +
        shell_quote(binary.string()) + " memory history --memory-root " + shell_quote(memory_root.string()));
    require(history.output.find("Build NMAP") != std::string::npos,
            "Expected CLI history view to include the prior build prompt.");
}

void test_cli_ask_build_tshark_uses_alias_flow() {
    const std::filesystem::path root = kBinaryRoot / "cli-tshark";
    const std::filesystem::path bin_dir = root / "bin";
    const std::filesystem::path memory_root = root / "home";
    const std::filesystem::path search_root = root / "search";
    safe_remove_all(root);
    std::filesystem::create_directories(bin_dir);
    std::filesystem::create_directories(memory_root);
    std::filesystem::create_directories(search_root);

    make_executable(bin_dir / "tshark",
                    "#!/bin/sh\n"
                    "if [ \"$1\" = \"--version\" ] || [ \"$1\" = \"-v\" ]; then echo 'TShark cli'; exit 0; fi\n"
                    "echo 'tshark cli'\n");

    const std::filesystem::path binary = kBinaryRoot / "omnix";
    const std::string command =
        "PATH=" + shell_quote(bin_dir.string()) + " " +
        "OMNIX_HOME=" + shell_quote(memory_root.string()) + " " +
        "OMNIX_NATIVE_SEARCH_ROOTS=" + shell_quote(search_root.string()) + " " +
        "OMNIX_NATIVE_HUNT_TIMEOUT_SECONDS='1' " +
        shell_quote(binary.string()) + " ask " + shell_quote("Build TShark") +
        " --memory-root " + shell_quote(memory_root.string()) +
        " --source-map " + shell_quote((kSourceRoot / "res" / "tze.txt").string());

    const CommandCapture ask = run_command_capture(command);
    require(ask.exit_code == 0, "Expected `omnix ask \"Build TShark\"` to succeed when native tshark is available.");
    require(ask.output.find("Project: tshark") != std::string::npos,
            "Expected alias-based build output to resolve the tshark project.");
    require(ask.output.find("Verdict: native_ready") != std::string::npos,
            "Expected alias-based tshark build output to prefer the native provider.");
    require(ask.output.find("Produced artifact:") != std::string::npos,
            "Expected alias-based tshark build output to report the native artifact path.");

    const CommandCapture history = run_command_capture(
        "PATH=" + shell_quote(bin_dir.string()) + " " +
        "OMNIX_HOME=" + shell_quote(memory_root.string()) + " " +
        "OMNIX_NATIVE_SEARCH_ROOTS=" + shell_quote(search_root.string()) + " " +
        shell_quote(binary.string()) + " memory history --memory-root " + shell_quote(memory_root.string()));
    require(history.output.find("Build TShark") != std::string::npos,
            "Expected CLI history view to include the prior tshark build prompt.");
}

void test_cli_tool_namespace_commands() {
    const std::filesystem::path root = kBinaryRoot / "cli-native-tools";
    const std::filesystem::path bin_dir = root / "bin";
    const std::filesystem::path memory_root = root / "memory";
    const std::filesystem::path search_root = root / "search";
    const std::filesystem::path sample_dir = root / "sample";
    const std::filesystem::path host_root = root / "host-root";
    safe_remove_all(root);
    std::filesystem::create_directories(bin_dir);
    std::filesystem::create_directories(search_root);
    std::filesystem::create_directories(sample_dir);
    std::filesystem::create_directories(host_root / "etc" / "sudoers.d");
    std::filesystem::create_directories(host_root / "etc" / "pacman.d");
    std::filesystem::create_directories(host_root / "var" / "log");
    std::filesystem::create_directories(host_root / "etc" / "systemd" / "system");
    std::filesystem::create_directories(host_root / "var" / "spool" / "cron");
    std::filesystem::create_directories(host_root / "boot");

    make_executable(bin_dir / "grep",
                    "#!/bin/sh\n"
                    "if [ \"$1\" = \"--version\" ]; then echo 'grep cli 1.0'; exit 0; fi\n"
                    "exec /usr/bin/grep \"$@\"\n");
    make_executable(bin_dir / "ssh",
                    "#!/bin/sh\n"
                    "if [ \"$1\" = \"-V\" ] || [ \"$1\" = \"--version\" ]; then echo 'OpenSSH_cli'; exit 0; fi\n"
                    "echo 'ssh cli'\n");
    make_executable(bin_dir / "apt-get",
                    "#!/bin/sh\n"
                    "echo '/usr/bin/apt-get'\n");
    make_executable(bin_dir / "systemctl",
                    "#!/bin/sh\n"
                    "printf 'sshd.service enabled\\ncron.service enabled\\n'\n");
    make_executable(bin_dir / "lastlog",
                    "#!/bin/sh\n"
                    "printf 'Username Port From Latest\\nroot pts/0 10.0.0.5 Tue Mar 31 10:00:00 -0400 2026\\n'\n");
    make_executable(search_root / "busybox",
                    "#!/bin/sh\n"
                    "if [ \"$1\" = \"--version\" ]; then echo 'BusyBox cli'; exit 0; fi\n"
                    "if [ \"$1\" = \"--list\" ]; then printf 'grep\\nsed\\nawk\\n'; exit 0; fi\n"
                    "applet=\"$1\"\n"
                    "shift\n"
                    "case \"$applet\" in\n"
                    "  grep) exec /usr/bin/grep \"$@\" ;;\n"
                    "  sed) exec /usr/bin/sed \"$@\" ;;\n"
                    "  awk) exec /usr/bin/awk \"$@\" ;;\n"
                    "esac\n"
                    "exit 1\n");
    make_executable(search_root / "nmap",
                    "#!/bin/sh\n"
                    "if [ \"$1\" = \"-V\" ] || [ \"$1\" = \"--version\" ]; then echo 'Nmap cli 7.95'; exit 0; fi\n"
                    "echo 'nmap cli'\n");
    make_executable(search_root / "systemctl",
                    "#!/bin/sh\n"
                    "echo \"fake systemctl $@\"\n"
                    "exit 0\n");

    {
        std::ofstream sample(sample_dir / "notes.txt");
        sample << "alpha Build beta\n";
        sample << "gamma delta\n";
    }
    {
        std::ofstream build_log(sample_dir / "build.log");
        build_log << "g++ -c main.cpp\n";
        build_log << "warning: build drift detected\n";
        build_log << "error: undefined reference to `symbol`\n";
    }
    {
        std::ofstream auth_log(sample_dir / "auth.log");
        auth_log << "sshd[1]: Failed password for invalid user alice from 10.0.0.5 port 22 ssh2\n";
    }
    std::filesystem::create_directories(sample_dir / "project");
    {
        std::ofstream cmake(sample_dir / "project" / "CMakeLists.txt");
        cmake << "cmake_minimum_required(VERSION 3.16)\n";
        cmake << "project(SmokeTool LANGUAGES CXX)\n";
        cmake << "add_executable(smoke main.cpp)\n";
    }
    {
        std::ofstream main_cpp(sample_dir / "project" / "main.cpp");
        main_cpp << "int main(){return 0;}\n";
    }
    {
        std::ofstream passwd(host_root / "etc" / "passwd");
        passwd << "root:x:0:0:root:/root:/bin/bash\n";
        passwd << "analyst:x:1000:1000:Analyst:/home/analyst:/bin/zsh\n";
    }
    {
        std::ofstream sudoers(host_root / "etc" / "sudoers");
        sudoers << "%wheel ALL=(ALL) ALL\n";
    }
    {
        std::ofstream sudoers_extra(host_root / "etc" / "sudoers.d" / "analyst");
        sudoers_extra << "analyst ALL=(ALL) NOPASSWD: /usr/bin/systemctl\n";
    }
    {
        std::ofstream mirrors(host_root / "etc" / "pacman.d" / "mirrorlist");
        mirrors << "Server = https://mirror.example.org/$repo/os/$arch\n";
    }
    {
        std::ofstream auth(host_root / "var" / "log" / "auth.log");
        auth << "sshd[1]: Failed password for invalid user alice from 10.0.0.5 port 22 ssh2\n";
        auth << "sudo: analyst : command not allowed\n";
    }
    {
        std::ofstream lastlog(host_root / "var" / "log" / "lastlog");
        lastlog << "fake";
    }
    {
        std::ofstream crontab(host_root / "etc" / "crontab");
        crontab << "0 * * * * root /usr/local/bin/nightly\n";
    }
    {
        std::ofstream user_cron(host_root / "var" / "spool" / "cron" / "analyst");
        user_cron << "15 2 * * * /usr/bin/true\n";
    }
    {
        std::ofstream service(host_root / "etc" / "systemd" / "system" / "sshd.service");
        service << "[Service]\nExecStart=/usr/sbin/sshd -D\n";
    }
    {
        std::ofstream initrd(host_root / "boot" / "initramfs-linux.img");
        initrd << "fake";
    }

    const std::filesystem::path binary = kBinaryRoot / "omnix";
    const std::string command_prefix =
        "PATH=" + shell_quote(bin_dir.string()) + " " +
        "OMNIX_NATIVE_SEARCH_ROOTS=" + shell_quote(search_root.string()) + " " +
        "OMNIX_HOST_INSPECT_ROOT=" + shell_quote(host_root.string()) + " " +
        "OMNIX_HOST_INSPECT_PLATFORM='linux' " +
        "OMNIX_NATIVE_HUNT_TIMEOUT_SECONDS='1' " +
        shell_quote(binary.string());

    const CommandCapture locate = run_command_capture(
        command_prefix + " tool locate grep --memory-root " + shell_quote(memory_root.string()));
    require(locate.exit_code == 0, "Expected `tool locate grep` to succeed.");
    require(locate.output.find("Tool resolution: found") != std::string::npos,
            "Expected locate output to show a found tool resolution.");

    const CommandCapture locate_mlp = run_command_capture(
        command_prefix + " tool locate mlp-lens --memory-root " + shell_quote(memory_root.string()));
    require(locate_mlp.exit_code == 0, "Expected `tool locate mlp-lens` to resolve the built-in module.");
    require(locate_mlp.output.find("Tool: mlp-lens") != std::string::npos,
            "Expected mlp-lens locate output to name the built-in tool.");
    require(locate_mlp.output.find("Provider: analyst_module") != std::string::npos,
            "Expected mlp-lens to resolve as an analyst module.");

    const CommandCapture doctor_mlp = run_command_capture(
        command_prefix + " tool doctor mlp-lens --memory-root " + shell_quote(memory_root.string()));
    require(doctor_mlp.exit_code == 0, "Expected `tool doctor mlp-lens` to succeed.");
    require(doctor_mlp.output.find("Tool doctor: builtin_ready") != std::string::npos,
            "Expected mlp-lens doctor output to report builtin readiness.");
    require(doctor_mlp.output.find("demo weights") != std::string::npos,
            "Expected mlp-lens doctor output to call out educational demo weights.");

    const CommandCapture mlp = run_command_capture(
        command_prefix + " tool mlp-lens --compact --memory-root " + shell_quote(memory_root.string()) +
        " -- " + shell_quote("Michael Jordan plays basketball"));
    require(mlp.exit_code == 0, "Expected `tool mlp-lens` to succeed.");
    require(mlp.output.rfind("{\"tool\":\"mlp-lens\"", 0) == 0,
            "Expected compact mlp-lens output to be raw JSON.");
    require(mlp.output.find("\"inputVector\"") != std::string::npos,
            "Expected mlp-lens JSON to include the input vector.");
    require(mlp.output.find("\"z1PreActivations\"") != std::string::npos,
            "Expected mlp-lens JSON to include pre-activations.");
    require(mlp.output.find("\"hiddenActivations\"") != std::string::npos,
            "Expected mlp-lens JSON to include hidden activations.");
    require(mlp.output.find("\"outputVector\"") != std::string::npos,
            "Expected mlp-lens JSON to include the output vector.");
    require(mlp.output.find("\"softmaxProbabilities\"") != std::string::npos,
            "Expected mlp-lens JSON to include softmax probabilities.");
    require(mlp.output.find("\"topActivatedNeurons\"") != std::string::npos,
            "Expected mlp-lens JSON to include top activated neurons.");
    require(mlp.output.find("not a real LLM") != std::string::npos,
            "Expected mlp-lens output to avoid real-LLM claims.");

    const CommandCapture mlp_verbose = run_command_capture(
        command_prefix + " tool mlp-lens --verbose --memory-root " + shell_quote(memory_root.string()) +
        " -- " + shell_quote("hello"));
    require(mlp_verbose.exit_code == 0, "Expected verbose `tool mlp-lens` to succeed.");
    require(mlp_verbose.output.find("\"trace\"") != std::string::npos,
            "Expected verbose mlp-lens output to include trace fields.");
    require(mlp_verbose.output.find("\"matrixShapes\"") != std::string::npos,
            "Expected verbose mlp-lens output to include matrix shapes.");

    const std::filesystem::path tensor_bundle = kSourceRoot / "res" / "mlp_lens" / "tiny_mlp_bundle.json";
    const CommandCapture mlp_loaded = run_command_capture(
        command_prefix + " tool mlp-lens --compact --memory-root " + shell_quote(memory_root.string()) +
        " -- --tensor-bundle " + shell_quote(tensor_bundle.string()) + " " +
        shell_quote("Michael Jordan plays basketball"));
    require(mlp_loaded.exit_code == 0, "Expected loaded tensor bundle `tool mlp-lens` to succeed.");
    require(mlp_loaded.output.rfind("{\"tool\":\"mlp-lens\"", 0) == 0,
            "Expected compact loaded tensor bundle output to be raw JSON.");
    require(mlp_loaded.output.find("\"modelSource\":\"loaded_tensor_bundle\"") != std::string::npos,
            "Expected loaded tensor bundle output to identify loaded tensor mode.");
    require(mlp_loaded.output.find("\"tensorBundlePath\"") != std::string::npos,
            "Expected loaded tensor bundle output to include the tensor bundle path.");
    require(mlp_loaded.output.find("\"tokenizerSource\"") != std::string::npos,
            "Expected loaded tensor bundle output to include tokenizer source.");
    require(mlp_loaded.output.find("\"tokens\"") != std::string::npos,
            "Expected loaded tensor bundle output to include tokens.");
    require(mlp_loaded.output.find("\"tokenIds\"") != std::string::npos,
            "Expected loaded tensor bundle output to include token ids.");
    require(mlp_loaded.output.find("\"embeddingVectors\"") != std::string::npos,
            "Expected loaded tensor bundle output to include loaded embedding vectors.");
    require(mlp_loaded.output.find("\"z1PreActivations\"") != std::string::npos,
            "Expected loaded tensor bundle output to include pre-activations.");
    require(mlp_loaded.output.find("\"hiddenActivations\"") != std::string::npos,
            "Expected loaded tensor bundle output to include hidden activations.");
    require(mlp_loaded.output.find("\"outputVector\"") != std::string::npos,
            "Expected loaded tensor bundle output to include the output vector.");
    require(mlp_loaded.output.find("\"softmaxProbabilities\"") != std::string::npos,
            "Expected loaded tensor bundle output to include softmax probabilities.");
    require(mlp_loaded.output.find("not a full LLM") != std::string::npos,
            "Expected loaded tensor bundle output to avoid full-LLM claims.");

    const CommandCapture mlp_loaded_verbose = run_command_capture(
        command_prefix + " tool mlp-lens --verbose --memory-root " + shell_quote(memory_root.string()) +
        " -- --tensor-bundle " + shell_quote(tensor_bundle.string()) + " " + shell_quote("blorple"));
    require(mlp_loaded_verbose.exit_code == 0,
            "Expected verbose loaded tensor bundle `tool mlp-lens` to handle unknown tokens.");
    require(mlp_loaded_verbose.output.find("\"trace\"") != std::string::npos,
            "Expected verbose loaded tensor bundle output to include trace fields.");
    require(mlp_loaded_verbose.output.find("\"tokenizer\"") != std::string::npos,
            "Expected verbose loaded tensor bundle output to include tokenizer metadata.");
    require(mlp_loaded_verbose.output.find("\"layer\"") != std::string::npos,
            "Expected verbose loaded tensor bundle output to include layer metadata.");
    require(mlp_loaded_verbose.output.find("\"tokenIds\":[0]") != std::string::npos,
            "Expected unknown tokens to route through the <unk> token id.");

    const CommandCapture mlp_missing_bundle = run_command_capture(
        command_prefix + " tool mlp-lens --compact --memory-root " + shell_quote(memory_root.string()) +
        " -- --tensor-bundle " + shell_quote((root / "missing-bundle.json").string()) + " " + shell_quote("hello"));
    require(mlp_missing_bundle.exit_code != 0, "Expected missing tensor bundle to fail.");
    require(mlp_missing_bundle.output.find("tensor_bundle_invalid") != std::string::npos,
            "Expected missing tensor bundle to return tensor_bundle_invalid.");

    const std::filesystem::path invalid_bundle = root / "bad-mlp-bundle.json";
    {
        std::ofstream out(invalid_bundle);
        out << "{\"format\":\"omnix.mlp_lens.tensor_bundle.v1\",\"modelName\":\"bad\","
            << "\"modelSource\":\"test\",\"tokenizer\":{\"source\":\"test\",\"unknownToken\":\"<unk>\","
            << "\"vocabulary\":[\"<unk>\"],\"embeddings\":[[0.1,0.2]]},"
            << "\"mlp\":{\"activation\":\"gelu\",\"W1\":[[0.1]],\"b1\":[0.0],"
            << "\"W2\":[[0.1]],\"b2\":[0.0]},\"outputLabels\":[\"bad\"]}";
    }
    const CommandCapture mlp_bad_bundle = run_command_capture(
        command_prefix + " tool mlp-lens --compact --memory-root " + shell_quote(memory_root.string()) +
        " -- --tensor-bundle " + shell_quote(invalid_bundle.string()) + " " + shell_quote("hello"));
    require(mlp_bad_bundle.exit_code != 0, "Expected invalid tensor dimensions to fail.");
    require(mlp_bad_bundle.output.find("tensor_bundle_invalid") != std::string::npos,
            "Expected invalid tensor dimensions to return tensor_bundle_invalid.");
    require(mlp_bad_bundle.output.find("\"modelSource\":\"demo_weights\"") == std::string::npos,
            "Expected invalid tensor bundle to avoid silent demo-weight fallback.");

    const CommandCapture mlp_empty = run_command_capture(
        command_prefix + " tool mlp-lens --compact --memory-root " + shell_quote(memory_root.string()) + " --");
    require(mlp_empty.exit_code != 0, "Expected empty `tool mlp-lens` input to fail.");
    require(mlp_empty.output.find("invalid_arguments") != std::string::npos,
            "Expected empty mlp-lens input to return invalid_arguments.");

    const CommandCapture locate_tensor = run_command_capture(
        command_prefix + " tool locate tensor --memory-root " + shell_quote(memory_root.string()));
    require(locate_tensor.exit_code == 0, "Expected `tool locate tensor` to resolve the built-in module.");
    require(locate_tensor.output.find("Tool: tensor") != std::string::npos,
            "Expected tensor locate output to name the built-in tool.");
    require(locate_tensor.output.find("Provider: analyst_module") != std::string::npos,
            "Expected tensor to resolve as an analyst module.");

    const CommandCapture doctor_tensor = run_command_capture(
        command_prefix + " tool doctor tensor --memory-root " + shell_quote(memory_root.string()));
    require(doctor_tensor.exit_code == 0, "Expected `tool doctor tensor` to succeed.");
    require(doctor_tensor.output.find("Native tensor literacy") != std::string::npos,
            "Expected tensor doctor output to describe local tensor literacy.");

    const CommandCapture tensor_inspect = run_command_capture(
        command_prefix + " tensor inspect " + shell_quote(tensor_bundle.string()) +
        " --compact --memory-root " + shell_quote(memory_root.string()));
    require(tensor_inspect.exit_code == 0, "Expected top-level `tensor inspect` to succeed.");
    require(tensor_inspect.output.rfind("{\"tool\":\"tensor\"", 0) == 0,
            "Expected compact tensor inspect output to be raw JSON.");
    require(tensor_inspect.output.find("\"valid\":true") != std::string::npos,
            "Expected tensor inspect output to report a valid bundle.");
    require(tensor_inspect.output.find("\"shapes\"") != std::string::npos,
            "Expected tensor inspect output to include tensor shapes.");
    const CommandCapture tensor_inspect_verbose = run_command_capture(
        command_prefix + " tensor inspect " + shell_quote(tensor_bundle.string()) +
        " --verbose --memory-root " + shell_quote(memory_root.string()));
    require(tensor_inspect_verbose.exit_code == 0, "Expected verbose top-level `tensor inspect` to succeed.");
    require(tensor_inspect_verbose.output.find("Intent: tensor_action") != std::string::npos,
            "Expected top-level tensor command to persist as a typed tensor route.");
    require(tensor_inspect_verbose.output.find("x.Tensor.Framework") != std::string::npos,
            "Expected verbose tensor output to include the tensor TZE stage.");

    const std::string shell_tensor_command =
        "printf '%s\\n' " + shell_quote("tensor inspect " + tensor_bundle.string() + " --compact") +
        " " + shell_quote("/quit") + " | " +
        command_prefix + " shell --compact --memory-root " + shell_quote(memory_root.string());
    const CommandCapture tensor_shell = run_command_capture(shell_tensor_command);
    require(tensor_shell.exit_code == 0, "Expected shell tensor command to exit cleanly.");
    require(tensor_shell.output.find("\"tool\":\"tensor\"") != std::string::npos,
            "Expected shell to route `tensor inspect` through the native tensor tool.");
    require(tensor_shell.output.find("unknown_intent") == std::string::npos,
            "Expected shell tensor command not to fall into unknown_intent.");

    const CommandCapture tensor_validate = run_command_capture(
        command_prefix + " tensor validate " + shell_quote(tensor_bundle.string()) +
        " --compact --memory-root " + shell_quote(memory_root.string()));
    require(tensor_validate.exit_code == 0, "Expected top-level `tensor validate` to succeed.");
    require(tensor_validate.output.find("\"valid\":true") != std::string::npos,
            "Expected tensor validate output to report a valid bundle.");

    const CommandCapture tensor_run = run_command_capture(
        command_prefix + " tensor run mlp " + shell_quote(tensor_bundle.string()) +
        " --input " + shell_quote("server secure") + " --compact --memory-root " +
        shell_quote(memory_root.string()));
    require(tensor_run.exit_code == 0, "Expected top-level `tensor run mlp` to succeed.");
    require(tensor_run.output.find("\"tensorMode\":\"run_mlp\"") != std::string::npos,
            "Expected tensor run output to identify run_mlp mode.");
    require(tensor_run.output.find("\"softmaxProbabilities\"") != std::string::npos,
            "Expected tensor run output to include MLP softmax probabilities.");

    const std::filesystem::path tensor_answer_fixture = root / "tensor-answer.txt";
    const std::filesystem::path tensor_training_log = root / "tensor-training.jsonl";
    {
        std::ofstream out(tensor_answer_fixture);
        out << "Local tensor literacy means OmniX can validate and trace tensors before relying on larger model formats.";
    }
    const CommandCapture tensor_ask = run_command_capture(
        command_prefix + " tensor ask --model citizen-ai:local --profile citizen-ai --kb " +
        shell_quote((kSourceRoot / "res" / "local_glossary.tsv").string()) +
        " --fixture-answer " + shell_quote(tensor_answer_fixture.string()) +
        " --training-log " + shell_quote(tensor_training_log.string()) +
        " " + shell_quote("What is local tensor literacy?") +
        " --compact --memory-root " + shell_quote(memory_root.string()));
    require(tensor_ask.exit_code == 0, "Expected top-level `tensor ask` to succeed with fixture-backed local model answer.");
    require(tensor_ask.output.find("\"profile\":\"citizen-ai\"") != std::string::npos,
            "Expected tensor ask output to identify Citizen-AI profile.");
    require(tensor_ask.output.find("\"answerSource\":\"fixture_local_model\"") != std::string::npos,
            "Expected tensor ask output to use the fixture local model answer.");
    require(tensor_ask.output.find("\"knowledgeSources\"") != std::string::npos,
            "Expected tensor ask output to include local knowledge sources.");
    require(std::filesystem::exists(tensor_training_log), "Expected tensor ask to write a supervised training capture.");
    require(xpp::read_text_file(tensor_training_log).find("omnix.tensor.training_example.v1") != std::string::npos,
            "Expected tensor training log to include its event type.");

    const CommandCapture tensor_ask_kb_only = run_command_capture(
        command_prefix + " tensor ask --model deepnimsec-omni:latest --kb " +
        shell_quote((kSourceRoot / "res" / "local_glossary.tsv").string()) +
        " " + shell_quote("What is local tensor literacy?") +
        " --compact --memory-root " + shell_quote(memory_root.string()));
    require(tensor_ask_kb_only.exit_code == 0,
            "Expected tensor ask to synthesize an evidence-bound answer from local KB context.");
    require(tensor_ask_kb_only.output.find("\"answerSource\":\"local_kb_synthesized\"") != std::string::npos,
            "Expected tensor ask without a model answer to identify local KB synthesis.");
    require(tensor_ask_kb_only.output.find("load, validate, inspect") != std::string::npos,
            "Expected local tensor literacy answer to come from the glossary hit.");

    const CommandCapture tensor_bad = run_command_capture(
        command_prefix + " tensor validate " + shell_quote(invalid_bundle.string()) +
        " --compact --memory-root " + shell_quote(memory_root.string()));
    require(tensor_bad.exit_code != 0, "Expected invalid tensor bundle validation to fail.");
    require(tensor_bad.output.find("tensor_bundle_invalid") != std::string::npos,
            "Expected invalid tensor validation to return tensor_bundle_invalid.");

    const std::filesystem::path ghostline_audit_fixture =
        kSourceRoot / "res" / "ghostline" / "ghostline_audit_fixture.jsonl";
    const std::filesystem::path ghostline_actions_fixture =
        kSourceRoot / "res" / "ghostline" / "ghostline_actions_fixture.jsonl";
    const std::filesystem::path gg_tview_out = root / "gg-tview.jsonl";

    const CommandCapture locate_gg = run_command_capture(
        command_prefix + " tool locate gg --memory-root " + shell_quote(memory_root.string()));
    require(locate_gg.exit_code == 0, "Expected `tool locate gg` to resolve the built-in module.");
    require(locate_gg.output.find("Tool: gg") != std::string::npos,
            "Expected gg locate output to name the built-in tool.");
    require(locate_gg.output.find("Provider: analyst_module") != std::string::npos,
            "Expected gg to resolve as an analyst module.");

    const CommandCapture doctor_gg = run_command_capture(
        command_prefix + " gg doctor --compact --memory-root " + shell_quote(memory_root.string()));
    require(doctor_gg.exit_code == 0, "Expected top-level `gg doctor` to succeed.");
    require(doctor_gg.output.find("Ghostline Gate") != std::string::npos,
            "Expected gg doctor to describe Ghostline Gate.");

    const CommandCapture tool_doctor_gg = run_command_capture(
        command_prefix + " tool doctor gg --memory-root " + shell_quote(memory_root.string()));
    require(tool_doctor_gg.exit_code == 0, "Expected `tool doctor gg` to succeed.");
    require(tool_doctor_gg.output.find("Safe Original Priority") != std::string::npos,
            "Expected gg tool doctor to preserve Ghostline safety wording.");

    const CommandCapture gg_search = run_command_capture(
        command_prefix + " gg search --port 1883 --listen-only --compact --memory-root " +
        shell_quote(memory_root.string()));
    require(gg_search.exit_code == 0, "Expected `gg search --port 1883 --listen-only` to invoke Ghostline search safely.");
    require(gg_search.output.find("Executed Ghostline search command") != std::string::npos ||
                gg_search.output.find("No TCP PID matches found") != std::string::npos,
            "Expected gg search output to show Ghostline search execution.");

    const CommandCapture gg_audit = run_command_capture(
        command_prefix + " gg audit " + shell_quote(ghostline_audit_fixture.string()) +
        " --out " + shell_quote(gg_tview_out.string()) +
        " --compact --memory-root " + shell_quote(memory_root.string()));
    require(gg_audit.exit_code == 0, "Expected `gg audit` to convert Ghostline audit JSONL.");
    require(gg_audit.output.find("gg_audit_converted") != std::string::npos,
            "Expected gg audit output to report conversion.");
    require(std::filesystem::exists(gg_tview_out), "Expected gg audit bridge to write a TView JSONL artifact.");
    {
        const std::string converted = xpp::read_text_file(gg_tview_out);
        require(converted.find("\"event_type\":\"omnix.tview.packet.v1\"") != std::string::npos,
                "Expected converted Ghostline evidence to use TView event type.");
        require(converted.find("NET.GHOSTLINE.MODIFIED_RELEASED") != std::string::npos,
                "Expected converted Ghostline evidence to include modified-release code.");
        require(converted.find("NET.GHOSTLINE.FALLBACK_ORIGINAL") != std::string::npos,
                "Expected converted Ghostline evidence to include fallback-original code.");
    }

    const CommandCapture gg_actions = run_command_capture(
        command_prefix + " gg actions " + shell_quote(ghostline_actions_fixture.string()) +
        " --compact --memory-root " + shell_quote(memory_root.string()));
    require(gg_actions.exit_code == 0, "Expected `gg actions` to summarize Ghostline actions JSONL.");
    require(gg_actions.output.find("\"event_type\":\"omnix.gg.actions.v1\"") != std::string::npos,
            "Expected gg actions output to use its summary event type.");

    const CommandCapture gg_route = run_command_capture(
        command_prefix + " nn route tview " + shell_quote(gg_tview_out.string()) +
        " --compact --memory-root " + shell_quote(memory_root.string()));
    require(gg_route.exit_code == 0, "Expected converted Ghostline TView JSONL to route through neural signal router.");
    require(gg_route.output.find("neural_route_complete") != std::string::npos,
            "Expected neural router to complete on Ghostline-converted evidence.");

    const std::filesystem::path symlink_target = root / "symlink-target.txt";
    const std::filesystem::path symlink_path = root / "links" / "target-link.txt";
    {
        std::ofstream out(symlink_target);
        out << "linked-data\n";
    }
    const CommandCapture locate_symlink = run_command_capture(
        command_prefix + " tool locate symlink --memory-root " + shell_quote(memory_root.string()));
    require(locate_symlink.exit_code == 0, "Expected `tool locate symlink` to resolve the built-in module.");
    require(locate_symlink.output.find("Tool: symlink") != std::string::npos,
            "Expected symlink locate output to name the built-in tool.");

    const CommandCapture doctor_symlink = run_command_capture(
        command_prefix + " tool doctor symlink --memory-root " + shell_quote(memory_root.string()));
    require(doctor_symlink.exit_code == 0, "Expected `tool doctor symlink` to succeed.");
    require(doctor_symlink.output.find("namespace-shim") != std::string::npos ||
                doctor_symlink.output.find("namespace shims") != std::string::npos,
            "Expected symlink doctor to describe namespace shim creation.");

    const CommandCapture create_symlink = run_command_capture(
        command_prefix + " tool symlink --compact --memory-root " + shell_quote(memory_root.string()) +
        " -- create " + shell_quote(symlink_target.string()) + " " + shell_quote(symlink_path.string()));
    require(create_symlink.exit_code == 0, "Expected `tool symlink create` to create a filesystem symlink.");
    require(std::filesystem::is_symlink(symlink_path), "Expected symlink tool to create a real symlink.");
    require(xpp::read_text_file(symlink_path).find("linked-data") != std::string::npos,
            "Expected symlink to point at the requested target file.");

    const std::filesystem::path shim_prefix = root / "shim-bin";
    const std::filesystem::path shim_path = shim_prefix / "tze-test";
    const CommandCapture create_shim = run_command_capture(
        command_prefix + " tool symlink --compact --memory-root " + shell_quote(memory_root.string()) +
        " -- shim tze-test tze --prefix " + shell_quote(shim_prefix.string()) +
        " --bin " + shell_quote(binary.string()));
    require(create_shim.exit_code == 0, "Expected `tool symlink shim` to write a POSIX shim.");
    require(std::filesystem::exists(shim_path), "Expected symlink shim tool to write the shim file.");
    require(xpp::read_text_file(shim_path).find("exec ") != std::string::npos,
            "Expected generated shim to use exec for namespace dispatch.");
    const CommandCapture run_shim = run_command_capture(shell_quote(shim_path.string()) + " latest --compact --memory-root " +
                                                        shell_quote(memory_root.string()));
    require(run_shim.exit_code == 0, "Expected generated TZE shim to execute through OmniX.");
    require(run_shim.output.find("tze_run") != std::string::npos || run_shim.output.find("No TZE runs") != std::string::npos,
            "Expected generated TZE shim to route into the TZE command surface.");

    const std::filesystem::path thresholds_scenario = kSourceRoot / "res" / "thresholds" / "rabbitmq-xxb-incident.json";
    const std::filesystem::path thresholds_seasonal =
        kSourceRoot / "res" / "thresholds" / "rabbitmq-xxb-seasonal-tolerated.json";
    const std::filesystem::path thresholds_unknown =
        kSourceRoot / "res" / "thresholds" / "rabbitmq-xxb-unknown-error.json";
    const std::filesystem::path gsmg_rabbit =
        kSourceRoot / "res" / "thresholds" / "gsmg-rabbitmq-xxb.json";
    const std::filesystem::path gsmg_outage =
        kSourceRoot / "res" / "thresholds" / "gsmg-outage-window-db-guard.json";
    const std::filesystem::path evidence_out = root / "threshold-evidence.json";
    const std::filesystem::path jira_out = root / "threshold-escalation.md";
    const std::filesystem::path gsmg_evidence_out = root / "gsmg-evidence.json";
    const std::filesystem::path gsmg_jira_out = root / "gsmg-escalation.md";
    const std::string thresholds_prefix =
        "OMNIX_THRESHOLDS_SYSTEMCTL=" + shell_quote((search_root / "systemctl").string()) + " " + command_prefix;

    const CommandCapture locate_thresholds = run_command_capture(
        command_prefix + " tool locate thresholds --memory-root " + shell_quote(memory_root.string()));
    require(locate_thresholds.exit_code == 0, "Expected `tool locate thresholds` to resolve the built-in module.");
    require(locate_thresholds.output.find("Tool: thresholds") != std::string::npos,
            "Expected thresholds locate output to name the built-in tool.");

    const CommandCapture doctor_thresholds = run_command_capture(
        command_prefix + " tool doctor thresholds --memory-root " + shell_quote(memory_root.string()));
    require(doctor_thresholds.exit_code == 0, "Expected `tool doctor thresholds` to succeed.");
    require(doctor_thresholds.output.find("Proactive Infrastructure Thresholds") != std::string::npos,
            "Expected thresholds doctor output to describe proactive threshold evaluation.");
    require(doctor_thresholds.output.find("Jira-ready Markdown") != std::string::npos,
            "Expected thresholds doctor output to mention Jira-ready Markdown output.");
    require(doctor_thresholds.output.find("GSMg mode") != std::string::npos,
            "Expected thresholds doctor output to mention generic signal mode.");

    const CommandCapture thresholds = run_command_capture(
        command_prefix + " tool thresholds --compact --memory-root " + shell_quote(memory_root.string()) +
        " -- evaluate " + shell_quote(thresholds_scenario.string()) +
        " --out " + shell_quote(evidence_out.string()) +
        " --jira-out " + shell_quote(jira_out.string()));
    require(thresholds.exit_code == 0, "Expected thresholds evaluation to succeed.");
    require(thresholds.output.find("thresholds_evaluated") != std::string::npos,
            "Expected thresholds output to report evaluation.");
    require(thresholds.output.find("app_worker_memory_exhaustion") != std::string::npos,
            "Expected Queue XXB fixture to identify app-worker memory exhaustion.");
    require(thresholds.output.find("recommend_runbook") != std::string::npos,
            "Expected Queue XXB fixture to recommend the runbook.");
    require(std::filesystem::exists(evidence_out), "Expected thresholds evidence JSON artifact to be written.");
    require(std::filesystem::exists(jira_out), "Expected thresholds Jira Markdown artifact to be written.");
    {
        const std::string evidence = xpp::read_text_file(evidence_out);
        require(evidence.find("\"event_type\":\"omnix.threshold.evidence.v1\"") != std::string::npos,
                "Expected threshold evidence to use the versioned event type.");
        require(evidence.find("\"matchedSignatures\":[\"EYZ-47281\"]") != std::string::npos,
                "Expected threshold evidence to include the heap error signature.");
        require(evidence.find("\"restartRecommended\":true") != std::string::npos,
                "Expected threshold evidence to recommend restart.");
    }
    {
        const std::string jira = xpp::read_text_file(jira_out);
        require(jira.find("P1-RMQ-2B-XXB-STOPPED-REPORTING") != std::string::npos,
                "Expected Jira packet to include the alarm id.");
        require(jira.find("abc-worker.service") != std::string::npos,
                "Expected Jira packet to include the service name.");
        require(jira.find("EYZ-47281") != std::string::npos,
                "Expected Jira packet to include the matched error signature.");
    }

    const CommandCapture seasonal = run_command_capture(
        command_prefix + " tool thresholds --compact --memory-root " + shell_quote(memory_root.string()) +
        " -- evaluate " + shell_quote(thresholds_seasonal.string()));
    require(seasonal.exit_code == 0, "Expected seasonal threshold fixture to evaluate.");
    require(seasonal.output.find("\"severity\":\"normal\"") != std::string::npos,
            "Expected active seasonal override to tolerate Queue XXB depth in the 200-400 range.");
    require(seasonal.output.find("\"applied\":true") != std::string::npos,
            "Expected seasonal override to be marked as applied.");

    const CommandCapture unknown = run_command_capture(
        command_prefix + " tool thresholds --compact --memory-root " + shell_quote(memory_root.string()) +
        " -- evaluate " + shell_quote(thresholds_unknown.string()));
    require(unknown.exit_code == 0, "Expected unknown threshold fixture to evaluate.");
    require(unknown.output.find("escalate_to_developer_or_infra_owner") != std::string::npos,
            "Expected unknown error signature to escalate instead of recommending remediation.");
    require(unknown.output.find("\"restartRecommended\":true") == std::string::npos,
            "Expected unknown error signature not to recommend blind restart.");

    const CommandCapture gsmg_rabbit_result = run_command_capture(
        command_prefix + " tool thresholds --compact --memory-root " + shell_quote(memory_root.string()) +
        " -- gsmg " + shell_quote(gsmg_rabbit.string()));
    require(gsmg_rabbit_result.exit_code == 0, "Expected GSMg RabbitMQ fixture to evaluate.");
    require(gsmg_rabbit_result.output.find("\"event_type\":\"omnix.threshold.gsmg.v1\"") != std::string::npos,
            "Expected GSMg output to use its versioned event type.");
    require(gsmg_rabbit_result.output.find("app_worker_memory_exhaustion") != std::string::npos,
            "Expected GSMg RabbitMQ fixture to identify app-worker memory exhaustion.");
    require(gsmg_rabbit_result.output.find("\"queue.depth\"") != std::string::npos,
            "Expected GSMg output to preserve generic queue.depth signal evidence.");
    require(gsmg_rabbit_result.output.find("\"memory.usage\"") != std::string::npos,
            "Expected GSMg output to preserve generic memory.usage signal evidence.");
    require(gsmg_rabbit_result.output.find("\"consumer.count\"") != std::string::npos,
            "Expected GSMg output to preserve generic consumer.count signal evidence.");

    const CommandCapture gsmg_outage_result = run_command_capture(
        command_prefix + " tool thresholds --compact --memory-root " + shell_quote(memory_root.string()) +
        " -- gsmg " + shell_quote(gsmg_outage.string()) +
        " --out " + shell_quote(gsmg_evidence_out.string()) +
        " --jira-out " + shell_quote(gsmg_jira_out.string()));
    require(gsmg_outage_result.exit_code == 0, "Expected GSMg outage-window fixture to evaluate.");
    require(gsmg_outage_result.output.find("block_unsafe_action") != std::string::npos,
            "Expected GSMg outage-window fixture to block unsafe DB shutdown.");
    require(gsmg_outage_result.output.find("unsafe_database_shutdown_path") != std::string::npos,
            "Expected GSMg outage-window fixture to identify unsafe shutdown path.");
    require(std::filesystem::exists(gsmg_evidence_out), "Expected GSMg evidence JSON artifact to be written.");
    require(std::filesystem::exists(gsmg_jira_out), "Expected GSMg Markdown artifact to be written.");
    {
        const std::string evidence = xpp::read_text_file(gsmg_evidence_out);
        require(evidence.find("\"event_type\":\"omnix.threshold.gsmg.v1\"") != std::string::npos,
                "Expected GSMg evidence to use the versioned event type.");
        require(evidence.find("\"connection_pool.open\"") != std::string::npos,
                "Expected GSMg evidence to include connection-pool signal.");
        require(evidence.find("\"database.active_writers\"") != std::string::npos,
                "Expected GSMg evidence to include database active-writer signal.");
    }

    const CommandCapture gsmg_execute = run_command_capture(
        command_prefix + " tool thresholds --compact --memory-root " + shell_quote(memory_root.string()) +
        " -- gsmg " + shell_quote(gsmg_rabbit.string()) + " --execute");
    require(gsmg_execute.exit_code != 0, "Expected GSMg --execute to be rejected in this phase.");
    require(gsmg_execute.output.find("action_not_allowed") != std::string::npos,
            "Expected GSMg --execute to report action_not_allowed.");

    const CommandCapture execute_declined = run_command_capture(
        thresholds_prefix + " tool thresholds --compact --memory-root " + shell_quote(memory_root.string()) +
        " -- evaluate " + shell_quote(thresholds_scenario.string()) + " --execute");
    require(execute_declined.exit_code != 0, "Expected --execute without exact confirmation not to run.");
    require(execute_declined.output.find("action_not_executed") != std::string::npos,
            "Expected declined threshold execution to report action_not_executed.");

    const std::filesystem::path execute_evidence = root / "threshold-execute-evidence.json";
    const CommandCapture execute_confirmed = run_command_capture(
        "printf 'EXECUTE restart-abc-worker\\n' | " + thresholds_prefix +
        " tool thresholds --compact --memory-root " + shell_quote(memory_root.string()) +
        " -- evaluate " + shell_quote(thresholds_scenario.string()) +
        " --execute --out " + shell_quote(execute_evidence.string()));
    require(execute_confirmed.exit_code == 0, "Expected exact confirmation to run fake systemctl.");
    require(execute_confirmed.output.find("remediation_unvalidated") != std::string::npos,
            "Expected post-execution state to remain remediation_unvalidated until validation evidence exists.");
    require(std::filesystem::exists(execute_evidence), "Expected execution evidence artifact to be written.");
    {
        const std::string evidence = xpp::read_text_file(execute_evidence);
        require(evidence.find("fake systemctl restart abc-worker.service") != std::string::npos,
                "Expected execution evidence to capture fake systemctl output.");
    }

    const std::filesystem::path vg_siem = kSourceRoot / "res" / "ops" / "elastic-siem-rabbitmq-xxb.json";
    const std::filesystem::path vg_metatag = kSourceRoot / "res" / "ops" / "elastic-siem-metatag-blob.json";
    const std::filesystem::path vg_packages = kSourceRoot / "res" / "ops" / "package-manager-activity.json";
    const std::filesystem::path vg_dependency = kSourceRoot / "res" / "ops" / "dependency-map-outage-window.json";
    const std::filesystem::path vg_recovery = kSourceRoot / "res" / "ops" / "recovery-comparison-worker-restart.json";
    const std::filesystem::path vg_eventviewer = kSourceRoot / "res" / "ops" / "windows-eventviewer-retention-low.json";
    const std::filesystem::path vg_sessions = kSourceRoot / "res" / "ops" / "syslog-lastlog-correlation.json";
    const std::filesystem::path vg_rum = kSourceRoot / "res" / "ops" / "rum-heuristic-behavior.json";
    const std::filesystem::path vg_encrypted = kSourceRoot / "res" / "ops" / "encrypted-siem-custody.json";
    const std::filesystem::path vg_verticals =
        kSourceRoot / "res" / "ops" / "verticals" / "scale-verticals-combined.json";
    const std::filesystem::path vg_k8s =
        kSourceRoot / "res" / "ops" / "verticals" / "kubernetes-docker-context.json";
    const std::filesystem::path vg_lb =
        kSourceRoot / "res" / "ops" / "verticals" / "load-balancer-front-back.json";
    const std::filesystem::path vg_network =
        kSourceRoot / "res" / "ops" / "verticals" / "network-map-scale.json";
    const std::filesystem::path vg_terraform =
        kSourceRoot / "res" / "ops" / "verticals" / "terraform-gcp-compute-plan-shape.json";
    const std::filesystem::path vg_minion =
        kSourceRoot / "res" / "ops" / "verticals" / "omnix-minion-neighbor-map.json";
    const std::filesystem::path vg_report_out = root / "vuplus-gate-report.json";
    const std::filesystem::path vg_metatag_out = root / "vuplus-gate-metatag-report.json";
    const std::filesystem::path vg_cab_out = root / "vuplus-gate-alarm-cab.json";
    const std::filesystem::path vg_shape_out = root / "vuplus-gate-shape.json";
    const std::filesystem::path vg_vertical_shape_out = root / "vuplus-gate-vertical-shape.json";
    const std::filesystem::path vg_terraform_shape_out = root / "vuplus-gate-terraform-shape.json";

    const CommandCapture vg_doctor = run_command_capture(
        command_prefix + " vg doctor --compact --memory-root " + shell_quote(memory_root.string()));
    require(vg_doctor.exit_code == 0, "Expected `vg doctor` to report readiness.");
    require(vg_doctor.output.find("Vuplus Gate") != std::string::npos,
            "Expected Vuplus Gate doctor output to name the segment.");
    require(vg_doctor.output.find("recommendation_only") != std::string::npos,
            "Expected Vuplus Gate doctor output to preserve recommendation-only mode.");

    const CommandCapture vg_explain = run_command_capture(
        command_prefix + " vg explain " + shell_quote(vg_siem.string()) +
        " --compact --out " + shell_quote(vg_report_out.string()) +
        " --memory-root " + shell_quote(memory_root.string()));
    require(vg_explain.exit_code == 0, "Expected `vg explain` to succeed on the SIEM fixture.");
    require(vg_explain.output.find("vg_explained") != std::string::npos,
            "Expected Vuplus Gate explain output to report vg_explained.");
    require(vg_explain.output.find("why:") != std::string::npos,
            "Expected Vuplus Gate compact output to include why.");
    require(vg_explain.output.find("signal: log.signature.EYZ-47281") != std::string::npos,
            "Expected Vuplus Gate explain output to include heap signature signal.");
    require(vg_explain.output.find("historical-correlation:") != std::string::npos,
            "Expected Vuplus Gate explain output to include historical correlation.");
    require(vg_explain.output.find("operational-blast-radius:") != std::string::npos,
            "Expected Vuplus Gate explain output to include blast radius.");
    require(vg_explain.output.find("rollback-impact:") != std::string::npos,
            "Expected Vuplus Gate explain output to include rollback impact.");
    require(std::filesystem::exists(vg_report_out), "Expected Vuplus Gate --out report to be written.");
    {
        const std::string vg_json = xpp::read_text_file(vg_report_out);
        require(vg_json.find("\"event_type\":\"omnix.vg.explain.v1\"") != std::string::npos,
                "Expected Vuplus Gate JSON to use its event type.");
        require(vg_json.find("\"remediationMode\":\"recommendation_only\"") != std::string::npos,
                "Expected Vuplus Gate JSON to preserve recommendation-only mode.");
    }

    const CommandCapture vg_metatag_explain = run_command_capture(
        command_prefix + " vg explain " + shell_quote(vg_metatag.string()) +
        " --compact --out " + shell_quote(vg_metatag_out.string()) +
        " --memory-root " + shell_quote(memory_root.string()));
    require(vg_metatag_explain.exit_code == 0,
            "Expected `vg explain` to extract SIEM meta-tag key/value evidence.");
    require(vg_metatag_explain.output.find("signal: siem.keypair.boundary_extracted") != std::string::npos,
            "Expected Vuplus Gate to report delimiter-aware key/value extraction.");
    require(vg_metatag_explain.output.find("key-pair: queue=XXB") != std::string::npos,
            "Expected Vuplus Gate to extract queue=XXB from event.original.");
    require(vg_metatag_explain.output.find("key-pair: consumer_count=0") != std::string::npos,
            "Expected Vuplus Gate to extract consumer_count=0 from event.original.");
    require(vg_metatag_explain.output.find("signal: log.signature.EYZ-47281") != std::string::npos,
            "Expected Vuplus Gate to derive the heap signature signal from embedded SIEM metadata.");
    require(std::filesystem::exists(vg_metatag_out),
            "Expected Vuplus Gate meta-tag report to be written.");
    {
        const std::string vg_json = xpp::read_text_file(vg_metatag_out);
        require(vg_json.find("\"keyPairs\"") != std::string::npos,
                "Expected Vuplus Gate JSON to include extracted keyPairs.");
        require(vg_json.find("\"key\":\"queue\"") != std::string::npos,
                "Expected Vuplus Gate JSON to include the extracted queue key.");
        require(vg_json.find("\"value\":\"XXB\"") != std::string::npos,
                "Expected Vuplus Gate JSON to include the extracted queue value.");
        require(vg_json.find("\"valueStart\"") != std::string::npos &&
                    vg_json.find("\"valueEnd\"") != std::string::npos,
                "Expected Vuplus Gate JSON to include key/value boundary offsets.");
    }

    const CommandCapture vg_shape = run_command_capture(
        command_prefix + " vg shape " + shell_quote(vg_metatag.string()) +
        " --compact --out " + shell_quote(vg_shape_out.string()) +
        " --memory-root " + shell_quote(memory_root.string()));
    require(vg_shape.exit_code == 0, "Expected `vg shape` to infer SIEM shaping fields.");
    require(vg_shape.output.find("vg_shaped") != std::string::npos,
            "Expected `vg shape` to report vg_shaped.");
    require(vg_shape.output.find("shape: consumer_count type=integer") != std::string::npos,
            "Expected `vg shape` to type consumer_count as integer.");
    require(vg_shape.output.find("semantic=queue consumer count") != std::string::npos,
            "Expected `vg shape` to infer semantic meaning for consumer_count.");
    require(vg_shape.output.find("signal=consumer.count.zero") != std::string::npos,
            "Expected `vg shape` to map consumer_count=0 to a signal.");
    require(vg_shape.output.find("shaping-rules:") != std::string::npos,
            "Expected `vg shape` to propose reusable shaping rules.");
    require(std::filesystem::exists(vg_shape_out), "Expected `vg shape --out` artifact to be written.");
    {
        const std::string shape_json = xpp::read_text_file(vg_shape_out);
        require(shape_json.find("\"shapedFields\"") != std::string::npos,
                "Expected shape artifact to include shapedFields.");
        require(shape_json.find("\"field\":\"consumer_count\"") != std::string::npos,
                "Expected shape artifact to include consumer_count field.");
        require(shape_json.find("\"value\":0") != std::string::npos,
                "Expected shape artifact to preserve integer value without string coercion.");
        require(shape_json.find("\"semanticMeaning\":\"queue consumer count\"") != std::string::npos,
                "Expected shape artifact to include semantic meaning.");
        require(shape_json.find("\"mappedSignal\":\"consumer.count.zero\"") != std::string::npos,
                "Expected shape artifact to include mapped signal.");
        require(shape_json.find("\"shapingRules\"") != std::string::npos,
                "Expected shape artifact to include reusable shaping rules.");
    }

    const CommandCapture vg_learn_shape = run_command_capture(
        command_prefix + " vg explain " + shell_quote(vg_metatag.string()) +
        " --learn-shape --compact --memory-root " + shell_quote(memory_root.string()));
    require(vg_learn_shape.exit_code == 0,
            "Expected `vg explain --learn-shape` to keep explanation green.");
    require(vg_learn_shape.output.find("siem.shape.rules_proposed") != std::string::npos,
            "Expected `vg explain --learn-shape` to include rule proposals.");

    const CommandCapture vg_vertical_shape = run_command_capture(
        command_prefix + " vg shape " + shell_quote(vg_verticals.string()) +
        " --compact --out " + shell_quote(vg_vertical_shape_out.string()) +
        " --memory-root " + shell_quote(memory_root.string()));
    require(vg_vertical_shape.exit_code == 0,
            "Expected `vg shape` to infer fields from the vertical scale fixture.");
    require(vg_vertical_shape.output.find("orchestrator.kubernetes") != std::string::npos,
            "Expected vertical shape output to map Kubernetes fields.");
    require(vg_vertical_shape.output.find("loadbalancer.frontend") != std::string::npos,
            "Expected vertical shape output to map load balancer fields.");
    require(vg_vertical_shape.output.find("config.ansible.inventory") != std::string::npos,
            "Expected vertical shape output to map Ansible inventory fields.");
    require(vg_vertical_shape.output.find("cloud.google.compute") != std::string::npos,
            "Expected vertical shape output to map GCP compute fields.");
    require(vg_vertical_shape.output.find("omnix.node.neighbor") != std::string::npos,
            "Expected vertical shape output to map OmniX node/master fields.");
    require(vg_vertical_shape.output.find("storage.dependency") != std::string::npos,
            "Expected vertical shape output to map storage dependency fields.");
    require(std::filesystem::exists(vg_vertical_shape_out),
            "Expected vertical `vg shape --out` artifact to be written.");

    const CommandCapture vg_k8s_explain = run_command_capture(
        command_prefix + " vg explain " + shell_quote(vg_k8s.string()) +
        " --learn-shape --compact --memory-root " + shell_quote(memory_root.string()));
    require(vg_k8s_explain.exit_code == 0,
            "Expected Vuplus Gate to explain Kubernetes/container vertical fixtures.");
    require(vg_k8s_explain.output.find("signal: orchestrator.kubernetes") != std::string::npos,
            "Expected Kubernetes explanation to include orchestrator signal.");
    require(vg_k8s_explain.output.find("signal: runtime.container") != std::string::npos,
            "Expected Kubernetes explanation to include container runtime signal.");

    const CommandCapture vg_lb_correlate = run_command_capture(
        command_prefix + " vg correlate " + shell_quote(vg_lb.string()) +
        " --dependency-map " + shell_quote(vg_network.string()) +
        " --compact --memory-root " + shell_quote(memory_root.string()));
    require(vg_lb_correlate.exit_code == 0,
            "Expected Vuplus Gate to correlate load balancer vertical fixtures.");
    require(vg_lb_correlate.output.find("signal: loadbalancer.frontend") != std::string::npos,
            "Expected load balancer correlation to include frontend signal.");
    require(vg_lb_correlate.output.find("signal: loadbalancer.backend") != std::string::npos,
            "Expected load balancer correlation to include backend signal.");
    require(vg_lb_correlate.output.find("signal: loadbalancer.target.unhealthy") != std::string::npos,
            "Expected load balancer correlation to include unhealthy target signal.");

    const CommandCapture vg_terraform_shape = run_command_capture(
        command_prefix + " vg shape " + shell_quote(vg_terraform.string()) +
        " --compact --out " + shell_quote(vg_terraform_shape_out.string()) +
        " --memory-root " + shell_quote(memory_root.string()));
    require(vg_terraform_shape.exit_code == 0,
            "Expected `vg shape` to infer fields from Terraform/GCP fixture.");
    require(vg_terraform_shape.output.find("iac.terraform.plan") != std::string::npos,
            "Expected Terraform shape output to include IaC plan signal.");
    require(vg_terraform_shape.output.find("cloud.google.compute") != std::string::npos,
            "Expected Terraform shape output to include GCP compute signal.");
    require(std::filesystem::exists(vg_terraform_shape_out),
            "Expected Terraform `vg shape --out` artifact to be written.");

    const CommandCapture vg_minion_explain = run_command_capture(
        command_prefix + " vg explain " + shell_quote(vg_minion.string()) +
        " --learn-shape --compact --memory-root " + shell_quote(memory_root.string()));
    require(vg_minion_explain.exit_code == 0,
            "Expected Vuplus Gate to explain OmniX minion neighbor fixtures.");
    require(vg_minion_explain.output.find("signal: omnix.node.neighbor") != std::string::npos,
            "Expected OmniX minion neighbor explanation to include node neighbor signal.");

    const CommandCapture vg_correlate = run_command_capture(
        command_prefix + " vg correlate " + shell_quote(vg_packages.string()) +
        " --dependency-map " + shell_quote(vg_dependency.string()) +
        " --compact --memory-root " + shell_quote(memory_root.string()));
    require(vg_correlate.exit_code == 0, "Expected `vg correlate` to succeed.");
    require(vg_correlate.output.find("vg_correlated") != std::string::npos,
            "Expected Vuplus Gate correlate output to report vg_correlated.");
    require(vg_correlate.output.find("dependency.package.apt") != std::string::npos,
            "Expected Vuplus Gate correlate output to identify apt package surface.");
    require(vg_correlate.output.find("dependency.script.powershell") != std::string::npos,
            "Expected Vuplus Gate correlate output to identify PowerShell script surface.");

    const CommandCapture vg_eventviewer_explain = run_command_capture(
        command_prefix + " vg explain " + shell_quote(vg_eventviewer.string()) +
        " --compact --memory-root " + shell_quote(memory_root.string()));
    require(vg_eventviewer_explain.exit_code == 0,
            "Expected Vuplus Gate to explain Event Viewer retention fixtures.");
    require(vg_eventviewer_explain.output.find("eventviewer.retention.below_1gb") != std::string::npos,
            "Expected Vuplus Gate Event Viewer explanation to flag retention below 1GB.");
    require(vg_eventviewer_explain.output.find("execution-topology: standalone_local_node") != std::string::npos,
            "Expected Vuplus Gate output to include execution topology.");
    require(vg_eventviewer_explain.output.find("alarm-cab: OMNIX-EVENTVIEWER-RETENTION-Security") != std::string::npos,
            "Expected Vuplus Gate Event Viewer explanation to include a CAB recommendation.");

    const CommandCapture vg_sessions_explain = run_command_capture(
        command_prefix + " vg explain " + shell_quote(vg_sessions.string()) +
        " --compact --memory-root " + shell_quote(memory_root.string()));
    require(vg_sessions_explain.exit_code == 0,
            "Expected Vuplus Gate to explain syslog/lastlog correlation fixtures.");
    require(vg_sessions_explain.output.find("session.syslog_lastlog.correlation") != std::string::npos,
            "Expected Vuplus Gate to include syslog/lastlog session correlation.");

    const CommandCapture vg_rum_explain = run_command_capture(
        command_prefix + " vg explain " + shell_quote(vg_rum.string()) +
        " --compact --memory-root " + shell_quote(memory_root.string()));
    require(vg_rum_explain.exit_code == 0,
            "Expected Vuplus Gate to explain heuristic/RUM fixtures.");
    require(vg_rum_explain.output.find("heuristic: rum.latency_spike") != std::string::npos,
            "Expected Vuplus Gate to include a latency heuristic signal.");
    require(vg_rum_explain.output.find("heuristic: rum.error_burst") != std::string::npos,
            "Expected Vuplus Gate to include an error-burst heuristic signal.");

    const CommandCapture vg_encrypted_explain = run_command_capture(
        command_prefix + " vg explain " + shell_quote(vg_encrypted.string()) +
        " --compact --memory-root " + shell_quote(memory_root.string()));
    require(vg_encrypted_explain.exit_code == 0,
            "Expected Vuplus Gate to explain encrypted SIEM custody fixtures.");
    require(vg_encrypted_explain.output.find("siem.encrypted_evidence") != std::string::npos,
            "Expected encrypted fixture to map encrypted evidence signal.");
    require(vg_encrypted_explain.output.find("custody.operator_approval_required") != std::string::npos,
            "Expected encrypted fixture to require operator approval for custody.");
    require(vg_encrypted_explain.output.find("key-custody: operator_approval_required anchor=XYZ_CLASSC1_CUST8") != std::string::npos,
            "Expected encrypted fixture to expose safe key-custody metadata without secrets.");

    const CommandCapture vg_cab = run_command_capture(
        command_prefix + " vg cab " + shell_quote(vg_eventviewer.string()) +
        " --compact --out " + shell_quote(vg_cab_out.string()) +
        " --memory-root " + shell_quote(memory_root.string()));
    require(vg_cab.exit_code == 0, "Expected `vg cab` to write Alarm CAB JSON.");
    require(vg_cab.output.find("vg_cab_ready") != std::string::npos,
            "Expected Vuplus Gate CAB command to report vg_cab_ready.");
    require(vg_cab.output.find("alarm-cab: OMNIX-EVENTVIEWER-RETENTION-Security") != std::string::npos,
            "Expected Vuplus Gate CAB command to include CAB id.");
    require(std::filesystem::exists(vg_cab_out), "Expected Vuplus Gate CAB JSON artifact to be written.");
    {
        const std::string cab_json = xpp::read_text_file(vg_cab_out);
        require(cab_json.find("\"alarmCab\"") != std::string::npos,
                "Expected Vuplus Gate CAB artifact to include alarmCab.");
        require(cab_json.find("\"approvalRequirement\":\"Requires elevated Administrator/SYSTEM approval") != std::string::npos,
                "Expected Vuplus Gate CAB artifact to include elevated approval requirement.");
    }

    const CommandCapture vg_compare = run_command_capture(
        command_prefix + " vg compare " + shell_quote(vg_recovery.string()) +
        " --compact --memory-root " + shell_quote(memory_root.string()));
    require(vg_compare.exit_code == 0, "Expected `vg compare` to succeed.");
    require(vg_compare.output.find("vg_compared") != std::string::npos,
            "Expected Vuplus Gate compare output to report vg_compared.");
    require(vg_compare.output.find("recovery.success_path") != std::string::npos,
            "Expected Vuplus Gate compare output to include success path signal.");
    require(vg_compare.output.find("recovery.failed_path") != std::string::npos,
            "Expected Vuplus Gate compare output to include failed path signal.");

    const CommandCapture vg_replay = run_command_capture(
        command_prefix + " tze replay latest --memory-root " + shell_quote(memory_root.string()));
    require(vg_replay.exit_code == 0, "Expected TZE replay after Vuplus Gate to succeed.");
    require(vg_replay.output.find("x.Vuplus.Gate") != std::string::npos,
            "Expected TZE replay to include x.Vuplus.Gate stage.");

    const CommandCapture vg_missing = run_command_capture(
        command_prefix + " vg explain " + shell_quote((root / "missing-vg.json").string()) +
        " --compact --memory-root " + shell_quote(memory_root.string()));
    require(vg_missing.exit_code != 0, "Expected missing Vuplus Gate input to fail cleanly.");
    require(vg_missing.output.find("vg_input_missing") != std::string::npos,
            "Expected missing Vuplus Gate input to report vg_input_missing.");

    const CommandCapture regex = run_command_capture(
        command_prefix + " tool regex-search --memory-root " + shell_quote(memory_root.string()) +
        " -- " + shell_quote("Build") + " " + shell_quote((sample_dir / "notes.txt").string()));
    require(regex.exit_code == 0, "Expected `tool regex-search` to succeed.");
    require(regex.output.find("alpha Build beta") != std::string::npos,
            "Expected regex-search output to include the matching line.");

    const CommandCapture regex_directory = run_command_capture(
        command_prefix + " tool regex-search --compact --memory-root " + shell_quote(memory_root.string()) +
        " -- " + shell_quote("Build") + " " + shell_quote(sample_dir.string()));
    require(regex_directory.exit_code == 0, "Expected `tool regex-search` over a directory to succeed recursively.");
    require(regex_directory.output.find("notes.txt:1") != std::string::npos,
            "Expected directory regex-search output to include the recursive match.");

    const CommandCapture deep = run_command_capture(
        command_prefix + " tool deep-grep --memory-root " + shell_quote(memory_root.string()) +
        " -- " + shell_quote("Build") + " " + shell_quote(sample_dir.string()));
    require(deep.exit_code == 0, "Expected `tool deep-grep` to succeed.");
    require(deep.output.find("notes.txt:1") != std::string::npos,
            "Expected deep-grep output to include the recursive match.");

    const CommandCapture busybox = run_command_capture(
        command_prefix + " tool busybox --memory-root " + shell_quote(memory_root.string()) +
        " -- grep " + shell_quote("Build") + " " + shell_quote((sample_dir / "notes.txt").string()));
    require(busybox.exit_code == 0, "Expected `tool busybox -- grep ...` to succeed.");
    require(busybox.output.find("alpha Build beta") != std::string::npos,
            "Expected busybox tool execution to include the matching line.");

    const CommandCapture inspect_log = run_command_capture(
        command_prefix + " tool inspect-log --memory-root " + shell_quote(memory_root.string()) +
        " -- " + shell_quote((sample_dir / "auth.log").string()));
    require(inspect_log.exit_code == 0, "Expected `tool inspect-log` to succeed.");
    require(inspect_log.output.find("ssh_auth_event") != std::string::npos,
            "Expected inspect-log output to include structured SSH/auth objects.");

    const CommandCapture inspect_build = run_command_capture(
        command_prefix + " tool inspect-build --memory-root " + shell_quote(memory_root.string()) +
        " -- " + shell_quote((sample_dir / "project").string()));
    require(inspect_build.exit_code == 0, "Expected `tool inspect-build` to succeed.");
    require(inspect_build.output.find("build_system=cmake") != std::string::npos,
            "Expected inspect-build output to include the detected build system.");

    const CommandCapture inspect_host = run_command_capture(
        command_prefix + " tool inspect-host --memory-root " + shell_quote(memory_root.string()) + " -- --linux");
    require(inspect_host.exit_code == 0, "Expected `tool inspect-host` to succeed.");
    require(inspect_host.output.find("Produced artifact:") != std::string::npos,
            "Expected inspect-host output to include the saved host report path.");
    require(inspect_host.output.find("user=root") != std::string::npos,
            "Expected inspect-host output to include sampled user information.");

    const std::size_t report_marker = inspect_host.output.find("Produced artifact: ");
    require(report_marker != std::string::npos, "Expected inspect-host to produce an artifact path.");
    const std::size_t report_end = inspect_host.output.find('\n', report_marker);
    const std::string host_report_path =
        inspect_host.output.substr(report_marker + std::string("Produced artifact: ").size(),
                                   report_end - (report_marker + std::string("Produced artifact: ").size()));
    require(std::filesystem::exists(host_report_path), "Expected inspect-host to write the host report artifact.");
    {
        std::ifstream host_report(host_report_path);
        std::ostringstream report_text;
        report_text << host_report.rdbuf();
        require(report_text.str().find("native_tool=nmap") != std::string::npos,
                "Expected inspect-host artifact to include native nmap detection.");
    }

    const CommandCapture analyze_host = run_command_capture(
        command_prefix + " analyze " + shell_quote(host_report_path) +
        " --memory-root " + shell_quote(memory_root.string()) +
        " --source-map " + shell_quote((kSourceRoot / "res" / "tze.txt").string()));
    require(analyze_host.exit_code == 0, "Expected `analyze` on the host report to succeed.");
    require(analyze_host.output.find("host_inventory_summary") != std::string::npos,
            "Expected host-report analysis to surface host inventory objects.");

    const CommandCapture pipeline = run_command_capture(
        command_prefix + " tool text-pipeline --memory-root " + shell_quote(memory_root.string()) +
        " -- " + shell_quote((sample_dir / "notes.txt").string()) +
        " --grep " + shell_quote("Build") +
        " --sed " + shell_quote("s/Build/BUILD/"));
    require(pipeline.exit_code == 0, "Expected `tool text-pipeline` to succeed.");
    require(pipeline.output.find("BUILD") != std::string::npos,
            "Expected text-pipeline output to include the transformed text.");

    const CommandCapture ssh = run_command_capture(
        command_prefix + " tool ssh --memory-root " + shell_quote(memory_root.string()) + " -- -V");
    require(ssh.exit_code == 0, "Expected `tool ssh -- -V` to succeed.");
    require(ssh.output.find("OpenSSH_cli") != std::string::npos,
            "Expected ssh tool output to include the fake version banner.");

    const std::string source_map_arg = " --source-map " + shell_quote((kSourceRoot / "res" / "tze.txt").string());
    const CommandCapture analyze_case = run_command_capture(
        command_prefix + " analyze " + shell_quote((sample_dir / "build.log").string()) +
        " --memory-root " + shell_quote(memory_root.string()) + source_map_arg);
    require(analyze_case.exit_code == 0, "Expected `analyze` to create a reportable case.");
    const std::string marker = "Case id: ";
    const std::size_t start = analyze_case.output.find(marker);
    require(start != std::string::npos, "Expected analyzed output to include a case id.");
    const std::size_t line_end = analyze_case.output.find('\n', start);
    const std::string case_id = analyze_case.output.substr(start + marker.size(), line_end - (start + marker.size()));

    const CommandCapture report_case = run_command_capture(
        command_prefix + " tool report-case --memory-root " + shell_quote(memory_root.string()) +
        " -- " + shell_quote(case_id));
    require(report_case.exit_code == 0, "Expected `tool report-case` to succeed.");
    require(report_case.output.find("Produced artifact:") != std::string::npos,
            "Expected report-case output to include the saved artifact path.");
    const std::filesystem::path report_path = memory_root / "reports" / (case_id + ".txt");
    require(std::filesystem::exists(report_path),
            "Expected report-case to write a persisted analyst report.");
    {
        std::ifstream report_input(report_path);
        std::ostringstream report_text;
        report_text << report_input.rdbuf();
        require(report_text.str().find("## Executive Summary") != std::string::npos,
                "Expected report-case artifact to include an executive summary section.");
        require(report_text.str().find("## TZE Run Links") != std::string::npos,
                "Expected report-case artifact to include the linked TZE runs section.");
        require(report_text.str().find("## Evidence List") != std::string::npos,
                "Expected report-case artifact to include an evidence list section.");
        require(report_text.str().find("## Normalized Objects") != std::string::npos,
                "Expected report-case artifact to include a normalized objects section.");
        require(report_text.str().find("## Recommended Next Actions") != std::string::npos,
                "Expected report-case artifact to include a recommended next actions section.");
        require(report_text.str().find("## Provenance Trail") != std::string::npos,
                "Expected report-case artifact to include a provenance trail section.");
    }

    const CommandCapture case_after_report = run_command_capture(
        command_prefix + " case " + shell_quote(case_id) +
        " --memory-root " + shell_quote(memory_root.string()) + source_map_arg);
    require(case_after_report.exit_code == 0, "Expected `case <id>` after report generation to succeed.");
    require(case_after_report.output.find("Case reported by run:") != std::string::npos,
            "Expected the persisted case record to retain the run that produced the saved report.");
}

void test_cli_analyst_commands() {
    const std::filesystem::path root = kBinaryRoot / "cli-analyst";
    const std::filesystem::path memory_root = root / "memory";
    const std::filesystem::path sample = root / "meeting.log";
    safe_remove_all(root);
    std::filesystem::create_directories(root);

    {
        std::ofstream output(sample);
        output << "warning: build ready\n";
        output << "ssh auth failure observed\n";
        output << "nmap requested for host survey\n";
    }

    const std::filesystem::path binary = kBinaryRoot / "omnix";
    const std::string base =
        shell_quote(binary.string()) + " --version";
    const CommandCapture version = run_command_capture(base);
    require(version.exit_code == 0, "Expected omnix binary to be runnable for analyst CLI tests.");

    const std::string common =
        shell_quote(binary.string()) + " ";
    const std::string memory_arg = " --memory-root " + shell_quote(memory_root.string()) +
        " --source-map " + shell_quote((kSourceRoot / "res" / "tze.txt").string());

    const CommandCapture ingest = run_command_capture(
        common + "ingest " + shell_quote(sample.string()) + memory_arg);
    require(ingest.exit_code == 0, "Expected `omnix ingest` to succeed.");
    require(ingest.output.find("Verdict: ingested") != std::string::npos,
            "Expected ingest output to report the ingested verdict.");

    const std::string marker = "Case id: ";
    const std::size_t start = ingest.output.find(marker);
    require(start != std::string::npos, "Expected ingest output to include a case id.");
    const std::size_t line_end = ingest.output.find('\n', start);
    const std::string case_id = ingest.output.substr(start + marker.size(), line_end - (start + marker.size()));
    require(!case_id.empty(), "Expected a non-empty case id from ingest output.");

    const CommandCapture analyze = run_command_capture(
        common + "analyze " + shell_quote(case_id) + memory_arg);
    require(analyze.exit_code == 0, "Expected `omnix analyze` to succeed.");
    require(analyze.output.find("Verdict: analyzed") != std::string::npos,
            "Expected analyze output to report the analyzed verdict.");

    const CommandCapture decide = run_command_capture(
        common + "decide " + shell_quote(case_id) + memory_arg);
    require(decide.exit_code == 0, "Expected `omnix decide` to succeed.");
    require(decide.output.find("Decision candidates:") != std::string::npos,
            "Expected decide output to include decision candidates.");

    const CommandCapture inspect = run_command_capture(
        common + "case " + shell_quote(case_id) + memory_arg);
    require(inspect.exit_code == 0, "Expected `omnix case` to succeed.");
    require(inspect.output.find("Verdict: case_loaded") != std::string::npos,
            "Expected case output to report the loaded verdict.");
    require(inspect.output.find("Normalized objects:") != std::string::npos,
            "Expected case output to include normalized object details.");
}

void test_cli_case_list_and_search() {
    const std::filesystem::path root = kBinaryRoot / "cli-case-links";
    const std::filesystem::path memory_root = root / "memory";
    const std::filesystem::path sample_one = root / "auth-one.log";
    const std::filesystem::path sample_two = root / "auth-two.log";
    safe_remove_all(root);
    std::filesystem::create_directories(root);

    {
        std::ofstream output(sample_one);
        output << "sshd[1]: Failed password for invalid user alice from 10.0.0.5 port 22 ssh2\n";
    }
    {
        std::ofstream output(sample_two);
        output << "sshd[2]: Failed password for invalid user bob from 10.0.0.5 port 22 ssh2\n";
    }

    const std::filesystem::path binary = kBinaryRoot / "omnix";
    const std::string common =
        shell_quote(binary.string()) + " ";
    const std::string args = " --memory-root " + shell_quote(memory_root.string()) +
        " --source-map " + shell_quote((kSourceRoot / "res" / "tze.txt").string());

    const CommandCapture analyze_one = run_command_capture(common + "analyze " + shell_quote(sample_one.string()) + args);
    require(analyze_one.exit_code == 0, "Expected first case analyze to succeed.");
    const CommandCapture analyze_two = run_command_capture(common + "analyze " + shell_quote(sample_two.string()) + args);
    require(analyze_two.exit_code == 0, "Expected second case analyze to succeed.");

    const CommandCapture list = run_command_capture(common + "case list" + args);
    require(list.exit_code == 0, "Expected `omnix case list` to succeed.");
    require(list.output.find("Case matches:") != std::string::npos,
            "Expected `omnix case list` to include case matches.");
    require(list.output.find("Case links:") != std::string::npos,
            "Expected `omnix case list` to include derived case links.");
    require(list.output.find("Case clusters:") != std::string::npos,
            "Expected `omnix case list` to include derived case clusters.");

    const CommandCapture search = run_command_capture(common + "case search " + shell_quote("10.0.0.5") + args);
    require(search.exit_code == 0, "Expected `omnix case search` to succeed.");
    require(search.output.find("Verdict: case_search_results") != std::string::npos,
            "Expected `omnix case search` to report search results.");
    require(search.output.find("10.0.0.5") != std::string::npos,
            "Expected `omnix case search` output to include the search term context.");
}

void test_cli_v2_case_timeline_and_decision_feedback() {
    const std::filesystem::path root = kBinaryRoot / "cli-v2-timeline";
    const std::filesystem::path memory_root = root / "memory";
    const std::filesystem::path sample = root / "timeline.log";
    safe_remove_all(root);
    std::filesystem::create_directories(root);

    {
        std::ofstream output(sample);
        output << "warning: build ready\n";
        output << "ssh auth failure observed\n";
        output << "nmap requested for host survey\n";
    }

    const std::filesystem::path binary = kBinaryRoot / "omnix";
    const std::string common = shell_quote(binary.string()) + " ";
    const std::string args = " --memory-root " + shell_quote(memory_root.string()) +
        " --source-map " + shell_quote((kSourceRoot / "res" / "tze.txt").string());

    const CommandCapture ingest = run_command_capture(common + "ingest " + shell_quote(sample.string()) + args);
    require(ingest.exit_code == 0, "Expected ingest to succeed for case timeline test.");
    const std::string case_id = extract_line_value(ingest.output, "Case id: ");
    require(!case_id.empty(), "Expected ingest output to expose a case id.");

    const CommandCapture decide = run_command_capture(common + "decide " + shell_quote(case_id) + args);
    require(decide.exit_code == 0, "Expected decide to succeed for case timeline test.");
    const std::size_t id_marker = decide.output.find("Decision candidates:\n - {");
    require(id_marker != std::string::npos, "Expected decide output to include a decision id.");
    const std::size_t id_start = decide.output.find('{', id_marker);
    const std::size_t id_end = decide.output.find('}', id_start);
    require(id_start != std::string::npos && id_end != std::string::npos && id_end > id_start,
            "Expected a parseable decision id in decide output.");
    const std::string decision_id = decide.output.substr(id_start + 1, id_end - id_start - 1);

    const CommandCapture feedback = run_command_capture(
        common + "decide feedback " + shell_quote(case_id) + " " + shell_quote(decision_id) +
        " helpful --note " + shell_quote("timeline smoke") + args);
    require(feedback.exit_code == 0, "Expected decision feedback command to succeed.");
    require(feedback.output.find("Verdict: decision_feedback_recorded") != std::string::npos,
            "Expected decision feedback command to report a recorded verdict.");

    const CommandCapture timeline = run_command_capture(common + "case timeline " + shell_quote(case_id) + args);
    require(timeline.exit_code == 0, "Expected `omnix case timeline` to succeed.");
    require(timeline.output.find("Verdict: case_timeline") != std::string::npos,
            "Expected case timeline command to report the timeline verdict.");
    require(timeline.output.find("decision_feedback") != std::string::npos,
            "Expected case timeline output to include decision feedback entries.");
}

void test_cli_v2_tze_chain_and_bundle_round_trip() {
    const std::filesystem::path root = kBinaryRoot / "cli-v2-tze";
    const std::filesystem::path memory_root = root / "memory";
    const std::filesystem::path import_memory_root = root / "import-memory";
    const std::filesystem::path sample = root / "chain.log";
    const std::filesystem::path case_bundle = root / "case-bundle.json";
    const std::filesystem::path tze_bundle = root / "tze-bundle.json";
    safe_remove_all(root);
    std::filesystem::create_directories(root);

    {
        std::ofstream output(sample);
        output << "warning: build ready\n";
        output << "ssh auth failure observed\n";
        output << "nmap requested for host survey\n";
    }

    const std::filesystem::path binary = kBinaryRoot / "omnix";
    const std::string common = shell_quote(binary.string()) + " ";
    const std::string args = " --memory-root " + shell_quote(memory_root.string()) +
        " --source-map " + shell_quote((kSourceRoot / "res" / "tze.txt").string());

    const CommandCapture ingest = run_command_capture(common + "ingest " + shell_quote(sample.string()) + args);
    require(ingest.exit_code == 0, "Expected ingest to succeed for chain/export test.");
    const std::string case_id = extract_line_value(ingest.output, "Case id: ");
    require(!case_id.empty(), "Expected ingest output to expose a case id.");

    const CommandCapture decide = run_command_capture(common + "decide " + shell_quote(case_id) + args);
    require(decide.exit_code == 0, "Expected decide to succeed for chain/export test.");

    const CommandCapture chain = run_command_capture(common + "tze chain latest" + args);
    require(chain.exit_code == 0, "Expected `omnix tze chain latest` to succeed.");
    require(chain.output.find("Verdict: tze_chain_rendered") != std::string::npos,
            "Expected TZE chain command to report the chain verdict.");

    const CommandCapture case_export = run_command_capture(
        common + "case export " + shell_quote(case_id) + " --out " + shell_quote(case_bundle.string()) + args);
    require(case_export.exit_code == 0, "Expected case export to succeed.");
    require(std::filesystem::exists(case_bundle), "Expected case export to write the requested bundle.");

    const CommandCapture tze_export = run_command_capture(
        common + "tze export latest --out " + shell_quote(tze_bundle.string()) +
        " --memory-root " + shell_quote(memory_root.string()));
    require(tze_export.exit_code == 0, "Expected TZE export to succeed.");
    require(std::filesystem::exists(tze_bundle), "Expected TZE export to write the requested bundle.");

    const CommandCapture case_import = run_command_capture(
        common + "case import " + shell_quote(case_bundle.string()) +
        " --memory-root " + shell_quote(import_memory_root.string()));
    require(case_import.exit_code == 0, "Expected case bundle import to succeed.");

    const CommandCapture tze_import = run_command_capture(
        common + "tze import " + shell_quote(tze_bundle.string()) +
        " --memory-root " + shell_quote(import_memory_root.string()));
    require(tze_import.exit_code == 0, "Expected TZE bundle import to succeed.");

    const CommandCapture imported_cases = run_command_capture(
        common + "case list --memory-root " + shell_quote(import_memory_root.string()) +
        " --source-map " + shell_quote((kSourceRoot / "res" / "tze.txt").string()));
    require(imported_cases.exit_code == 0, "Expected imported memory root to render case list.");
    require(imported_cases.output.find(case_id) != std::string::npos,
            "Expected imported case list to include the exported case id.");

    const CommandCapture imported_runs = run_command_capture(
        common + "tze runs --memory-root " + shell_quote(import_memory_root.string()));
    require(imported_runs.exit_code == 0, "Expected imported memory root to render TZE runs.");
    require(imported_runs.output.find("TZE Runs:") != std::string::npos,
            "Expected imported memory root to expose the TZE ledger.");
}

void test_cli_v2_incident_list_and_report() {
    const std::filesystem::path root = kBinaryRoot / "cli-v2-incident";
    const std::filesystem::path memory_root = root / "memory";
    const std::filesystem::path sample_one = root / "incident-one.log";
    const std::filesystem::path sample_two = root / "incident-two.log";
    const std::filesystem::path incident_report = root / "incident.txt";
    safe_remove_all(root);
    std::filesystem::create_directories(root);

    {
        std::ofstream output(sample_one);
        output << "ssh auth failure observed\n";
        output << "nmap requested for host survey\n";
    }
    {
        std::ofstream output(sample_two);
        output << "ssh access denied\n";
        output << "build and tool review requested\n";
    }

    const std::filesystem::path binary = kBinaryRoot / "omnix";
    const std::string common = shell_quote(binary.string()) + " ";
    const std::string args = " --memory-root " + shell_quote(memory_root.string()) +
        " --source-map " + shell_quote((kSourceRoot / "res" / "tze.txt").string());

    require(run_command_capture(common + "ingest " + shell_quote(sample_one.string()) + args).exit_code == 0,
            "Expected first incident ingest to succeed.");
    require(run_command_capture(common + "ingest " + shell_quote(sample_two.string()) + args).exit_code == 0,
            "Expected second incident ingest to succeed.");

    const CommandCapture incident_list = run_command_capture(common + "incident list --memory-root " + shell_quote(memory_root.string()));
    require(incident_list.exit_code == 0, "Expected incident list to succeed.");
    require(incident_list.output.find("Verdict: incident_listed") != std::string::npos,
            "Expected incident list command to report the listed verdict.");
    const std::string incident_id = extract_line_value(incident_list.output, " - ");
    require(incident_id.find("incident-") != std::string::npos, "Expected incident list output to include an incident id.");
    const std::string resolved_incident_id = incident_id.substr(0, incident_id.find(" | "));

    const CommandCapture incident_inspect =
        run_command_capture(common + "incident " + shell_quote(resolved_incident_id) + " --memory-root " + shell_quote(memory_root.string()));
    require(incident_inspect.exit_code == 0, "Expected incident inspect to succeed.");
    require(incident_inspect.output.find("Verdict: incident_loaded") != std::string::npos,
            "Expected incident inspect command to report the loaded verdict.");

    const CommandCapture incident_report_cmd =
        run_command_capture(common + "incident report " + shell_quote(resolved_incident_id) + " --out " +
                            shell_quote(incident_report.string()) + " --memory-root " + shell_quote(memory_root.string()));
    require(incident_report_cmd.exit_code == 0, "Expected incident report command to succeed.");
    require(std::filesystem::exists(incident_report), "Expected incident report command to write the requested artifact.");
}

}  // namespace

int main() {
    try {
        test_parser_discovers_top_level_sections();
        test_parser_preserves_raw_and_operator_lines();
        test_parser_extracts_build_cmake_stage_graph();
        test_parser_supports_python_like_fixture();
        test_query_runtime_tracks_stateful_session();
        test_language_engine_resolves_native_context();
        test_language_engine_recovers_legacy_decompression_ladder();
        test_preprocessor_runtime_resolves_uac_state();
        test_build_executor_probes_modules();
        test_native_tool_registry_caches_path_hit();
        test_native_tool_registry_deep_scan_and_busybox_applets();
        test_build_executor_inspects_and_builds_project();
        test_build_executor_builds_configure_project();
        test_build_executor_builds_make_project();
        test_build_executor_preflights_nmap_alias();
        test_build_executor_preflights_tshark_alias();
        test_build_executor_doctor_reports_package_guidance();
        test_build_executor_doctor_reports_universal_cmake_guidance();
        test_build_executor_doctor_guides_tshark_dependencies();
        test_packet_capture_parser_decodes_http_payload();
        test_packet_capture_parser_labels_tls_payload();
        test_packet_capture_parser_classifies_plain_utf8_payload();
        test_cli_tview_pcap_fixture_decodes_http();
        test_cli_tview_pcap_fixture_exports_jsonl();
        test_unix_evidence_parser_parses_tview_jsonl();
        test_intent_resolver_routes_port_investigation_to_tview();
        test_defense_diagnostic_routes_and_stays_non_destructive();
        test_preflight_validates_recipe_overrides();
        test_build_executor_stages_cmake_install();
        test_build_executor_stages_lua_recipe_install();
        test_symbol_index_maps_known_symbols();
        test_symbol_index_finishes_translation_ready_families();
        test_security_symbols_are_classified_safely();
        test_emitter_writes_manifest_and_section_files();
        test_generated_sections_meet_tze_conformance_thresholds();
        test_intent_resolver_and_definition_engine();
        test_general_definition_routing_prefers_definition_over_context_summary();
        test_memory_store_persists_and_renders_history();
        test_memory_store_compacts_definition_history_summary();
        test_memory_store_renders_operator_persona_view();
        test_memory_store_persists_learned_recipes();
        test_unix_evidence_parser_parses_json_logs();
        test_unix_evidence_parser_parses_build_logs();
        test_unix_evidence_parser_parses_ssh_and_tool_output();
        test_unix_evidence_parser_parses_host_inventory_report();
        test_memory_store_persists_cases_and_renders_view();
        test_processing_engine_uses_source_backed_mappings();
        test_runtime_backbone_conforms_to_source_stage_graph();
        test_processing_engine_executes_build_flow();
        test_processing_engine_routes_define_through_feedback_loop();
        test_processing_engine_resolves_language_context_for_define_flow();
        test_processing_engine_resolves_uac_state_for_define_flow();
        test_security_manager_simulates_safe_and_blocked_branches();
        test_processing_engine_resolves_security_audit_for_define_flow();
        test_source_mapped_runtime_semantics_stay_in_sync();
        test_processing_engine_build_nmap_prefers_native_tool();
        test_processing_engine_build_tshark_prefers_native_tool();
        test_defense_detection_reports_local_environment_changes();
        test_processing_engine_keeps_ollama_dormant_when_configured();
        test_processing_engine_runs_analyst_case_flow();
        test_processing_engine_decide_uses_prior_success_history();
        test_processing_engine_decide_uses_helpful_tze_feedback();
        test_processing_engine_lists_searches_and_links_cases();
        test_processing_engine_derives_campaign_clusters();
        test_processing_engine_decide_surfaces_safe_modules();
        test_cli_define_and_memory_history();
        test_cli_language_resolution_and_memory_view();
        test_cli_uac_state_and_memory_view();
        test_cli_security_audit_and_memory_view();
        test_cli_legacy_coverage_and_report();
        test_cli_provider_probe_modes();
        test_cli_instance_identity_is_stable_and_private();
        test_cli_provider_probe_openai_fixture();
        test_cli_guarded_assist_falls_back_deterministically();
        test_cli_assist_tool_plan_executes_allowlisted_builtin();
        test_cli_assist_build_plan_selects_allowlisted_recipe();
        test_cli_recipe_author_authors_and_reuses_local_recipe();
        test_cli_recipe_author_repairs_failed_first_attempt();
        test_cli_assist_command_plan_routes_ask_to_preflight();
        test_cli_run_nmap_routes_to_safe_tool_flow();
        test_cli_assist_command_plan_routes_ask_to_tool_nmap();
        test_cli_assist_scan_request_rejects_explicit_external_target();
        test_cli_assist_command_plan_routes_ask_to_tool_nmap_openai_fixture();
        test_cli_openai_freeform_answers_after_local_miss();
        test_cli_openai_freeform_security_guidance_is_non_executing();
        test_cli_openai_freeform_does_not_override_command_route();
        test_cli_shell_provider_and_context();
        test_cli_recursive_why_api_and_link_ux();
        test_cli_salt_style_jinja_node_master();
        test_cli_recursive_why_mines_learned_definition_route();
        test_cli_context_reset_clears_volatile_definition_cache();
        test_cli_shell_nmap_results_alias();
        test_cli_shell_secure_my_system_routes_to_inspect_host();
        test_cli_shell_local_slash24_routes_to_safe_nmap_discovery();
        test_cli_shell_handles_greeting_and_identity();
        test_cli_shell_broad_aliases_and_persona_identity();
        test_cli_persona_modes_are_display_only();
        test_cli_shell_persona_mode_shortcut();
        test_cli_shell_next_step_assist_persists_memory();
        test_cli_shell_tolerates_nmap_typos_and_full_results();
        test_cli_general_definition_uses_system_dictionary_fixture();
        test_cli_shell_ask_prefix_preserves_definition_query();
        test_cli_science_definition_beats_what_matters_route();
        test_cli_what_matters_stays_contextual();
        test_cli_general_definition_glossary_fallback_and_clarification();
        test_cli_general_definition_supports_who_is_and_technology_domain();
        test_cli_neuromorphic_backloop_glossary_terms();
        test_cli_neuralnetwork_glossary_terms();
        test_cli_neural_math_perceptron_lab();
        test_cli_neural_signal_router_tview_jsonl();
        test_definition_flow_records_math_attribution();
        test_tensorflow_env_check_reports_missing_dependencies();
        test_definition_engine_macos_system_dictionary_bridge();
        test_definition_engine_operator_teaching_beats_system_dictionary_fixture();
        test_definition_engine_operator_domain_ambiguity_clarifies();
        test_definition_engine_reference_cache_does_not_outrank_operator_override();
        test_definition_engine_local_retrieval_recovers_close_local_miss();
        test_processing_engine_records_pre_and_post_runtime_stages();
        test_cli_provider_probe_detects_unusable_ollama_model();
        test_definition_engine_webster_fixture_meta_fallback();
        test_definition_engine_webster_fixture_dttext_fallback();
        test_definition_engine_webster_parse_failure_stays_unresolved();
        test_docs_manpage_and_readme_stay_aligned();
        test_cli_review_and_patch_proposal_round_trip();
        test_cli_incident_report_assist_summary();
        test_cli_tze_replay_and_diff();
        test_cli_tze_latest_prune_report_and_feedback();
        test_cli_doctor_and_recipe_override_output();
        test_cli_ask_build_nmap_uses_alias_flow();
        test_cli_ask_build_tshark_uses_alias_flow();
        test_cli_tool_namespace_commands();
        test_cli_analyst_commands();
        test_cli_case_list_and_search();
        test_cli_v2_case_timeline_and_decision_feedback();
        test_cli_v2_tze_chain_and_bundle_round_trip();
        test_cli_v2_incident_list_and_report();
        std::cout << "omnix_tests: all checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "omnix_tests failure: " << error.what() << "\n";
        return 1;
    }
}
