#pragma once

#include <filesystem>
#include <vector>

#include "tze/types.hpp"

namespace tze {

class BuildExecutor {
public:
    std::vector<ToolchainModuleStatus> probe_modules() const;
    PreflightReport preflight(const RequestProfile& profile,
                              const std::optional<ProjectAlias>& alias = std::nullopt,
                              const std::filesystem::path& source_path = {}) const;
    DoctorReport doctor(const RequestProfile& profile,
                        const std::optional<ProjectAlias>& alias = std::nullopt,
                        const std::filesystem::path& source_path = {}) const;
    AcquisitionResult acquire_source(const ProjectAlias& alias, const RequestProfile& profile) const;
    SourceInspection inspect_source(const std::filesystem::path& source_path) const;
    BuildExecution build_source(const RequestProfile& profile,
                                const std::optional<ProjectAlias>& alias = std::nullopt) const;
};

}  // namespace tze
