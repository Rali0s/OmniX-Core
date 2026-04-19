#include "tze/build_executor.hpp"
#include "tze/definition_engine.hpp"
#include "tze/intent_resolver.hpp"
#include "tze/language_engine.hpp"
#include "tze/memory_store.hpp"
#include "tze/native_tool_registry.hpp"
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
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
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
    require(!session.operations.empty(), "Expected language resolution to index evidence through the query session.");
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
    require(!session.operations.empty(), "Expected uAC resolution to index evidence through the query session.");
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
    require(provider_stage->status == "configured_dormant",
            "Expected the provider gate to remain probe-only and dormant.");
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

    const CommandCapture regex = run_command_capture(
        command_prefix + " tool regex-search --memory-root " + shell_quote(memory_root.string()) +
        " -- " + shell_quote("Build") + " " + shell_quote((sample_dir / "notes.txt").string()));
    require(regex.exit_code == 0, "Expected `tool regex-search` to succeed.");
    require(regex.output.find("alpha Build beta") != std::string::npos,
            "Expected regex-search output to include the matching line.");

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
        test_preflight_validates_recipe_overrides();
        test_build_executor_stages_cmake_install();
        test_build_executor_stages_lua_recipe_install();
        test_symbol_index_maps_known_symbols();
        test_symbol_index_finishes_translation_ready_families();
        test_security_symbols_are_classified_safely();
        test_emitter_writes_manifest_and_section_files();
        test_generated_sections_meet_tze_conformance_thresholds();
        test_intent_resolver_and_definition_engine();
        test_memory_store_persists_and_renders_history();
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
        test_cli_provider_probe_modes();
        test_cli_guarded_assist_falls_back_deterministically();
        test_cli_assist_tool_plan_executes_allowlisted_builtin();
        test_cli_assist_build_plan_selects_allowlisted_recipe();
        test_cli_assist_command_plan_routes_ask_to_preflight();
        test_cli_run_nmap_routes_to_safe_tool_flow();
        test_cli_assist_command_plan_routes_ask_to_tool_nmap();
        test_cli_shell_provider_and_context();
        test_cli_shell_nmap_results_alias();
        test_cli_shell_secure_my_system_routes_to_inspect_host();
        test_cli_shell_local_slash24_routes_to_safe_nmap_discovery();
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
