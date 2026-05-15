#include "tze/self_review.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <regex>
#include <sstream>

namespace tze {
namespace {

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

std::string lowercase_local(std::string_view value) {
    std::string lowered;
    lowered.reserve(value.size());
    for (char c : value) {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return lowered;
}

std::vector<std::string> split_lines_local(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream input(text);
    for (std::string line; std::getline(input, line);) {
        lines.push_back(line);
    }
    return lines;
}

std::string read_text_local(const std::filesystem::path& path) {
    std::ifstream input(path);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::string now_review_timestamp() {
    using clock = std::chrono::system_clock;
    const std::time_t now = clock::to_time_t(clock::now());
    std::tm tm_value{};
#if defined(_WIN32)
    gmtime_s(&tm_value, &now);
#else
    gmtime_r(&now, &tm_value);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm_value);
    return buffer;
}

std::string make_id(std::string_view prefix, std::string_view seed) {
    return std::string(prefix) + "-" + std::to_string(std::hash<std::string_view>{}(seed));
}

bool skip_dir(const std::filesystem::path& path) {
    const std::string lowered = lowercase_local(path.filename().string());
    return lowered == ".git" || lowered == "build" || lowered == "node_modules" || lowered == ".idea";
}

}  // namespace

std::optional<std::filesystem::path> SelfReviewEngine::resolve_target(std::string_view target,
                                                                      const std::filesystem::path& project_root) const {
    const std::string trimmed = trim_local(target);
    if (trimmed.empty()) {
        return std::nullopt;
    }
    const std::filesystem::path candidate(trimmed);
    if (std::filesystem::exists(candidate)) {
        return std::filesystem::absolute(candidate);
    }
    const std::filesystem::path root = project_root.empty() ? std::filesystem::current_path() : project_root;
    const std::string lowered_target = lowercase_local(trimmed);
    for (std::filesystem::recursive_directory_iterator it(root), end; it != end; ++it) {
        if (it->is_directory() && skip_dir(it->path())) {
            it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file()) {
            continue;
        }
        const std::string filename = lowercase_local(it->path().filename().string());
        const std::string stem = lowercase_local(it->path().stem().string());
        if (filename == lowered_target || stem == lowered_target || filename.find(lowered_target) != std::string::npos) {
            return std::filesystem::absolute(it->path());
        }
    }
    return std::nullopt;
}

ReviewArtifact SelfReviewEngine::review_target(std::string_view target,
                                               const MemorySnapshot& memory,
                                               const std::filesystem::path& project_root) const {
    ReviewArtifact artifact;
    artifact.id = make_id("review", target);
    artifact.target = std::string(target);
    artifact.persisted_at = now_review_timestamp();
    artifact.status = "reviewed";

    const std::optional<std::filesystem::path> resolved = resolve_target(target, project_root);
    if (!resolved.has_value()) {
        artifact.status = "missing_target";
        artifact.summary = "Could not resolve the requested review target inside the local workspace.";
        return artifact;
    }

    const std::filesystem::path path = *resolved;
    const std::string text = read_text_local(path);
    const std::vector<std::string> lines = split_lines_local(text);
    artifact.target = path.string();

    std::regex symbol_re(R"(\b(class|struct|namespace|enum)\s+([A-Za-z_][A-Za-z0-9_]*)|\b([A-Za-z_][A-Za-z0-9_:<>]*)\s+([A-Za-z_][A-Za-z0-9_]*)\s*\()");
    std::smatch match;
    std::istringstream input(text);
    for (std::string line; std::getline(input, line);) {
        if (std::regex_search(line, match, symbol_re)) {
            const std::string symbol = !match[2].str().empty() ? match[2].str() : match[4].str();
            if (!symbol.empty() &&
                std::find(artifact.nearby_symbols.begin(), artifact.nearby_symbols.end(), symbol) == artifact.nearby_symbols.end()) {
                artifact.nearby_symbols.push_back(symbol);
            }
        }
        if (artifact.nearby_symbols.size() >= 8) {
            break;
        }
    }

    for (std::size_t index = 0; index < lines.size(); ++index) {
        const std::string lowered = lowercase_local(lines[index]);
        if (lowered.find("todo") != std::string::npos || lowered.find("fixme") != std::string::npos) {
            artifact.findings.push_back({"medium", "maintainability_issue", path.string(), index + 1,
                                         "Outstanding TODO/FIXME marker",
                                         "The file still contains deferred work markers that should be resolved or tracked explicitly."});
        }
        if (lines[index].size() > 140) {
            artifact.findings.push_back({"low", "maintainability_issue", path.string(), index + 1,
                                         "Long line reduces readability",
                                         "Very long lines are harder to review and often hide multi-purpose logic."});
        }
        if (lowered.find("system(") != std::string::npos || lowered.find("popen(") != std::string::npos) {
            artifact.findings.push_back({"high", "bug_risk", path.string(), index + 1,
                                         "Direct shell execution present",
                                         "Shell execution paths need careful input validation, escaping, and test coverage to remain safe."});
        }
    }

    if (lines.size() > 500) {
        artifact.findings.push_back({"medium", "maintainability_issue", path.string(), 0,
                                     "Large implementation unit",
                                     "Large files tend to mix responsibilities and are harder to test and refactor safely."});
    }

    const std::string filename = path.filename().string();
    if (path.string().find("/src/") != std::string::npos) {
        const std::filesystem::path tests_path = memory.paths.root.empty()
            ? std::filesystem::current_path() / "tests" / "xpp_tests.cpp"
            : std::filesystem::current_path() / "tests" / "xpp_tests.cpp";
        if (std::filesystem::exists(tests_path)) {
            const std::string tests_text = read_text_local(tests_path);
            if (tests_text.find(path.stem().string()) == std::string::npos) {
                artifact.findings.push_back({"medium", "test_gap", path.string(), 0,
                                             "No obvious regression coverage reference",
                                             "The main test file does not appear to reference this module by name, so targeted regression coverage may be missing."});
            }
        }
    }

    artifact.suggested_tests.push_back("cmake --build build -j4");
    artifact.suggested_tests.push_back("./build/omnix_tests");
    if (path.string().find("session_coordinator") != std::string::npos || path.string().find("reasoning_provider") != std::string::npos) {
        artifact.suggested_tests.push_back("cmake --build build --target validate_tze_conformance");
    }

    std::ostringstream summary;
    summary << "Reviewed `" << filename << "` with " << artifact.findings.size() << " finding(s) across "
            << lines.size() << " line(s).";
    artifact.summary = summary.str();

    const std::filesystem::path artifact_dir = memory.paths.logs_root / "reviews";
    std::filesystem::create_directories(artifact_dir);
    artifact.artifact_path = (artifact_dir / (artifact.id + ".md")).string();
    std::ofstream output(artifact.artifact_path);
    output << "# OmniX Review Artifact\n\n";
    output << "- Target: " << artifact.target << "\n";
    output << "- Status: " << artifact.status << "\n";
    output << "- Summary: " << artifact.summary << "\n\n";
    if (!artifact.nearby_symbols.empty()) {
        output << "## Nearby Symbols\n\n";
        for (const std::string& symbol : artifact.nearby_symbols) {
            output << "- " << symbol << "\n";
        }
        output << "\n";
    }
    output << "## Findings\n\n";
    if (artifact.findings.empty()) {
        output << "- No deterministic findings were raised.\n";
    } else {
        for (const ReviewFinding& finding : artifact.findings) {
            output << "- [" << finding.severity << "] " << finding.category << " :: " << finding.summary;
            if (!finding.file_path.empty()) {
                output << " (" << finding.file_path;
                if (finding.line != 0) {
                    output << ":" << finding.line;
                }
                output << ")";
            }
            output << "\n";
            if (!finding.rationale.empty()) {
                output << "  - " << finding.rationale << "\n";
            }
        }
    }
    output << "\n## Suggested Tests\n\n";
    for (const std::string& test : artifact.suggested_tests) {
        output << "- `" << test << "`\n";
    }
    return artifact;
}

PatchProposalArtifact SelfReviewEngine::propose_patch(std::string_view target,
                                                      const MemorySnapshot& memory,
                                                      const ReviewArtifact& review,
                                                      const std::filesystem::path& project_root) const {
    PatchProposalArtifact artifact;
    artifact.id = make_id("patch-proposal", target);
    artifact.target = review.target.empty() ? std::string(target) : review.target;
    artifact.persisted_at = now_review_timestamp();
    artifact.status = "proposed";
    artifact.summary = "Generated a guarded patch proposal from the deterministic review findings.";

    const std::optional<std::filesystem::path> resolved = resolve_target(target, project_root);
    if (resolved.has_value()) {
        artifact.target_files.push_back(resolved->string());
    } else if (!review.target.empty()) {
        artifact.target_files.push_back(review.target);
    }
    for (const ReviewFinding& finding : review.findings) {
        artifact.intended_behavior_changes.push_back(finding.summary);
    }
    artifact.acceptance_tests = review.suggested_tests;

    std::ostringstream diff;
    for (const std::string& file : artifact.target_files) {
        diff << "--- " << file << "\n";
        diff << "+++ " << file << "\n";
        diff << "@@\n";
        diff << "-// current behavior preserved until an operator applies a reviewed patch\n";
        diff << "+// proposed change: address deterministic review findings for " << file << "\n";
    }
    if (artifact.target_files.empty()) {
        diff << "--- unresolved-target\n+++ unresolved-target\n@@\n"
             << "-// target could not be resolved\n"
             << "+// proposed change: resolve the target before generating a concrete patch\n";
    }
    artifact.unified_diff_text = diff.str();

    const std::filesystem::path artifact_dir = memory.paths.logs_root / "patch-proposals";
    std::filesystem::create_directories(artifact_dir);
    artifact.artifact_path = (artifact_dir / (artifact.id + ".md")).string();
    std::ofstream output(artifact.artifact_path);
    output << "# OmniX Patch Proposal\n\n";
    output << "- Target: " << artifact.target << "\n";
    output << "- Status: " << artifact.status << "\n";
    output << "- Summary: " << artifact.summary << "\n\n";
    output << "## Intended Behavior Changes\n\n";
    if (artifact.intended_behavior_changes.empty()) {
        output << "- No deterministic behavior changes were inferred.\n";
    } else {
        for (const std::string& change : artifact.intended_behavior_changes) {
            output << "- " << change << "\n";
        }
    }
    output << "\n## Acceptance Tests\n\n";
    for (const std::string& test : artifact.acceptance_tests) {
        output << "- `" << test << "`\n";
    }
    output << "\n## Patch Skeleton\n\n```diff\n" << artifact.unified_diff_text << "```\n";
    return artifact;
}

}  // namespace tze
