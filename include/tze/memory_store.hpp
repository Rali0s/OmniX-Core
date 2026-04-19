#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "tze/types.hpp"

namespace tze {

class MemoryStore {
public:
    MemoryPaths resolve_paths(const std::filesystem::path& requested_root = {}) const;
    MemorySnapshot load(const std::filesystem::path& requested_root = {}) const;
    void persist_snapshot(const MemorySnapshot& snapshot) const;
    std::string resolve_tze_run_id(const MemorySnapshot& snapshot,
                                   std::string_view reference,
                                   bool important_only = false) const;
    const TzeRunRecord* find_tze_run(const MemorySnapshot& snapshot, std::string_view id) const;
    std::string render_tze_run(const MemorySnapshot& snapshot, std::string_view id) const;
    std::string render_tze_chain(const MemorySnapshot& snapshot, std::string_view id) const;
    std::string render_tze_diff(const MemorySnapshot& snapshot,
                                std::string_view left_id,
                                std::string_view right_id) const;
    std::string render_tze_change_explanation(const MemorySnapshot& snapshot,
                                              std::string_view left_id,
                                              std::string_view right_id) const;
    std::string write_tze_run_report(const MemorySnapshot& snapshot,
                                     std::string_view id,
                                     const std::filesystem::path& explicit_output = {}) const;
    std::string write_tze_bundle(const MemorySnapshot& snapshot,
                                 std::string_view id,
                                 const std::filesystem::path& explicit_output = {}) const;
    std::string import_tze_bundle(MemorySnapshot& snapshot,
                                  const std::filesystem::path& bundle_path) const;
    std::string write_tze_diff_report(const MemorySnapshot& snapshot,
                                      std::string_view left_id,
                                      std::string_view right_id,
                                      const std::filesystem::path& explicit_output = {}) const;
    std::string prune_tze_runs(MemorySnapshot& snapshot,
                               std::size_t keep_count,
                               bool important_only) const;
    std::string prune_memory(MemorySnapshot& snapshot,
                             std::size_t keep_count,
                             bool important_only) const;
    bool mark_tze_run_feedback(MemorySnapshot& snapshot,
                               std::string_view reference,
                               std::string_view feedback_value,
                               std::string_view feedback_note,
                               std::string* resolved_id = nullptr) const;
    std::string render_case_timeline(const MemorySnapshot& snapshot, std::string_view case_reference) const;
    bool mark_decision_feedback(MemorySnapshot& snapshot,
                                std::string_view case_reference,
                                std::string_view decision_reference,
                                std::string_view feedback_value,
                                std::string_view feedback_note,
                                std::string* resolved_case_id = nullptr,
                                std::string* resolved_decision_id = nullptr) const;
    bool mark_decision_outcome(MemorySnapshot& snapshot,
                               std::string_view case_reference,
                               std::string_view decision_reference,
                               std::string_view outcome_status,
                               std::string_view outcome_note,
                               std::string* resolved_case_id = nullptr,
                               std::string* resolved_decision_id = nullptr) const;
    std::string write_case_bundle(const MemorySnapshot& snapshot,
                                  std::string_view case_reference,
                                  const std::filesystem::path& explicit_output = {}) const;
    std::string import_case_bundle(MemorySnapshot& snapshot,
                                   const std::filesystem::path& bundle_path) const;
    std::string render_incident_list(const MemorySnapshot& snapshot) const;
    std::string render_incident(const MemorySnapshot& snapshot, std::string_view incident_reference) const;
    std::string write_incident_report(const MemorySnapshot& snapshot,
                                      std::string_view incident_reference,
                                      const std::filesystem::path& explicit_output = {}) const;
    void record_interaction(MemorySnapshot& snapshot, const ProcessingReport& report) const;
    void remember_tze_run(MemorySnapshot& snapshot, const TzeRunRecord& record) const;
    void remember_definition(MemorySnapshot& snapshot, const DefinitionAnswer& answer) const;
    void remember_project(MemorySnapshot& snapshot,
                          std::string_view canonical_name,
                          std::string_view resolved_source_path,
                          std::string_view build_system,
                          std::string_view status,
                          std::string_view upstream_url) const;
    void remember_recipe_result(MemorySnapshot& snapshot, const LearnedRecipeRecord& record) const;
    void remember_native_tool(MemorySnapshot& snapshot, const NativeToolRecord& record) const;
    void remember_security_audit(MemorySnapshot& snapshot, const SecurityAudit& record) const;
    void remember_language_context(MemorySnapshot& snapshot, const LanguageResolutionRecord& record) const;
    void remember_uac_state(MemorySnapshot& snapshot, const UacStateRecord& record) const;
    void remember_observation(MemorySnapshot& snapshot, const ObservationRecord& record) const;
    void remember_normalized_object(MemorySnapshot& snapshot, const NormalizedObject& record) const;
    void remember_evidence_link(MemorySnapshot& snapshot, const EvidenceLink& record) const;
    void remember_analyst_comment(MemorySnapshot& snapshot, const AnalystComment& record) const;
    void remember_decision_candidate(MemorySnapshot& snapshot, const DecisionCandidate& record) const;
    void remember_case_record(MemorySnapshot& snapshot, const CaseRecord& record) const;
    void remember_case_link(MemorySnapshot& snapshot, const CaseLink& record) const;
    std::string render_view(const MemorySnapshot& snapshot, std::string_view view) const;
};

}  // namespace tze
