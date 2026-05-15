#include "tze/recipe_authoring_engine.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
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
    std::ostringstream out;
    out << std::put_time(&local, "%Y-%m-%dT%H:%M:%S");
    return out.str();
}

std::string trim(std::string_view value) {
    std::size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start]))) {
        ++start;
    }
    std::size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    return std::string(value.substr(start, end - start));
}

std::string lowercase(std::string_view value) {
    std::string lowered;
    lowered.reserve(value.size());
    for (char c : value) {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return lowered;
}

std::string first_non_empty_line(std::string_view text) {
    std::istringstream input{std::string(text)};
    std::string line;
    while (std::getline(input, line)) {
        line = trim(line);
        if (!line.empty()) {
            return line;
        }
    }
    return {};
}

void push_unique(std::vector<std::string>& values, std::string value) {
    if (value.empty()) {
        return;
    }
    if (std::find(values.begin(), values.end(), value) == values.end()) {
        values.push_back(std::move(value));
    }
}

std::string canonical_project_name_for(const std::filesystem::path& path) {
    const std::string label = path.filename().string();
    return label.empty() ? "project" : label;
}

std::vector<std::string> ranked_evidence_for(const std::filesystem::path& source_root,
                                             const SourceInspection& inspection) {
    std::vector<std::string> ranked;
    const auto add_file = [&](std::string_view label) {
        const std::filesystem::path candidate = source_root / std::string(label);
        std::error_code ec;
        if (!std::filesystem::exists(candidate, ec)) {
            return;
        }
        ranked.push_back("file:" + std::string(label));
    };

    add_file("CMakeLists.txt");
    add_file("Makefile");
    add_file("GNUmakefile");
    add_file("configure");
    add_file("meson.build");
    add_file("package.json");
    add_file("pyproject.toml");

    for (const std::string& module : inspection.recommended_modules) {
        ranked.push_back("module:" + module);
    }
    for (const std::string& module : inspection.missing_modules) {
        ranked.push_back("missing:" + module);
    }

    std::error_code ec;
    for (std::string_view directory_name : {"build", "bin", "lib", "include", "install"}) {
        const std::filesystem::path candidate = source_root / std::string(directory_name);
        if (std::filesystem::exists(candidate, ec) && std::filesystem::is_directory(candidate, ec)) {
            ranked.push_back("layout:" + candidate.filename().string());
        }
        ec.clear();
    }
    if (!inspection.build_system.empty()) {
        ranked.push_back("detected_build_system:" + inspection.build_system);
    }
    return ranked;
}

bool validate_authored_recipe_plan(const RecipeAuthoringPlan& plan,
                                   std::string* reason) {
    if (plan.recipe.id.empty()) {
        if (reason != nullptr) {
            *reason = "Authored recipe must include a non-empty `recipe.id`.";
        }
        return false;
    }
    if (plan.recipe.build_system.empty()) {
        if (reason != nullptr) {
            *reason = "Authored recipe must include a non-empty `recipe.build_system`.";
        }
        return false;
    }
    const std::string build_system = lowercase(plan.recipe.build_system);
    if (build_system != "cmake" && build_system != "configure" && build_system != "make") {
        if (reason != nullptr) {
            *reason = "Authored recipe build system must be one of `cmake`, `configure`, or `make` in this phase.";
        }
        return false;
    }
    if (plan.recipe.artifact_patterns.empty()) {
        if (reason != nullptr) {
            *reason = "Authored recipe must include at least one artifact pattern.";
        }
        return false;
    }
    if (plan.rationale.empty()) {
        if (reason != nullptr) {
            *reason = "Authored recipe plan must include a rationale.";
        }
        return false;
    }
    if (plan.confidence < 0.0 || plan.confidence > 1.0) {
        if (reason != nullptr) {
            *reason = "Authored recipe confidence must be between 0.0 and 1.0.";
        }
        return false;
    }
    if (reason != nullptr) {
        *reason = "Validated authored recipe `" + plan.recipe.id + "` for build system `" + plan.recipe.build_system + "`.";
    }
    return true;
}

ProjectAlias synthetic_alias_for(const std::string& canonical_name, const BuildRecipe& recipe) {
    ProjectAlias alias;
    alias.canonical_name = canonical_name;
    alias.aliases = {canonical_name};
    alias.default_ref = "local";
    alias.recipes = {recipe};
    return alias;
}

AuthoredRecipeRecord authored_record_from_plan(const RecipeAuthoringPlan& plan,
                                               std::string_view resolved_source_path,
                                               std::string_view canonical_name,
                                               const std::vector<std::string>& evidence_summary,
                                               std::string_view authoring_run_id,
                                               bool repair_attempted) {
    AuthoredRecipeRecord record;
    record.recipe = plan.recipe;
    record.resolved_source_path = std::string(resolved_source_path);
    record.canonical_name = std::string(canonical_name);
    record.origin_provider = plan.provider_id;
    record.origin_model = plan.model;
    record.evidence_summary = evidence_summary;
    record.validation_status = "schema_validated";
    record.last_validation_summary = "Schema validation succeeded.";
    record.authoring_run_id = std::string(authoring_run_id);
    record.active_scope = "exact_source_path";
    record.active = true;
    record.repair_attempted = repair_attempted;
    record.persisted_at = now_timestamp();
    return record;
}

RecipeAuthoringArtifact artifact_from_record(const AuthoredRecipeRecord& record) {
    RecipeAuthoringArtifact artifact;
    artifact.id = "recipe-author-" + std::to_string(std::hash<std::string>{}(record.resolved_source_path + "|" + record.recipe.id));
    artifact.source_path = record.resolved_source_path;
    artifact.resolved_source_path = record.resolved_source_path;
    artifact.canonical_project_name = record.canonical_name;
    artifact.provider_id = record.origin_provider;
    artifact.model = record.origin_model;
    artifact.status = record.validation_status;
    artifact.generated_recipe_id = record.recipe.id;
    artifact.generated_build_system = record.recipe.build_system;
    artifact.rationale = record.last_validation_summary;
    artifact.ranked_evidence = record.evidence_summary;
    artifact.validation_feedback = {};
    artifact.repair_attempted = record.repair_attempted;
    artifact.activated = record.active;
    artifact.persisted_at = record.persisted_at;
    return artifact;
}

std::vector<std::string> validation_feedback_for(const PreflightReport* preflight,
                                                 const DoctorReport* doctor,
                                                 const BuildExecution* execution) {
    std::vector<std::string> feedback;
    if (preflight != nullptr) {
        push_unique(feedback, "preflight_status=" + preflight->status);
        push_unique(feedback, preflight->summary);
        for (const std::string& module : preflight->missing_modules) {
            push_unique(feedback, "missing_module:" + module);
        }
    }
    if (doctor != nullptr) {
        push_unique(feedback, "doctor_status=" + doctor->status);
        push_unique(feedback, doctor->summary);
        for (const std::string& check : doctor->dependency_checks) {
            push_unique(feedback, check);
        }
    }
    if (execution != nullptr) {
        push_unique(feedback, "build_status=" + execution->status);
        push_unique(feedback, execution->summary);
        for (const std::string& line : execution->log_excerpt) {
            push_unique(feedback, line);
        }
    }
    return feedback;
}

void remember_run_project(MemorySnapshot& memory,
                          const MemoryStore& memory_store,
                          std::string_view canonical_name,
                          std::string_view resolved_source_path,
                          std::string_view build_system,
                          std::string_view status) {
    memory_store.remember_project(memory, canonical_name, resolved_source_path, build_system, status, {});
}

}  // namespace

void RecipeAuthoringEngine::run(const RequestProfile& profile,
                                MemorySnapshot& memory,
                                ProcessingReport& report,
                                const BuildExecutor& builder,
                                const CacheCoordinator& cache,
                                const MemoryStore& memory_store,
                                const ReasoningProvider& provider,
                                const SecurityManager& security) const {
    (void)cache;
    report.security = security.verify(profile);

    const std::filesystem::path requested_source = profile.project_reference.empty()
        ? std::filesystem::path(profile.build_source_path)
        : std::filesystem::path(profile.project_reference);
    const SourceInspection inspection = builder.inspect_source(requested_source);
    report.source_inspection = inspection;
    report.resolved_project = canonical_project_name_for(requested_source);
    report.resolved_project_path = inspection.resolved_source_path;

    report.tze_stages.push_back({
        "RecipeAuthoring.SourceIntake",
        "SourceIntake",
        "RecipeAuthoringEngine",
        inspection.exists ? "ok" : "failed",
        inspection.summary,
        {requested_source.string()},
        {inspection.resolved_source_path, inspection.build_system},
    });

    if (!inspection.exists) {
        report.answer_status = "recipe_authoring_invalid_source";
        report.answer_explanation = "Recipe authoring is local-path only in this phase, and the requested source path was not found.";
        report.next_action = "Point `omnix recipe author` at an existing local source tree.";
        return;
    }

    const std::vector<std::string> ranked_evidence =
        ranked_evidence_for(std::filesystem::path(inspection.resolved_source_path), inspection);
    report.tze_stages.push_back({
        "RecipeAuthoring.EvidenceRanking",
        "EvidenceRanking",
        "RecipeAuthoringEngine",
        "ok",
        "Ranked source evidence and build-module signals for model-authored recipe generation.",
        {inspection.resolved_source_path},
        ranked_evidence,
    });

    if (provider.id() != std::string_view("ollama")) {
        report.answer_status = "recipe_authoring_provider_unsupported";
        report.answer_explanation = "Recipe authoring currently supports the Ollama provider only.";
        report.next_action = "Set `OMNIX_REASONING_PROVIDER=ollama` and rerun the recipe authoring request.";
        return;
    }
    if (!provider.available() &&
        (std::getenv("OMNIX_OLLAMA_RECIPE_PLAN_FILE") == nullptr ||
         *std::getenv("OMNIX_OLLAMA_RECIPE_PLAN_FILE") == '\0')) {
        report.answer_status = "recipe_authoring_provider_unavailable";
        report.answer_explanation = "Ollama recipe authoring is not ready because the selected local model is unavailable.";
        report.next_action = "Run `omnix provider probe` to verify Ollama readiness, then rerun recipe authoring.";
        return;
    }

    const auto attempt_authoring = [&](const std::vector<std::string>& prior_feedback,
                                       bool repair_attempted,
                                       std::optional<RecipeAuthoringPlan>* accepted_plan,
                                       AuthoredRecipeRecord* record,
                                       std::string* validation_note) {
        RecipeAuthoringRequest request;
        request.task_id = repair_attempted ? "recipe_author_repair" : "recipe_author";
        request.source_path = requested_source.string();
        request.resolved_source_path = inspection.resolved_source_path;
        request.canonical_project_name = report.resolved_project;
        request.detected_build_system = inspection.build_system;
        request.requested_target = profile.build_target;
        request.build_type = profile.build_type;
        request.install_prefix = profile.install_prefix;
        request.detected_files = inspection.detected_files;
        request.recommended_modules = inspection.recommended_modules;
        request.missing_modules = inspection.missing_modules;
        request.ranked_evidence = ranked_evidence;
        request.validation_feedback = prior_feedback;

        std::optional<RecipeAuthoringPlan> plan = provider.propose_authored_recipe(request);
        if (!plan.has_value()) {
            if (validation_note != nullptr) {
                *validation_note = repair_attempted
                    ? "Ollama did not return a repair candidate recipe."
                    : "Ollama did not return a candidate recipe.";
            }
            return false;
        }
        report.recipe_authoring_plan = plan;
        report.assist_status = "assist_used";
        report.tze_stages.push_back({
            "RecipeAuthoring.RecipeDraft",
            "RecipeDraft",
            "ReasoningProvider",
            "assist_planned",
            "Provider `" + std::string(provider.id()) + "` proposed recipe `" + plan->recipe.id + "`.",
            {request.resolved_source_path, request.detected_build_system},
            {plan->recipe.id, plan->recipe.build_system},
        });

        std::string local_validation;
        if (!validate_authored_recipe_plan(*plan, &local_validation)) {
            if (validation_note != nullptr) {
                *validation_note = std::move(local_validation);
            }
            return false;
        }

        AuthoredRecipeRecord provisional = authored_record_from_plan(*plan,
                                                                    inspection.resolved_source_path,
                                                                    report.resolved_project,
                                                                    ranked_evidence,
                                                                    report.tze_run_id,
                                                                    repair_attempted);
        provisional.last_validation_summary = local_validation;
        memory_store.remember_authored_recipe(memory, provisional);
        if (accepted_plan != nullptr) {
            *accepted_plan = plan;
        }
        if (record != nullptr) {
            *record = provisional;
        }
        if (validation_note != nullptr) {
            *validation_note = std::move(local_validation);
        }
        return true;
    };

    std::optional<RecipeAuthoringPlan> plan;
    AuthoredRecipeRecord record;
    std::string validation_note;
    if (!attempt_authoring({}, false, &plan, &record, &validation_note)) {
        report.answer_status = "recipe_authoring_invalid_plan";
        report.answer_explanation = validation_note.empty()
            ? "Ollama did not return a valid authored recipe plan."
            : validation_note;
        report.next_action = "Retry with Ollama fixture or model output that matches the strict recipe schema.";
        return;
    }

    auto execute_validation = [&](AuthoredRecipeRecord* current_record,
                                  bool repair_attempted) {
        RequestProfile runtime_profile = profile;
        runtime_profile.project_reference = inspection.resolved_source_path;
        runtime_profile.build_source_path = inspection.resolved_source_path;
        runtime_profile.selected_recipe_id = current_record->recipe.id;
        runtime_profile.execute_build = true;
        runtime_profile.assist_requested = false;

        ProjectAlias alias = synthetic_alias_for(report.resolved_project, current_record->recipe);
        report.preflight_report = builder.preflight(runtime_profile, alias, inspection.resolved_source_path);
        report.preflight_report->recipe_selection_reason = "authored_recipe(path_match)";
        if (!report.preflight_report->ready) {
            report.doctor_report = builder.doctor(runtime_profile, alias, inspection.resolved_source_path);
            current_record->validation_status = "preflight_failed";
            current_record->last_validation_summary = report.preflight_report->summary;
            current_record->last_validation_log = report.doctor_report->summary;
            current_record->active = false;
            current_record->repair_attempted = repair_attempted;
            report.recipe_authoring_artifact = artifact_from_record(*current_record);
            report.recipe_authoring_artifact->validation_feedback =
                validation_feedback_for(&*report.preflight_report, &*report.doctor_report, nullptr);
            report.recipe_authoring_artifact->status = current_record->validation_status;
            report.recipe_authoring_artifact->rationale = current_record->last_validation_summary;
            report.recipe_authoring_artifact->repair_attempted = repair_attempted;
            report.recipe_authoring_artifact->activated = false;
            report.tze_stages.push_back({
                "RecipeAuthoring.ValidateRepairStore",
                "ValidateRepairStore",
                "RecipeAuthoringEngine",
                "preflight_failed",
                report.preflight_report->summary,
                {current_record->recipe.id},
                report.recipe_authoring_artifact->validation_feedback,
            });
            return false;
        }

        report.build_execution = builder.build_source(runtime_profile, alias);
        current_record->last_validation_log = report.build_execution->log_path;
        current_record->last_artifact = report.build_execution->artifact_hint;
        current_record->repair_attempted = repair_attempted;
        if (report.build_execution->status == "built" || report.build_execution->status == "installed") {
            current_record->validation_status = "validated_active";
            current_record->last_validation_summary = report.build_execution->summary;
            current_record->active = true;
            report.produced_artifact = report.build_execution->artifact_hint;
            report.recipe_authoring_artifact = artifact_from_record(*current_record);
            report.recipe_authoring_artifact->validation_feedback =
                validation_feedback_for(&*report.preflight_report, nullptr, &*report.build_execution);
            report.recipe_authoring_artifact->status = current_record->validation_status;
            report.recipe_authoring_artifact->rationale = current_record->last_validation_summary;
            report.recipe_authoring_artifact->repair_attempted = repair_attempted;
            report.recipe_authoring_artifact->activated = true;
            report.tze_stages.push_back({
                "RecipeAuthoring.ValidateRepairStore",
                "ValidateRepairStore",
                "RecipeAuthoringEngine",
                "validated_active",
                report.build_execution->summary,
                {current_record->recipe.id},
                {report.build_execution->status, report.build_execution->artifact_hint},
            });
            return true;
        }

        current_record->validation_status = "build_failed";
        current_record->last_validation_summary = report.build_execution->summary;
        current_record->active = false;
        report.recipe_authoring_artifact = artifact_from_record(*current_record);
        report.recipe_authoring_artifact->validation_feedback =
            validation_feedback_for(&*report.preflight_report, nullptr, &*report.build_execution);
        report.recipe_authoring_artifact->status = current_record->validation_status;
        report.recipe_authoring_artifact->rationale = current_record->last_validation_summary;
        report.recipe_authoring_artifact->repair_attempted = repair_attempted;
        report.recipe_authoring_artifact->activated = false;
        report.tze_stages.push_back({
            "RecipeAuthoring.ValidateRepairStore",
            "ValidateRepairStore",
            "RecipeAuthoringEngine",
            "build_failed",
            report.build_execution->summary,
            {current_record->recipe.id},
            report.recipe_authoring_artifact->validation_feedback,
        });
        return false;
    };

    bool success = execute_validation(&record, false);
    if (!success) {
        const std::vector<std::string> feedback = validation_feedback_for(
            report.preflight_report.has_value() ? &*report.preflight_report : nullptr,
            report.doctor_report.has_value() ? &*report.doctor_report : nullptr,
            report.build_execution.has_value() ? &*report.build_execution : nullptr);
        std::optional<RecipeAuthoringPlan> repaired_plan;
        AuthoredRecipeRecord repaired_record;
        std::string repair_validation_note;
        if (attempt_authoring(feedback, true, &repaired_plan, &repaired_record, &repair_validation_note)) {
            report.preflight_report.reset();
            report.doctor_report.reset();
            report.build_execution.reset();
            success = execute_validation(&repaired_record, true);
            record = repaired_record;
        } else {
            push_unique(record.evidence_summary, "repair_rejected:" + repair_validation_note);
            record.validation_status = "repair_failed";
            record.last_validation_summary = repair_validation_note;
            record.active = false;
            record.repair_attempted = true;
            report.recipe_authoring_artifact = artifact_from_record(record);
            report.recipe_authoring_artifact->validation_feedback = feedback;
            report.recipe_authoring_artifact->status = record.validation_status;
            report.recipe_authoring_artifact->rationale = record.last_validation_summary;
            report.recipe_authoring_artifact->repair_attempted = true;
            report.recipe_authoring_artifact->activated = false;
        }
    }

    memory_store.remember_authored_recipe(memory, record);
    remember_run_project(memory,
                         memory_store,
                         report.resolved_project,
                         inspection.resolved_source_path,
                         record.recipe.build_system,
                         record.validation_status);
    push_unique(report.memory_writes, memory.paths.authored_recipes_path.string());
    push_unique(report.memory_writes, memory.paths.projects_path.string());
    report.storage_writes.push_back("x.Store(authored.recipe -> " + record.recipe.id + ")");

    if (success) {
        report.answer_status = record.repair_attempted ? "recipe_authoring_repaired" : "recipe_authored";
        report.answer_explanation =
            "Authored build recipe `" + record.recipe.id + "` for `" + report.resolved_project +
            "` and validated it against the local source tree.";
        report.next_action = "Reuse it with `omnix preflight " + inspection.resolved_source_path +
            "` or `omnix build " + inspection.resolved_source_path + "`.";
    } else {
        report.answer_status = "recipe_authoring_failed";
        report.answer_explanation = record.last_validation_summary.empty()
            ? "Authored recipe did not validate successfully."
            : record.last_validation_summary;
        report.next_action = report.doctor_report.has_value()
            ? "Use the doctor guidance, then rerun `omnix recipe author " + inspection.resolved_source_path + "`."
            : "Adjust the source tree or rerun recipe authoring with a cleaner local target.";
    }
}

}  // namespace tze
