#include "tze/definition_flow_interpreter.hpp"

#include <algorithm>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "tze/language_engine.hpp"
#include "tze/omni_bridge.hpp"
#include "tze/preprocessor_runtime.hpp"
#include "tze/security_manager.hpp"

namespace tze {
namespace {

std::vector<std::string> recent_history_for_term(const MemorySnapshot& memory, std::string_view term) {
    std::vector<std::string> matches;
    if (term.empty()) {
        return matches;
    }

    for (auto it = memory.history.rbegin(); it != memory.history.rend(); ++it) {
        if (it->prompt.find(term) == std::string::npos && it->project.find(term) == std::string::npos) {
            continue;
        }
        matches.push_back(it->timestamp + " | " + it->status + " | " + it->summary);
        if (matches.size() >= 5) {
            break;
        }
    }
    return matches;
}

std::vector<KnowledgeReference> knowledge_order(std::string_view source_map_path, const MemorySnapshot& memory) {
    std::vector<KnowledgeReference> references = {
        {"memory", "Local OmniX memory store with persisted definitions and prior outcomes.", 1},
        {"native_tool_inventory", "Cached native tool providers and command recipes discovered on this host.", 2},
        {"source_map", "Current source-backed mapping index from the active X++/pseudo map.", 2},
        {"operand_catalogue", "Bundled operand catalogue for canonical operator and runtime semantics.", 3},
    };

    if (source_map_path.empty()) {
        references.erase(references.begin() + 1);
    }

    if (memory.definitions.empty()) {
        references.front().excerpt = "No stored definition override yet; fall through to source-backed mappings first.";
        references.front().priority = source_map_path.empty() ? 2 : 3;
    }

    if (memory.native_tools.empty()) {
        references[1].excerpt = "No cached native tool providers yet; fall through to source-backed mappings and static tool definitions.";
        references[1].priority = source_map_path.empty() ? 3 : 4;
    }

    std::stable_sort(references.begin(), references.end(), [](const KnowledgeReference& lhs, const KnowledgeReference& rhs) {
        return lhs.priority < rhs.priority;
    });
    return references;
}

std::string join_lines(const std::vector<std::string>& lines) {
    std::ostringstream out;
    for (std::size_t index = 0; index < lines.size(); ++index) {
        if (index != 0) {
            out << '\n';
        }
        out << lines[index];
    }
    return out.str();
}

bool is_language_answer(const DefinitionAnswer& answer) {
    if (answer.semantic_family == "language") {
        return true;
    }
    return answer.mapped_cpp_target.find("tze::LanguageEngine::") != std::string::npos;
}

bool is_preprocessor_answer(const DefinitionAnswer& answer) {
    if (answer.semantic_family == "preprocessor") {
        return true;
    }
    return answer.mapped_cpp_target.find("tze::PreprocessorRuntime::") != std::string::npos;
}

bool is_security_answer(const DefinitionAnswer& answer) {
    return answer.semantic_family == "security_safe" ||
        answer.semantic_family == "security_blocked" ||
        answer.mapped_cpp_target.find("tze::SecurityManager::") != std::string::npos;
}

bool is_omni_bridge_answer(const DefinitionAnswer& answer) {
    return answer.semantic_family == "omni_bridge" ||
        answer.query.find("xXOmni") != std::string::npos ||
        answer.mapped_cpp_target.find("tze::OmniBridge::") != std::string::npos;
}

MathAttribution make_definition_attribution(std::string name,
                                            double raw_value,
                                            double weight,
                                            std::string source,
                                            std::string rationale) {
    MathAttribution attribution;
    attribution.name = std::move(name);
    attribution.raw_value = raw_value;
    attribution.weight = weight;
    attribution.contribution = raw_value * weight;
    attribution.source = std::move(source);
    attribution.rationale = std::move(rationale);
    return attribution;
}

double authority_value(const DefinitionAnswer& answer) {
    if (answer.selected_authority_tier == "operator_override") {
        return 1.0;
    }
    if (answer.selected_authority_tier == "memory_artifact") {
        return 0.72;
    }
    if (answer.selected_authority_tier == "reference_cache") {
        return 0.45;
    }
    if (answer.selected_source_type == "system_dictionary") {
        return 0.55;
    }
    if (answer.selected_source_type == "webster") {
        return 0.40;
    }
    return answer.found ? 0.35 : 0.0;
}

std::vector<MathAttribution> definition_math_attributions(const DefinitionAnswer& answer) {
    const bool retrieval_route = answer.comparison_rationale.find("retrieval") != std::string::npos ||
        answer.selected_source_type == "retrieval";
    const double exact_or_retrieval = answer.found ? (retrieval_route ? std::max(0.0, answer.confidence) : 1.0) : 0.0;
    const double domain_match = answer.domain_hint.empty() ? 0.50 : 1.0;
    const double confidence = std::clamp(answer.confidence, 0.0, 1.0);

    std::vector<MathAttribution> attributions;
    attributions.push_back(make_definition_attribution(
        "source_authority",
        authority_value(answer),
        0.30,
        "DefinitionEngine::precedence",
        "Higher-authority sources, especially operator-authored entries, carry more decision weight."));
    attributions.push_back(make_definition_attribution(
        retrieval_route ? "retrieval_score" : "exact_match_score",
        exact_or_retrieval,
        0.25,
        retrieval_route ? "DefinitionEngine::retrieval" : "DefinitionEngine::exact_lookup",
        retrieval_route
            ? "Accepted retrieval candidates must clear the local similarity threshold."
            : "Exact local matches beat fallback dictionary or assistant paths."));
    attributions.push_back(make_definition_attribution(
        "domain_match",
        domain_match,
        0.15,
        "DefinitionEngine::domain_normalizer",
        answer.domain_hint.empty()
            ? "No explicit domain was supplied, so domain confidence is partial."
            : "The answer matched the requested domain hint."));
    attributions.push_back(make_definition_attribution(
        "answer_confidence",
        confidence,
        0.20,
        "DefinitionEngine::confidence",
        "Normalized confidence reflects the selected definition source and matching path."));
    attributions.push_back(make_definition_attribution(
        "operator_teaching_bias",
        answer.selected_authority_tier == "operator_override" ? 1.0 : 0.0,
        0.10,
        "res/local_glossary.tsv",
        "Operator teaching is allowed to outrank reference material on exact matches."));
    return attributions;
}

}  // namespace

void DefinitionFlowInterpreter::run(const RequestProfile& profile,
                                    std::string_view target,
                                    MemorySnapshot& memory,
                                    ProcessingReport& report,
                                    const DefinitionEngine& definitions,
                                    const MemoryStore& memory_store) const {
    const std::string effective_target = target.empty() ? profile.raw_prompt : std::string(target);
    const std::string definition_target =
        !profile.definition_concept.empty() ? profile.definition_concept : effective_target;

    report.references = knowledge_order(profile.source_map_path, memory);
    report.feedback_loop = recent_history_for_term(memory, definition_target);

    DefinitionAnswer answer = definitions.lookup(definition_target,
                                                 profile.source_map_path,
                                                 memory,
                                                 profile.definition_domain_hint,
                                                 profile.definition_comparison_rationale,
                                                 profile.resolved_intent == RequestIntent::GeneralDefinitionQuery);
    answer.math_attributions = definition_math_attributions(answer);
    report.definition_answer = answer;
    if (profile.resolved_intent == RequestIntent::GeneralDefinitionQuery &&
        !answer.found &&
        answer.comparison_rationale.find("needs clarification") != std::string::npos) {
        report.answer_status = "clarify_needed";
    } else {
        report.answer_status = answer.found ? "defined" : "unknown_query";
    }

    std::vector<std::string> explanation_lines = {
        answer.summary,
    };
    if (!answer.mapped_cpp_target.empty()) {
        explanation_lines.push_back("Mapped target: " + answer.mapped_cpp_target);
    }
    if (!answer.semantic_family.empty()) {
        explanation_lines.push_back("Semantic family: " + answer.semantic_family);
    }
    if (!answer.selected_source_type.empty()) {
        explanation_lines.push_back("Definition source: " + answer.selected_source_type);
    }
    if (!answer.selected_source_label.empty()) {
        explanation_lines.push_back("Definition source label: " + answer.selected_source_label);
    }
    if (answer.confidence > 0.0) {
        explanation_lines.push_back("Definition confidence: " + std::to_string(answer.confidence));
    }
    if (!answer.comparison_rationale.empty()) {
        explanation_lines.push_back("Comparison rationale: " + answer.comparison_rationale);
    }
    if (!answer.math_attributions.empty()) {
        const MathAttribution& top = *std::max_element(
            answer.math_attributions.begin(),
            answer.math_attributions.end(),
            [](const MathAttribution& lhs, const MathAttribution& rhs) {
                return lhs.contribution < rhs.contribution;
            });
        explanation_lines.push_back("Top math attribution: " + top.name +
                                    " contribution=" + std::to_string(top.contribution));
    }
    if (!answer.sources.empty()) {
        explanation_lines.push_back("Sources: " + join_lines(answer.sources));
    }
    if (!report.feedback_loop.empty()) {
        explanation_lines.push_back("Recent feedback: " + join_lines(report.feedback_loop));
    }
    if (!answer.suggestions.empty()) {
        explanation_lines.push_back("Suggestions: " + join_lines(answer.suggestions));
    }

    if (answer.found && is_language_answer(answer)) {
        report.language_resolution = LanguageEngine::resolve_context(
            effective_target,
            profile.source_map_path,
            profile.language_confirmation,
            memory,
            report.query_session.has_value() ? &*report.query_session : nullptr);
        memory_store.remember_language_context(memory, *report.language_resolution);
        report.memory_writes.push_back(memory.paths.language_contexts_path.string());
        report.storage_writes.push_back("x.Store(language.context -> " + report.language_resolution->combined_context + ")");
        report.storage_writes.push_back("x.Store(language.confidence -> " + std::to_string(report.language_resolution->confidence) + ")");
        explanation_lines.push_back("Language context: " + report.language_resolution->combined_context);
        explanation_lines.push_back("Language confidence: " + std::to_string(report.language_resolution->confidence));
        if (report.language_resolution->manual_confirmation_required &&
            report.language_resolution->manual_confirmation_response.empty()) {
            explanation_lines.push_back("Manual confirmation prompt: " + report.language_resolution->manual_confirmation_prompt);
        }
    }
    if (answer.found && is_preprocessor_answer(answer)) {
        report.uac_state = PreprocessorRuntime::resolve_uac_state(
            effective_target,
            memory,
            report.query_session.has_value() ? &*report.query_session : nullptr);
        memory_store.remember_uac_state(memory, *report.uac_state);
        report.memory_writes.push_back(memory.paths.uac_states_path.string());
        report.storage_writes.push_back("x.Store(uac.epoch -> " + report.uac_state->epoch_marker + ")");
        report.storage_writes.push_back("x.Store(uac.namespace -> " + report.uac_state->store_namespace + ")");
        explanation_lines.push_back("uAC epoch: " + report.uac_state->epoch_marker);
        explanation_lines.push_back("uAC machine: " + report.uac_state->machine_identifier);
        explanation_lines.push_back("uAC namespace: " + report.uac_state->store_namespace);
        if (!report.uac_state->recovery_hints.empty()) {
            explanation_lines.push_back("uAC recovery hints: " + join_lines(report.uac_state->recovery_hints));
        }
    }
    if (answer.found && is_omni_bridge_answer(answer)) {
        report.legacy_bridge_report = OmniBridge::recover_legacy_bridge(
            effective_target,
            memory,
            report.query_session.has_value() ? &*report.query_session : nullptr);
        explanation_lines.push_back("Legacy bridge: " + report.legacy_bridge_report->summary);
        explanation_lines.push_back("Legacy bridge mode: " + report.legacy_bridge_report->bridge_mode);
        if (!report.legacy_bridge_report->safe_actions.empty()) {
            explanation_lines.push_back("Safe bridge actions: " + join_lines(report.legacy_bridge_report->safe_actions));
        }
        if (!report.legacy_bridge_report->research_actions.empty()) {
            explanation_lines.push_back("Research bridge actions: " + join_lines(report.legacy_bridge_report->research_actions));
        }
        if (!report.legacy_bridge_report->blocked_actions.empty()) {
            explanation_lines.push_back("Blocked bridge actions: " + join_lines(report.legacy_bridge_report->blocked_actions));
        }

        LegacyResearchArtifact artifact;
        artifact.id = "legacy-research-" + std::to_string(std::hash<std::string>{}(effective_target));
        artifact.label = "xXOmni research track";
        artifact.category = "research-only";
        artifact.status = "research-only";
        artifact.summary = "Legacy xXOmni tunnel/orchestration semantics are preserved as research artifacts only.";
        artifact.source_origin = "Tzu.cpp";
        artifact.evidence = report.legacy_bridge_report->correlation_signals;
        artifact.blocked_paths = report.legacy_bridge_report->blocked_actions;
        report.legacy_research_artifacts.push_back(std::move(artifact));
    }
    const bool security_symbol = answer.found && is_security_answer(answer);
    if (security_symbol) {
        SecurityManager security_manager;
        report.security = security_manager.simulate_symbol(
            effective_target,
            answer.semantic_family,
            memory,
            report.query_session.has_value() ? &*report.query_session : nullptr);
        memory_store.remember_security_audit(memory, report.security);
        report.memory_writes.push_back(memory.paths.security_audits_path.string());
        report.storage_writes.push_back("x.Store(security.status -> " + report.security.status + ")");
        report.storage_writes.push_back("x.Store(security.mode -> " + report.security.behavior_mode + ")");
        explanation_lines.push_back("Security status: " + report.security.status);
        explanation_lines.push_back("Security mode: " + report.security.behavior_mode);
        explanation_lines.push_back("Threat bracket: " + report.security.threat_bracket);
        if (!report.security.simulated_actions.empty()) {
            explanation_lines.push_back("Simulated actions: " + join_lines(report.security.simulated_actions));
        }
        if (!report.security.blocked_paths.empty()) {
            explanation_lines.push_back("Blocked paths: " + join_lines(report.security.blocked_paths));
            LegacyResearchArtifact artifact;
            artifact.id = "legacy-security-" +
                std::to_string(std::hash<std::string>{}(effective_target + report.security.status));
            artifact.label = "Legacy security research track";
            artifact.category = answer.semantic_family == "security_blocked" ? "blocked" : "research-only";
            artifact.status = artifact.category;
            artifact.summary = "Legacy risky security semantics remain non-executable and are preserved as research metadata.";
            artifact.source_origin = "Tzu.cpp";
            artifact.evidence = report.security.evidence;
            artifact.blocked_paths = report.security.blocked_paths;
            report.legacy_research_artifacts.push_back(std::move(artifact));
        }
    }

    report.answer_explanation = join_lines(explanation_lines);
    if (report.language_resolution.has_value() &&
        report.language_resolution->manual_confirmation_required &&
        report.language_resolution->manual_confirmation_response.empty()) {
        report.next_action =
            "Re-run with `--lang-confirm yes` or `--lang-confirm no` to confirm the detected language context.";
    } else if (security_symbol && !report.security.blocked_paths.empty()) {
        report.next_action =
            "Inspect the blocked-path audit in `omnix tze replay latest` or run a safe symbol like `omnix define xXOmni::Detection`.";
    } else if (security_symbol && !report.security.simulated_actions.empty()) {
        report.next_action =
            "Use `omnix tze replay latest` or `omnix memory security` to inspect the simulated defensive audit path.";
    } else if (report.legacy_bridge_report.has_value()) {
        report.next_action =
            "Use `omnix legacy coverage` or `omnix explain xXOmni::Premit /Volumes/CoE/Tzu.cpp` to inspect the recovered bridge semantics.";
    } else {
        report.next_action = report.answer_status == "clarify_needed"
            ? "Try `What is " + definition_target + "?` for a concept definition or `What matters here?` for a contextual summary."
            : answer.found
            ? "Use `omnix ask \"Build <project>\"`, `omnix doctor <project>`, or `omnix search <symbol>` for the next step."
            : "Try one of the suggested symbols or ask a build-oriented prompt.";
    }

    if (answer.found) {
        memory_store.remember_definition(memory, answer);
        report.memory_writes.push_back(memory.paths.definitions_path.string());
        report.storage_writes.push_back("x.Store(definition.term -> " + answer.query + ")");
        report.storage_writes.push_back("x.Store(definition.summary -> " + answer.summary + ")");
    }
}

}  // namespace tze
