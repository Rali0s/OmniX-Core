#pragma once

#include <cstddef>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include "tze/types.hpp"

namespace tze {

class QueryRuntime {
public:
    QuerySessionRecord open_session(std::string_view command_label, std::string_view seed) const;
    void index_values(QuerySessionRecord& session,
                      std::string_view label,
                      const std::vector<std::string>& values) const;
    std::vector<KnowledgeReference> rank_references(QuerySessionRecord& session,
                                                    std::string_view label,
                                                    const std::vector<KnowledgeReference>& references) const;
    std::vector<std::string> rank_feedback(QuerySessionRecord& session,
                                           std::string_view label,
                                           const std::vector<std::string>& feedback) const;
    void rank_decisions(QuerySessionRecord& session,
                        std::string_view label,
                        std::vector<DecisionCandidate>& decisions,
                        const std::vector<std::string>& evidence_signals) const;

    static std::string instruction_slot(std::string_view slot);
    static std::string index_value(std::string_view value);
    static std::string seek_value(std::string_view value);
    static std::vector<std::string> seek_parameters(std::initializer_list<std::string_view> parameters);
    static std::string find_matches(std::string_view needle);
    static std::string determine_value(std::string_view label);
    static std::string read_value(std::string_view label);
    static std::string read_last(std::size_t count = 1);
    static bool check_value(std::string_view label);
    static bool match_values(std::string_view lhs, std::string_view rhs);
    static std::string results_view(std::string_view label);
    static std::string rank_value(std::string_view label);
    static std::string index_marker(std::string_view label);
};

}  // namespace tze
