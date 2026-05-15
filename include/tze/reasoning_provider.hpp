#pragma once

#include <optional>
#include <memory>
#include <string>
#include <string_view>

#include "tze/types.hpp"

namespace tze {

class ReasoningProvider {
public:
    virtual ~ReasoningProvider() = default;

    virtual std::string_view id() const = 0;
    virtual bool configured() const = 0;
    virtual bool available() const = 0;
    virtual ProviderProbeReport probe() const = 0;
    virtual std::optional<AssistAnnotation> assist_annotation(const AssistRequest& request) const = 0;
    virtual std::optional<ToolAssistPlan> propose_tool_action(
        std::string_view prompt,
        const std::vector<std::string>& allowlisted_tools) const = 0;
    virtual std::optional<BuildAssistPlan> propose_build_recipe(const BuildAssistRequest& request) const = 0;
    virtual std::optional<RecipeAuthoringPlan> propose_authored_recipe(const RecipeAuthoringRequest& request) const = 0;
    virtual std::optional<CommandAssistPlan> propose_command_route(
        std::string_view prompt,
        const std::vector<std::string>& allowlisted_commands) const = 0;
    virtual std::optional<NextStepAssistPlan> propose_next_step(
        std::string_view prompt,
        std::string_view deterministic_guidance) const = 0;
    virtual std::optional<CaseSummaryAssistPlan> propose_case_summary(
        std::string_view target_label,
        std::string_view deterministic_summary) const = 0;
    virtual std::optional<FreeformAssistAnswer> resolve_freeform(std::string_view prompt) const = 0;
};

class NullProvider final : public ReasoningProvider {
public:
    std::string_view id() const override;
    bool configured() const override;
    bool available() const override;
    ProviderProbeReport probe() const override;
    std::optional<AssistAnnotation> assist_annotation(const AssistRequest& request) const override;
    std::optional<ToolAssistPlan> propose_tool_action(
        std::string_view prompt,
        const std::vector<std::string>& allowlisted_tools) const override;
    std::optional<BuildAssistPlan> propose_build_recipe(const BuildAssistRequest& request) const override;
    std::optional<RecipeAuthoringPlan> propose_authored_recipe(const RecipeAuthoringRequest& request) const override;
    std::optional<CommandAssistPlan> propose_command_route(
        std::string_view prompt,
        const std::vector<std::string>& allowlisted_commands) const override;
    std::optional<NextStepAssistPlan> propose_next_step(
        std::string_view prompt,
        std::string_view deterministic_guidance) const override;
    std::optional<CaseSummaryAssistPlan> propose_case_summary(
        std::string_view target_label,
        std::string_view deterministic_summary) const override;
    std::optional<FreeformAssistAnswer> resolve_freeform(std::string_view prompt) const override;
};

class OllamaProvider final : public ReasoningProvider {
public:
    OllamaProvider(std::string base_url, std::string model);

    std::string_view id() const override;
    bool configured() const override;
    bool available() const override;
    ProviderProbeReport probe() const override;
    std::optional<AssistAnnotation> assist_annotation(const AssistRequest& request) const override;
    std::optional<ToolAssistPlan> propose_tool_action(
        std::string_view prompt,
        const std::vector<std::string>& allowlisted_tools) const override;
    std::optional<BuildAssistPlan> propose_build_recipe(const BuildAssistRequest& request) const override;
    std::optional<RecipeAuthoringPlan> propose_authored_recipe(const RecipeAuthoringRequest& request) const override;
    std::optional<CommandAssistPlan> propose_command_route(
        std::string_view prompt,
        const std::vector<std::string>& allowlisted_commands) const override;
    std::optional<NextStepAssistPlan> propose_next_step(
        std::string_view prompt,
        std::string_view deterministic_guidance) const override;
    std::optional<CaseSummaryAssistPlan> propose_case_summary(
        std::string_view target_label,
        std::string_view deterministic_summary) const override;
    std::optional<FreeformAssistAnswer> resolve_freeform(std::string_view prompt) const override;

private:
    std::string base_url_;
    std::string model_;
};

class OpenAIProvider final : public ReasoningProvider {
public:
    OpenAIProvider(std::string base_url,
                   std::string api_key,
                   std::string model,
                   std::string organization,
                   std::string project);

    std::string_view id() const override;
    bool configured() const override;
    bool available() const override;
    ProviderProbeReport probe() const override;
    std::optional<AssistAnnotation> assist_annotation(const AssistRequest& request) const override;
    std::optional<ToolAssistPlan> propose_tool_action(
        std::string_view prompt,
        const std::vector<std::string>& allowlisted_tools) const override;
    std::optional<BuildAssistPlan> propose_build_recipe(const BuildAssistRequest& request) const override;
    std::optional<RecipeAuthoringPlan> propose_authored_recipe(const RecipeAuthoringRequest& request) const override;
    std::optional<CommandAssistPlan> propose_command_route(
        std::string_view prompt,
        const std::vector<std::string>& allowlisted_commands) const override;
    std::optional<NextStepAssistPlan> propose_next_step(
        std::string_view prompt,
        std::string_view deterministic_guidance) const override;
    std::optional<CaseSummaryAssistPlan> propose_case_summary(
        std::string_view target_label,
        std::string_view deterministic_summary) const override;
    std::optional<FreeformAssistAnswer> resolve_freeform(std::string_view prompt) const override;

private:
    std::string base_url_;
    std::string api_key_;
    std::string model_;
    std::string organization_;
    std::string project_;
};

class UnsupportedProvider final : public ReasoningProvider {
public:
    explicit UnsupportedProvider(std::string selected_id);

    std::string_view id() const override;
    bool configured() const override;
    bool available() const override;
    ProviderProbeReport probe() const override;
    std::optional<AssistAnnotation> assist_annotation(const AssistRequest& request) const override;
    std::optional<ToolAssistPlan> propose_tool_action(
        std::string_view prompt,
        const std::vector<std::string>& allowlisted_tools) const override;
    std::optional<BuildAssistPlan> propose_build_recipe(const BuildAssistRequest& request) const override;
    std::optional<RecipeAuthoringPlan> propose_authored_recipe(const RecipeAuthoringRequest& request) const override;
    std::optional<CommandAssistPlan> propose_command_route(
        std::string_view prompt,
        const std::vector<std::string>& allowlisted_commands) const override;
    std::optional<NextStepAssistPlan> propose_next_step(
        std::string_view prompt,
        std::string_view deterministic_guidance) const override;
    std::optional<CaseSummaryAssistPlan> propose_case_summary(
        std::string_view target_label,
        std::string_view deterministic_summary) const override;
    std::optional<FreeformAssistAnswer> resolve_freeform(std::string_view prompt) const override;

private:
    std::string selected_id_;
};

std::unique_ptr<ReasoningProvider> make_reasoning_provider_from_env();

}  // namespace tze
