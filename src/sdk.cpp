#include "tze/sdk.hpp"

namespace tze {

ProcessingReport TzeClient::run(const RequestProfile& profile) const {
    return engine_.process(profile);
}

ProcessingReport TzeClient::replay_latest(const std::filesystem::path& memory_root) const {
    RequestProfile profile;
    profile.raw_prompt = "tze latest";
    profile.memory_root_path = memory_root.string();
    profile.resolved_intent = RequestIntent::ReplayTzeRun;
    profile.source_map_path.clear();
    return engine_.process(profile);
}

ProcessingReport AssistClient::route_prompt(std::string_view prompt,
                                            const std::filesystem::path& memory_root,
                                            bool assist) const {
    RequestProfile profile;
    profile.raw_prompt = std::string(prompt);
    profile.memory_root_path = memory_root.string();
    profile.assist_requested = assist;
    return engine_.process(profile);
}

ProcessingReport AssistClient::next_step(std::string_view prompt,
                                         std::string_view deterministic_guidance,
                                         const std::filesystem::path& memory_root,
                                         bool assist) const {
    RequestProfile profile;
    profile.raw_prompt = std::string(prompt) + "\nDeterministic guidance: " + std::string(deterministic_guidance);
    profile.memory_root_path = memory_root.string();
    profile.assist_requested = assist;
    profile.resolved_intent = RequestIntent::Conversation;
    return engine_.process(profile);
}

ProcessingReport ReviewClient::review(std::string_view target,
                                      const std::filesystem::path& memory_root,
                                      bool assist) const {
    RequestProfile profile;
    profile.raw_prompt = "review " + std::string(target);
    profile.review_target = std::string(target);
    profile.memory_root_path = memory_root.string();
    profile.assist_requested = assist;
    profile.resolved_intent = RequestIntent::ReviewModule;
    return engine_.process(profile);
}

ProcessingReport ReviewClient::patch_proposal(std::string_view target,
                                              const std::filesystem::path& memory_root,
                                              bool assist) const {
    RequestProfile profile;
    profile.raw_prompt = "patch-proposal " + std::string(target);
    profile.review_target = std::string(target);
    profile.memory_root_path = memory_root.string();
    profile.assist_requested = assist;
    profile.resolved_intent = RequestIntent::PatchProposal;
    return engine_.process(profile);
}

}  // namespace tze
