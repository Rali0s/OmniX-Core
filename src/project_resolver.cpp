#include "tze/project_resolver.hpp"

#include <algorithm>
#include <filesystem>
#include <string>

namespace tze {
namespace {

bool looks_like_path(std::string_view value) {
    return value.find('/') != std::string::npos || value.find('.') != std::string::npos || value.find('\\') != std::string::npos;
}

std::filesystem::path find_nearby_project(std::string_view name, const std::filesystem::path& start) {
    if (name.empty()) {
        return {};
    }

    const std::vector<std::filesystem::path> direct_candidates = {
        start / std::string(name),
        start.parent_path() / std::string(name),
        start / "repos" / std::string(name),
        start / "workspace" / std::string(name),
    };
    for (const std::filesystem::path& candidate : direct_candidates) {
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec) && std::filesystem::is_directory(candidate, ec)) {
            return candidate;
        }
    }

    std::error_code ec;
    if (!std::filesystem::exists(start, ec)) {
        return {};
    }
    for (const auto& entry : std::filesystem::directory_iterator(start, ec)) {
        if (ec || !entry.is_directory()) {
            continue;
        }
        if (entry.path().filename() == name) {
            return entry.path();
        }
    }
    return {};
}

}  // namespace

ProjectResolution ProjectResolver::resolve(std::string_view project_or_path,
                                           const RequestProfile& profile,
                                           const MemorySnapshot& memory,
                                           const BuildExecutor& builder,
                                           const ProjectAliasRegistry& aliases) const {
    ProjectResolution resolution;
    const std::string query(project_or_path);
    const std::filesystem::path cwd = std::filesystem::current_path();

    if (!query.empty() && (looks_like_path(query) || std::filesystem::exists(query))) {
        const std::filesystem::path path = query;
        if (std::filesystem::exists(path)) {
            resolution.resolved = true;
            resolution.canonical_name = path.filename().string();
            resolution.source_path = path;
            return resolution;
        }
    }

    const std::optional<ProjectAlias> alias = aliases.find(query);
    if (!alias.has_value()) {
        return resolution;
    }

    resolution.alias = alias;
    resolution.canonical_name = alias->canonical_name;

    for (const ProjectRecord& record : memory.projects) {
        if (record.canonical_name != alias->canonical_name) {
            continue;
        }
        if (!record.resolved_source_path.empty() && std::filesystem::exists(record.resolved_source_path)) {
            resolution.resolved = true;
            resolution.source_path = record.resolved_source_path;
            resolution.acquisition = AcquisitionResult{
                "cached_project",
                "Reused the previously resolved project source from local memory.",
                alias->canonical_name,
                record.resolved_source_path,
                record.upstream_url,
                false,
                {},
            };
            return resolution;
        }
    }

    const std::filesystem::path nearby = find_nearby_project(alias->canonical_name, cwd);
    if (!nearby.empty()) {
        resolution.resolved = true;
        resolution.source_path = nearby;
        resolution.acquisition = AcquisitionResult{
            "nearby_project",
            "Found a nearby working tree that matches the requested project alias.",
            alias->canonical_name,
            nearby.string(),
            alias->upstream_url,
            false,
            {},
        };
        return resolution;
    }

    if (profile.offline || profile.acquisition_policy == AcquisitionPolicy::LocalOnly) {
        resolution.acquisition = AcquisitionResult{
            "local_only_miss",
            "The requested project alias was not found locally and acquisition is disabled.",
            alias->canonical_name,
            {},
            alias->upstream_url,
            false,
            {},
        };
        return resolution;
    }

    (void)builder;
    resolution.acquisition = AcquisitionResult{
        "pending_acquisition",
        "The requested project alias can be acquired into the managed OmniX workspace after preflight succeeds.",
        alias->canonical_name,
        {},
        alias->upstream_url,
        false,
        {},
    };
    return resolution;
}

}  // namespace tze
