#include "tze/workflow_support.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace tze {
namespace {

std::string join(std::initializer_list<std::string_view> parts) {
    std::ostringstream oss;
    bool first = true;
    for (std::string_view part : parts) {
        if (!first) {
            oss << ' ';
        }
        first = false;
        oss << part;
    }
    return oss.str();
}

}  // namespace

bool WorkflowSupport::check_condition(std::string_view label) {
    return !label.empty();
}

std::string WorkflowSupport::display_text(std::string_view text) {
    return std::string(text);
}

std::vector<std::string> WorkflowSupport::locate_paths(const std::filesystem::path& root, std::string_view needle) {
    std::vector<std::string> matches;
    if (!std::filesystem::exists(root)) {
        return matches;
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const std::string candidate = entry.path().string();
        if (needle.empty() || candidate.find(needle) != std::string::npos) {
            matches.push_back(candidate);
        }
        if (matches.size() >= 16) {
            break;
        }
    }
    return matches;
}

std::string WorkflowSupport::run_named_step(std::string_view step) {
    return join({"run", step});
}

std::vector<std::string> WorkflowSupport::sweep_paths(const std::filesystem::path& root, std::size_t limit) {
    std::vector<std::string> paths;
    if (!std::filesystem::exists(root)) {
        return paths;
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        paths.push_back(entry.path().string());
        if (paths.size() >= limit) {
            break;
        }
    }
    return paths;
}

std::string WorkflowSupport::hold_temp(std::string_view label) {
    return join({"hold-temp", label});
}

std::string WorkflowSupport::call_subroutine(std::string_view name) {
    return join({"call", name});
}

std::string WorkflowSupport::parse_input(std::string_view text) {
    return join({"parse", text});
}

bool WorkflowSupport::match_values(std::string_view lhs, std::string_view rhs) {
    return lhs == rhs;
}

std::vector<std::string> WorkflowSupport::narrow_candidates(const std::vector<std::string>& input, std::size_t max_count) {
    if (input.size() <= max_count) {
        return input;
    }
    return std::vector<std::string>(input.begin(), input.begin() + static_cast<std::ptrdiff_t>(max_count));
}

std::string WorkflowSupport::ensure_directory(const std::filesystem::path& path) {
    std::filesystem::create_directories(path);
    return path.string();
}

std::string WorkflowSupport::move_index(std::string_view label) {
    return join({"move-index", label});
}

std::vector<std::string> WorkflowSupport::search_filesystem_branch(const std::filesystem::path& root, std::string_view pattern) {
    return locate_paths(root, pattern);
}

std::string WorkflowSupport::seek_index(std::string_view key) {
    return join({"seek-index", key});
}

std::vector<std::string> WorkflowSupport::seek_parameters(std::initializer_list<std::string_view> parameters) {
    std::vector<std::string> values;
    for (std::string_view parameter : parameters) {
        values.emplace_back(parameter);
    }
    return values;
}

std::string WorkflowSupport::map_namespace(std::string_view name) {
    return join({"map-namespace", name});
}

std::string WorkflowSupport::map_value(std::string_view name, std::string_view value) {
    return std::string(name) + "=" + std::string(value);
}

std::string WorkflowSupport::results_view(std::string_view label) {
    return join({"results", label});
}

std::string WorkflowSupport::file_state(const std::filesystem::path& path) {
    return std::filesystem::exists(path) ? "present" : "missing";
}

std::string WorkflowSupport::index_marker(std::string_view label) {
    return join({"index-mark", label});
}

std::string WorkflowSupport::record_observation(std::string_view label) {
    return join({"know", label});
}

std::string WorkflowSupport::base_namespace() {
    return "xBase";
}

std::string WorkflowSupport::calculate_ruling(std::string_view label) {
    return join({"ruling", label});
}

std::string WorkflowSupport::generated_key_budget(std::string_view label) {
    return join({"genx-budget", label});
}

}  // namespace tze
