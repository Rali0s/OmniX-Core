#include "tze/processing_engine.hpp"
#include "xpp/emitter.hpp"
#include "xpp/index.hpp"
#include "xpp/parser.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(__APPLE__) || defined(__linux__) || defined(__unix__)
#include <unistd.h>
#endif

#ifndef OMNIX_VERSION
#define OMNIX_VERSION "0.1.0-dev"
#endif

namespace {

enum class OutputMode {
    Auto,
    Compact,
    Verbose,
};

struct CommonCliOptions {
    std::string source_map_path;
    std::string memory_root_path;
    std::string build_dir;
    std::string build_target;
    std::string build_type = "Release";
    std::string install_prefix;
    std::string git_ref_override;
    std::string selected_recipe_id;
    std::string language_confirmation = "auto";
    std::string output_path;
    std::string feedback_note;
    std::size_t keep_count = 12;
    tze::AcquisitionPolicy acquisition_policy = tze::AcquisitionPolicy::FetchIfMissing;
    bool clean = false;
    bool perform_install = true;
    bool offline = false;
    bool important_only = false;
    bool assist = false;
    OutputMode output_mode = OutputMode::Auto;
};

std::filesystem::path find_project_root(std::filesystem::path start) {
    if (start.empty()) {
        start = std::filesystem::current_path();
    }

    if (std::filesystem::is_regular_file(start)) {
        start = start.parent_path();
    }

    for (std::filesystem::path cursor = start; !cursor.empty(); cursor = cursor.parent_path()) {
        if (std::filesystem::exists(cursor / "CMakeLists.txt")) {
            return cursor;
        }
        if (cursor == cursor.root_path()) {
            break;
        }
    }

    return {};
}

std::filesystem::path optional_source_file(const std::filesystem::path& anchor = std::filesystem::current_path()) {
    const std::filesystem::path project_root = find_project_root(anchor);
    if (!project_root.empty()) {
        const std::filesystem::path candidate = project_root / "res" / "tze.txt";
        if (std::filesystem::exists(candidate)) {
            return candidate;
        }
    }
    return {};
}

std::filesystem::path default_source_file() {
    const std::filesystem::path candidate = optional_source_file();
    if (!candidate.empty()) {
        return candidate;
    }
    throw std::runtime_error("Unable to locate res/tze.txt from the current working directory.");
}

std::filesystem::path default_emit_dir(const std::filesystem::path& source_file) {
    const std::filesystem::path project_root = find_project_root(source_file);
    if (!project_root.empty()) {
        return project_root / "build" / "generated" / "xpp";
    }
    return std::filesystem::current_path() / "generated" / "xpp";
}

void print_usage() {
    std::cout << "Usage:\n";
    std::cout << "  omnix --version\n";
    std::cout << "  omnix ask <prompt> [--assist] [--compact|--verbose] [--source-map file] [--memory-root dir] [--lang-confirm auto|yes|no] [--build-dir dir] [--target name] [--build-type type] [--install-prefix dir] [--recipe id] [--ref git-ref] [--clean] [--build-only] [--no-install] [--offline] [--local-only]\n";
    std::cout << "  omnix ingest <path|command> [--compact|--verbose] [--memory-root dir] [--source-map file]\n";
    std::cout << "  omnix analyze <case|source> [--compact|--verbose] [--memory-root dir] [--source-map file]\n";
    std::cout << "  omnix decide <case|source> [--compact|--verbose] [--memory-root dir] [--source-map file]\n";
    std::cout << "  omnix decide feedback <case-id> <decision-id> <helpful|not-helpful> [--note text] [--memory-root dir]\n";
    std::cout << "  omnix decide outcome <case-id> <decision-id> <success|failed|partial> [--note text] [--memory-root dir]\n";
    std::cout << "  omnix case <id|source> [--compact|--verbose] [--memory-root dir] [--source-map file]\n";
    std::cout << "  omnix case list [--compact|--verbose] [--memory-root dir] [--source-map file]\n";
    std::cout << "  omnix case search <term> [--compact|--verbose] [--memory-root dir] [--source-map file]\n";
    std::cout << "  omnix case timeline <id|source> [--compact|--verbose] [--memory-root dir]\n";
    std::cout << "  omnix case export <id|source> [--out file] [--memory-root dir]\n";
    std::cout << "  omnix case import <bundle.json> [--memory-root dir]\n";
    std::cout << "  omnix incident list [--compact|--verbose] [--memory-root dir]\n";
    std::cout << "  omnix incident <id> [--compact|--verbose] [--memory-root dir]\n";
    std::cout << "  omnix incident report <id> [--compact|--verbose] [--out file] [--memory-root dir]\n";
    std::cout << "  omnix define <symbol-or-term> [file] [--compact|--verbose] [--memory-root dir] [--lang-confirm auto|yes|no]\n";
    std::cout << "  omnix explain <command-or-symbol> [file] [--compact|--verbose] [--memory-root dir] [--lang-confirm auto|yes|no]\n";
    std::cout << "  omnix build <project-or-path> [--assist] [--compact|--verbose] [--source-map file] [--memory-root dir] [--build-dir dir] [--target name] [--build-type type] [--install-prefix dir] [--recipe id] [--ref git-ref] [--clean] [--build-only] [--no-install] [--offline] [--local-only]\n";
    std::cout << "  omnix preflight <project-or-path> [--assist] [--compact|--verbose] [--memory-root dir] [--target name] [--install-prefix dir] [--recipe id] [--ref git-ref] [--offline] [--local-only]\n";
    std::cout << "  omnix doctor <project-or-path> [--compact|--verbose] [--memory-root dir] [--recipe id] [--offline] [--local-only]\n";
    std::cout << "  omnix provider probe [--compact|--verbose] [--memory-root dir]\n";
    std::cout << "  omnix shell [--source-map file] [--memory-root dir] [--assist] [--compact|--verbose]\n";
    std::cout << "  omnix memory [history|prefs|definitions|language|security|uac|cases|runs|tze] [--compact|--verbose] [--memory-root dir]\n";
    std::cout << "  omnix memory prune [--keep n] [--important-only] [--memory-root dir]\n";
    std::cout << "  omnix tze runs [--compact|--verbose] [--memory-root dir]\n";
    std::cout << "  omnix tze latest [--compact|--verbose] [--memory-root dir]\n";
    std::cout << "  omnix tze replay <run-id> [--compact|--verbose] [--memory-root dir]\n";
    std::cout << "  omnix tze chain <run-id|latest> [--compact|--verbose] [--memory-root dir]\n";
    std::cout << "  omnix tze diff <left-run-id> <right-run-id> [--compact|--verbose] [--memory-root dir]\n";
    std::cout << "  omnix tze diff-latest [--compact|--verbose] [--important-only] [--memory-root dir]\n";
    std::cout << "  omnix tze explain-change <left-run-id> <right-run-id> [--assist] [--compact|--verbose] [--memory-root dir]\n";
    std::cout << "  omnix tze explain-change-latest [--assist] [--compact|--verbose] [--important-only] [--memory-root dir]\n";
    std::cout << "  omnix tze report <run-id> [--assist] [--compact|--verbose] [--out file] [--memory-root dir]\n";
    std::cout << "  omnix tze diff-report <left-run-id> <right-run-id> [--out file] [--memory-root dir]\n";
    std::cout << "  omnix tze export <run-id|latest> [--out file] [--memory-root dir]\n";
    std::cout << "  omnix tze import <bundle.json> [--memory-root dir]\n";
    std::cout << "  omnix tze prune [--keep n] [--important-only] [--memory-root dir]\n";
    std::cout << "  omnix tze mark <run-id> <helpful|not-helpful> [--note text] [--memory-root dir]\n";
    std::cout << "  omnix tool list [--compact|--verbose] [--memory-root dir]\n";
    std::cout << "  omnix tool locate <name> [--compact|--verbose] [--memory-root dir]\n";
    std::cout << "  omnix tool doctor <name> [--compact|--verbose] [--memory-root dir]\n";
    std::cout << "  omnix tool <name> -- <args...> [--compact|--verbose] [--memory-root dir]\n";
    std::cout << "    Built-in analyst modules: inspect-log, inspect-build, inspect-host, report-case, text-pipeline\n";
    std::cout << "  omnix map <file>\n";
    std::cout << "  omnix search <symbol> [file]\n";
    std::cout << "  omnix emit-cpp <file> [output-dir]\n";
    std::cout << "  omnix build-cmake [source-dir] [--target name] [--clean] [--build-dir dir] [--build-type type]\n";
}

void run_map(const std::filesystem::path& source_file) {
    const std::string source = xpp::read_text_file(source_file);
    const xpp::MappingUnit unit = xpp::parse_xpp(source, source_file.string());
    const xpp::SymbolIndex index = xpp::build_symbol_index(unit);

    std::cout << "Source: " << source_file << "\n";
    std::cout << "Dialect: " << xpp::to_string(unit.dialect) << "\n";
    std::cout << "Lines: " << unit.lines.size() << "\n";
    std::cout << "Sections: " << unit.sections.size() << "\n";
    for (const xpp::SectionNode& section : unit.sections) {
        std::cout << " - " << section.title << " (lines " << section.line_start << "-" << section.line_end
                  << ", nodes " << section.nodes.size() << ")\n";
    }
    std::cout << "Indexed symbols: " << index.mappings.size() << "\n";
}

void run_search(const std::string& query, const std::filesystem::path& source_file) {
    const std::string source = xpp::read_text_file(source_file);
    const xpp::MappingUnit unit = xpp::parse_xpp(source, source_file.string());
    const xpp::SymbolIndex index = xpp::build_symbol_index(unit);
    const xpp::SymbolMapping* mapping = xpp::find_mapping(index, query);

    if (mapping == nullptr) {
        throw std::runtime_error("Symbol not found: " + query);
    }

    std::cout << "Source: " << source_file << "\n";
    std::cout << "Symbol: " << mapping->raw_symbol << "\n";
    std::cout << "Normalized: " << mapping->normalized_symbol << "\n";
    std::cout << "Status: " << xpp::to_string(mapping->status) << "\n";
    std::cout << "Family: " << xpp::to_string(mapping->family) << "\n";
    std::cout << "Meaning: " << mapping->inferred_meaning << "\n";
    std::cout << "Mapped C++ target: " << mapping->mapped_cpp_target << "\n";
    std::cout << "Occurrences: " << mapping->occurrences.size() << "\n";

    const std::size_t preview = std::min<std::size_t>(mapping->occurrences.size(), 8);
    for (std::size_t index_value = 0; index_value < preview; ++index_value) {
        const xpp::SymbolOccurrence& occurrence = mapping->occurrences[index_value];
        std::cout << " - " << occurrence.section_title << " line " << occurrence.line
                  << " (" << xpp::to_string(occurrence.node_kind) << ")\n";
    }
}

void run_emit(const std::filesystem::path& source_file, const std::filesystem::path& output_dir) {
    const std::string source = xpp::read_text_file(source_file);
    const xpp::MappingUnit unit = xpp::parse_xpp(source, source_file.string());
    const xpp::SymbolIndex index = xpp::build_symbol_index(unit);
    const xpp::EmitReport report = xpp::emit_cpp(unit, index, {output_dir, "generated::xpp", true});

    std::cout << "Source: " << source_file << "\n";
    std::cout << "Output: " << output_dir << "\n";
    for (const xpp::EmitArtifact& artifact : report.artifacts) {
        std::cout << " - " << artifact.section_title << " -> " << artifact.path;
        if (artifact.contains_unsupported) {
            std::cout << " [contains inert unsupported stubs]";
        }
        std::cout << "\n";
    }
    std::cout << "Manifest: " << report.manifest_path << "\n";
}

void print_toolchain(const std::vector<tze::ToolchainModuleStatus>& modules) {
    std::cout << "Toolchain:\n";
    for (const tze::ToolchainModuleStatus& module : modules) {
        std::cout << " - " << module.name << ": " << (module.available ? "available" : "missing");
        if (!module.version.empty()) {
            std::cout << " (" << module.version << ")";
        }
        std::cout << "\n";
    }
}

bool stdout_is_tty() {
#if defined(__APPLE__) || defined(__linux__) || defined(__unix__)
    return ::isatty(fileno(stdout)) != 0;
#else
    return false;
#endif
}

std::string first_line(std::string_view value) {
    const std::size_t newline = value.find('\n');
    if (newline == std::string::npos) {
        return std::string(value);
    }
    return std::string(value.substr(0, newline));
}

std::string compact_summary(const tze::ProcessingReport& report) {
    if (report.provider_probe_report.has_value() && !report.provider_probe_report->summary.empty()) {
        return report.provider_probe_report->summary;
    }
    if (report.build_execution.has_value() && !report.build_execution->summary.empty()) {
        return report.build_execution->summary;
    }
    if (report.preflight_report.has_value() && !report.preflight_report->summary.empty()) {
        return report.preflight_report->summary;
    }
    if (report.doctor_report.has_value() && !report.doctor_report->summary.empty()) {
        return report.doctor_report->summary;
    }
    if (report.tool_invocation_report.has_value() && !report.tool_invocation_report->summary.empty()) {
        return report.tool_invocation_report->summary;
    }
    if (report.tool_doctor_report.has_value() && !report.tool_doctor_report->summary.empty()) {
        return report.tool_doctor_report->summary;
    }
    if (report.tool_resolution.has_value() && !report.tool_resolution->summary.empty()) {
        return report.tool_resolution->summary;
    }
    if (report.definition_answer.has_value() && !report.definition_answer->summary.empty()) {
        return report.definition_answer->summary;
    }
    if (!report.answer_explanation.empty()) {
        return first_line(report.answer_explanation);
    }
    return {};
}

bool should_show_next_action_in_compact(const tze::ProcessingReport& report) {
    if (report.answer_status.empty()) {
        return false;
    }
    return report.answer_status.find("failed") != std::string::npos ||
        report.answer_status.find("missing") != std::string::npos ||
        report.answer_status.find("attention") != std::string::npos ||
        report.answer_status == "build_ready" ||
        report.answer_status == "doctor_ready" ||
        report.answer_status == "provider_inactive" ||
        report.answer_status == "provider_probe_failed" ||
        report.answer_status == "unknown_intent";
}

bool use_verbose_output(OutputMode mode, bool prefer_verbose) {
    if (mode == OutputMode::Verbose) {
        return true;
    }
    if (mode == OutputMode::Compact) {
        return false;
    }
    if (prefer_verbose) {
        return true;
    }
    return !stdout_is_tty();
}

void print_processing_report_compact(const tze::ProcessingReport& report) {
    if (!report.answer_status.empty()) {
        std::cout << report.answer_status;
        const std::string summary = compact_summary(report);
        if (!summary.empty()) {
            std::cout << ": " << summary;
        }
        std::cout << "\n";
    } else if (!compact_summary(report).empty()) {
        std::cout << compact_summary(report) << "\n";
    }

    if (!report.resolved_project.empty()) {
        std::cout << "project: " << report.resolved_project << "\n";
    }
    if (report.case_record.has_value()) {
        std::cout << "case: " << report.case_record->id << "\n";
    }
    if (report.command_assist_plan.has_value() && report.assist_status == "assist_used") {
        std::cout << "assist command: " << report.command_assist_plan->canonical_command << "\n";
    }
    if (report.tool_assist_plan.has_value() && report.assist_status == "assist_used") {
        std::cout << "assist tool: " << report.tool_assist_plan->tool_name << "\n";
    }
    if (report.build_assist_plan.has_value() && report.assist_status == "assist_used") {
        std::cout << "assist recipe: " << report.build_assist_plan->selected_recipe_id << "\n";
    }
    if (report.preflight_report.has_value() && !report.preflight_report->recipe_id.empty()) {
        std::cout << "recipe: " << report.preflight_report->recipe_id << "\n";
    } else if (report.build_execution.has_value() && !report.build_execution->selected_recipe_id.empty()) {
        std::cout << "recipe: " << report.build_execution->selected_recipe_id << "\n";
    }
    if (report.tool_resolution.has_value() && report.tool_resolution->found &&
        !report.tool_resolution->executable_path.empty()) {
        std::cout << "provider: " << report.tool_resolution->executable_path << "\n";
    }
    if (report.tool_invocation_report.has_value()) {
        const tze::ToolInvocationReport& invocation = *report.tool_invocation_report;
        if (!invocation.command_line.empty()) {
            std::cout << "command: " << invocation.command_line << "\n";
        }
        if (!invocation.output_excerpt.empty()) {
            std::cout << "result: " << invocation.output_excerpt.front() << "\n";
        }
    }
    if (!report.produced_artifact.empty()) {
        std::cout << "artifact: " << report.produced_artifact << "\n";
    }
    if (should_show_next_action_in_compact(report) && !report.next_action.empty()) {
        std::cout << "next: " << report.next_action << "\n";
    }
}

void print_processing_report_verbose(const tze::ProcessingReport& report) {
    if (!report.tze_run_id.empty()) {
        std::cout << "TZE run: " << report.tze_run_id << "\n";
    }
    if (!report.resolved_intent.empty()) {
        std::cout << "Intent: " << report.resolved_intent << "\n";
    }
    if (!report.decoded_instruction.empty()) {
        std::cout << "Instruction: " << report.decoded_instruction << "\n";
    }
    if (!report.cache.name.empty()) {
        std::cout << "Cache: " << report.cache.name << " (" << report.cache.size_bytes << " bytes)\n";
    }
    if (!report.resolved_project.empty()) {
        std::cout << "Project: " << report.resolved_project << "\n";
    }
    if (!report.resolved_project_path.empty()) {
        std::cout << "Project path: " << report.resolved_project_path << "\n";
    }
    if (!report.answer_status.empty()) {
        std::cout << "Verdict: " << report.answer_status << "\n";
    }
    if (!report.version_string.empty()) {
        std::cout << "Version: " << report.version_string << "\n";
    }
    if (!report.reasoning_provider.empty()) {
        std::cout << "Reasoning provider: " << report.reasoning_provider << "\n";
    }
    if (report.provider_probe_report.has_value()) {
        const tze::ProviderProbeReport& probe = *report.provider_probe_report;
        std::cout << "Provider probe: " << probe.status << "\n";
        if (!probe.base_url.empty()) {
            std::cout << "Provider base URL: " << probe.base_url << "\n";
        }
        if (!probe.model.empty()) {
            std::cout << "Provider model: " << probe.model << "\n";
        }
        if (!probe.checks.empty()) {
            std::cout << "Provider checks:\n";
            for (const std::string& check : probe.checks) {
                std::cout << " - " << check << "\n";
            }
        }
        if (!probe.warnings.empty()) {
            std::cout << "Provider warnings:\n";
            for (const std::string& warning : probe.warnings) {
                std::cout << " - " << warning << "\n";
            }
        }
    }
    if (!report.assist_status.empty()) {
        std::cout << "Assist: " << report.assist_status << "\n";
    }
    if (report.assist_annotation.has_value()) {
        const tze::AssistAnnotation& assist = *report.assist_annotation;
        std::cout << "Assist provider: " << assist.provider_id;
        if (!assist.model.empty()) {
            std::cout << " (" << assist.model << ")";
        }
        std::cout << "\n";
        if (!assist.summary.empty()) {
            std::cout << "Assist summary: " << assist.summary << "\n";
        }
        if (!assist.highlights.empty()) {
            std::cout << "Assist highlights:\n";
            for (const std::string& highlight : assist.highlights) {
                std::cout << " - " << highlight << "\n";
            }
        }
        if (!assist.operator_takeaway.empty()) {
            std::cout << "Assist takeaway: " << assist.operator_takeaway << "\n";
        }
        if (!assist.warnings.empty()) {
            std::cout << "Assist warnings:\n";
            for (const std::string& warning : assist.warnings) {
                std::cout << " - " << warning << "\n";
            }
        }
    }
    if (report.command_assist_plan.has_value()) {
        const tze::CommandAssistPlan& plan = *report.command_assist_plan;
        std::cout << "Assist command plan: " << plan.canonical_command << "\n";
        std::cout << "Assist command family: " << plan.command_family << "\n";
        std::cout << "Assist command confidence: " << plan.confidence << "\n";
        if (!plan.rationale.empty()) {
            std::cout << "Assist command rationale: " << plan.rationale << "\n";
        }
        if (!plan.safety_notes.empty()) {
            std::cout << "Assist command safety notes:\n";
            for (const std::string& note : plan.safety_notes) {
                std::cout << " - " << note << "\n";
            }
        }
    }
    if (report.tool_assist_plan.has_value()) {
        const tze::ToolAssistPlan& plan = *report.tool_assist_plan;
        std::cout << "Assist tool plan: " << plan.tool_name << "\n";
        if (!plan.arguments.empty()) {
            std::cout << "Assist tool args:\n";
            for (const std::string& argument : plan.arguments) {
                std::cout << " - " << argument << "\n";
            }
        }
        if (!plan.rationale.empty()) {
            std::cout << "Assist rationale: " << plan.rationale << "\n";
        }
        if (!plan.safety_notes.empty()) {
            std::cout << "Assist safety notes:\n";
            for (const std::string& note : plan.safety_notes) {
                std::cout << " - " << note << "\n";
            }
        }
    }
    if (report.build_assist_plan.has_value()) {
        const tze::BuildAssistPlan& plan = *report.build_assist_plan;
        std::cout << "Assist build plan: " << plan.selected_recipe_id << "\n";
        if (!plan.fallback_recipe_id.empty()) {
            std::cout << "Assist fallback recipe: " << plan.fallback_recipe_id << "\n";
        }
        std::cout << "Assist build confidence: " << plan.confidence << "\n";
        if (!plan.rationale.empty()) {
            std::cout << "Assist build rationale: " << plan.rationale << "\n";
        }
        if (!plan.safety_notes.empty()) {
            std::cout << "Assist build safety notes:\n";
            for (const std::string& note : plan.safety_notes) {
                std::cout << " - " << note << "\n";
            }
        }
    }
    if (!report.produced_artifact.empty()) {
        std::cout << "Produced artifact: " << report.produced_artifact << "\n";
    }
    if (!report.answer_explanation.empty()) {
        std::cout << "Explanation:\n" << report.answer_explanation << "\n";
    }
    if (report.definition_answer.has_value()) {
        const tze::DefinitionAnswer& answer = *report.definition_answer;
        std::cout << "Definition query: " << answer.query << "\n";
        if (!answer.mapped_cpp_target.empty()) {
            std::cout << "Mapped target: " << answer.mapped_cpp_target << "\n";
        }
        if (!answer.semantic_family.empty()) {
            std::cout << "Semantic family: " << answer.semantic_family << "\n";
        }
        if (!answer.sources.empty()) {
            std::cout << "Definition sources:\n";
            for (const std::string& source : answer.sources) {
                std::cout << " - " << source << "\n";
            }
        }
        if (!answer.suggestions.empty()) {
            std::cout << "Suggestions:\n";
            for (const std::string& suggestion : answer.suggestions) {
                std::cout << " - " << suggestion << "\n";
            }
        }
    }
    if (report.language_resolution.has_value()) {
        const tze::LanguageResolutionRecord& language = *report.language_resolution;
        std::cout << "Language context: " << language.combined_context
                  << " [confidence=" << language.confidence << "]\n";
        std::cout << "Language passes: " << language.passes << "\n";
        if (language.manual_confirmation_required) {
            std::cout << "Language confirmation: required";
            if (!language.manual_confirmation_response.empty()) {
                std::cout << " (" << language.manual_confirmation_response << ")";
            }
            std::cout << "\n";
            if (!language.manual_confirmation_prompt.empty()) {
                std::cout << "Language prompt: " << language.manual_confirmation_prompt << "\n";
            }
        } else if (language.manual_confirmation_used) {
            std::cout << "Language confirmation: " << language.manual_confirmation_response << "\n";
        }
    }
    if (report.uac_state.has_value()) {
        const tze::UacStateRecord& uac = *report.uac_state;
        std::cout << "uAC epoch: " << uac.epoch_marker << "\n";
        std::cout << "uAC machine: " << uac.machine_identifier << "\n";
        std::cout << "uAC namespace: " << uac.store_namespace << "\n";
        if (!uac.recovery_hints.empty()) {
            std::cout << "uAC recovery hints:\n";
            for (const std::string& hint : uac.recovery_hints) {
                std::cout << " - " << hint << "\n";
            }
        }
    }
    const bool render_security_audit = !report.security.simulated_actions.empty() ||
        (!report.security.blocked_paths.empty() && report.security.threat_label != "privilege-gate");
    if (render_security_audit) {
        std::cout << "Security status: " << report.security.status << "\n";
        std::cout << "Security mode: " << report.security.behavior_mode << "\n";
        if (!report.security.threat_label.empty()) {
            std::cout << "Threat label: " << report.security.threat_label << "\n";
        }
        if (!report.security.threat_bracket.empty()) {
            std::cout << "Threat bracket: " << report.security.threat_bracket << "\n";
        }
        if (!report.security.simulated_actions.empty()) {
            std::cout << "Simulated actions:\n";
            for (const std::string& action : report.security.simulated_actions) {
                std::cout << " - " << action << "\n";
            }
        }
        if (!report.security.blocked_paths.empty()) {
            std::cout << "Blocked paths:\n";
            for (const std::string& path : report.security.blocked_paths) {
                std::cout << " - " << path << "\n";
            }
        }
    }
    if (report.acquisition_result.has_value()) {
        const tze::AcquisitionResult& acquisition = *report.acquisition_result;
        std::cout << "Acquisition: " << acquisition.status << "\n";
        std::cout << "Acquisition summary: " << acquisition.summary << "\n";
        if (!acquisition.resolved_source_path.empty()) {
            std::cout << "Acquired source: " << acquisition.resolved_source_path << "\n";
        }
    }
    if (report.preflight_report.has_value()) {
        const tze::PreflightReport& preflight = *report.preflight_report;
        std::cout << "Preflight: " << preflight.status << "\n";
        std::cout << "Preflight summary: " << preflight.summary << "\n";
        if (!preflight.recipe_id.empty()) {
            std::cout << "Recipe: " << preflight.recipe_id << "\n";
        }
        if (!preflight.recipe_selection_reason.empty()) {
            std::cout << "Recipe selection: " << preflight.recipe_selection_reason << "\n";
        }
        if (!preflight.available_recipe_ids.empty()) {
            std::cout << "Available recipes:\n";
            for (const std::string& recipe : preflight.available_recipe_ids) {
                std::cout << " - " << recipe << "\n";
            }
        }
        if (!preflight.build_system.empty()) {
            std::cout << "Planned build system: " << preflight.build_system << "\n";
        }
        if (!preflight.environment_signature.empty()) {
            std::cout << "Environment: " << preflight.environment_signature << "\n";
        }
        if (!preflight.install_prefix.empty()) {
            std::cout << "Install prefix: " << preflight.install_prefix << "\n";
        }
        if (!preflight.dependency_hints.empty()) {
            std::cout << "Dependency hints:\n";
            for (const std::string& hint : preflight.dependency_hints) {
                std::cout << " - " << hint << "\n";
            }
        }
        if (!preflight.expected_steps.empty()) {
            std::cout << "Expected steps:\n";
            for (const std::string& step : preflight.expected_steps) {
                std::cout << " - " << step << "\n";
            }
        }
        if (!preflight.missing_modules.empty()) {
            std::cout << "Preflight missing modules:\n";
            for (const std::string& module : preflight.missing_modules) {
                std::cout << " - " << module << "\n";
            }
        }
    }
    if (report.doctor_report.has_value()) {
        const tze::DoctorReport& doctor = *report.doctor_report;
        std::cout << "Doctor: " << doctor.status << "\n";
        std::cout << "Doctor summary: " << doctor.summary << "\n";
        if (!doctor.detected_platform.empty()) {
            std::cout << "Detected platform: " << doctor.detected_platform << "\n";
        }
        if (!doctor.detected_package_manager.empty()) {
            std::cout << "Detected package manager: " << doctor.detected_package_manager << "\n";
        }
        if (!doctor.dependency_checks.empty()) {
            std::cout << "Dependency checks:\n";
            for (const std::string& check : doctor.dependency_checks) {
                std::cout << " - " << check << "\n";
            }
        }
        if (!doctor.package_guidance.empty()) {
            for (const tze::PackageManagerGuidance& guidance : doctor.package_guidance) {
                std::cout << (guidance.primary ? "Primary guidance" : "Other guidance")
                          << " [" << guidance.label << "]:\n";
                for (const std::string& command : guidance.commands) {
                    std::cout << " - " << command << "\n";
                }
            }
        }
        if (!doctor.bootstrap_guidance.empty()) {
            std::cout << "Bootstrap guidance:\n";
            for (const std::string& line : doctor.bootstrap_guidance) {
                std::cout << " - " << line << "\n";
            }
        }
    }
    if (report.tool_resolution.has_value()) {
        const tze::ToolResolution& tool = *report.tool_resolution;
        std::cout << "Tool resolution: " << (tool.found ? "found" : "missing") << "\n";
        if (!tool.logical_name.empty()) {
            std::cout << "Tool: " << tool.logical_name << "\n";
        }
        if (!tool.provider_type.empty()) {
            std::cout << "Provider: " << tool.provider_type << "\n";
        }
        if (!tool.executable_path.empty()) {
            std::cout << "Executable: " << tool.executable_path << "\n";
        }
        if (!tool.applet_name.empty()) {
            std::cout << "Applet: " << tool.applet_name << "\n";
        }
        if (!tool.version_fingerprint.empty()) {
            std::cout << "Version fingerprint: " << tool.version_fingerprint << "\n";
        }
        if (!tool.cache_origin.empty()) {
            std::cout << "Resolution origin: " << tool.cache_origin << "\n";
        }
        if (!tool.validation_signature.empty()) {
            std::cout << "Validation: " << tool.validation_signature << "\n";
        }
    }
    if (report.tool_doctor_report.has_value()) {
        const tze::ToolDoctorReport& doctor = *report.tool_doctor_report;
        std::cout << "Tool doctor: " << doctor.status << "\n";
        std::cout << "Tool doctor summary: " << doctor.summary << "\n";
        if (!doctor.selected_provider.empty()) {
            std::cout << "Selected provider: " << doctor.selected_provider << "\n";
        }
        if (!doctor.executable_path.empty()) {
            std::cout << "Provider path: " << doctor.executable_path << "\n";
        }
        if (!doctor.cache_origin.empty()) {
            std::cout << "Provider origin: " << doctor.cache_origin << "\n";
        }
        if (!doctor.discovered_paths.empty()) {
            std::cout << "Discovered paths:\n";
            for (const std::string& path : doctor.discovered_paths) {
                std::cout << " - " << path << "\n";
            }
        }
        if (!doctor.busybox_applets.empty()) {
            std::cout << "BusyBox applets:\n";
            for (const std::string& applet : doctor.busybox_applets) {
                std::cout << " - " << applet << "\n";
            }
        }
        if (!doctor.capability_notes.empty()) {
            std::cout << "Capability notes:\n";
            for (const std::string& note : doctor.capability_notes) {
                std::cout << " - " << note << "\n";
            }
        }
    }
    if (report.tool_invocation_report.has_value()) {
        const tze::ToolInvocationReport& invocation = *report.tool_invocation_report;
        std::cout << "Tool invocation: " << invocation.status << "\n";
        if (!invocation.command_line.empty()) {
            std::cout << "Command line: " << invocation.command_line << "\n";
        }
        std::cout << "Exit code: " << invocation.exit_code << "\n";
        if (!invocation.output_excerpt.empty()) {
            std::cout << "Output excerpt:\n";
            for (const std::string& line : invocation.output_excerpt) {
                std::cout << " - " << line << "\n";
            }
        }
    }
    if (report.source_inspection.has_value()) {
        const tze::SourceInspection& inspection = *report.source_inspection;
        std::cout << "Inspection: " << inspection.summary << "\n";
        if (!inspection.detected_files.empty()) {
            std::cout << "Detected files:\n";
            for (const std::string& file : inspection.detected_files) {
                std::cout << " - " << file << "\n";
            }
        }
        if (!inspection.missing_modules.empty()) {
            std::cout << "Missing modules:\n";
            for (const std::string& module : inspection.missing_modules) {
                std::cout << " - " << module << "\n";
            }
        }
    }
    if (report.build_execution.has_value()) {
        const tze::BuildExecution& build = *report.build_execution;
        std::cout << "Build status: " << build.status << "\n";
        std::cout << "Build summary: " << build.summary << "\n";
        if (!build.build_dir.empty()) {
            std::cout << "Build dir: " << build.build_dir << "\n";
        }
        if (!build.log_path.empty()) {
            std::cout << "Build log: " << build.log_path << "\n";
        }
        if (!build.artifact_hint.empty()) {
            std::cout << "Artifact: " << build.artifact_hint << "\n";
        }
        if (!build.selected_recipe_id.empty()) {
            std::cout << "Selected recipe: " << build.selected_recipe_id << "\n";
        }
        if (!build.recipe_selection_reason.empty()) {
            std::cout << "Recipe selection: " << build.recipe_selection_reason << "\n";
        }
        if (!build.environment_signature.empty()) {
            std::cout << "Build environment: " << build.environment_signature << "\n";
        }
        if (!build.install_status.empty()) {
            std::cout << "Install status: " << build.install_status << "\n";
        }
        if (!build.install_prefix.empty()) {
            std::cout << "Install prefix: " << build.install_prefix << "\n";
        }
        if (!build.verified_artifacts.empty()) {
            std::cout << "Verified artifacts:\n";
            for (const std::string& artifact : build.verified_artifacts) {
                std::cout << " - " << artifact << "\n";
            }
        }
        if (!build.verified_install_outputs.empty()) {
            std::cout << "Verified install outputs:\n";
            for (const std::string& artifact : build.verified_install_outputs) {
                std::cout << " - " << artifact << "\n";
            }
        }
        if (!build.commands.empty()) {
            std::cout << "Build commands:\n";
            for (const std::string& command : build.commands) {
                std::cout << " - " << command << "\n";
            }
        }
    }
    if (report.permission_context.has_value()) {
        const tze::PermissionContext& permission = *report.permission_context;
        std::cout << "Permission role: " << permission.role << "\n";
        std::cout << "Permission flags: view_raw=" << (permission.can_view_raw ? "true" : "false")
                  << " run_actions=" << (permission.can_run_actions ? "true" : "false")
                  << " store_feedback=" << (permission.can_store_feedback ? "true" : "false") << "\n";
    }
    if (report.case_record.has_value()) {
        const tze::CaseRecord& entry = *report.case_record;
        std::cout << "Case id: " << entry.id << "\n";
        std::cout << "Case title: " << entry.title << "\n";
        if (!entry.primary_source.empty()) {
            std::cout << "Case source: " << entry.primary_source << "\n";
        }
        std::cout << "Case status: " << entry.status << "\n";
        if (!entry.created_by_run_id.empty()) {
            std::cout << "Case created by run: " << entry.created_by_run_id << "\n";
        }
        if (!entry.analyzed_by_run_id.empty()) {
            std::cout << "Case analyzed by run: " << entry.analyzed_by_run_id << "\n";
        }
        if (!entry.decided_by_run_id.empty()) {
            std::cout << "Case decided by run: " << entry.decided_by_run_id << "\n";
        }
        if (!entry.reported_by_run_id.empty()) {
            std::cout << "Case reported by run: " << entry.reported_by_run_id << "\n";
        }
    }
    if (!report.case_matches.empty()) {
        std::cout << "Case matches:\n";
        for (const tze::CaseRecord& entry : report.case_matches) {
            std::cout << " - " << entry.id << " | " << entry.status << " | " << entry.title;
            if (!entry.primary_source.empty()) {
                std::cout << " | source=" << entry.primary_source;
            }
            std::cout << "\n";
        }
    }
    if (!report.observations.empty()) {
        std::cout << "Observations:\n";
        for (const tze::ObservationRecord& observation : report.observations) {
            std::cout << " - " << observation.id << " [" << observation.source_kind << "] " << observation.source_ref
                      << " => " << observation.summary << "\n";
        }
    }
    if (!report.normalized_objects.empty()) {
        std::cout << "Normalized objects:\n";
        for (const tze::NormalizedObject& object : report.normalized_objects) {
            std::cout << " - " << object.id << " [" << object.object_type << "] " << object.summary << "\n";
        }
    }
    if (!report.evidence_links.empty()) {
        std::cout << "Evidence links:\n";
        for (const tze::EvidenceLink& link : report.evidence_links) {
            std::cout << " - " << link.id << " " << link.source_observation_id << " -> " << link.target_object_id
                      << " (" << link.relation << ")\n";
        }
    }
    if (!report.analyst_comments.empty()) {
        std::cout << "Analyst comments:\n";
        for (const tze::AnalystComment& comment : report.analyst_comments) {
            std::cout << " - " << comment.author << ": " << comment.text << "\n";
        }
    }
    if (!report.decision_candidates.empty()) {
        std::cout << "Decision candidates:\n";
        for (const tze::DecisionCandidate& decision : report.decision_candidates) {
            std::cout << " - {" << decision.id << "} [score=" << decision.score
                      << " likelihood=" << decision.probability_likelihood
                      << " confidence=" << decision.confidence
                      << " valid=" << (decision.valid ? "yes" : "no")
                      << " coverage=" << decision.evidence_coverage
                      << " prior=" << decision.prior_success_score
                      << "] " << decision.title;
            if (!decision.recommended_command.empty()) {
                std::cout << " => " << decision.recommended_command;
            }
            std::cout << "\n";
            if (!decision.supporting_signals.empty()) {
                std::cout << "   signals: ";
                for (std::size_t index = 0; index < decision.supporting_signals.size(); ++index) {
                    if (index != 0) {
                        std::cout << ", ";
                    }
                    std::cout << decision.supporting_signals[index];
                }
                std::cout << "\n";
            }
            if (!decision.validation_checks.empty()) {
                std::cout << "   checks: ";
                for (std::size_t index = 0; index < decision.validation_checks.size(); ++index) {
                    if (index != 0) {
                        std::cout << ", ";
                    }
                    std::cout << decision.validation_checks[index];
                }
                std::cout << "\n";
            }
            if (!decision.score_trace.empty()) {
                std::cout << "   model: ";
                for (std::size_t index = 0; index < decision.score_trace.size(); ++index) {
                    if (index != 0) {
                        std::cout << ", ";
                    }
                    std::cout << decision.score_trace[index];
                }
                std::cout << "\n";
            }
            if (!decision.operator_feedback.empty()) {
                std::cout << "   feedback: " << decision.operator_feedback;
                if (!decision.feedback_timestamp.empty()) {
                    std::cout << " @ " << decision.feedback_timestamp;
                }
                if (!decision.feedback_note.empty()) {
                    std::cout << " | " << decision.feedback_note;
                }
                std::cout << "\n";
            }
            if (!decision.outcome_status.empty()) {
                std::cout << "   outcome: " << decision.outcome_status;
                if (!decision.outcome_timestamp.empty()) {
                    std::cout << " @ " << decision.outcome_timestamp;
                }
                if (!decision.outcome_note.empty()) {
                    std::cout << " | " << decision.outcome_note;
                }
                std::cout << "\n";
            }
        }
    }
    if (!report.case_links.empty()) {
        std::cout << "Case links:\n";
        for (const tze::CaseLink& link : report.case_links) {
            std::cout << " - [" << link.strength << "] " << link.left_case_id << " <-> " << link.right_case_id
                      << " | " << link.link_type << "=" << link.link_value;
            if (!link.rationale.empty()) {
                std::cout << " | " << link.rationale;
            }
            std::cout << "\n";
        }
    }
    if (!report.case_clusters.empty()) {
        std::cout << "Case clusters:\n";
        for (const tze::CaseCluster& cluster : report.case_clusters) {
            std::cout << " - [" << cluster.correlation_score << "] " << cluster.cluster_type
                      << " | " << cluster.title
                      << " | cases=" << cluster.case_count
                      << " | likelihood=" << cluster.likelihood << "\n";
            if (!cluster.summary.empty()) {
                std::cout << "   " << cluster.summary << "\n";
            }
        }
    }
    if (!report.next_action.empty()) {
        std::cout << "Next action: " << report.next_action << "\n";
    }
    if (report.query_session.has_value()) {
        std::cout << "Query session: " << report.query_session->id;
        if (!report.query_session->final_results.empty()) {
            std::cout << " | results=";
            for (std::size_t index = 0; index < report.query_session->final_results.size(); ++index) {
                if (index != 0) {
                    std::cout << ", ";
                }
                std::cout << report.query_session->final_results[index];
            }
        }
        std::cout << "\n";
    }
    if (!report.tze_stages.empty()) {
        std::cout << "TZE stages:\n";
        for (const tze::TzeStageRecord& stage : report.tze_stages) {
            std::cout << " - " << stage.stage_id << " [" << stage.status << "] " << stage.module;
            if (!stage.detail.empty()) {
                std::cout << " | " << stage.detail;
            }
            if (!stage.source_section.empty() || stage.source_line != 0) {
                std::cout << " | source=";
                if (!stage.source_section.empty()) {
                    std::cout << stage.source_section;
                }
                if (stage.source_line != 0) {
                    if (!stage.source_section.empty()) {
                        std::cout << ":";
                    }
                    std::cout << stage.source_line;
                }
            }
            std::cout << "\n";
        }
    }
    if (!report.memory_reads.empty()) {
        std::cout << "Memory used:\n";
        for (const std::string& path : report.memory_reads) {
            std::cout << " - " << path << "\n";
        }
    }
    if (!report.memory_writes.empty()) {
        std::cout << "Memory updated:\n";
        for (const std::string& path : report.memory_writes) {
            std::cout << " - " << path << "\n";
        }
    }
    if (!report.toolchain.empty()) {
        print_toolchain(report.toolchain);
    }
}

void print_processing_report(const tze::ProcessingReport& report,
                             OutputMode mode = OutputMode::Auto,
                             bool prefer_verbose = false) {
    if (use_verbose_output(mode, prefer_verbose)) {
        print_processing_report_verbose(report);
        return;
    }
    print_processing_report_compact(report);
}

CommonCliOptions parse_common_options(const std::vector<std::string>& args, std::size_t start_index, std::vector<std::string>* positional) {
    CommonCliOptions options;
    for (std::size_t index = start_index; index < args.size(); ++index) {
        const std::string& arg = args[index];
        if (arg == "--source-map") {
            if (index + 1 >= args.size()) {
                throw std::runtime_error("--source-map requires a value.");
            }
            options.source_map_path = args[++index];
            continue;
        }
        if (arg == "--memory-root") {
            if (index + 1 >= args.size()) {
                throw std::runtime_error("--memory-root requires a value.");
            }
            options.memory_root_path = args[++index];
            continue;
        }
        if (arg == "--target") {
            if (index + 1 >= args.size()) {
                throw std::runtime_error("--target requires a value.");
            }
            options.build_target = args[++index];
            continue;
        }
        if (arg == "--build-dir") {
            if (index + 1 >= args.size()) {
                throw std::runtime_error("--build-dir requires a value.");
            }
            options.build_dir = args[++index];
            continue;
        }
        if (arg == "--build-type") {
            if (index + 1 >= args.size()) {
                throw std::runtime_error("--build-type requires a value.");
            }
            options.build_type = args[++index];
            continue;
        }
        if (arg == "--install-prefix") {
            if (index + 1 >= args.size()) {
                throw std::runtime_error("--install-prefix requires a value.");
            }
            options.install_prefix = args[++index];
            continue;
        }
        if (arg == "--out") {
            if (index + 1 >= args.size()) {
                throw std::runtime_error("--out requires a value.");
            }
            options.output_path = args[++index];
            continue;
        }
        if (arg == "--note") {
            if (index + 1 >= args.size()) {
                throw std::runtime_error("--note requires a value.");
            }
            options.feedback_note = args[++index];
            continue;
        }
        if (arg == "--keep") {
            if (index + 1 >= args.size()) {
                throw std::runtime_error("--keep requires a value.");
            }
            options.keep_count = static_cast<std::size_t>(std::stoull(args[++index]));
            continue;
        }
        if (arg == "--ref") {
            if (index + 1 >= args.size()) {
                throw std::runtime_error("--ref requires a value.");
            }
            options.git_ref_override = args[++index];
            continue;
        }
        if (arg == "--recipe") {
            if (index + 1 >= args.size()) {
                throw std::runtime_error("--recipe requires a value.");
            }
            options.selected_recipe_id = args[++index];
            continue;
        }
        if (arg == "--lang-confirm") {
            if (index + 1 >= args.size()) {
                throw std::runtime_error("--lang-confirm requires a value.");
            }
            options.language_confirmation = args[++index];
            continue;
        }
        if (arg == "--clean") {
            options.clean = true;
            continue;
        }
        if (arg == "--build-only" || arg == "--no-install") {
            options.perform_install = false;
            continue;
        }
        if (arg == "--offline") {
            options.offline = true;
            continue;
        }
        if (arg == "--important-only") {
            options.important_only = true;
            continue;
        }
        if (arg == "--assist") {
            options.assist = true;
            continue;
        }
        if (arg == "--verbose") {
            options.output_mode = OutputMode::Verbose;
            continue;
        }
        if (arg == "--compact") {
            options.output_mode = OutputMode::Compact;
            continue;
        }
        if (arg == "--local-only") {
            options.acquisition_policy = tze::AcquisitionPolicy::LocalOnly;
            continue;
        }
        positional->push_back(arg);
    }
    return options;
}

tze::RequestProfile make_base_profile(const CommonCliOptions& options) {
    tze::RequestProfile profile;
    profile.operator_handle = "omnix-cli";
    profile.operator_is_admin = true;
    profile.persist_on_success = true;
    profile.estimated_size = 256 * 1024;
    profile.acquisition_policy = options.acquisition_policy;
    profile.memory_root_path = options.memory_root_path;
    profile.clean_build = options.clean;
    profile.perform_install = options.perform_install;
    profile.offline = options.offline;
    profile.build_dir = options.build_dir;
    profile.build_target = options.build_target;
    profile.build_type = options.build_type;
    profile.install_prefix = options.install_prefix;
    profile.git_ref_override = options.git_ref_override;
    profile.selected_recipe_id = options.selected_recipe_id;
    profile.language_confirmation = options.language_confirmation;
    profile.assist_requested = options.assist;
    profile.feedback_note = options.feedback_note;
    profile.prune_keep_count = options.keep_count;
    profile.important_only = options.important_only;
    profile.output_path = options.output_path;
    if (!options.source_map_path.empty()) {
        profile.source_map_path = options.source_map_path;
    }
    return profile;
}

std::string join_positional_arguments(const std::vector<std::string>& positional) {
    std::ostringstream joined;
    for (std::size_t index = 0; index < positional.size(); ++index) {
        if (index != 0) {
            joined << ' ';
        }
        joined << positional[index];
    }
    return joined.str();
}

std::vector<std::string> split_whitespace(std::string_view text) {
    std::vector<std::string> tokens;
    std::string current;
    for (char c : text) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!current.empty()) {
                tokens.push_back(current);
                current.clear();
            }
            continue;
        }
        current.push_back(c);
    }
    if (!current.empty()) {
        tokens.push_back(current);
    }
    return tokens;
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

struct ToolCliInvocation {
    CommonCliOptions options;
    tze::ToolCommandMode mode = tze::ToolCommandMode::None;
    std::string tool_name;
    std::vector<std::string> tool_arguments;
};

ToolCliInvocation parse_tool_invocation(const std::vector<std::string>& args) {
    if (args.size() < 3) {
        throw std::runtime_error("tool requires a subcommand or tool name.");
    }

    ToolCliInvocation invocation;
    const std::string subcommand = args[2];
    if (subcommand == "list") {
        invocation.mode = tze::ToolCommandMode::List;
        std::vector<std::string> positional;
        invocation.options = parse_common_options(args, 3, &positional);
        return invocation;
    }

    if (subcommand == "locate" || subcommand == "doctor") {
        std::vector<std::string> positional;
        invocation.options = parse_common_options(args, 3, &positional);
        if (positional.empty()) {
            throw std::runtime_error("tool locate/doctor requires a tool name.");
        }
        invocation.mode = subcommand == "locate" ? tze::ToolCommandMode::Locate : tze::ToolCommandMode::Doctor;
        invocation.tool_name = positional.front();
        if (positional.size() > 1) {
            invocation.tool_arguments.assign(positional.begin() + 1, positional.end());
        }
        return invocation;
    }

    invocation.mode = tze::ToolCommandMode::Run;
    invocation.tool_name = subcommand;
    bool passthrough = false;
    for (std::size_t index = 3; index < args.size(); ++index) {
        const std::string& arg = args[index];
        if (!passthrough && arg == "--") {
            passthrough = true;
            continue;
        }
        if (!passthrough && arg == "--memory-root") {
            if (index + 1 >= args.size()) {
                throw std::runtime_error("--memory-root requires a value.");
            }
            invocation.options.memory_root_path = args[++index];
            continue;
        }
        if (!passthrough && arg == "--source-map") {
            if (index + 1 >= args.size()) {
                throw std::runtime_error("--source-map requires a value.");
            }
            invocation.options.source_map_path = args[++index];
            continue;
        }
        invocation.tool_arguments.push_back(arg);
    }
    return invocation;
}

bool build_like_success(const tze::ProcessingReport& report) {
    if (report.answer_status == "native_ready") {
        return true;
    }
    if (report.build_execution.has_value()) {
        return report.build_execution->status == "built" || report.build_execution->status == "installed";
    }
    return report.preflight_report.has_value() && report.preflight_report->ready;
}

int run_build_cmake(const std::vector<std::string>& args) {
    std::vector<std::string> positional;
    const CommonCliOptions options = parse_common_options(args, 2, &positional);
    const std::filesystem::path source_dir = positional.empty()
        ? (find_project_root(std::filesystem::current_path()).empty() ? std::filesystem::current_path() : find_project_root(std::filesystem::current_path()))
        : std::filesystem::path(positional.front());

    tze::RequestProfile profile = make_base_profile(options);
    profile.raw_prompt = "Build CMake";
    profile.instruction_slot = "aZ::1";
    profile.resolved_intent = tze::RequestIntent::BuildProject;
    profile.project_reference = source_dir.string();
    profile.execute_build = true;
    profile.perform_install = false;
    profile.build_source_path = source_dir.string();
    if (profile.source_map_path == "res/tze.txt") {
        const std::filesystem::path candidate = optional_source_file(source_dir);
        if (!candidate.empty()) {
            profile.source_map_path = candidate.string();
        } else {
            profile.source_map_path.clear();
        }
    }

    tze::ProcessingEngine engine;
    const tze::ProcessingReport report = engine.process(profile);
    print_processing_report(report, options.output_mode, false);
    return build_like_success(report) ? 0 : 1;
}

int run_build_prompt(const std::vector<std::string>& args) {
    std::vector<std::string> positional;
    const CommonCliOptions options = parse_common_options(args, 2, &positional);
    if (positional.empty()) {
        throw std::runtime_error("build requires a project alias or source path.");
    }

    tze::RequestProfile profile = make_base_profile(options);
    profile.raw_prompt = "Build " + positional.front();
    profile.project_reference = positional.front();
    profile.instruction_slot = "aZ::1";
    profile.resolved_intent = tze::RequestIntent::BuildProject;
    profile.execute_build = true;
    if (profile.source_map_path == "res/tze.txt") {
        const std::filesystem::path candidate = optional_source_file();
        if (!candidate.empty()) {
            profile.source_map_path = candidate.string();
        } else {
            profile.source_map_path.clear();
        }
    }

    tze::ProcessingEngine engine;
    const tze::ProcessingReport report = engine.process(profile);
    print_processing_report(report, options.output_mode, false);
    return build_like_success(report) ? 0 : 1;
}

int run_preflight(const std::vector<std::string>& args) {
    std::vector<std::string> positional;
    const CommonCliOptions options = parse_common_options(args, 2, &positional);
    if (positional.empty()) {
        throw std::runtime_error("preflight requires a project alias or source path.");
    }

    tze::RequestProfile profile = make_base_profile(options);
    profile.raw_prompt = "Build " + positional.front();
    profile.project_reference = positional.front();
    profile.instruction_slot = "aZ::1";
    profile.resolved_intent = tze::RequestIntent::BuildProject;
    profile.execute_build = true;
    profile.preflight_only = true;
    if (profile.source_map_path == "res/tze.txt") {
        const std::filesystem::path candidate = optional_source_file();
        if (!candidate.empty()) {
            profile.source_map_path = candidate.string();
        } else {
            profile.source_map_path.clear();
        }
    }

    tze::ProcessingEngine engine;
    const tze::ProcessingReport report = engine.process(profile);
    print_processing_report(report, options.output_mode, false);
    return build_like_success(report) ? 0 : 1;
}

int run_doctor(const std::vector<std::string>& args) {
    std::vector<std::string> positional;
    const CommonCliOptions options = parse_common_options(args, 2, &positional);
    if (positional.empty()) {
        throw std::runtime_error("doctor requires a project alias or source path.");
    }

    tze::RequestProfile profile = make_base_profile(options);
    profile.raw_prompt = "doctor " + positional.front();
    profile.project_reference = positional.front();
    profile.instruction_slot = "aZ::2";
    profile.resolved_intent = tze::RequestIntent::DoctorProject;
    if (profile.source_map_path == "res/tze.txt") {
        const std::filesystem::path candidate = optional_source_file();
        if (!candidate.empty()) {
            profile.source_map_path = candidate.string();
        } else {
            profile.source_map_path.clear();
        }
    }

    tze::ProcessingEngine engine;
    const tze::ProcessingReport report = engine.process(profile);
    print_processing_report(report, options.output_mode, false);
    if (report.answer_status == "native_ready" || report.answer_status == "build_ready") {
        return 0;
    }
    return report.preflight_report.has_value() && report.preflight_report->ready ? 0 : 1;
}

int run_provider(const std::vector<std::string>& args) {
    std::vector<std::string> positional;
    const CommonCliOptions options = parse_common_options(args, 2, &positional);
    if (positional.empty() || positional.front() != "probe") {
        throw std::runtime_error("provider currently supports only `probe`.");
    }

    tze::RequestProfile profile = make_base_profile(options);
    profile.raw_prompt = "provider probe";
    profile.resolved_intent = tze::RequestIntent::ProbeProvider;
    profile.source_map_path.clear();

    tze::ProcessingEngine engine;
    const tze::ProcessingReport report = engine.process(profile);
    print_processing_report(report, options.output_mode, false);
    return report.answer_status == "provider_ready" || report.answer_status == "provider_inactive" ? 0 : 1;
}

int run_analyst_command(const std::vector<std::string>& args,
                        tze::RequestIntent intent,
                        const std::string& command_name,
                        const std::string& success_status) {
    std::vector<std::string> positional;
    const CommonCliOptions options = parse_common_options(args, 2, &positional);
    if (positional.empty()) {
        throw std::runtime_error(command_name + " requires a case id, source path, or command reference.");
    }

    const std::string target = join_positional_arguments(positional);
    tze::RequestProfile profile = make_base_profile(options);
    profile.raw_prompt = command_name + " " + target;
    profile.analyst_reference = target;
    profile.project_reference = target;
    profile.instruction_slot = "aZ::99";
    profile.resolved_intent = intent;
    if (profile.source_map_path == "res/tze.txt") {
        const std::filesystem::path candidate = optional_source_file();
        if (!candidate.empty()) {
            profile.source_map_path = candidate.string();
        } else {
            profile.source_map_path.clear();
        }
    }

    tze::ProcessingEngine engine;
    const tze::ProcessingReport report = engine.process(profile);
    print_processing_report(report, options.output_mode, false);
    return report.answer_status == success_status ? 0 : 1;
}

int run_ask(const std::vector<std::string>& args) {
    std::vector<std::string> positional;
    const CommonCliOptions options = parse_common_options(args, 2, &positional);
    if (positional.empty()) {
        throw std::runtime_error("ask requires a prompt.");
    }

    std::ostringstream prompt;
    for (std::size_t index = 0; index < positional.size(); ++index) {
        if (index != 0) {
            prompt << ' ';
        }
        prompt << positional[index];
    }

    tze::RequestProfile profile = make_base_profile(options);
    profile.raw_prompt = prompt.str();
    if (profile.source_map_path == "res/tze.txt") {
        const std::filesystem::path candidate = optional_source_file();
        if (!candidate.empty()) {
            profile.source_map_path = candidate.string();
        } else {
            profile.source_map_path.clear();
        }
    }

    tze::ProcessingEngine engine;
    const tze::ProcessingReport report = engine.process(profile);
    print_processing_report(report, options.output_mode, false);
    if (report.answer_status == "native_ready") {
        return 0;
    }
    return report.build_execution.has_value()
        ? ((report.build_execution->status == "built" || report.build_execution->status == "installed") ? 0 : 1)
        : (report.preflight_report.has_value() && !report.preflight_report->ready ? 1 : 0);
}

int run_ingest(const std::vector<std::string>& args) {
    return run_analyst_command(args, tze::RequestIntent::IngestData, "ingest", "ingested");
}

int run_analyze(const std::vector<std::string>& args) {
    return run_analyst_command(args, tze::RequestIntent::AnalyzeCase, "analyze", "analyzed");
}

int run_decide(const std::vector<std::string>& args) {
    std::vector<std::string> positional;
    const CommonCliOptions options = parse_common_options(args, 2, &positional);
    if (!positional.empty() && positional.front() == "feedback") {
        if (positional.size() < 4) {
            throw std::runtime_error("decide feedback requires <case-id> <decision-id> <helpful|not-helpful>.");
        }
        if (positional[3] != "helpful" && positional[3] != "not-helpful") {
            throw std::runtime_error("decide feedback requires `helpful` or `not-helpful`.");
        }
        tze::RequestProfile profile = make_base_profile(options);
        profile.raw_prompt = "decide feedback " + positional[1] + " " + positional[2] + " " + positional[3];
        profile.analyst_reference = positional[1];
        profile.decision_reference = positional[2];
        profile.feedback_value = positional[3];
        profile.resolved_intent = tze::RequestIntent::MarkDecisionFeedback;
        if (profile.source_map_path == "res/tze.txt") {
            const std::filesystem::path candidate = optional_source_file();
            profile.source_map_path = candidate.empty() ? std::string{} : candidate.string();
        }
        tze::ProcessingEngine engine;
        const tze::ProcessingReport report = engine.process(profile);
        print_processing_report(report, options.output_mode, false);
        return report.answer_status == "decision_feedback_recorded" ? 0 : 1;
    }

    if (!positional.empty() && positional.front() == "outcome") {
        if (positional.size() < 4) {
            throw std::runtime_error("decide outcome requires <case-id> <decision-id> <success|failed|partial>.");
        }
        if (positional[3] != "success" && positional[3] != "failed" && positional[3] != "partial") {
            throw std::runtime_error("decide outcome requires `success`, `failed`, or `partial`.");
        }
        tze::RequestProfile profile = make_base_profile(options);
        profile.raw_prompt = "decide outcome " + positional[1] + " " + positional[2] + " " + positional[3];
        profile.analyst_reference = positional[1];
        profile.decision_reference = positional[2];
        profile.feedback_value = positional[3];
        profile.resolved_intent = tze::RequestIntent::MarkDecisionOutcome;
        if (profile.source_map_path == "res/tze.txt") {
            const std::filesystem::path candidate = optional_source_file();
            profile.source_map_path = candidate.empty() ? std::string{} : candidate.string();
        }
        tze::ProcessingEngine engine;
        const tze::ProcessingReport report = engine.process(profile);
        print_processing_report(report, options.output_mode, false);
        return report.answer_status == "decision_outcome_recorded" ? 0 : 1;
    }

    return run_analyst_command(args, tze::RequestIntent::DecideAction, "decide", "decided");
}

int run_case(const std::vector<std::string>& args) {
    std::vector<std::string> positional;
    const CommonCliOptions options = parse_common_options(args, 2, &positional);
    if (positional.empty()) {
        throw std::runtime_error("case requires an id, `list`, or `search <term>`.");
    }

    tze::RequestProfile profile = make_base_profile(options);
    profile.instruction_slot = "aZ::99";
    profile.resolved_intent = tze::RequestIntent::InspectCase;

    if (positional.front() == "list") {
        profile.analyst_mode = "list";
        profile.analyst_reference = "list";
        profile.raw_prompt = "case list";
    } else if (positional.front() == "search") {
        if (positional.size() < 2) {
            throw std::runtime_error("case search requires a search term.");
        }
        profile.analyst_mode = "search";
        profile.analyst_query = join_positional_arguments(
            std::vector<std::string>(positional.begin() + 1, positional.end()));
        profile.analyst_reference = "search " + profile.analyst_query;
        profile.raw_prompt = profile.analyst_reference;
    } else if (positional.front() == "timeline") {
        if (positional.size() < 2) {
            throw std::runtime_error("case timeline requires a case id or source.");
        }
        profile.resolved_intent = tze::RequestIntent::CaseTimeline;
        profile.analyst_mode = "timeline";
        profile.analyst_reference = join_positional_arguments(std::vector<std::string>(positional.begin() + 1, positional.end()));
        profile.raw_prompt = "case timeline " + profile.analyst_reference;
    } else if (positional.front() == "export") {
        if (positional.size() < 2) {
            throw std::runtime_error("case export requires a case id or source.");
        }
        profile.resolved_intent = tze::RequestIntent::ExportCaseBundle;
        profile.analyst_mode = "export";
        profile.analyst_reference = join_positional_arguments(std::vector<std::string>(positional.begin() + 1, positional.end()));
        profile.raw_prompt = "case export " + profile.analyst_reference;
    } else if (positional.front() == "import") {
        if (positional.size() < 2) {
            throw std::runtime_error("case import requires a bundle path.");
        }
        profile.resolved_intent = tze::RequestIntent::ImportCaseBundle;
        profile.analyst_mode = "import";
        profile.analyst_reference = positional[1];
        profile.raw_prompt = "case import " + positional[1];
    } else {
        const std::string target = join_positional_arguments(positional);
        profile.analyst_mode = "inspect";
        profile.analyst_reference = target;
        profile.raw_prompt = "case " + target;
    }

    if (profile.source_map_path == "res/tze.txt") {
        const std::filesystem::path candidate = optional_source_file();
        if (!candidate.empty()) {
            profile.source_map_path = candidate.string();
        } else {
            profile.source_map_path.clear();
        }
    }

    tze::ProcessingEngine engine;
    const tze::ProcessingReport report = engine.process(profile);
    print_processing_report(report, options.output_mode, false);
    return report.answer_status == "case_loaded" || report.answer_status == "case_listed" ||
               report.answer_status == "case_search_results" || report.answer_status == "case_search_empty" ||
               report.answer_status == "case_timeline" || report.answer_status == "case_bundle_written" ||
               report.answer_status == "case_bundle_imported"
        ? 0
        : 1;
}

int run_incident(const std::vector<std::string>& args) {
    std::vector<std::string> positional;
    const CommonCliOptions options = parse_common_options(args, 2, &positional);
    if (positional.empty()) {
        throw std::runtime_error("incident requires an id, `list`, or `report <id>`.");
    }

    tze::RequestProfile profile = make_base_profile(options);
    profile.source_map_path.clear();

    if (positional.front() == "list") {
        profile.raw_prompt = "incident list";
        profile.resolved_intent = tze::RequestIntent::ListIncidents;
    } else if (positional.front() == "report") {
        if (positional.size() < 2) {
            throw std::runtime_error("incident report requires an incident id.");
        }
        profile.raw_prompt = "incident report " + positional[1];
        profile.incident_reference = positional[1];
        profile.resolved_intent = tze::RequestIntent::ReportIncident;
    } else {
        profile.raw_prompt = "incident " + positional.front();
        profile.incident_reference = positional.front();
        profile.resolved_intent = tze::RequestIntent::InspectIncident;
    }

    tze::ProcessingEngine engine;
    const tze::ProcessingReport report = engine.process(profile);
    print_processing_report(report, options.output_mode, false);
    return report.answer_status == "incident_listed" || report.answer_status == "incident_loaded" ||
               report.answer_status == "incident_report_written"
        ? 0
        : 1;
}

int run_define(const std::vector<std::string>& args) {
    std::vector<std::string> positional;
    const CommonCliOptions options = parse_common_options(args, 2, &positional);
    if (positional.empty()) {
        throw std::runtime_error("define requires a symbol or term.");
    }

    tze::RequestProfile profile = make_base_profile(options);
    profile.raw_prompt = "define " + positional.front();
    profile.instruction_slot = "aZ::99";
    profile.resolved_intent = tze::RequestIntent::DefineSymbol;
    if (positional.size() >= 2 && options.source_map_path.empty()) {
        profile.source_map_path = positional[1];
    } else if (profile.source_map_path == "res/tze.txt") {
        const std::filesystem::path candidate = optional_source_file();
        if (!candidate.empty()) {
            profile.source_map_path = candidate.string();
        } else {
            profile.source_map_path.clear();
        }
    }

    tze::ProcessingEngine engine;
    const tze::ProcessingReport report = engine.process(profile);
    print_processing_report(report, options.output_mode, true);
    return report.definition_answer.has_value() && report.definition_answer->found ? 0 : 1;
}

int run_explain(const std::vector<std::string>& args) {
    std::vector<std::string> positional;
    const CommonCliOptions options = parse_common_options(args, 2, &positional);
    if (positional.empty()) {
        throw std::runtime_error("explain requires a command or symbol.");
    }

    tze::RequestProfile profile = make_base_profile(options);
    profile.raw_prompt = "explain " + positional.front();
    profile.instruction_slot = "aZ::99";
    profile.resolved_intent = tze::RequestIntent::ExplainCommand;
    if (positional.size() >= 2 && options.source_map_path.empty()) {
        profile.source_map_path = positional[1];
    } else if (profile.source_map_path == "res/tze.txt") {
        const std::filesystem::path candidate = optional_source_file();
        if (!candidate.empty()) {
            profile.source_map_path = candidate.string();
        } else {
            profile.source_map_path.clear();
        }
    }

    tze::ProcessingEngine engine;
    const tze::ProcessingReport report = engine.process(profile);
    print_processing_report(report, options.output_mode, true);
    return report.definition_answer.has_value() && report.definition_answer->found ? 0 : 1;
}

int run_memory(const std::vector<std::string>& args) {
    std::vector<std::string> positional;
    const CommonCliOptions options = parse_common_options(args, 2, &positional);

    tze::RequestProfile profile = make_base_profile(options);
    if (!positional.empty() && positional.front() == "prune") {
        profile.raw_prompt = "memory prune";
        profile.resolved_intent = tze::RequestIntent::PruneMemory;
    } else {
        profile.raw_prompt = positional.empty() ? "memory history" : "memory " + positional.front();
        profile.memory_view = positional.empty() ? "history" : positional.front();
        profile.resolved_intent = tze::RequestIntent::ShowMemory;
    }
    profile.source_map_path.clear();

    tze::ProcessingEngine engine;
    const tze::ProcessingReport report = engine.process(profile);
    print_processing_report(report, options.output_mode, true);
    return 0;
}

int run_tze(const std::vector<std::string>& args) {
    std::vector<std::string> positional;
    const CommonCliOptions options = parse_common_options(args, 2, &positional);
    if (positional.empty()) {
        throw std::runtime_error("tze requires `runs`, `latest`, `replay`, `chain`, `diff`, `diff-latest`, `explain-change`, `explain-change-latest`, `report`, `diff-report`, `export`, `import`, `prune`, or `mark`.");
    }

    auto run_profile = [&](tze::RequestProfile profile, std::string_view success_status) -> int {
        tze::ProcessingEngine engine;
        const tze::ProcessingReport report = engine.process(profile);
        print_processing_report(report, options.output_mode, true);
        return report.answer_status == success_status ? 0 : 1;
    };

    if (positional.front() == "runs") {
        tze::RequestProfile profile = make_base_profile(options);
        profile.raw_prompt = "memory runs";
        profile.memory_view = "runs";
        profile.resolved_intent = tze::RequestIntent::ShowMemory;
        profile.source_map_path.clear();

        return run_profile(profile, "memory");
    }

    if (positional.front() == "latest") {
        tze::RequestProfile profile = make_base_profile(options);
        profile.raw_prompt = "tze latest";
        profile.tze_run_reference = options.important_only ? "latest-important" : "latest";
        profile.resolved_intent = tze::RequestIntent::ReplayTzeRun;
        profile.source_map_path.clear();
        return run_profile(profile, "tze_run_replayed");
    }

    if (positional.front() == "replay") {
        if (positional.size() < 2) {
            throw std::runtime_error("tze replay requires a run id.");
        }
        tze::RequestProfile profile = make_base_profile(options);
        profile.raw_prompt = "tze replay " + positional[1];
        profile.tze_run_reference = positional[1];
        profile.resolved_intent = tze::RequestIntent::ReplayTzeRun;
        profile.source_map_path.clear();
        return run_profile(profile, "tze_run_replayed");
    }

    if (positional.front() == "chain") {
        if (positional.size() < 2) {
            throw std::runtime_error("tze chain requires a run id or `latest`.");
        }
        tze::RequestProfile profile = make_base_profile(options);
        profile.raw_prompt = "tze chain " + positional[1];
        profile.tze_run_reference = positional[1];
        profile.resolved_intent = tze::RequestIntent::ChainTzeRun;
        profile.source_map_path.clear();
        return run_profile(profile, "tze_chain_rendered");
    }

    if (positional.front() == "diff") {
        if (positional.size() < 3) {
            throw std::runtime_error("tze diff requires two run ids.");
        }
        tze::RequestProfile profile = make_base_profile(options);
        profile.raw_prompt = "tze diff " + positional[1] + " " + positional[2];
        profile.tze_run_reference = positional[1];
        profile.tze_compare_reference = positional[2];
        profile.resolved_intent = tze::RequestIntent::DiffTzeRuns;
        profile.source_map_path.clear();
        return run_profile(profile, "tze_run_diffed");
    }

    if (positional.front() == "diff-latest") {
        tze::RequestProfile profile = make_base_profile(options);
        profile.raw_prompt = "tze diff-latest";
        profile.tze_run_reference = options.important_only ? "latest-important" : "latest";
        profile.tze_compare_reference = options.important_only ? "previous-important" : "previous";
        profile.resolved_intent = tze::RequestIntent::DiffTzeRuns;
        profile.source_map_path.clear();
        return run_profile(profile, "tze_run_diffed");
    }

    if (positional.front() == "explain-change") {
        if (positional.size() < 3) {
            throw std::runtime_error("tze explain-change requires two run ids.");
        }
        tze::RequestProfile profile = make_base_profile(options);
        profile.raw_prompt = "tze explain-change " + positional[1] + " " + positional[2];
        profile.tze_run_reference = positional[1];
        profile.tze_compare_reference = positional[2];
        profile.resolved_intent = tze::RequestIntent::ExplainTzeChange;
        profile.source_map_path.clear();
        return run_profile(profile, "tze_change_explained");
    }

    if (positional.front() == "explain-change-latest") {
        tze::RequestProfile profile = make_base_profile(options);
        profile.raw_prompt = "tze explain-change-latest";
        profile.tze_run_reference = options.important_only ? "latest-important" : "latest";
        profile.tze_compare_reference = options.important_only ? "previous-important" : "previous";
        profile.resolved_intent = tze::RequestIntent::ExplainTzeChange;
        profile.source_map_path.clear();
        return run_profile(profile, "tze_change_explained");
    }

    if (positional.front() == "report") {
        if (positional.size() < 2) {
            throw std::runtime_error("tze report requires a run id.");
        }
        tze::RequestProfile profile = make_base_profile(options);
        profile.raw_prompt = "tze report " + positional[1];
        profile.tze_run_reference = positional[1];
        profile.resolved_intent = tze::RequestIntent::ReportTzeRun;
        profile.source_map_path.clear();
        return run_profile(profile, "tze_report_written");
    }

    if (positional.front() == "diff-report") {
        if (positional.size() < 3) {
            throw std::runtime_error("tze diff-report requires two run ids.");
        }
        tze::RequestProfile profile = make_base_profile(options);
        profile.raw_prompt = "tze diff-report " + positional[1] + " " + positional[2];
        profile.tze_run_reference = positional[1];
        profile.tze_compare_reference = positional[2];
        profile.resolved_intent = tze::RequestIntent::DiffReportTzeRuns;
        profile.source_map_path.clear();
        return run_profile(profile, "tze_diff_report_written");
    }

    if (positional.front() == "export") {
        if (positional.size() < 2) {
            throw std::runtime_error("tze export requires a run id or `latest`.");
        }
        tze::RequestProfile profile = make_base_profile(options);
        profile.raw_prompt = "tze export " + positional[1];
        profile.tze_run_reference = positional[1];
        profile.resolved_intent = tze::RequestIntent::ExportTzeBundle;
        profile.source_map_path.clear();
        return run_profile(profile, "tze_bundle_written");
    }

    if (positional.front() == "import") {
        if (positional.size() < 2) {
            throw std::runtime_error("tze import requires a bundle path.");
        }
        tze::RequestProfile profile = make_base_profile(options);
        profile.raw_prompt = "tze import " + positional[1];
        profile.tze_run_reference = positional[1];
        profile.resolved_intent = tze::RequestIntent::ImportTzeBundle;
        profile.source_map_path.clear();
        return run_profile(profile, "tze_bundle_imported");
    }

    if (positional.front() == "prune") {
        tze::RequestProfile profile = make_base_profile(options);
        profile.raw_prompt = "tze prune";
        profile.resolved_intent = tze::RequestIntent::PruneTzeRuns;
        profile.source_map_path.clear();
        return run_profile(profile, "tze_pruned");
    }

    if (positional.front() == "mark") {
        if (positional.size() < 3) {
            throw std::runtime_error("tze mark requires a run id and either `helpful` or `not-helpful`.");
        }
        if (positional[2] != "helpful" && positional[2] != "not-helpful") {
            throw std::runtime_error("tze mark requires `helpful` or `not-helpful` as the feedback value.");
        }
        tze::RequestProfile profile = make_base_profile(options);
        profile.raw_prompt = "tze mark " + positional[1] + " " + positional[2];
        profile.tze_run_reference = positional[1];
        profile.feedback_value = positional[2];
        profile.resolved_intent = tze::RequestIntent::MarkTzeRunOutcome;
        profile.source_map_path.clear();
        return run_profile(profile, "tze_feedback_recorded");
    }

    throw std::runtime_error("tze requires `runs`, `latest`, `replay`, `chain`, `diff`, `diff-latest`, `explain-change`, `explain-change-latest`, `report`, `diff-report`, `export`, `import`, `prune`, or `mark`.");
}

struct ShellState {
    CommonCliOptions options;
    bool assist_enabled = false;
    std::string current_case_id;
    std::string current_incident_id;
    std::string current_run_id;
    std::optional<tze::ProcessingReport> last_report;
};

void print_shell_help() {
    std::cout << "OmniX shell commands:\n";
    std::cout << " - /help\n";
    std::cout << " - /status\n";
    std::cout << " - /provider\n";
    std::cout << " - /assist on|off\n";
    std::cout << " - /verbose on|off\n";
    std::cout << " - /why\n";
    std::cout << " - /case <id>\n";
    std::cout << " - /incident <id>\n";
    std::cout << " - /replay [run-id|latest]\n";
    std::cout << " - /report [run-id|latest]\n";
    std::cout << " - /diff <left-run-id> <right-run-id>\n";
    std::cout << " - /quit\n";
    std::cout << "Plain input is routed through the normal OmniX intent resolver.\n";
}

std::string shell_prompt(const ShellState& state) {
    std::ostringstream prompt;
    prompt << "omnix";
    if (!state.current_case_id.empty()) {
        prompt << "[case:" << state.current_case_id << "]";
    }
    if (state.assist_enabled) {
        prompt << "[assist]";
    }
    prompt << "> ";
    return prompt.str();
}

std::string expand_shell_input(std::string line, const ShellState& state) {
    line = trim(line);
    if (line.rfind("./build/omnix ", 0) == 0) {
        line = trim(line.substr(std::string("./build/omnix ").size()));
    } else if (line.rfind("build/omnix ", 0) == 0) {
        line = trim(line.substr(std::string("build/omnix ").size()));
    } else if (line.rfind("omnix ", 0) == 0) {
        line = trim(line.substr(std::string("omnix ").size()));
    }

    std::string lowered;
    lowered.reserve(line.size());
    for (char c : line) {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    if (lowered.rfind("ollama,", 0) == 0) {
        line = trim(line.substr(std::string("ollama,").size()));
        lowered = lowered.substr(std::string("ollama,").size());
        lowered = trim(lowered);
    } else if (lowered.rfind("ollama ", 0) == 0) {
        line = trim(line.substr(std::string("ollama ").size()));
        lowered = lowered.substr(std::string("ollama ").size());
        lowered = trim(lowered);
    }

    if (lowered == "provider") {
        return "provider probe";
    }
    if (lowered.rfind("ask ", 0) == 0) {
        line = trim(line.substr(std::string("ask ").size()));
        lowered = trim(lowered.substr(std::string("ask ").size()));
    }
    if (line == "case" && !state.current_case_id.empty()) {
        return "case " + state.current_case_id;
    }
    if (line == "decide" && !state.current_case_id.empty()) {
        return "decide " + state.current_case_id;
    }
    if (line == "incident" && !state.current_incident_id.empty()) {
        return "incident " + state.current_incident_id;
    }
    return line;
}

bool shell_requests_last_results(std::string_view line, const ShellState& state) {
    const std::string trimmed = trim(line);
    std::string lowered;
    lowered.reserve(trimmed.size());
    for (char c : trimmed) {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }

    if (lowered == "results" || lowered == "last results") {
        return true;
    }
    if (!state.last_report.has_value()) {
        return false;
    }

    if (!state.last_report->resolved_project.empty()) {
        std::string project = state.last_report->resolved_project;
        std::transform(project.begin(), project.end(), project.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (lowered == project + " results") {
            return true;
        }
        if (lowered.find(project) != std::string::npos && lowered.find("results") != std::string::npos) {
            return true;
        }
    }
    if (state.last_report->tool_invocation_report.has_value() &&
        !state.last_report->tool_invocation_report->logical_name.empty()) {
        std::string logical_name = state.last_report->tool_invocation_report->logical_name;
        std::transform(logical_name.begin(), logical_name.end(), logical_name.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (lowered == logical_name + " results") {
            return true;
        }
        if (lowered.find(logical_name) != std::string::npos && lowered.find("results") != std::string::npos) {
            return true;
        }
    }
    if (lowered.find("where are my") != std::string::npos && lowered.find("results") != std::string::npos) {
        return true;
    }
    return false;
}

void print_shell_last_results(const ShellState& state) {
    if (!state.last_report.has_value()) {
        std::cout << "No prior results are available in this shell.\n";
        return;
    }

    const tze::ProcessingReport& report = *state.last_report;
    if (report.tool_invocation_report.has_value()) {
        const tze::ToolInvocationReport& invocation = *report.tool_invocation_report;
        std::cout << "Last results:\n";
        if (!invocation.command_line.empty()) {
            std::cout << " - command: " << invocation.command_line << "\n";
        }
        if (!invocation.output_excerpt.empty()) {
            for (const std::string& line : invocation.output_excerpt) {
                std::cout << " - " << line << "\n";
            }
        } else {
            std::cout << " - No tool output was captured.\n";
        }
        return;
    }

    print_processing_report(report, OutputMode::Verbose, true);
}

void print_shell_status(const ShellState& state) {
    std::cout << "Shell status:\n";
    std::cout << " - Assist mode: " << (state.assist_enabled ? "on" : "off") << "\n";
    std::cout << " - Output mode: "
              << (state.options.output_mode == OutputMode::Verbose
                      ? "verbose"
                      : (state.options.output_mode == OutputMode::Compact ? "compact" : "auto"))
              << "\n";
    std::cout << " - Memory root: "
              << (state.options.memory_root_path.empty() ? std::string("(default)") : state.options.memory_root_path) << "\n";
    std::cout << " - Source map: "
              << (state.options.source_map_path.empty() ? std::string("(auto)") : state.options.source_map_path) << "\n";
    if (!state.current_case_id.empty()) {
        std::cout << " - Current case: " << state.current_case_id << "\n";
    }
    if (!state.current_incident_id.empty()) {
        std::cout << " - Current incident: " << state.current_incident_id << "\n";
    }
    if (!state.current_run_id.empty()) {
        std::cout << " - Current run: " << state.current_run_id << "\n";
    }
}

void update_shell_state(ShellState& state, const tze::ProcessingReport& report, std::string_view expanded_input) {
    state.last_report = report;
    if (!report.tze_run_id.empty()) {
        state.current_run_id = report.tze_run_id;
    }
    if (report.case_record.has_value()) {
        state.current_case_id = report.case_record->id;
    }
    const std::string line = trim(expanded_input);
    if (line.rfind("incident ", 0) == 0 && line != "incident list" && line.rfind("incident report ", 0) != 0) {
        state.current_incident_id = trim(line.substr(std::string("incident ").size()));
    }
}

int run_shell(const std::vector<std::string>& args) {
    std::vector<std::string> positional;
    ShellState state;
    state.options = parse_common_options(args, 2, &positional);
    state.assist_enabled = state.options.assist;

    tze::ProcessingEngine engine;
    std::cout << "OmniX shell started. Type /help for commands.\n";

    for (std::string line; std::cout << shell_prompt(state), std::getline(std::cin, line);) {
        line = trim(line);
        if (line.empty()) {
            continue;
        }

        if (line.front() == '/') {
            const std::vector<std::string> tokens = split_whitespace(line);
            const std::string command = tokens.front();
            if (command == "/quit" || command == "/exit") {
                std::cout << "Closing OmniX shell.\n";
                return 0;
            }
            if (command == "/help") {
                print_shell_help();
                continue;
            }
            if (command == "/status") {
                print_shell_status(state);
                continue;
            }
            if (command == "/assist") {
                if (tokens.size() < 2) {
                    std::cout << "Assist mode is " << (state.assist_enabled ? "on" : "off") << ".\n";
                } else if (tokens[1] == "on") {
                    state.assist_enabled = true;
                    std::cout << "Assist mode enabled for guarded tasks.\n";
                } else if (tokens[1] == "off") {
                    state.assist_enabled = false;
                    std::cout << "Assist mode disabled; OmniX will stay deterministic-only.\n";
                } else {
                    std::cout << "Use `/assist on` or `/assist off`.\n";
                }
                continue;
            }
            if (command == "/verbose") {
                if (tokens.size() < 2) {
                    std::cout << "Output mode is "
                              << (state.options.output_mode == OutputMode::Verbose ? "verbose" : "compact") << ".\n";
                } else if (tokens[1] == "on") {
                    state.options.output_mode = OutputMode::Verbose;
                    std::cout << "Verbose output enabled.\n";
                } else if (tokens[1] == "off") {
                    state.options.output_mode = OutputMode::Compact;
                    std::cout << "Compact output enabled.\n";
                } else {
                    std::cout << "Use `/verbose on` or `/verbose off`.\n";
                }
                continue;
            }
            if (command == "/why") {
                if (!state.last_report.has_value()) {
                    std::cout << "No prior command has been executed in this shell.\n";
                } else {
                    print_processing_report(*state.last_report, OutputMode::Verbose, true);
                }
                continue;
            }
            if (command == "/case") {
                if (tokens.size() < 2) {
                    if (state.current_case_id.empty()) {
                        std::cout << "No current case is set.\n";
                    } else {
                        std::cout << "Current case: " << state.current_case_id << "\n";
                    }
                } else {
                    state.current_case_id = tokens[1];
                    std::cout << "Current case set to " << state.current_case_id << ".\n";
                }
                continue;
            }
            if (command == "/incident") {
                if (tokens.size() < 2) {
                    if (state.current_incident_id.empty()) {
                        std::cout << "No current incident is set.\n";
                    } else {
                        std::cout << "Current incident: " << state.current_incident_id << "\n";
                    }
                } else {
                    state.current_incident_id = tokens[1];
                    std::cout << "Current incident set to " << state.current_incident_id << ".\n";
                }
                continue;
            }
            if (command == "/provider") {
                line = "provider probe";
            } else if (command == "/replay") {
                line = "tze replay " + (tokens.size() >= 2 ? tokens[1] : std::string("latest"));
            } else if (command == "/report") {
                line = "tze report " + (tokens.size() >= 2 ? tokens[1] : std::string("latest"));
            } else if (command == "/diff") {
                if (tokens.size() < 3) {
                    std::cout << "Use `/diff <left-run-id> <right-run-id>`.\n";
                    continue;
                }
                line = "tze diff " + tokens[1] + " " + tokens[2];
            } else {
                std::cout << "Unknown shell command. Type /help for commands.\n";
                continue;
            }
        }

        const std::string expanded = expand_shell_input(line, state);
        if (shell_requests_last_results(expanded, state)) {
            print_shell_last_results(state);
            continue;
        }
        tze::RequestProfile profile = make_base_profile(state.options);
        profile.assist_requested = state.assist_enabled;
        profile.raw_prompt = expanded;
        if (profile.source_map_path == "res/tze.txt") {
            const std::filesystem::path candidate = optional_source_file();
            if (!candidate.empty()) {
                profile.source_map_path = candidate.string();
            } else {
                profile.source_map_path.clear();
            }
        }

        if (expanded.rfind("case ", 0) == 0 && expanded != "case list") {
            profile.analyst_reference = trim(expanded.substr(std::string("case ").size()));
        } else if (expanded.rfind("decide ", 0) == 0) {
            profile.analyst_reference = trim(expanded.substr(std::string("decide ").size()));
        } else if (expanded.rfind("incident ", 0) == 0 && expanded != "incident list" &&
                   expanded.rfind("incident report ", 0) != 0) {
            profile.incident_reference = trim(expanded.substr(std::string("incident ").size()));
        }

        const tze::ProcessingReport report = engine.process(profile);
        print_processing_report(report, state.options.output_mode, false);
        update_shell_state(state, report, expanded);
    }

    std::cout << "Closing OmniX shell.\n";
    return 0;
}

int run_tool(const std::vector<std::string>& args) {
    const ToolCliInvocation invocation = parse_tool_invocation(args);
    tze::RequestProfile profile = make_base_profile(invocation.options);
    profile.resolved_intent = tze::RequestIntent::ToolAction;
    profile.tool_mode = invocation.mode;
    profile.requested_tool_name = invocation.tool_name;
    profile.tool_arguments = invocation.tool_arguments;
    profile.raw_prompt = invocation.mode == tze::ToolCommandMode::List
        ? "tool list"
        : ("tool " + invocation.tool_name);
    if (profile.source_map_path == "res/tze.txt") {
        const std::filesystem::path candidate = optional_source_file();
        if (!candidate.empty()) {
            profile.source_map_path = candidate.string();
        } else {
            profile.source_map_path.clear();
        }
    }

    tze::ProcessingEngine engine;
    const tze::ProcessingReport report = engine.process(profile);
    print_processing_report(report, invocation.options.output_mode, false);
    if (invocation.mode == tze::ToolCommandMode::List) {
        return 0;
    }
    if (invocation.mode == tze::ToolCommandMode::Locate) {
        return report.tool_resolution.has_value() && report.tool_resolution->found ? 0 : 1;
    }
    if (invocation.mode == tze::ToolCommandMode::Doctor) {
        return report.tool_doctor_report.has_value() &&
                   (report.tool_doctor_report->status == "native_ready" ||
                    report.tool_doctor_report->status == "builtin_ready")
            ? 0
            : 1;
    }
    return report.tool_invocation_report.has_value() && report.tool_invocation_report->status == "ok" ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 2) {
            print_usage();
            return 1;
        }

        const std::vector<std::string> args(argv, argv + argc);
        const std::string command = args[1];
        if (command == "--version" || command == "version") {
            std::cout << "omnix " << OMNIX_VERSION << "\n";
            return 0;
        }
        if (command == "map") {
            if (argc < 3) {
                throw std::runtime_error("map requires a source file path.");
            }
            run_map(args[2]);
            return 0;
        }

        if (command == "search") {
            if (argc < 3) {
                throw std::runtime_error("search requires a symbol query.");
            }
            const std::filesystem::path source_file = argc >= 4 ? std::filesystem::path(args[3]) : default_source_file();
            run_search(args[2], source_file);
            return 0;
        }

        if (command == "emit-cpp") {
            if (argc < 3) {
                throw std::runtime_error("emit-cpp requires a source file path.");
            }
            const std::filesystem::path source_file = args[2];
            const std::filesystem::path output_dir = argc >= 4 ? std::filesystem::path(args[3]) : default_emit_dir(source_file);
            run_emit(source_file, output_dir);
            return 0;
        }

        if (command == "ask") {
            return run_ask(args);
        }

        if (command == "ingest") {
            return run_ingest(args);
        }

        if (command == "analyze") {
            return run_analyze(args);
        }

        if (command == "decide") {
            return run_decide(args);
        }

        if (command == "case") {
            return run_case(args);
        }

        if (command == "incident") {
            return run_incident(args);
        }

        if (command == "define") {
            return run_define(args);
        }

        if (command == "explain") {
            return run_explain(args);
        }

        if (command == "build") {
            return run_build_prompt(args);
        }

        if (command == "preflight") {
            return run_preflight(args);
        }

        if (command == "doctor") {
            return run_doctor(args);
        }

        if (command == "provider") {
            return run_provider(args);
        }

        if (command == "shell") {
            return run_shell(args);
        }

        if (command == "memory") {
            return run_memory(args);
        }

        if (command == "tool") {
            return run_tool(args);
        }

        if (command == "tze") {
            return run_tze(args);
        }

        if (command == "build-cmake") {
            return run_build_cmake(args);
        }

        print_usage();
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << "\n";
        return 1;
    }
}
