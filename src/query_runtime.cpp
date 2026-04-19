#include "tze/query_runtime.hpp"

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <set>
#include <sstream>

namespace tze {
namespace {

constexpr std::size_t kMaxIndexedValues = 16;
constexpr std::size_t kMaxCandidatesPerOperation = 6;
constexpr std::size_t kMaxContextTokens = 12;

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
    std::string result;
    result.reserve(value.size());
    for (char c : value) {
        result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return result;
}

std::vector<std::string> tokenize(std::string_view value) {
    std::vector<std::string> tokens;
    std::string current;
    for (char c : value) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            current.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            continue;
        }
        if (!current.empty()) {
            tokens.push_back(current);
            current.clear();
        }
    }
    if (!current.empty()) {
        tokens.push_back(current);
    }
    return tokens;
}

void push_unique(std::vector<std::string>& values, std::string value, std::size_t limit) {
    if (value.empty()) {
        return;
    }
    if (std::find(values.begin(), values.end(), value) != values.end()) {
        return;
    }
    if (values.size() >= limit) {
        return;
    }
    values.push_back(std::move(value));
}

std::string prefix(std::string_view verb, std::string_view value) {
    return std::string(verb) + ":" + std::string(value);
}

std::string make_session_id(std::string_view command_label, std::string_view seed) {
    return "query-" + std::to_string(std::hash<std::string>{}(std::string(command_label) + "|" + std::string(seed)));
}

std::vector<std::string> matched_context_tokens(const QuerySessionRecord& session, std::string_view text) {
    const std::vector<std::string> tokens = tokenize(text);
    std::vector<std::string> matches;
    for (const std::string& token : tokens) {
        if (std::find(session.active_context.begin(), session.active_context.end(), token) != session.active_context.end()) {
            push_unique(matches, token, kMaxContextTokens);
        }
    }
    return matches;
}

int overlap_score(const QuerySessionRecord& session, std::string_view text) {
    const std::vector<std::string> matches = matched_context_tokens(session, text);
    return static_cast<int>(matches.size()) * 12;
}

void append_context(QuerySessionRecord& session, std::string_view text) {
    for (const std::string& token : tokenize(text)) {
        push_unique(session.active_context, token, kMaxContextTokens);
    }
}

void append_operation(QuerySessionRecord& session, QueryOperation operation) {
    if (operation.candidates.size() > kMaxCandidatesPerOperation) {
        operation.candidates.resize(kMaxCandidatesPerOperation);
    }
    session.operations.push_back(std::move(operation));
}

std::string join_strings(const std::vector<std::string>& values) {
    std::ostringstream out;
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) {
            out << ' ';
        }
        out << values[index];
    }
    return out.str();
}

}  // namespace

QuerySessionRecord QueryRuntime::open_session(std::string_view command_label, std::string_view seed) const {
    QuerySessionRecord session;
    session.id = make_session_id(command_label, seed);
    session.command_label = std::string(command_label);
    session.query_seed = trim(seed);
    append_context(session, command_label);
    append_context(session, seed);
    return session;
}

void QueryRuntime::index_values(QuerySessionRecord& session,
                                std::string_view label,
                                const std::vector<std::string>& values) const {
    QueryOperation operation;
    operation.operator_name = "index";
    operation.label = std::string(label);
    operation.inputs = values;

    for (const std::string& value : values) {
        const std::string trimmed = trim(value);
        if (trimmed.empty()) {
            continue;
        }

        push_unique(session.indexed_values, trimmed, kMaxIndexedValues);
        append_context(session, trimmed);

        QueryCandidate candidate;
        candidate.label = trimmed;
        candidate.detail = prefix("index", trimmed);
        candidate.matched_context = matched_context_tokens(session, trimmed);
        candidate.score = 50 + static_cast<int>(candidate.matched_context.size()) * 10;
        candidate.reasons.push_back("indexed");
        if (!candidate.matched_context.empty()) {
            candidate.reasons.push_back("context-match");
        }
        operation.candidates.push_back(std::move(candidate));
    }

    operation.outputs = session.indexed_values;
    operation.trace.push_back("indexed=" + std::to_string(operation.candidates.size()));
    append_operation(session, std::move(operation));
}

std::vector<KnowledgeReference> QueryRuntime::rank_references(QuerySessionRecord& session,
                                                              std::string_view label,
                                                              const std::vector<KnowledgeReference>& references) const {
    struct RankedReference {
        KnowledgeReference reference;
        QueryCandidate candidate;
    };

    std::vector<RankedReference> ranked;
    ranked.reserve(references.size());
    for (const KnowledgeReference& reference : references) {
        QueryCandidate candidate;
        candidate.label = reference.source;
        candidate.detail = reference.excerpt;
        candidate.matched_context = matched_context_tokens(session, reference.source + " " + reference.excerpt);
        candidate.score = std::max(0, 100 - (reference.priority * 15)) + overlap_score(session, reference.source + " " + reference.excerpt);
        candidate.reasons.push_back("priority=" + std::to_string(reference.priority));
        if (!candidate.matched_context.empty()) {
            candidate.reasons.push_back("context=" + std::to_string(candidate.matched_context.size()));
        }
        ranked.push_back({reference, candidate});
    }

    std::stable_sort(ranked.begin(), ranked.end(), [](const RankedReference& lhs, const RankedReference& rhs) {
        if (lhs.candidate.score != rhs.candidate.score) {
            return lhs.candidate.score > rhs.candidate.score;
        }
        if (lhs.reference.priority != rhs.reference.priority) {
            return lhs.reference.priority < rhs.reference.priority;
        }
        return lhs.reference.source < rhs.reference.source;
    });

    QueryOperation operation;
    operation.operator_name = "rank";
    operation.label = std::string(label);
    for (const RankedReference& entry : ranked) {
        operation.inputs.push_back(entry.reference.source);
        operation.candidates.push_back(entry.candidate);
        push_unique(operation.outputs, entry.reference.source, kMaxCandidatesPerOperation);
    }
    if (!ranked.empty()) {
        session.final_results = operation.outputs;
        operation.trace.push_back("selected=" + ranked.front().reference.source);
    }
    append_operation(session, std::move(operation));

    std::vector<KnowledgeReference> ordered;
    ordered.reserve(ranked.size());
    for (const RankedReference& entry : ranked) {
        ordered.push_back(entry.reference);
    }
    return ordered;
}

std::vector<std::string> QueryRuntime::rank_feedback(QuerySessionRecord& session,
                                                     std::string_view label,
                                                     const std::vector<std::string>& feedback) const {
    struct RankedFeedback {
        std::string line;
        QueryCandidate candidate;
    };

    std::vector<RankedFeedback> found;
    found.reserve(feedback.size());
    for (std::size_t index = 0; index < feedback.size(); ++index) {
        const std::string& line = feedback[index];
        QueryCandidate candidate;
        candidate.label = line;
        candidate.detail = prefix("find", line);
        candidate.matched_context = matched_context_tokens(session, line);
        candidate.score = overlap_score(session, line) + static_cast<int>(std::max<std::size_t>(0, feedback.size() - index));
        candidate.reasons.push_back("feedback-order=" + std::to_string(index));
        if (!candidate.matched_context.empty()) {
            candidate.reasons.push_back("context=" + std::to_string(candidate.matched_context.size()));
        }
        found.push_back({line, candidate});
    }

    std::stable_sort(found.begin(), found.end(), [](const RankedFeedback& lhs, const RankedFeedback& rhs) {
        if (lhs.candidate.score != rhs.candidate.score) {
            return lhs.candidate.score > rhs.candidate.score;
        }
        return lhs.line < rhs.line;
    });

    QueryOperation operation;
    operation.operator_name = "find";
    operation.label = std::string(label);
    for (const RankedFeedback& entry : found) {
        operation.inputs.push_back(entry.line);
        operation.candidates.push_back(entry.candidate);
        push_unique(operation.outputs, entry.line, kMaxCandidatesPerOperation);
    }
    if (!found.empty()) {
        session.final_results = operation.outputs;
        operation.trace.push_back("matched=" + std::to_string(found.size()));
    }
    append_operation(session, std::move(operation));

    std::vector<std::string> ordered;
    ordered.reserve(found.size());
    for (const RankedFeedback& entry : found) {
        ordered.push_back(entry.line);
    }
    return ordered;
}

void QueryRuntime::rank_decisions(QuerySessionRecord& session,
                                  std::string_view label,
                                  std::vector<DecisionCandidate>& decisions,
                                  const std::vector<std::string>& evidence_signals) const {
    for (const std::string& signal : evidence_signals) {
        push_unique(session.indexed_values, signal, kMaxIndexedValues);
        append_context(session, signal);
    }

    QueryOperation operation;
    operation.operator_name = "determine";
    operation.label = std::string(label);
    for (DecisionCandidate& decision : decisions) {
        QueryCandidate candidate;
        candidate.label = decision.id;
        candidate.detail = decision.title;
        candidate.matched_context = matched_context_tokens(
            session,
            decision.title + " " + decision.recommended_command + " " + decision.rationale + " " +
                prefix("rank", join_strings(decision.supporting_signals)));

        const int context_bonus = static_cast<int>(candidate.matched_context.size()) * 4;
        const int query_rank = decision.score + context_bonus;
        candidate.score = query_rank;
        candidate.reasons.push_back("base-score=" + std::to_string(decision.score));
        if (!candidate.matched_context.empty()) {
            candidate.reasons.push_back("context=" + std::to_string(candidate.matched_context.size()));
        }

        decision.score_trace.push_back("query_context=" + std::to_string(candidate.matched_context.size()));
        decision.score_trace.push_back("query_rank=" + std::to_string(query_rank));
        operation.inputs.push_back(decision.title);
        operation.candidates.push_back(candidate);
    }

    std::stable_sort(decisions.begin(), decisions.end(), [&](const DecisionCandidate& lhs, const DecisionCandidate& rhs) {
        const auto lhs_it = std::find_if(operation.candidates.begin(), operation.candidates.end(), [&](const QueryCandidate& candidate) {
            return candidate.label == lhs.id;
        });
        const auto rhs_it = std::find_if(operation.candidates.begin(), operation.candidates.end(), [&](const QueryCandidate& candidate) {
            return candidate.label == rhs.id;
        });
        const int lhs_rank = lhs_it == operation.candidates.end() ? lhs.score : lhs_it->score;
        const int rhs_rank = rhs_it == operation.candidates.end() ? rhs.score : rhs_it->score;
        if (lhs_rank != rhs_rank) {
            return lhs_rank > rhs_rank;
        }
        if (lhs.score != rhs.score) {
            return lhs.score > rhs.score;
        }
        if (lhs.probability_likelihood != rhs.probability_likelihood) {
            return lhs.probability_likelihood > rhs.probability_likelihood;
        }
        return lhs.confidence > rhs.confidence;
    });

    for (const DecisionCandidate& decision : decisions) {
        push_unique(operation.outputs, decision.id, kMaxCandidatesPerOperation);
    }
    if (!decisions.empty()) {
        session.final_results = operation.outputs;
        operation.trace.push_back("selected=" + decisions.front().id);
    }
    append_operation(session, std::move(operation));
}

std::string QueryRuntime::instruction_slot(std::string_view slot) {
    return prefix("instruction-slot", slot);
}

std::string QueryRuntime::index_value(std::string_view value) {
    return prefix("index", value);
}

std::string QueryRuntime::seek_value(std::string_view value) {
    return prefix("seek", value);
}

std::vector<std::string> QueryRuntime::seek_parameters(std::initializer_list<std::string_view> parameters) {
    std::vector<std::string> values;
    values.reserve(parameters.size());
    for (std::string_view parameter : parameters) {
        values.emplace_back(parameter);
    }
    return values;
}

std::string QueryRuntime::find_matches(std::string_view needle) {
    return prefix("find", needle);
}

std::string QueryRuntime::determine_value(std::string_view label) {
    return prefix("determine", label);
}

std::string QueryRuntime::read_value(std::string_view label) {
    return prefix("read", label);
}

std::string QueryRuntime::read_last(std::size_t count) {
    return "read-last:" + std::to_string(count);
}

bool QueryRuntime::check_value(std::string_view label) {
    return !label.empty();
}

bool QueryRuntime::match_values(std::string_view lhs, std::string_view rhs) {
    return lhs == rhs;
}

std::string QueryRuntime::results_view(std::string_view label) {
    return prefix("results", label);
}

std::string QueryRuntime::rank_value(std::string_view label) {
    return prefix("rank", label);
}

std::string QueryRuntime::index_marker(std::string_view label) {
    return prefix("index-mark", label);
}

}  // namespace tze
