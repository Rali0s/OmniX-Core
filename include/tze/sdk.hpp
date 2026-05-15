#pragma once

#include <filesystem>
#include <string_view>

#include "tze/processing_engine.hpp"

namespace tze {

class TzeClient {
public:
    ProcessingReport run(const RequestProfile& profile) const;
    ProcessingReport replay_latest(const std::filesystem::path& memory_root = {}) const;

private:
    ProcessingEngine engine_;
};

class AssistClient {
public:
    ProcessingReport route_prompt(std::string_view prompt,
                                  const std::filesystem::path& memory_root = {},
                                  bool assist = true) const;
    ProcessingReport next_step(std::string_view prompt,
                               std::string_view deterministic_guidance,
                               const std::filesystem::path& memory_root = {},
                               bool assist = true) const;

private:
    ProcessingEngine engine_;
};

class ReviewClient {
public:
    ProcessingReport review(std::string_view target,
                            const std::filesystem::path& memory_root = {},
                            bool assist = false) const;
    ProcessingReport patch_proposal(std::string_view target,
                                    const std::filesystem::path& memory_root = {},
                                    bool assist = false) const;

private:
    ProcessingEngine engine_;
};

}  // namespace tze
