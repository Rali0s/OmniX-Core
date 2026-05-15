#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include "tze/types.hpp"

namespace tze {

class SelfReviewEngine {
public:
    ReviewArtifact review_target(std::string_view target,
                                 const MemorySnapshot& memory,
                                 const std::filesystem::path& project_root = {}) const;

    PatchProposalArtifact propose_patch(std::string_view target,
                                        const MemorySnapshot& memory,
                                        const ReviewArtifact& review,
                                        const std::filesystem::path& project_root = {}) const;

private:
    std::optional<std::filesystem::path> resolve_target(std::string_view target,
                                                        const std::filesystem::path& project_root) const;
};

}  // namespace tze
