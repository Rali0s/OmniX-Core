#pragma once

#include <optional>
#include <string_view>
#include <vector>

#include "tze/types.hpp"

namespace tze {

class ProjectAliasRegistry {
public:
    const std::vector<ProjectAlias>& aliases() const;
    std::optional<ProjectAlias> find(std::string_view name) const;
    std::vector<std::string> suggest(std::string_view query) const;
    const BuildRecipe* select_recipe(const ProjectAlias& alias,
                                     std::string_view platform,
                                     std::string_view requested_recipe_id = {}) const;
};

}  // namespace tze
