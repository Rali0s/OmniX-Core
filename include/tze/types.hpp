#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tze {

enum class RequestIntent {
    Unknown,
    Conversation,
    GeneralDefinitionQuery,
    PacketCapture,
    DefenseDiagnostic,
    NeuralMath,
    NeuralRoute,
    SetPersonaMode,
    AuthorBuildRecipe,
    ReviewModule,
    PatchProposal,
    IngestData,
    AnalyzeCase,
    DecideAction,
    InspectCase,
    CaseTimeline,
    ReplayTzeRun,
    ChainTzeRun,
    RecursiveWhyDiff,
    DiffTzeRuns,
    ExplainTzeChange,
    ReportTzeRun,
    DiffReportTzeRuns,
    ExportTzeBundle,
    ImportTzeBundle,
    PruneTzeRuns,
    PruneMemory,
    MarkTzeRunOutcome,
    MarkDecisionFeedback,
    MarkDecisionOutcome,
    ExportCaseBundle,
    ImportCaseBundle,
    ListIncidents,
    InspectIncident,
    ReportIncident,
    BuildProject,
    DoctorProject,
    ToolAction,
    ProbeProvider,
    DefineSymbol,
    ExplainCommand,
    InspectToolchain,
    ShowMemory,
};

enum class AcquisitionPolicy {
    PreferLocal,
    FetchIfMissing,
    LocalOnly,
};

enum class ToolCommandMode {
    None,
    List,
    Locate,
    Doctor,
    Run,
};

struct CacheCell {
    std::string name;
    std::size_t size_bytes = 0;
    bool persistent = false;
    std::vector<std::string> operations;
};

struct QueryCandidate {
    std::string label;
    std::string detail;
    int score = 0;
    std::vector<std::string> matched_context;
    std::vector<std::string> reasons;
};

struct ShellLexiconEntry {
    std::string phrase;
    std::string canonical;
    std::string category;
    double confidence = 0.0;
    std::vector<std::string> correction_notes;
    bool clarification_required = false;
};

struct OperatorPersonaRecord {
    std::string preferred_label;
    std::string role_label;
    std::string local_username;
    std::string host_identifier;
    std::string last_source_map;
    std::string last_memory_root;
    std::string self_description;
    std::string persona_mode;
    std::string tone_profile;
    std::string interaction_style;
    std::string safety_posture;
    std::string preferred_next_action_style;
    std::vector<std::string> custom_phrases;
};

struct QueryOperation {
    std::string operator_name;
    std::string label;
    std::vector<std::string> inputs;
    std::vector<QueryCandidate> candidates;
    std::vector<std::string> outputs;
    std::vector<std::string> trace;
};

struct QuerySessionRecord {
    std::string id;
    std::string command_label;
    std::string query_seed;
    std::vector<std::string> active_context;
    std::vector<std::string> indexed_values;
    std::vector<QueryOperation> operations;
    std::vector<std::string> final_results;
};

struct LanguageCandidate {
    std::string candidate_type;
    std::string label;
    double probability = 0.0;
    std::string status;
    std::vector<std::string> evidence;
};

struct DecompressionCandidate {
    std::string label;
    std::string status;
    double confidence = 0.0;
    std::vector<std::string> notes;
};

struct LanguageResolutionRecord {
    std::string id;
    std::string query;
    std::string native_os;
    std::string observed_locale;
    std::string selected_os;
    std::string selected_language;
    std::string combined_context;
    int passes = 0;
    double confidence = 0.0;
    bool manual_confirmation_required = false;
    bool manual_confirmation_used = false;
    std::string manual_confirmation_prompt;
    std::string manual_confirmation_response;
    std::vector<LanguageCandidate> os_candidates;
    std::vector<LanguageCandidate> language_candidates;
    std::vector<DecompressionCandidate> decompression_candidates;
    std::vector<std::string> research_notes;
    std::vector<std::string> reasoning_trace;
    std::string persisted_at;
};

struct UacTraitRecord {
    std::string trait_name;
    std::string trait_value;
    std::string source;
    int weight = 0;
    bool recovery_relevant = false;
};

struct UacStateRecord {
    std::string id;
    std::string query;
    std::string normalized_prompt;
    std::vector<std::string> query_tokens;
    std::string instruction_family_hint;
    std::string epoch_marker;
    std::string machine_identifier;
    std::string chapter_reference;
    std::string store_namespace;
    std::string search_namespace;
    std::string genx_token_value;
    std::string compression_label;
    std::string encoded_value;
    std::string encrypted_value;
    std::string key_store_address_value;
    std::string key_budget_value;
    std::string operational_usage_habit;
    std::string chapter_series_label;
    std::string epoch_tier_label;
    std::vector<UacTraitRecord> indexed_traits;
    std::vector<std::string> recovery_hints;
    std::vector<std::string> deletion_discrepancies;
    std::vector<std::string> search_context_habits;
    std::vector<std::string> time_on_site_traits;
    std::vector<std::string> reasoning_trace;
    std::string persisted_at;
};

struct PostProcessRecord {
    std::string status;
    std::string final_artifact_summary;
    std::string provenance;
    std::string retention_decision;
    bool dropped_transient_chain = false;
    std::vector<std::string> retained_fields;
    std::vector<std::string> discarded_fields;
    std::string persisted_at;
};

struct LegacySourceRecord {
    std::string id;
    std::string source_path;
    std::string source_label;
    std::string source_kind;
    std::string source_hash;
    std::size_t line_count = 0;
    std::size_t section_count = 0;
    std::size_t symbol_count = 0;
    std::string tracked_at;
};

struct LegacySymbolCoverage {
    std::string symbol;
    std::string section_title;
    std::string recovery_status;
    std::string semantic_family;
    std::string mapped_cpp_target;
    std::string source_origin;
    std::size_t occurrence_count = 0;
    std::vector<std::string> notes;
};

struct LegacyRecoveryStatus {
    std::string source_label;
    std::size_t implemented_count = 0;
    std::size_t partial_count = 0;
    std::size_t missing_count = 0;
    std::size_t research_only_count = 0;
    std::size_t blocked_count = 0;
    std::vector<std::string> summary_lines;
};

struct LegacyResearchArtifact {
    std::string id;
    std::string label;
    std::string category;
    std::string status;
    std::string summary;
    std::string source_origin;
    std::vector<std::string> evidence;
    std::vector<std::string> blocked_paths;
};

struct LegacyCorrelationRecord {
    std::string id;
    std::string left_label;
    std::string right_label;
    std::string correlation_type;
    std::string summary;
    int strength = 0;
    std::vector<std::string> evidence;
};

struct LegacyBridgeReport {
    std::string id;
    std::string query;
    std::string status;
    std::string bridge_mode;
    std::string summary;
    std::vector<std::string> bridge_steps;
    std::vector<std::string> safe_actions;
    std::vector<std::string> research_actions;
    std::vector<std::string> blocked_actions;
    std::vector<std::string> correlation_signals;
    std::vector<std::string> reasoning_trace;
};

struct ProviderProbeReport {
    std::string provider_id;
    std::string status;
    std::string summary;
    bool configured = false;
    bool available = false;
    std::string base_url;
    std::string model;
    std::vector<std::string> checks;
    std::vector<std::string> warnings;
};

struct AssistRequest {
    std::string task_id;
    std::string target_label;
    std::string deterministic_text;
};

struct AssistAnnotation {
    std::string task_id;
    std::string provider_id;
    std::string model;
    std::string status;
    std::string summary;
    std::vector<std::string> highlights;
    std::string operator_takeaway;
    std::vector<std::string> warnings;
};

struct AssistPlanMetadata {
    std::string provider;
    std::string model;
    std::string status;
    double confidence = 0.0;
    std::string rationale;
    std::vector<std::string> warnings;
    std::vector<std::string> validation_notes;
};

struct ToolAssistPlan {
    std::string task_id;
    std::string provider_id;
    std::string model;
    std::string status;
    std::string tool_name;
    std::vector<std::string> arguments;
    std::string rationale;
    std::vector<std::string> safety_notes;
    bool validated = false;
    AssistPlanMetadata metadata;
};

struct BuildAssistRequest {
    std::string task_id;
    std::string canonical_project_name;
    std::string prompt;
    std::string build_system;
    std::string environment_signature;
    std::string native_status;
    bool will_acquire = false;
    bool will_install = false;
    std::vector<std::string> available_recipe_ids;
    std::vector<std::string> learned_recipe_summaries;
};

struct BuildAssistPlan {
    std::string task_id;
    std::string provider_id;
    std::string model;
    std::string status;
    std::string selected_recipe_id;
    std::string fallback_recipe_id;
    std::string rationale;
    double confidence = 0.0;
    std::vector<std::string> safety_notes;
    bool validated = false;
    AssistPlanMetadata metadata;
};

struct RecipeAuthoringRequest {
    std::string task_id;
    std::string source_path;
    std::string resolved_source_path;
    std::string canonical_project_name;
    std::string detected_build_system;
    std::string requested_target;
    std::string build_type;
    std::string install_prefix;
    std::vector<std::string> detected_files;
    std::vector<std::string> recommended_modules;
    std::vector<std::string> missing_modules;
    std::vector<std::string> ranked_evidence;
    std::vector<std::string> validation_feedback;
};

struct CommandAssistPlan {
    std::string task_id;
    std::string provider_id;
    std::string model;
    std::string status;
    std::string canonical_command;
    std::string command_family;
    std::string rationale;
    double confidence = 0.0;
    bool requires_confirmation = false;
    std::vector<std::string> safety_notes;
    bool validated = false;
    AssistPlanMetadata metadata;
};

struct NextStepAssistPlan {
    std::string task_id;
    std::string provider_id;
    std::string model;
    std::string status;
    std::string suggested_next_step;
    std::string safer_alternative;
    std::string rationale;
    double confidence = 0.0;
    std::vector<std::string> warnings;
    std::vector<std::string> validation_notes;
    bool validated = false;
    AssistPlanMetadata metadata;
};

struct CaseSummaryAssistPlan {
    std::string task_id;
    std::string provider_id;
    std::string model;
    std::string status;
    std::string summary_title;
    std::string executive_summary;
    std::vector<std::string> highlights;
    std::string rationale;
    double confidence = 0.0;
    std::vector<std::string> warnings;
    std::vector<std::string> validation_notes;
    bool validated = false;
    AssistPlanMetadata metadata;
};

struct FreeformAssistAnswer {
    std::string task_id;
    std::string provider_id;
    std::string model;
    std::string status;
    std::string answer;
    std::string rationale;
    double confidence = 0.0;
    std::vector<std::string> suggested_commands;
    std::vector<std::string> safety_warnings;
    std::vector<std::string> used_context;
    bool validated = false;
    AssistPlanMetadata metadata;
};

struct ReviewFinding {
    std::string severity;
    std::string category;
    std::string file_path;
    std::size_t line = 0;
    std::string summary;
    std::string rationale;
};

struct ReviewArtifact {
    std::string id;
    std::string target;
    std::string status;
    std::string summary;
    std::vector<ReviewFinding> findings;
    std::vector<std::string> nearby_symbols;
    std::vector<std::string> suggested_tests;
    std::string persisted_at;
    std::string artifact_path;
    std::optional<AssistAnnotation> assist_annotation;
};

struct PatchProposalArtifact {
    std::string id;
    std::string target;
    std::string status;
    std::string summary;
    std::vector<std::string> target_files;
    std::vector<std::string> intended_behavior_changes;
    std::vector<std::string> acceptance_tests;
    std::string unified_diff_text;
    std::string persisted_at;
    std::string artifact_path;
    std::optional<AssistAnnotation> assist_annotation;
};

struct RecipeAuthoringArtifact {
    std::string id;
    std::string source_path;
    std::string resolved_source_path;
    std::string canonical_project_name;
    std::string provider_id;
    std::string model;
    std::string status;
    std::string generated_recipe_id;
    std::string generated_build_system;
    std::string rationale;
    std::vector<std::string> ranked_evidence;
    std::vector<std::string> validation_feedback;
    bool repair_attempted = false;
    bool activated = false;
    std::string persisted_at;
};

struct AssistOutcomeRecord {
    std::string id;
    std::string task_type;
    std::string plan_type;
    std::string provider_id;
    std::string model;
    std::string status;
    std::string target_label;
    std::string canonical_value;
    std::string rejection_reason;
    std::string host_platform;
    std::string environment_signature;
    std::string persisted_at;
};

struct AssistCorrectionRecord {
    std::string id;
    std::string original_prompt;
    std::string corrected_value;
    std::string category;
    std::string host_platform;
    std::string persisted_at;
};

struct AssistLearningRecord {
    std::string id;
    std::string category;
    std::string prompt_fragment;
    std::string learned_value;
    std::string host_platform;
    int success_count = 0;
    int rejection_count = 0;
    std::string last_status;
    std::string persisted_at;
};

struct HostAssistPreferenceRecord {
    std::string id;
    std::string host_platform;
    std::string provider_id;
    std::string model;
    std::vector<std::string> preferred_shell_phrases;
    std::vector<std::string> preferred_tool_routes;
    std::vector<std::string> preferred_recipe_routes;
    std::string persisted_at;
};

struct PluginActionDescriptor {
    std::string id;
    std::string label;
    std::string action_type;
    std::vector<std::string> allowed_intents;
    std::vector<std::string> validation_rules;
};

struct TzeStageRecord {
    std::string stage_id;
    std::string stage_name;
    std::string module;
    std::string status;
    std::string detail;
    std::vector<std::string> inputs;
    std::vector<std::string> outputs;
    std::string graph_origin;
    std::string source_section;
    std::size_t source_line = 0;
    std::string source_excerpt;
};

struct KnowledgeReference {
    std::string source;
    std::string excerpt;
    int priority = 0;
};

struct SecurityAudit {
    std::string id;
    std::string query;
    std::string status;
    std::string behavior_mode;
    std::string threat_label;
    std::string threat_bracket;
    bool admin_verified = false;
    std::vector<std::string> phases;
    std::vector<std::string> communications;
    std::vector<std::string> mitigations;
    std::vector<std::string> trace_paths;
    std::vector<std::string> simulated_actions;
    std::vector<std::string> blocked_paths;
    std::vector<std::string> evidence;
    std::vector<std::string> reasoning_trace;
    std::string persisted_at;
};

struct SourceBackedMapping {
    std::string symbol;
    std::string inferred_meaning;
    std::string mapped_cpp_target;
    std::string status;
    std::string semantic_family;
    std::size_t occurrence_count = 0;
};

struct MathAttribution {
    std::string name;
    double raw_value = 0.0;
    double weight = 0.0;
    double contribution = 0.0;
    std::string source;
    std::string rationale;
};

struct DefinitionAnswer {
    std::string query;
    bool found = false;
    std::string summary;
    std::string mapped_cpp_target;
    std::string semantic_family;
    std::string normalized_concept;
    std::string domain_hint;
    std::string route_class;
    std::string selected_source_type;
    std::string selected_source_label;
    std::string selected_authority_tier;
    double confidence = 0.0;
    std::string comparison_rationale;
    std::vector<std::string> sources;
    std::vector<std::string> suggestions;
    std::vector<MathAttribution> math_attributions;
};

struct NeuralMathSample {
    std::vector<double> inputs;
    int expected = 0;
    int predicted = 0;
};

struct NeuralMathReport {
    std::string status;
    std::string summary;
    std::string model_type;
    std::string dataset;
    std::size_t epochs_requested = 0;
    std::size_t epochs_ran = 0;
    double learning_rate = 0.0;
    std::vector<double> weights;
    double bias = 0.0;
    double accuracy = 0.0;
    std::vector<NeuralMathSample> predictions;
    std::vector<std::string> math_trace;
    std::vector<std::string> warnings;
};

struct NeuralFeatureVector {
    std::string input_path;
    std::size_t packet_count = 0;
    std::size_t flow_count = 0;
    std::size_t control_count = 0;
    std::size_t http_plaintext_count = 0;
    std::size_t text_utf8_count = 0;
    std::size_t tls_opaque_count = 0;
    std::size_t opaque_payload_count = 0;
    std::size_t parse_error_count = 0;
    std::size_t payload_packet_count = 0;
    std::size_t unknown_port_count = 0;
    std::size_t total_payload_bytes = 0;
    std::vector<std::string> feature_summary;
};

struct NeuralRoutePrediction {
    std::string label;
    double confidence = 0.0;
    std::string rationale;
    std::vector<MathAttribution> attributions;
};

struct NeuralRouteReport {
    std::string status;
    std::string summary;
    std::string input_path;
    std::size_t packet_count = 0;
    std::size_t flow_count = 0;
    NeuralFeatureVector features;
    std::vector<NeuralRoutePrediction> predictions;
    std::vector<std::string> warnings;
    std::string artifact_path;
};

struct MemoryHistoryEntry {
    std::string timestamp;
    std::string prompt;
    std::string intent;
    std::string project;
    std::string status;
    std::string summary;
    std::string scope;
    std::string created_at;
    std::string expires_at;
};

struct ReasoningSlotRecord {
    std::string slot_id;
    std::string label;
    std::vector<std::string> facts;
    std::vector<std::string> evidence_refs;
    double confidence = 0.0;
};

struct RecursivePathRecord {
    std::string path_type;
    std::vector<std::string> ordered_steps;
    std::vector<std::string> assumptions;
    std::vector<std::string> evidence_refs;
    double score = 0.0;
};

struct RecursiveDiffReport {
    std::string status;
    std::string route_learning_status;
    std::string source_run_id;
    std::string current_state;
    std::string likely_goal;
    std::string logs_and_reasoning;
    std::string successful_path_pattern;
    std::string difference_found;
    std::string best_estimate_answer;
    std::string why_this_matters;
    std::string next_action;
    std::string diff_category;
    double confidence = 0.0;
    std::vector<ReasoningSlotRecord> slots;
    RecursivePathRecord success_path;
    RecursivePathRecord failure_path;
};

struct TzeRunRecord {
    std::string id;
    std::string timestamp;
    std::string intent;
    std::string prompt;
    std::string target;
    std::string linked_case_id;
    std::string status;
    std::string source_map_path;
    std::string reasoning_provider;
    std::string provider_probe_status;
    std::string assist_status;
    std::string next_action;
    std::string produced_artifact;
    std::string feedback_status;
    std::string feedback_note;
    std::string feedback_timestamp;
    std::optional<ProviderProbeReport> provider_probe_report;
    std::optional<AssistAnnotation> assist_annotation;
    std::optional<CommandAssistPlan> command_assist_plan;
    std::optional<ToolAssistPlan> tool_assist_plan;
    std::optional<BuildAssistPlan> build_assist_plan;
    std::optional<NextStepAssistPlan> next_step_assist_plan;
    std::optional<CaseSummaryAssistPlan> case_summary_assist_plan;
    std::optional<FreeformAssistAnswer> freeform_assist_answer;
    std::optional<SecurityAudit> security_audit;
    std::optional<LanguageResolutionRecord> language_resolution;
    std::optional<UacStateRecord> uac_state;
    std::optional<PostProcessRecord> postprocess_record;
    std::optional<LegacySourceRecord> legacy_source;
    std::optional<LegacyBridgeReport> legacy_bridge_report;
    std::optional<QuerySessionRecord> query_session;
    std::vector<LegacyResearchArtifact> legacy_research_artifacts;
    std::vector<LegacyCorrelationRecord> legacy_correlations;
    std::vector<LegacySymbolCoverage> legacy_symbol_coverage;
    std::optional<LegacyRecoveryStatus> legacy_recovery_status;
    std::optional<ReviewArtifact> review_artifact;
    std::optional<PatchProposalArtifact> patch_proposal_artifact;
    std::optional<RecipeAuthoringArtifact> recipe_authoring_artifact;
    std::optional<DefinitionAnswer> definition_answer;
    std::optional<NeuralMathReport> neural_math_report;
    std::optional<NeuralRouteReport> neural_route_report;
    std::optional<RecursiveDiffReport> recursive_diff_report;
    std::vector<TzeStageRecord> stages;
};

struct StoredDefinition {
    std::string term;
    std::string normalized_concept;
    std::string domain_hint;
    std::string summary;
    std::string mapped_cpp_target;
    std::string semantic_family;
    std::string source;
    std::string source_type;
    std::string authority_tier;
    double confidence = 0.0;
    std::string scope;
    std::string created_at;
    std::string expires_at;
};

struct BuildRecipe {
    std::string id;
    std::string acquisition_method = "git";
    std::string build_system;
    std::vector<std::string> supported_platforms;
    std::string default_target;
    std::string install_target;
    std::vector<std::string> artifact_patterns;
    std::vector<std::string> install_output_patterns;
    std::vector<std::string> fallback_stage_patterns;
    std::vector<std::string> dependency_hints;
    std::vector<std::string> configure_arguments;
    bool supports_install = true;
    bool copy_artifacts_on_install = false;
};

struct ProjectAlias {
    std::string canonical_name;
    std::vector<std::string> aliases;
    std::string upstream_url;
    std::string default_ref;
    std::vector<BuildRecipe> recipes;
};

struct ProjectRecord {
    std::string canonical_name;
    std::string resolved_source_path;
    std::string build_system;
    std::string status;
    std::string upstream_url;
};

struct LearnedRecipeRecord {
    std::string canonical_name;
    std::string recipe_id;
    std::string environment_key;
    std::string build_system;
    int success_count = 0;
    int failure_count = 0;
    std::string last_success_at;
    std::string last_failure_at;
    std::string last_status;
    std::string last_artifact;
    std::string last_install_prefix;
    int confidence_score = 50;
};

struct RecipeAuthoringPlan {
    std::string task_id;
    std::string provider_id;
    std::string model;
    std::string status;
    BuildRecipe recipe;
    std::string rationale;
    std::vector<std::string> evidence_references;
    double confidence = 0.0;
    std::vector<std::string> warnings;
    AssistPlanMetadata metadata;
};

struct AuthoredRecipeRecord {
    BuildRecipe recipe;
    std::string resolved_source_path;
    std::string canonical_name;
    std::string origin_provider;
    std::string origin_model;
    std::vector<std::string> evidence_summary;
    std::string validation_status;
    std::string last_validation_summary;
    std::string last_validation_log;
    std::string last_artifact;
    std::string authoring_run_id;
    std::string active_scope;
    bool active = false;
    bool repair_attempted = false;
    std::string persisted_at;
};

struct NativeToolRecord {
    std::string logical_name;
    std::string provider_type;
    std::string executable_path;
    std::string applet_name;
    std::string version_fingerprint;
    std::vector<std::string> capability_flags;
    std::string environment_signature;
    std::uintmax_t size_bytes = 0;
    long long modified_timestamp = 0;
    std::string discovery_origin;
    std::string last_verified;
};

struct ObservationRecord {
    std::string id;
    std::string case_id;
    std::string source_kind;
    std::string source_ref;
    std::string collected_at;
    std::string summary;
    std::string raw_content;
    std::string content_hash;
};

struct NormalizedObject {
    std::string id;
    std::string case_id;
    std::string observation_id;
    std::string object_type;
    std::string title;
    std::string summary;
    std::vector<std::string> attributes;
};

struct EvidenceLink {
    std::string id;
    std::string case_id;
    std::string source_observation_id;
    std::string target_object_id;
    std::string relation;
    std::string rationale;
};

struct AnalystComment {
    std::string id;
    std::string case_id;
    std::string author;
    std::string text;
    std::string created_at;
};

struct DecisionCandidate {
    std::string id;
    std::string case_id;
    std::string title;
    std::string rationale;
    std::string recommended_command;
    std::string status;
    int score = 0;
    bool valid = true;
    int validity_score = 100;
    int evidence_coverage = 0;
    int prior_success_score = 50;
    double confidence = 0.0;
    double probability_likelihood = 0.0;
    std::vector<std::string> supporting_signals;
    std::vector<std::string> validation_checks;
    std::vector<std::string> score_trace;
    std::vector<MathAttribution> math_attributions;
    std::string operator_feedback;
    std::string feedback_note;
    std::string feedback_timestamp;
    std::string outcome_status;
    std::string outcome_note;
    std::string outcome_timestamp;
};

struct PermissionContext {
    std::string role = "analyst";
    bool can_view_raw = true;
    bool can_run_actions = true;
    bool can_store_feedback = true;
};

struct CaseRecord {
    std::string id;
    std::string title;
    std::string primary_source;
    std::string status;
    std::string created_at;
    std::string updated_at;
    std::string created_by_run_id;
    std::string analyzed_by_run_id;
    std::string decided_by_run_id;
    std::string reported_by_run_id;
    PermissionContext permission;
    std::vector<std::string> observation_ids;
    std::vector<std::string> object_ids;
    std::vector<std::string> evidence_link_ids;
    std::vector<std::string> comment_ids;
    std::vector<std::string> decision_ids;
    std::string latest_summary;
};

struct CaseLink {
    std::string id;
    std::string left_case_id;
    std::string right_case_id;
    std::string link_type;
    std::string link_value;
    std::string rationale;
    int strength = 0;
};

struct CaseCluster {
    std::string id;
    std::string cluster_type;
    std::string title;
    std::string summary;
    std::vector<std::string> case_ids;
    std::vector<std::string> link_ids;
    std::vector<std::string> link_types;
    std::vector<std::string> shared_indicators;
    int case_count = 0;
    int correlation_score = 0;
    double likelihood = 0.0;
};

struct MemoryPaths {
    std::filesystem::path root;
    std::filesystem::path history_path;
    std::filesystem::path tze_runs_path;
    std::filesystem::path definitions_path;
    std::filesystem::path preferences_path;
    std::filesystem::path projects_path;
    std::filesystem::path authored_recipes_path;
    std::filesystem::path native_tools_path;
    std::filesystem::path language_contexts_path;
    std::filesystem::path uac_states_path;
    std::filesystem::path security_audits_path;
    std::filesystem::path legacy_sources_path;
    std::filesystem::path assist_memory_path;
    std::filesystem::path cases_path;
    std::filesystem::path workspaces_root;
    std::filesystem::path installs_root;
    std::filesystem::path logs_root;
};

struct MemorySnapshot {
    MemoryPaths paths;
    std::vector<MemoryHistoryEntry> history;
    std::vector<TzeRunRecord> tze_runs;
    std::vector<StoredDefinition> definitions;
    std::vector<std::string> source_preference_order;
    std::optional<OperatorPersonaRecord> operator_persona;
    std::vector<ShellLexiconEntry> shell_lexicon_overlay;
    std::vector<ProjectRecord> projects;
    std::vector<LearnedRecipeRecord> learned_recipes;
    std::vector<AuthoredRecipeRecord> authored_recipes;
    std::vector<NativeToolRecord> native_tools;
    std::vector<LanguageResolutionRecord> language_contexts;
    std::vector<UacStateRecord> uac_states;
    std::vector<SecurityAudit> security_audits;
    std::vector<LegacySourceRecord> legacy_sources;
    std::vector<AssistOutcomeRecord> assist_outcomes;
    std::vector<AssistCorrectionRecord> assist_corrections;
    std::vector<AssistLearningRecord> assist_learning;
    std::vector<HostAssistPreferenceRecord> host_assist_preferences;
    std::vector<ObservationRecord> observations;
    std::vector<NormalizedObject> normalized_objects;
    std::vector<EvidenceLink> evidence_links;
    std::vector<AnalystComment> analyst_comments;
    std::vector<DecisionCandidate> decision_candidates;
    std::vector<CaseRecord> case_records;
    std::vector<CaseLink> case_links;
};

struct ToolchainModuleStatus {
    std::string id;
    std::string name;
    std::string command;
    std::string resolved_path;
    std::string version;
    bool available = false;
    std::vector<std::string> capabilities;
};

struct SourceInspection {
    std::string source_path;
    std::string resolved_source_path;
    std::string build_system;
    bool exists = false;
    bool ready = false;
    std::string summary;
    std::vector<std::string> detected_files;
    std::vector<std::string> recommended_modules;
    std::vector<std::string> missing_modules;
};

struct AcquisitionResult {
    std::string status;
    std::string summary;
    std::string canonical_project_name;
    std::string resolved_source_path;
    std::string upstream_url;
    bool fetched = false;
    std::vector<std::string> commands;
};

struct PreflightReport {
    std::string status;
    std::string summary;
    std::string canonical_project_name;
    std::string recipe_id;
    std::vector<std::string> available_recipe_ids;
    std::string recipe_selection_reason;
    std::string build_system;
    std::string environment_signature;
    std::string install_prefix;
    bool platform_supported = false;
    bool ready = false;
    bool will_acquire = false;
    bool will_install = false;
    std::vector<std::string> missing_modules;
    std::vector<std::string> dependency_hints;
    std::vector<std::string> expected_steps;
    std::vector<std::string> expected_artifacts;
    std::vector<std::string> expected_install_outputs;
};

struct PackageManagerGuidance {
    std::string id;
    std::string label;
    bool primary = false;
    std::vector<std::string> commands;
};

struct DoctorReport {
    std::string status;
    std::string summary;
    std::string canonical_project_name;
    std::string recipe_id;
    std::string build_system;
    std::string detected_platform;
    std::string detected_package_manager;
    std::vector<std::string> available_package_managers;
    std::vector<std::string> missing_modules;
    std::vector<std::string> dependency_checks;
    std::vector<std::string> dependency_hints;
    std::vector<std::string> configure_flags;
    std::vector<std::string> build_guidance;
    std::vector<std::string> next_steps;
    std::vector<PackageManagerGuidance> package_guidance;
    std::vector<std::string> bootstrap_guidance;
};

struct ToolResolution {
    std::string requested_name;
    std::string logical_name;
    std::string provider_type;
    std::string executable_path;
    std::string applet_name;
    std::string version_fingerprint;
    std::vector<std::string> capability_flags;
    std::string environment_signature;
    std::string cache_origin;
    std::string validation_signature;
    std::string summary;
    std::vector<std::string> candidate_paths;
    std::vector<std::string> provider_notes;
    std::string recommended_command;
    bool found = false;
    bool cache_validated = false;
};

struct ToolDoctorReport {
    std::string status;
    std::string summary;
    std::string logical_name;
    std::string selected_provider;
    std::string executable_path;
    std::string cache_origin;
    std::string recommended_next_command;
    std::vector<std::string> discovered_paths;
    std::vector<std::string> busybox_applets;
    std::vector<std::string> capability_notes;
    std::vector<std::string> missing_tools;
};

struct ToolInvocationReport {
    std::string status;
    std::string summary;
    std::string logical_name;
    std::string selected_provider;
    std::string executable_path;
    std::string cache_origin;
    std::string command_line;
    int exit_code = 0;
    std::vector<std::string> output_excerpt;
};

struct PacketCaptureRequest {
    std::string mode = "live";
    std::string interface_name;
    std::string pcap_path;
    std::string export_path;
    std::string host_filter;
    int port = 0;
    std::size_t packet_count = 10;
    std::size_t seconds = 5;
    std::size_t payload_bytes = 96;
};

struct PacketRecord {
    std::string timestamp;
    std::string src_ip;
    std::string dst_ip;
    int src_port = 0;
    int dst_port = 0;
    std::string tcp_flags;
    std::size_t captured_length = 0;
    std::size_t original_length = 0;
    std::size_t payload_length = 0;
    std::string hex_preview;
    std::string ascii_preview;
    std::string analysis_code;
    std::string payload_text_utf8;
    std::string payload_text_status;
    std::vector<std::string> plaintext_decode;
    std::string classification;
};

struct PacketCaptureReport {
    std::string status;
    std::string summary;
    std::string mode;
    std::string interface_name;
    std::string pcap_path;
    std::string export_path;
    std::string filter;
    std::size_t packet_count = 0;
    std::vector<PacketRecord> packets;
    std::vector<std::string> flow_summary;
    std::vector<std::string> warnings;
    std::vector<std::string> privilege_diagnostics;
    std::vector<std::string> interfaces;
};

struct DefenseDiagnosticRequest {
    std::string mode = "summary";
    std::string target;
    int port = 0;
    int pid = 0;
};

struct DefenseDiagnosticReport {
    std::string status;
    std::string summary;
    std::string mode;
    std::string target;
    std::vector<std::string> evidence_lines;
    std::vector<std::string> proposed_actions;
    std::vector<std::string> warnings;
};

struct BuildExecution {
    std::string source_path;
    std::string resolved_source_path;
    std::string build_system;
    bool built = false;
    bool installed = false;
    std::string status;
    std::string summary;
    std::string build_dir;
    std::string log_path;
    std::vector<std::string> commands;
    std::vector<std::string> detected_files;
    std::vector<std::string> missing_modules;
    std::vector<std::string> log_excerpt;
    std::vector<std::string> diagnostic_excerpt;
    std::string artifact_hint;
    std::string install_status;
    std::string install_prefix;
    std::vector<std::string> verified_artifacts;
    std::vector<std::string> verified_install_outputs;
    std::string failure_category;
    std::string environment_signature;
    std::string selected_recipe_id;
    std::string recipe_selection_reason;
};

struct ProcessingReport {
    std::string version_string;
    std::string raw_prompt;
    std::string tze_run_id;
    std::string resolved_intent;
    std::string resolved_project;
    std::string resolved_project_path;
    std::string source_preference_path;
    std::string reasoning_provider = "null";
    std::optional<ProviderProbeReport> provider_probe_report;
    std::string assist_status;
    std::optional<AssistAnnotation> assist_annotation;
    std::optional<CommandAssistPlan> command_assist_plan;
    std::optional<ToolAssistPlan> tool_assist_plan;
    std::optional<BuildAssistPlan> build_assist_plan;
    std::optional<RecipeAuthoringPlan> recipe_authoring_plan;
    std::optional<NextStepAssistPlan> next_step_assist_plan;
    std::optional<CaseSummaryAssistPlan> case_summary_assist_plan;
    std::optional<FreeformAssistAnswer> freeform_assist_answer;
    std::string answer_status;
    std::string answer_explanation;
    std::string next_action;
    std::string produced_artifact;
    std::string source_map_path;
    std::optional<ShellLexiconEntry> shell_normalization;
    CacheCell cache;
    std::string decoded_instruction;
    std::vector<TzeStageRecord> tze_stages;
    std::optional<QuerySessionRecord> query_session;
    std::vector<KnowledgeReference> references;
    std::vector<std::string> feedback_loop;
    SecurityAudit security;
    std::vector<std::string> storage_writes;
    std::vector<std::string> memory_reads;
    std::vector<std::string> memory_writes;
    std::vector<SourceBackedMapping> source_backed_mappings;
    std::optional<LegacySourceRecord> legacy_source;
    std::optional<LegacyBridgeReport> legacy_bridge_report;
    std::vector<LegacyResearchArtifact> legacy_research_artifacts;
    std::vector<LegacyCorrelationRecord> legacy_correlations;
    std::vector<LegacySymbolCoverage> legacy_symbol_coverage;
    std::optional<LegacyRecoveryStatus> legacy_recovery_status;
    std::vector<ToolchainModuleStatus> toolchain;
    std::optional<DefinitionAnswer> definition_answer;
    std::optional<LanguageResolutionRecord> language_resolution;
    std::optional<UacStateRecord> uac_state;
    std::optional<PostProcessRecord> postprocess_record;
    std::optional<AcquisitionResult> acquisition_result;
    std::optional<PreflightReport> preflight_report;
    std::optional<DoctorReport> doctor_report;
    std::optional<ToolResolution> tool_resolution;
    std::optional<ToolDoctorReport> tool_doctor_report;
    std::optional<ToolInvocationReport> tool_invocation_report;
    std::optional<PacketCaptureReport> packet_capture_report;
    std::optional<DefenseDiagnosticReport> defense_diagnostic_report;
    std::optional<NeuralMathReport> neural_math_report;
    std::optional<NeuralRouteReport> neural_route_report;
    std::optional<RecursiveDiffReport> recursive_diff_report;
    std::optional<SourceInspection> source_inspection;
    std::optional<BuildExecution> build_execution;
    std::optional<ReviewArtifact> review_artifact;
    std::optional<PatchProposalArtifact> patch_proposal_artifact;
    std::optional<RecipeAuthoringArtifact> recipe_authoring_artifact;
    std::optional<PermissionContext> permission_context;
    std::optional<CaseRecord> case_record;
    std::vector<ObservationRecord> observations;
    std::vector<NormalizedObject> normalized_objects;
    std::vector<EvidenceLink> evidence_links;
    std::vector<AnalystComment> analyst_comments;
    std::vector<DecisionCandidate> decision_candidates;
    std::vector<CaseRecord> case_matches;
    std::vector<CaseLink> case_links;
    std::vector<CaseCluster> case_clusters;
};

struct RequestProfile {
    std::string raw_prompt;
    std::string instruction_slot;
    RequestIntent resolved_intent = RequestIntent::Unknown;
    std::string operator_handle;
    bool operator_is_admin = false;
    bool first_run = false;
    bool persist_on_success = true;
    std::size_t estimated_size = 0;
    std::string source_map_path = "res/tze.txt";
    std::string project_reference;
    std::string project_alias;
    std::string selected_recipe_id;
    std::string analyst_reference;
    std::string analyst_mode = "inspect";
    std::string analyst_query;
    std::string decision_reference;
    std::string incident_reference;
    std::string tze_run_reference;
    std::string tze_compare_reference;
    std::string feedback_value;
    std::string feedback_note;
    std::string persona_mode;
    std::string language_confirmation = "auto";
    std::string provider_selection;
    bool assist_requested = false;
    std::string requested_tool_name;
    std::vector<std::string> tool_arguments;
    ToolCommandMode tool_mode = ToolCommandMode::None;
    bool verbose_output = false;
    std::string packet_capture_mode;
    std::string packet_interface;
    std::string packet_pcap_path;
    std::string packet_export_path;
    std::string packet_host_filter;
    int packet_port = 0;
    std::size_t packet_count = 10;
    std::size_t packet_seconds = 5;
    std::size_t packet_payload_bytes = 96;
    std::string defense_mode;
    std::string defense_target;
    int defense_port = 0;
    int defense_pid = 0;
    std::string neural_math_mode;
    std::string neural_dataset;
    std::size_t neural_epochs = 32;
    double neural_learning_rate = 0.2;
    std::string neural_route_mode;
    std::string neural_route_input_path;
    std::string neural_route_output_path;
    AcquisitionPolicy acquisition_policy = AcquisitionPolicy::FetchIfMissing;
    std::string memory_root_path;
    std::string memory_view = "history";
    std::size_t prune_keep_count = 12;
    bool important_only = false;
    bool execute_build = false;
    bool clean_build = false;
    bool perform_install = true;
    bool offline = false;
    bool preflight_only = false;
    std::string build_source_path;
    std::string build_dir;
    std::string build_target;
    std::string build_type = "Release";
    std::string install_prefix;
    std::string output_path;
    std::string git_ref_override;
    std::string shell_correction_note;
    std::string review_target;
    std::string definition_concept;
    std::string definition_domain_hint;
    std::string definition_comparison_rationale;
};

std::string_view to_string(RequestIntent intent);
std::string_view to_string(AcquisitionPolicy policy);
std::string_view to_string(ToolCommandMode mode);

}  // namespace tze
