#include "tze/definition_flow_interpreter.hpp"

#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

#include "tze/language_engine.hpp"
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

}  // namespace

void DefinitionFlowInterpreter::run(const RequestProfile& profile,
                                    std::string_view target,
                                    MemorySnapshot& memory,
                                    ProcessingReport& report,
                                    const DefinitionEngine& definitions,
                                    const MemoryStore& memory_store) const {
    const std::string effective_target = target.empty() ? profile.raw_prompt : std::string(target);

    report.references = knowledge_order(profile.source_map_path, memory);
    report.feedback_loop = recent_history_for_term(memory, effective_target);

    const DefinitionAnswer answer = definitions.lookup(effective_target, profile.source_map_path, memory);
    report.definition_answer = answer;
    report.answer_status = answer.found ? "defined" : "unknown_query";

    std::vector<std::string> explanation_lines = {
        answer.summary,
    };
    if (!answer.mapped_cpp_target.empty()) {
        explanation_lines.push_back("Mapped target: " + answer.mapped_cpp_target);
    }
    if (!answer.semantic_family.empty()) {
        explanation_lines.push_back("Semantic family: " + answer.semantic_family);
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
    } else {
        report.next_action = answer.found
            ? "Use `omnix ask \"Build <project>\"`, `omnix doctor <project>`, or `omnix search <symbol>` for the next step."
            : "Try one of the suggested symbols or ask a build-oriented prompt.";
    }

    memory_store.remember_definition(memory, answer);
    report.memory_writes.push_back(memory.paths.definitions_path.string());
    report.storage_writes.push_back("x.Store(definition.term -> " + answer.query + ")");
    report.storage_writes.push_back("x.Store(definition.summary -> " + answer.summary + ")");
}

}  // namespace tze
