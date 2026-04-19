#pragma once

#include <filesystem>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace tze {

class WorkflowSupport {
public:
    static bool check_condition(std::string_view label);
    static std::string display_text(std::string_view text);
    static std::vector<std::string> locate_paths(const std::filesystem::path& root, std::string_view needle = {});
    static std::string run_named_step(std::string_view step);
    static std::vector<std::string> sweep_paths(const std::filesystem::path& root, std::size_t limit = 16);
    static std::string hold_temp(std::string_view label);
    static std::string call_subroutine(std::string_view name);
    static std::string parse_input(std::string_view text);
    static bool match_values(std::string_view lhs, std::string_view rhs);
    static std::vector<std::string> narrow_candidates(const std::vector<std::string>& input, std::size_t max_count);
    static std::string ensure_directory(const std::filesystem::path& path);
    static std::string move_index(std::string_view label);
    static std::vector<std::string> search_filesystem_branch(const std::filesystem::path& root, std::string_view pattern = {});
    static std::string seek_index(std::string_view key);
    static std::vector<std::string> seek_parameters(std::initializer_list<std::string_view> parameters);
    static std::string map_namespace(std::string_view name);
    static std::string map_value(std::string_view name, std::string_view value);
    static std::string results_view(std::string_view label);
    static std::string file_state(const std::filesystem::path& path);
    static std::string index_marker(std::string_view label);
    static std::string record_observation(std::string_view label);
    static std::string base_namespace();
    static std::string calculate_ruling(std::string_view label);
    static std::string generated_key_budget(std::string_view label);
};

}  // namespace tze
