#include "tze/build_flow_interpreter.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace tze {
namespace {

std::string now_timestamp() {
    using clock = std::chrono::system_clock;
    const std::time_t raw = clock::to_time_t(clock::now());
    std::tm local{};
#if defined(__APPLE__) || defined(__unix__)
    localtime_r(&raw, &local);
#else
    local = *std::localtime(&raw);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &local);
    return buffer;
}

int clamp_score(int value) {
    return std::max(0, std::min(100, value));
}

NativeToolRecord native_record_from_resolution(const ToolResolution& resolution) {
    NativeToolRecord record;
    record.logical_name = resolution.logical_name;
    record.provider_type = resolution.provider_type;
    record.executable_path = resolution.executable_path;
    record.applet_name = resolution.applet_name;
    record.version_fingerprint = resolution.version_fingerprint;
    record.capability_flags = resolution.capability_flags;
    record.environment_signature = resolution.environment_signature;
    record.discovery_origin = resolution.cache_origin;
    record.last_verified = now_timestamp();
    std::error_code ec;
    if (!resolution.executable_path.empty()) {
        const auto size = std::filesystem::file_size(resolution.executable_path, ec);
        record.size_bytes = ec ? 0 : size;
        ec.clear();
        const auto write_time = std::filesystem::last_write_time(resolution.executable_path, ec);
        record.modified_timestamp = ec
            ? 0
            : static_cast<long long>(
                  std::chrono::duration_cast<std::chrono::seconds>(write_time.time_since_epoch()).count());
    }
    return record;
}

void push_unique_string(std::vector<std::string>& values, std::string value) {
    if (value.empty()) {
        return;
    }
    if (std::find(values.begin(), values.end(), value) == values.end()) {
        values.push_back(std::move(value));
    }
}

std::vector<std::string> native_tool_candidates_for_alias(const ProjectAlias& alias,
                                                          const NativeToolRegistry& tools) {
    std::vector<std::string> candidates;
    const auto add_candidate = [&](std::string_view value) {
        const std::string canonical = tools.canonical_name(value);
        if (tools.is_known_tool(canonical)) {
            push_unique_string(candidates, canonical);
        }
    };

    add_candidate(alias.canonical_name);
    for (const std::string& value : alias.aliases) {
        add_candidate(value);
    }
    return candidates;
}

std::string native_tool_verify_command(std::string_view logical_name) {
    if (logical_name == "nmap") {
        return "omnix tool nmap -- -V";
    }
    return "omnix tool " + std::string(logical_name) + " -- --version";
}

std::vector<std::string> recent_history(const MemorySnapshot& memory,
                                        std::string_view canonical_name,
                                        std::string_view environment_key) {
    std::vector<std::string> matches;
    for (auto it = memory.history.rbegin(); it != memory.history.rend(); ++it) {
        if (!canonical_name.empty() && it->project != canonical_name) {
            continue;
        }
        std::string line = it->timestamp + " | " + it->status + " | " + it->summary;
        if (!environment_key.empty()) {
            line += " | " + std::string(environment_key);
        }
        matches.push_back(std::move(line));
        if (matches.size() >= 5) {
            break;
        }
    }
    return matches;
}

const LearnedRecipeRecord* best_recipe_record(const MemorySnapshot& memory,
                                              const ProjectAlias& alias,
                                              std::string_view environment_key) {
    const LearnedRecipeRecord* best = nullptr;
    for (const LearnedRecipeRecord& record : memory.learned_recipes) {
        if (record.canonical_name != alias.canonical_name || record.environment_key != environment_key) {
            continue;
        }
        if (best == nullptr || record.confidence_score > best->confidence_score) {
            best = &record;
        }
    }
    return best;
}

std::string selection_reason_for_learned(const LearnedRecipeRecord& record) {
    return "learned_recipe(" + record.recipe_id + ", confidence=" + std::to_string(record.confidence_score) + ")";
}

std::vector<std::string> learned_recipe_summaries(const MemorySnapshot& memory,
                                                  const ProjectAlias& alias,
                                                  std::string_view environment_key) {
    std::vector<std::string> summaries;
    for (const LearnedRecipeRecord& record : memory.learned_recipes) {
        if (record.canonical_name != alias.canonical_name || record.environment_key != environment_key) {
            continue;
        }
        summaries.push_back(
            record.recipe_id + " | status=" + record.last_status +
            " | success=" + std::to_string(record.success_count) +
            " | failure=" + std::to_string(record.failure_count) +
            " | confidence=" + std::to_string(record.confidence_score));
    }
    return summaries;
}

std::string selection_reason_for_assist(const BuildAssistPlan& plan) {
    std::ostringstream out;
    out << "assist_selected(" << plan.selected_recipe_id;
    if (plan.confidence >= 0.0) {
        out << ", confidence=" << std::fixed << std::setprecision(2) << plan.confidence;
    }
    out << ")";
    return out.str();
}

bool validate_build_assist_plan(const BuildAssistPlan& proposed,
                                const PreflightReport& preflight,
                                BuildAssistPlan* validated_plan,
                                std::string* reason) {
    if (proposed.selected_recipe_id.empty()) {
        if (reason != nullptr) {
            *reason = "Build assist did not select a recipe id.";
        }
        return false;
    }
    if (proposed.confidence < 0.0 || proposed.confidence > 1.0) {
        if (reason != nullptr) {
            *reason = "Build assist confidence must be between 0.0 and 1.0.";
        }
        return false;
    }
    if (std::find(preflight.available_recipe_ids.begin(),
                  preflight.available_recipe_ids.end(),
                  proposed.selected_recipe_id) == preflight.available_recipe_ids.end()) {
        if (reason != nullptr) {
            *reason = "Build assist selected recipe `" + proposed.selected_recipe_id +
                "`, which is not in the alias allowlist.";
        }
        return false;
    }
    if (!proposed.fallback_recipe_id.empty() &&
        std::find(preflight.available_recipe_ids.begin(),
                  preflight.available_recipe_ids.end(),
                  proposed.fallback_recipe_id) == preflight.available_recipe_ids.end()) {
        if (reason != nullptr) {
            *reason = "Build assist selected fallback recipe `" + proposed.fallback_recipe_id +
                "`, which is not in the alias allowlist.";
        }
        return false;
    }
    if (!proposed.fallback_recipe_id.empty() && proposed.fallback_recipe_id == proposed.selected_recipe_id) {
        if (reason != nullptr) {
            *reason = "Build assist fallback recipe must differ from the selected recipe.";
        }
        return false;
    }

    if (validated_plan != nullptr) {
        *validated_plan = proposed;
        validated_plan->validated = true;
        validated_plan->status = "validated";
    }
    if (reason != nullptr) {
        *reason = "Validated assist-selected recipe `" + proposed.selected_recipe_id + "` against " +
            std::to_string(preflight.available_recipe_ids.size()) + " allowlisted recipe(s).";
    }
    return true;
}

LearnedRecipeRecord update_recipe_record(const LearnedRecipeRecord* existing,
                                         std::string_view canonical_name,
                                         std::string_view recipe_id,
                                         std::string_view environment_key,
                                         std::string_view build_system,
                                         const BuildExecution& execution) {
    LearnedRecipeRecord updated;
    if (existing != nullptr) {
        updated = *existing;
    } else {
        updated.canonical_name = std::string(canonical_name);
        updated.recipe_id = std::string(recipe_id);
        updated.environment_key = std::string(environment_key);
        updated.build_system = std::string(build_system);
        updated.confidence_score = 50;
    }

    updated.canonical_name = std::string(canonical_name);
    updated.recipe_id = std::string(recipe_id);
    updated.environment_key = std::string(environment_key);
    updated.build_system = std::string(build_system);
    updated.last_status = execution.status;
    updated.last_artifact = execution.artifact_hint;
    updated.last_install_prefix = execution.install_prefix;

    const bool verified_success = execution.status == "built" || execution.status == "installed";
    if (verified_success) {
        ++updated.success_count;
        updated.last_success_at = now_timestamp();
        updated.confidence_score = clamp_score(updated.confidence_score + 15 + 10);
    } else {
        ++updated.failure_count;
        updated.last_failure_at = now_timestamp();
        updated.confidence_score = clamp_score(updated.confidence_score - 20 - 10);
    }
    return updated;
}

}  // namespace

void BuildFlowInterpreter::run(const RequestProfile& profile,
                               MemorySnapshot& memory,
                               ProcessingReport& report,
                               const BuildExecutor& builder,
                               const CacheCoordinator& cache,
                               const KnowledgeEngine& knowledge,
                               const MemoryStore& memory_store,
                               const NativeToolRegistry& tools,
                               const ProjectAliasRegistry& aliases,
                               const ProjectResolver& projects,
                               const ReasoningProvider& provider,
                               const SecurityManager& security) const {
    (void)cache;
    (void)knowledge;
    report.security = security.verify(profile);

    const std::string target = !profile.project_reference.empty()
        ? profile.project_reference
        : (!profile.raw_prompt.empty() ? profile.raw_prompt : profile.build_source_path);

    ProjectResolution project = projects.resolve(target, profile, memory, builder, aliases);
    if (project.alias.has_value()) {
        report.resolved_project = project.alias->canonical_name;
    } else if (!project.canonical_name.empty()) {
        report.resolved_project = project.canonical_name;
    } else {
        report.resolved_project = target;
    }
    if (project.acquisition.has_value()) {
        report.acquisition_result = project.acquisition;
    }

    if (profile.selected_recipe_id.empty() &&
        !target.empty() &&
        !std::filesystem::exists(target) &&
        project.alias.has_value()) {
        for (const std::string& native_candidate : native_tool_candidates_for_alias(*project.alias, tools)) {
            const ToolResolution native_tool = tools.resolve(native_candidate, memory, true);
            if (!native_tool.found) {
                continue;
            }

            report.resolved_project = project.alias->canonical_name;
            report.tool_resolution = native_tool;
            report.feedback_loop = recent_history(memory, project.alias->canonical_name, native_tool.environment_signature);

            PreflightReport preflight;
            preflight.status = "native_ready";
            preflight.summary = "Verified native `" + native_tool.logical_name +
                "` provider found; OmniX will reuse it instead of building `" + project.alias->canonical_name + "` from source.";
            preflight.canonical_project_name = project.alias->canonical_name;
            preflight.recipe_selection_reason = "native_provider(" + native_tool.logical_name + ":" + native_tool.executable_path + ")";
            preflight.environment_signature = native_tool.environment_signature;
            preflight.ready = true;
            preflight.platform_supported = true;
            preflight.expected_steps = {
                "validate cached/native " + native_tool.logical_name,
                "reuse native " + native_tool.logical_name,
            };
            preflight.expected_artifacts = {native_tool.executable_path};
            report.preflight_report = preflight;

            report.answer_status = "native_ready";
            report.answer_explanation = preflight.summary;
            report.produced_artifact = native_tool.executable_path;
            report.next_action = "Use `" + native_tool_verify_command(native_tool.logical_name) + "` or run the native binary directly.";
            report.storage_writes.push_back("x.Store(native." + native_tool.logical_name + " -> " + native_tool.executable_path + ")");

            memory_store.remember_native_tool(memory, native_record_from_resolution(native_tool));
            report.memory_writes.push_back(memory.paths.native_tools_path.string());
            return;
        }
    }

    RequestProfile effective_profile = profile;
    effective_profile.project_alias = report.resolved_project;
    effective_profile.execute_build = true;

    report.preflight_report = builder.preflight(effective_profile,
                                                project.alias,
                                                project.resolved ? project.source_path : std::filesystem::path{});

    if (!profile.selected_recipe_id.empty()) {
        report.preflight_report->recipe_selection_reason = "manual_override(" + profile.selected_recipe_id + ")";
    } else if (profile.assist_requested && project.alias.has_value() && !report.preflight_report->available_recipe_ids.empty()) {
        const BuildAssistRequest assist_request = {
            "build_recipe",
            report.resolved_project,
            profile.raw_prompt,
            report.preflight_report->build_system,
            report.preflight_report->environment_signature,
            "native_absent_or_not_applicable",
            report.preflight_report->will_acquire,
            report.preflight_report->will_install,
            report.preflight_report->available_recipe_ids,
            learned_recipe_summaries(memory, *project.alias, report.preflight_report->environment_signature),
        };
        std::optional<BuildAssistPlan> proposed_plan = provider.propose_build_recipe(assist_request);
        if (proposed_plan.has_value()) {
            report.build_assist_plan = proposed_plan;
            report.tze_stages.push_back({
                "x.Assist.BuildPlan",
                "Ask the provider for an allowlisted build-recipe selection",
                "ReasoningProvider",
                "assist_planned",
                "Provider `" + std::string(provider.id()) + "` proposed recipe `" +
                    proposed_plan->selected_recipe_id + "` for `" + report.resolved_project + "`.",
                {report.resolved_project, report.preflight_report->environment_signature},
                {proposed_plan->selected_recipe_id},
            });

            BuildAssistPlan validated_plan;
            std::string validation_detail;
            if (validate_build_assist_plan(*proposed_plan,
                                           *report.preflight_report,
                                           &validated_plan,
                                           &validation_detail)) {
                report.assist_status = "assist_used";
                report.build_assist_plan = validated_plan;
                effective_profile.selected_recipe_id = validated_plan.selected_recipe_id;
                report.tze_stages.push_back({
                    "x.Assist.BuildValidate",
                    "Validate the provider-proposed build recipe against Omni guardrails",
                    "BuildFlowInterpreter",
                    "assist_validated",
                    validation_detail,
                    {validated_plan.selected_recipe_id},
                    validated_plan.fallback_recipe_id.empty()
                        ? std::vector<std::string>{validated_plan.selected_recipe_id}
                        : std::vector<std::string>{validated_plan.selected_recipe_id, validated_plan.fallback_recipe_id},
                });
                report.preflight_report = builder.preflight(effective_profile,
                                                            project.alias,
                                                            project.resolved ? project.source_path : std::filesystem::path{});
                report.preflight_report->recipe_selection_reason = selection_reason_for_assist(validated_plan);
            } else {
                BuildAssistPlan rejected_plan = *proposed_plan;
                rejected_plan.status = "rejected";
                if (report.assist_status.empty()) {
                    report.assist_status = "assist_bypassed";
                }
                report.build_assist_plan = rejected_plan;
                report.tze_stages.push_back({
                    "x.Assist.BuildValidate",
                    "Validate the provider-proposed build recipe against Omni guardrails",
                    "BuildFlowInterpreter",
                    "assist_rejected",
                    validation_detail,
                    {proposed_plan->selected_recipe_id},
                    {},
                });
            }
        } else {
            if (report.assist_status.empty()) {
                report.assist_status = "assist_bypassed";
            }
            report.tze_stages.push_back({
                "x.Assist.BuildPlan",
                "Ask the provider for an allowlisted build-recipe selection",
                "ReasoningProvider",
                "assist_bypassed",
                "Provider `" + std::string(provider.id()) + "` did not return a validated build-recipe suggestion.",
                {report.resolved_project},
                {},
            });
        }
    }

    if (effective_profile.selected_recipe_id.empty() && project.alias.has_value()) {
        const LearnedRecipeRecord* learned = best_recipe_record(memory,
                                                                *project.alias,
                                                                report.preflight_report->environment_signature);
        if (learned != nullptr && learned->confidence_score > 0) {
            effective_profile.selected_recipe_id = learned->recipe_id;
            report.preflight_report = builder.preflight(effective_profile,
                                                        project.alias,
                                                        project.resolved ? project.source_path : std::filesystem::path{});
            report.preflight_report->recipe_selection_reason = selection_reason_for_learned(*learned);
        } else if (!report.preflight_report->recipe_id.empty()) {
            report.preflight_report->recipe_selection_reason =
                "alias_default(" + report.preflight_report->recipe_id + ")";
        }
    } else if (effective_profile.selected_recipe_id.empty() && !report.preflight_report->recipe_id.empty()) {
        report.preflight_report->recipe_selection_reason =
            "default(" + report.preflight_report->recipe_id + ")";
    }

    report.feedback_loop = recent_history(memory,
                                          report.resolved_project,
                                          report.preflight_report->environment_signature);

    if (profile.preflight_only) {
        report.answer_status = report.preflight_report->status;
        report.answer_explanation = report.preflight_report->summary;
        report.next_action = report.preflight_report->ready
            ? "Run `omnix build <project>` or `omnix ask \"Build <project>\"` to execute the portable recipe."
            : "Resolve the reported preflight blockers before building.";
        return;
    }

    if (!report.preflight_report->ready) {
        report.answer_status = report.preflight_report->status;
        report.answer_explanation = report.preflight_report->summary;
        report.next_action = "Resolve the preflight blockers and retry the build.";
        return;
    }

    if (!project.resolved) {
        if (project.alias.has_value()) {
            const AcquisitionResult acquisition = builder.acquire_source(*project.alias, effective_profile);
            report.acquisition_result = acquisition;
            if (acquisition.status == "acquired" || acquisition.status == "reused_workspace") {
                project.resolved = true;
                project.source_path = acquisition.resolved_source_path;
            } else {
                report.answer_status = acquisition.status;
                report.answer_explanation = acquisition.summary;
                report.next_action = "Fix source acquisition and rerun the portable build.";
                return;
            }
        } else {
            report.answer_status = report.acquisition_result.has_value()
                ? report.acquisition_result->status
                : "unresolved_project";
            report.answer_explanation = report.acquisition_result.has_value()
                ? report.acquisition_result->summary
                : "The requested project could not be resolved as a local path or bundled alias.";
            report.next_action = "Provide a local path or use one of the bundled aliases.";
            return;
        }
    }

    report.resolved_project_path = project.source_path.string();
    report.source_inspection = builder.inspect_source(project.source_path);
    effective_profile.build_source_path = project.source_path.string();

    if (effective_profile.selected_recipe_id.empty() && report.preflight_report.has_value()) {
        effective_profile.selected_recipe_id = report.preflight_report->recipe_id;
    }

    report.build_execution = builder.build_source(effective_profile, project.alias);
    if (report.build_execution.has_value() && report.preflight_report.has_value()) {
        report.build_execution->recipe_selection_reason = report.preflight_report->recipe_selection_reason;
    }
    const BuildExecution& build = *report.build_execution;
    report.answer_status = build.status;
    report.answer_explanation = build.summary;
    report.produced_artifact = build.artifact_hint;
    report.next_action = (build.status == "built" || build.status == "installed")
        ? "Run the verified artifact or inspect the staged install prefix."
        : "Inspect the preflight, build log, or install prefix details before retrying.";

    report.storage_writes.push_back("x.Store(build.log -> " + build.log_path + ")");
    if (!build.artifact_hint.empty()) {
        report.storage_writes.push_back("x.Store(build.artifact -> " + build.artifact_hint + ")");
    }
    if (!build.install_prefix.empty()) {
        report.storage_writes.push_back("x.Store(build.install_prefix -> " + build.install_prefix + ")");
    }

    memory_store.remember_project(memory,
                                  report.resolved_project,
                                  report.resolved_project_path,
                                  report.source_inspection.has_value() ? report.source_inspection->build_system : build.build_system,
                                  build.status,
                                  project.alias.has_value() ? project.alias->upstream_url : std::string());
    report.memory_writes.push_back(memory.paths.projects_path.string());

    if (project.alias.has_value() && !build.selected_recipe_id.empty() && !build.environment_signature.empty()) {
        const LearnedRecipeRecord* existing = best_recipe_record(memory,
                                                                 *project.alias,
                                                                 build.environment_signature);
        const LearnedRecipeRecord updated = update_recipe_record(existing,
                                                                 report.resolved_project,
                                                                 build.selected_recipe_id,
                                                                 build.environment_signature,
                                                                 build.build_system,
                                                                 build);
        memory_store.remember_recipe_result(memory, updated);
        report.memory_writes.push_back(memory.paths.projects_path.string());
    }
}

}  // namespace tze
