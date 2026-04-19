#pragma once

#include <filesystem>
#include <optional>
#include <string_view>

#include "tze/build_executor.hpp"
#include "tze/project_alias_registry.hpp"
#include "tze/types.hpp"

namespace tze {

struct ProjectResolution {
    bool resolved = false;
    std::string canonical_name;
    std::filesystem::path source_path;
    std::optional<ProjectAlias> alias;
    std::optional<AcquisitionResult> acquisition;
};

class ProjectResolver {
public:
    ProjectResolver() = default;

    ProjectResolution resolve(std::string_view project_or_path,
                              const RequestProfile& profile,
                              const MemorySnapshot& memory,
                              const BuildExecutor& builder,
                              const ProjectAliasRegistry& aliases) const;
};

}  // namespace tze
