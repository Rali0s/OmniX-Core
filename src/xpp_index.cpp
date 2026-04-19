#include "xpp/index.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <string>
#include <utility>

namespace xpp {
namespace {

struct SemanticHint {
    std::string meaning;
    std::string cpp_target;
    MappingStatus status = MappingStatus::Stubbed;
    SemanticFamily family = SemanticFamily::Unknown;
};

SemanticHint mapped(std::string meaning, std::string cpp_target, SemanticFamily family) {
    return {std::move(meaning), std::move(cpp_target), MappingStatus::Mapped, family};
}

SemanticHint abstracted(std::string meaning, std::string cpp_target, SemanticFamily family) {
    return {std::move(meaning), std::move(cpp_target), MappingStatus::Abstracted, family};
}

SemanticHint stubbed(std::string meaning, std::string cpp_target, SemanticFamily family) {
    return {std::move(meaning), std::move(cpp_target), MappingStatus::Stubbed, family};
}

SemanticHint unsupported(std::string meaning, std::string cpp_target) {
    return {std::move(meaning), std::move(cpp_target), MappingStatus::Unsupported, SemanticFamily::SecurityBlocked};
}

std::string lowercase(std::string_view value) {
    std::string lowered;
    lowered.reserve(value.size());
    for (char c : value) {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return lowered;
}

bool starts_with(std::string_view value, std::string_view prefix) {
    return value.substr(0, prefix.size()) == prefix;
}

bool contains_any(std::string_view value, std::initializer_list<std::string_view> needles) {
    for (std::string_view needle : needles) {
        if (value.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool is_security_section(std::string_view title) {
    return lowercase(title).find("security") != std::string::npos;
}

bool is_language_section(std::string_view title) {
    return lowercase(title).find("language") != std::string::npos;
}

bool is_preprocessor_section(std::string_view title) {
    const std::string lowered = lowercase(title);
    return lowered.find("pre-processor") != std::string::npos || lowered.find("preprocessor") != std::string::npos;
}

bool is_build_section(std::string_view title) {
    const std::string lowered = lowercase(title);
    return lowered.find("build") != std::string::npos;
}

const std::map<std::string, SemanticHint>& semantic_hints() {
    static const std::map<std::string, SemanticHint> hints = {
        {"contextual_match",
         abstracted("Contextual match or bridge operator between two operands.",
                    "generated::xpp::support::abstracted_symbol",
                    SemanticFamily::Query)},
        {"carry_forward",
         abstracted("Carry-forward operator that reuses the latter context or prior result.",
                    "generated::xpp::support::abstracted_symbol",
                    SemanticFamily::Query)},
        {"bounded_transform",
         abstracted("Bounded transform operator that narrows or reduces a result.",
                    "generated::xpp::support::abstracted_symbol",
                    SemanticFamily::Query)},
        {"x", abstracted("Root X++ runtime symbol preserved as a safe placeholder anchor.",
                         "tze::NamespaceRegistry::root_symbol",
                         SemanticFamily::Storage)},
        {"xprocessingcache",
         mapped("Prepare cache context for a new request.", "tze::CacheCoordinator::prepare", SemanticFamily::Storage)},
        {"xize", mapped("Estimate storage requirements for the active request.",
                        "tze::CacheCoordinator::prepare",
                        SemanticFamily::Storage)},
        {"xcell_create",
         mapped("Provision a temporary or persistent cache cell.", "tze::CacheCoordinator::prepare", SemanticFamily::Storage)},
        {"xprocessingdefine",
         mapped("Attach retention and seek behaviours to the active cache.",
                "tze::CacheCoordinator::define",
                SemanticFamily::Storage)},
        {"x_destroy", mapped("Destroy or retain cache contents after processing.",
                             "tze::CacheCoordinator::destroy",
                             SemanticFamily::Storage)},
        {"x_define_low",
         mapped("Resolve an instruction slot into a concrete command.",
                "tze::KnowledgeEngine::decode_instruction",
                SemanticFamily::Query)},
        {"x3m", mapped("Fetch contextual knowledge from an external or ranked source.",
                       "tze::KnowledgeEngine::prioritize",
                       SemanticFamily::Query)},
        {"x_displaypriorityprocessinggate",
         mapped("Display ranked sources and capture preference ordering.",
                "tze::IoRuntime::write_output",
                SemanticFamily::Io)},
        {"x_displayfeedbackloop",
         mapped("Replay prior answers and cached context.",
                "tze::KnowledgeEngine::replay_feedback",
                SemanticFamily::Io)},
        {"x_store", mapped("Persist structured output into a temporary or permanent map.",
                           "tze::NamespaceRegistry::store_value",
                           SemanticFamily::Storage)},
        {"x_return", mapped("Return a computed value to the active pipeline.",
                            "native return / report output",
                            SemanticFamily::Query)},
        {"x_comms", abstracted("Raise a communications or admin-intervention workflow.",
                               "tze::SecurityManager::abstract_operation",
                               SemanticFamily::SecuritySafe)},
        {"x_security", mapped("Validate operator privileges and record audit phases.",
                              "tze::SecurityManager::verify",
                              SemanticFamily::SecuritySafe)},
        {"x_c_p_1", abstracted("Administrative pipeline phase 1.", "tze::SecurityManager::abstract_operation", SemanticFamily::SecuritySafe)},
        {"x_c_p_2", abstracted("Administrative pipeline phase 2.", "tze::SecurityManager::abstract_operation", SemanticFamily::SecuritySafe)},
        {"x_c_p_3", abstracted("Administrative pipeline phase 3.", "tze::SecurityManager::abstract_operation", SemanticFamily::SecuritySafe)},
        {"xx_kill_all",
         unsupported("Emergency purge / kill-switch semantics from the pseudo language.",
                     "generated::xpp::support::unsupported_symbol")},
        {"x_superadmin",
         unsupported("Escalation path into a more privileged lockdown mode.",
                     "generated::xpp::support::unsupported_symbol")},
        {"x_lockout",
         unsupported("Lockout semantics for suspicious access attempts.",
                     "generated::xpp::support::unsupported_symbol")},
        {"xmap_temp", mapped("Ephemeral working map namespace.", "tze::NamespaceRegistry::temporary_namespace", SemanticFamily::Storage)},
        {"xmap_perm", mapped("Persistent canonical map namespace.", "tze::NamespaceRegistry::persistent_namespace", SemanticFamily::Storage)},
        {"xmap_core", mapped("Core system datastore namespace.", "tze::NamespaceRegistry::core_namespace", SemanticFamily::Storage)},
        {"genx", mapped("Generated key or regeneration token shared across preprocessing and language-detection flows.",
                        "tze::PreprocessorRuntime::genx_token",
                        SemanticFamily::Preprocessor)},
        {"x_activeping",
         mapped("Diagnostic reachability probe used by the Omni bridge flow.",
                "tze::OmniBridge::active_ping_placeholder",
                SemanticFamily::OmniBridge)},
        {"x_holdtemp", mapped("Retain transient values for the current workflow pass.",
                              "tze::WorkflowSupport::hold_temp",
                              SemanticFamily::Storage)},
        {"x_readnativeos", mapped("Read the host operating-system identity.",
                                  "tze::LanguageEngine::read_native_os",
                                  SemanticFamily::Language)},
        {"xxindex", mapped("Index records in an Omni-side context store.",
                           "tze::OmniBridge::index_context",
                           SemanticFamily::OmniBridge)},
        {"x_results", mapped("Reference the current result set view.", "tze::QueryRuntime::results_view", SemanticFamily::Query)},
        {"x_check", mapped("Evaluate a workflow condition or checkpoint.", "tze::QueryRuntime::check_value", SemanticFamily::Query)},
        {"x_display", mapped("Display or render human-readable workflow output.", "tze::IoRuntime::display_text", SemanticFamily::Io)},
        {"x_locate", mapped("Locate matching paths or records.", "tze::WorkflowSupport::locate_paths", SemanticFamily::Query)},
        {"x_run", mapped("Run a named workflow step.", "tze::WorkflowSupport::run_named_step", SemanticFamily::Query)},
        {"x_sweep", mapped("Sweep a filesystem branch for candidate inputs.", "tze::WorkflowSupport::sweep_paths", SemanticFamily::Query)},
        {"xmap_cde_store", mapped("Deep-language parse storage namespace.",
                                  "tze::NamespaceRegistry::resolve_namespace",
                                  SemanticFamily::Storage)},
        {"xmap_perm_epoch_manual_languagesystem", mapped("Manual language-system storage namespace.",
                                                         "tze::NamespaceRegistry::persistent_namespace",
                                                         SemanticFamily::Storage)},
        {"xmap_perm_admin_os_epoch_osdat", mapped("Admin OS metadata storage namespace.",
                                                  "tze::NamespaceRegistry::persistent_namespace",
                                                  SemanticFamily::Storage)},
        {"xxomni_contextualizecache", mapped("Contextualize or hydrate a cached Omni record set.",
                                             "tze::OmniBridge::contextualize_cache",
                                             SemanticFamily::OmniBridge)},
        {"xxomni_map", mapped("Map Omni-side context into a local representation.",
                              "tze::OmniBridge::map_context",
                              SemanticFamily::OmniBridge)},
        {"xxomni_seek", mapped("Seek records within an Omni-side context set.",
                               "tze::OmniBridge::seek_records",
                               SemanticFamily::OmniBridge)},
        {"xze", mapped("Namespace for process and compression helper routines.",
                       "tze::OmniBridge::running_process_list",
                       SemanticFamily::OmniBridge)},
        {"xccess", mapped("Namespace for contextual search and comparison helpers.",
                          "tze::OmniBridge::access_search",
                          SemanticFamily::OmniBridge)},
        {"genxof", mapped("Generated-key sizing or tunnel-budget token.",
                          "tze::PreprocessorRuntime::key_budget",
                          SemanticFamily::Preprocessor)},
        {"x_ccd_test", abstracted("Placeholder for verification in safe mode.",
                                  "tze::SecurityManager::abstract_operation",
                                  SemanticFamily::SecuritySafe)},
        {"x_dnlio_postprocessing", mapped("Post-process native-language detection output.",
                                          "tze::LanguageEngine::postprocess_language_detection",
                                          SemanticFamily::Language)},
        {"x_decompress", mapped("Decompress or unwrap a staged artifact.",
                                "tze::LanguageEngine::decompress_artifact",
                                SemanticFamily::Language)},
        {"x_map_nativeoslanguage", mapped("Map the native OS to its language profile.",
                                          "tze::LanguageEngine::map_native_os_language",
                                          SemanticFamily::Language)},
        {"x_map_perm_admin_os_epoch_osdat", mapped("Store OS-type metadata in the admin OS map namespace.",
                                                   "tze::LanguageEngine::store_os_type",
                                                   SemanticFamily::Language)},
        {"x_trans",
         unsupported("Tunnel or transfer branch intentionally left inert in v1.",
                     "generated::xpp::support::unsupported_symbol")},
        {"x_call", mapped("Invoke a named subroutine in the workflow layer.",
                          "tze::WorkflowSupport::call_subroutine",
                          SemanticFamily::Query)},
        {"x_checkfile", mapped("Check whether a file is coherent and readable.",
                               "tze::LanguageEngine::check_file_coherence",
                               SemanticFamily::Language)},
        {"x_dnl", mapped("Shorthand for native-language detection.",
                         "tze::LanguageEngine::detect_native_language",
                         SemanticFamily::Language)},
        {"x_detectnativelanguage", mapped("Detect the host or input language.",
                                          "tze::LanguageEngine::detect_native_language",
                                          SemanticFamily::Language)},
        {"x_detectnativelanguageindexoperating", mapped("Detect native language and index it against the operating environment.",
                                                        "tze::LanguageEngine::detect_native_language_index_operating",
                                                        SemanticFamily::Language)},
        {"x_determineoslanguage", mapped("Determine the dominant OS language profile.",
                                         "tze::LanguageEngine::determine_os_language",
                                         SemanticFamily::Language)},
        {"x_file", mapped("Resolve the current file state or presence.", "tze::WorkflowSupport::file_state", SemanticFamily::Storage)},
        {"x_indexmark", mapped("Mark an index position or checkpoint.", "tze::QueryRuntime::index_marker", SemanticFamily::Query)},
        {"x_know", mapped("Record an observation for later reasoning.",
                          "tze::WorkflowSupport::record_observation",
                          SemanticFamily::Query)},
        {"x_map", mapped("Map a name to a derived value.", "tze::NamespaceRegistry::map_value", SemanticFamily::Storage)},
        {"x_map_os_type_store", mapped("Store the detected OS type in a canonical slot.",
                                       "tze::LanguageEngine::store_os_type",
                                       SemanticFamily::Language)},
        {"x_match", mapped("Match or compare two candidate values.", "tze::QueryRuntime::match_values", SemanticFamily::Query)},
        {"x_mkdir", mapped("Ensure a directory exists before storing results.",
                           "tze::WorkflowSupport::ensure_directory",
                           SemanticFamily::Storage)},
        {"x_movindex", mapped("Move or relabel an index position.", "tze::WorkflowSupport::move_index", SemanticFamily::Query)},
        {"x_narrow", mapped("Reduce a candidate set to the strongest matches.",
                            "tze::WorkflowSupport::narrow_candidates",
                            SemanticFamily::Query)},
        {"x_parse", mapped("Parse an input stream or filesystem representation.",
                           "tze::WorkflowSupport::parse_input",
                           SemanticFamily::Query)},
        {"x_permitunboundparse", mapped("Allow permissive parsing when the exact shape is not known ahead of time.",
                                        "tze::LanguageEngine::permit_unbound_parse",
                                        SemanticFamily::Language)},
        {"x_search_filesystembranch", mapped("Search a filesystem branch for matching files.",
                                             "tze::WorkflowSupport::search_filesystem_branch",
                                             SemanticFamily::Query)},
        {"x_seek_index", mapped("Seek a named index entry.", "tze::QueryRuntime::seek_value", SemanticFamily::Query)},
        {"x_seekparms", mapped("Capture the parameter set used by a seek operation.",
                               "tze::QueryRuntime::seek_parameters",
                               SemanticFamily::Query)},
        {"xbase", mapped("Base namespace handle for a generated storage root.",
                         "tze::NamespaceRegistry::base_namespace",
                         SemanticFamily::Storage)},
        {"xencrypt", mapped("Mark a value for local encryption or encoding semantics.",
                            "tze::PreprocessorRuntime::encrypt_value",
                            SemanticFamily::Preprocessor)},
        {"xlanguageengine", mapped("Language or OS detection engine root.", "tze::LanguageEngine", SemanticFamily::Language)},
        {"xmap_pem_core_rootsystem", mapped("Core root-system map namespace.",
                                            "tze::NamespaceRegistry::core_namespace",
                                            SemanticFamily::Storage)},
        {"xmap_perm_nativeos_quest_epoch", mapped("Native OS question/answer storage namespace.",
                                                  "tze::NamespaceRegistry::persistent_namespace",
                                                  SemanticFamily::Storage)},
        {"xmap_perm_nativeosdetection_epoch_algo", mapped("Native OS detection algorithm namespace.",
                                                          "tze::NamespaceRegistry::persistent_namespace",
                                                          SemanticFamily::Storage)},
        {"xmap_perm_adminlanguage_epoch_lang", mapped("Admin language preference namespace.",
                                                      "tze::NamespaceRegistry::persistent_namespace",
                                                      SemanticFamily::Storage)},
        {"xmap_perm_index_failreport_epoch_oslangtranslation_lang", mapped("OS-language translation failure-report namespace.",
                                                                           "tze::NamespaceRegistry::persistent_namespace",
                                                                           SemanticFamily::Storage)},
        {"xmap_perm_nativeoslang_epoch_fail_deepdetection_genx_lang", mapped("Deep-detection language namespace for GENx fallback.",
                                                                              "tze::NamespaceRegistry::persistent_namespace",
                                                                              SemanticFamily::Storage)},
        {"xmap_temp_nonnativeos_store_epoch_dnl_parse_caseaddress", mapped("Non-native OS parse-case address namespace.",
                                                                           "tze::NamespaceRegistry::temporary_namespace",
                                                                           SemanticFamily::Storage)},
        {"xmap_temp_possibleoslist_os_epoch_algo", mapped("Possible-OS ranking namespace.",
                                                          "tze::NamespaceRegistry::temporary_namespace",
                                                          SemanticFamily::Storage)},
        {"xruling", mapped("Compute or record a ranking value.", "tze::WorkflowSupport::calculate_ruling", SemanticFamily::Query)},
        {"xtrans_cde_lp", mapped("Translate or normalize a deep-language parse result.",
                                 "tze::LanguageEngine::translate_deep_language_parse",
                                 SemanticFamily::Language)},
        {"xtranslanguageinput", mapped("Capture translated user language input.",
                                       "tze::LanguageEngine::trans_language_input",
                                       SemanticFamily::Language)},
        {"xxdetect", mapped("Detect Omni-side patterns or classifications.",
                            "tze::OmniBridge::detect_patterns",
                            SemanticFamily::OmniBridge)},
        {"xxmap", mapped("Map Omni-side records into a local index.", "tze::OmniBridge::map_records", SemanticFamily::OmniBridge)},
        {"xxomni_index", mapped("Index data inside the Omni bridge context.",
                                "tze::OmniBridge::index_context",
                                SemanticFamily::OmniBridge)},
        {"xxomni_host", mapped("Bind or describe an Omni host link.", "tze::OmniBridge::host_link", SemanticFamily::OmniBridge)},
        {"xxomni_loadnativosbound", mapped("Load native-OS-bound Omni context.",
                                           "tze::OmniBridge::load_native_os_bound",
                                           SemanticFamily::OmniBridge)},
        {"xxomni_match", mapped("Match Omni-side context against another record set.",
                                "tze::OmniBridge::match_context",
                                SemanticFamily::OmniBridge)},
        {"xxomni_premit", mapped("Permit-mode toggle in the Omni bridge layer.",
                                 "tze::OmniBridge::permit_mode",
                                 SemanticFamily::OmniBridge)},
        {"xxomni_read", mapped("Read data from the Omni bridge context.",
                               "tze::OmniBridge::read_context",
                               SemanticFamily::OmniBridge)},
        {"xxomni_request", mapped("Request an Omni bridge operation.",
                                  "tze::OmniBridge::request_operation",
                                  SemanticFamily::OmniBridge)},
        {"xxomni_requestsecurekeytunnel",
         unsupported("Secure-tunnel request branch intentionally left inert in v1.",
                     "generated::xpp::support::unsupported_symbol")},
        {"xxomni_testtunnelsecurity",
         unsupported("Tunnel security test branch intentionally left inert in v1.",
                     "generated::xpp::support::unsupported_symbol")},
        {"xxomnibase", mapped("Base handle for Omni bridge state.", "tze::OmniBridge::omnibase", SemanticFamily::OmniBridge)},
        {"xxomni_x2", mapped("Secondary Omni link identifier.", "tze::OmniBridge::x2_link", SemanticFamily::OmniBridge)},
        {"xze_binarydecompresser", mapped("Binary decompression helper namespace.",
                                          "tze::OmniBridge::binary_decompressor",
                                          SemanticFamily::OmniBridge)},
        {"xze_nativecompressionalgoclasses", mapped("Known native compression algorithm catalogue.",
                                                    "tze::OmniBridge::compression_algo_classes",
                                                    SemanticFamily::OmniBridge)},
        {"xze_pid_temp_location", mapped("Temporary PID location list helper.",
                                         "tze::OmniBridge::pid_temp_location",
                                         SemanticFamily::OmniBridge)},
        {"xze_runningprocesslist", mapped("Running-process list helper.",
                                          "tze::OmniBridge::running_process_list",
                                          SemanticFamily::OmniBridge)},
        {"xccess_searchengine", mapped("Contextual search helper for local language or OS discovery.",
                                       "tze::OmniBridge::access_search",
                                       SemanticFamily::OmniBridge)},
        {"xccess_compare", mapped("Contextual comparison helper.",
                                  "tze::OmniBridge::access_compare",
                                  SemanticFamily::OmniBridge)},
    };
    return hints;
}

bool is_blocked_symbol(const SymbolOccurrence& occurrence) {
    const std::string lowered = lowercase(occurrence.normalized_symbol);
    return contains_any(lowered,
                        {"kill",
                         "penetration",
                         "exploit",
                         "securekeytunnel",
                         "testtunnelsecurity",
                         "reverseengineer",
                         "tunnelpair",
                         "destructor",
                         "shred",
                         "dropfalsekeydata",
                         "delete",
                         "burnout",
                         "predator",
                         "ghostengine",
                         "obtainlock"});
}

bool is_security_safe_symbol(const SymbolOccurrence& occurrence) {
    const std::string lowered = lowercase(occurrence.normalized_symbol);
    return is_security_section(occurrence.section_title) &&
           contains_any(lowered,
                        {"detect",
                         "classification",
                         "classify",
                         "identify",
                         "isolate",
                         "trace",
                         "scope",
                         "defense",
                         "protect",
                         "watch",
                         "log",
                         "legacyscan",
                         "permitrouteradmin"});
}

bool is_storage_symbol(const SymbolOccurrence& occurrence) {
    const std::string lowered = lowercase(occurrence.normalized_symbol);
    const std::string raw_lower = lowercase(occurrence.raw_symbol);
    return contains_any(lowered, {"xmap_", "core_xmap_", "namespace", "xbase"}) || contains_any(raw_lower, {"xmap_", "x.map_", "x_map_", "core_xmap_"});
}

bool is_query_symbol(const SymbolOccurrence& occurrence) {
    const std::string lowered = lowercase(occurrence.normalized_symbol);
    return starts_with(lowered, "az_") ||
           contains_any(lowered, {"x_index", "x_seek", "x_find", "x_determine", "x_read", "x_check", "x_match", "x_results", "x_rank", "x_indexmark", "x_seekparms"});
}

bool is_io_symbol(const SymbolOccurrence& occurrence) {
    const std::string lowered = lowercase(occurrence.normalized_symbol);
    return contains_any(lowered, {"xout", "xin", "xtrans", "x_error", "x_display", "prompt", "emergency"});
}

bool is_language_symbol(const SymbolOccurrence& occurrence) {
    const std::string lowered = lowercase(occurrence.normalized_symbol);
    return contains_any(lowered,
                        {"nativeos",
                         "native_language",
                         "dnl",
                         "dnlio",
                         "oslanguage",
                         "lang",
                         "language",
                         "cde_lp",
                         "decompress"});
}

bool is_preprocessor_symbol(const SymbolOccurrence& occurrence) {
    const std::string lowered = lowercase(occurrence.normalized_symbol);
    return contains_any(lowered, {"genx", "bpp", "binarypreprocessor", "regenx", "encode", "encrypt", "compression", "cipher", "keystore"});
}

bool is_omni_symbol(const SymbolOccurrence& occurrence) {
    const std::string lowered = lowercase(occurrence.normalized_symbol);
    return starts_with(lowered, "xxomni") || starts_with(lowered, "xccess") || starts_with(lowered, "xxdetect") ||
           starts_with(lowered, "xxmap") || starts_with(lowered, "xze");
}

SemanticHint infer_query_hint(const SymbolOccurrence& occurrence) {
    const std::string lowered = lowercase(occurrence.normalized_symbol);
    if (starts_with(lowered, "az_")) {
        return mapped("Instruction-slot token captured from the pseudo source.",
                      "tze::QueryRuntime::instruction_slot",
                      SemanticFamily::Query);
    }
    if (contains_any(lowered, {"seekparms"})) {
        return mapped("Parameter list that configures a seek operation.",
                      "tze::QueryRuntime::seek_parameters",
                      SemanticFamily::Query);
    }
    if (contains_any(lowered, {"indexmark"})) {
        return mapped("Index marker or checkpoint state.",
                      "tze::QueryRuntime::index_marker",
                      SemanticFamily::Query);
    }
    if (contains_any(lowered, {"read_last"})) {
        return mapped("Request to read a recent value from the active context.",
                      "tze::QueryRuntime::read_last",
                      SemanticFamily::Query);
    }
    if (contains_any(lowered, {"x_index"})) {
        return mapped("Index a value or checkpoint inside the active query runtime.",
                      "tze::QueryRuntime::index_value",
                      SemanticFamily::Query);
    }
    if (contains_any(lowered, {"x_seek"})) {
        return mapped("Seek a value or branch inside the active query runtime.",
                      "tze::QueryRuntime::seek_value",
                      SemanticFamily::Query);
    }
    if (contains_any(lowered, {"x_find"})) {
        return mapped("Find matching candidates for the active query request.",
                      "tze::QueryRuntime::find_matches",
                      SemanticFamily::Query);
    }
    if (contains_any(lowered, {"x_determine"})) {
        return mapped("Determine the best candidate or state transition.",
                      "tze::QueryRuntime::determine_value",
                      SemanticFamily::Query);
    }
    if (contains_any(lowered, {"x_read"})) {
        return mapped("Read a value from the active query or cache context.",
                      "tze::QueryRuntime::read_value",
                      SemanticFamily::Query);
    }
    if (contains_any(lowered, {"x_check"})) {
        return mapped("Check a condition before the next workflow step.",
                      "tze::QueryRuntime::check_value",
                      SemanticFamily::Query);
    }
    if (contains_any(lowered, {"x_match"})) {
        return mapped("Match candidate values inside the query runtime.",
                      "tze::QueryRuntime::match_values",
                      SemanticFamily::Query);
    }
    if (contains_any(lowered, {"x_results"})) {
        return mapped("Expose the current result view for a query operation.",
                      "tze::QueryRuntime::results_view",
                      SemanticFamily::Query);
    }
    return mapped("Rank or compare candidate answers in the query runtime.",
                  "tze::QueryRuntime::rank_value",
                  SemanticFamily::Query);
}

SemanticHint infer_io_hint(const SymbolOccurrence& occurrence) {
    const std::string lowered = lowercase(occurrence.normalized_symbol);
    if (contains_any(lowered, {"emergency"})) {
        return mapped("Emergency output channel for operator-visible alerts.",
                      "tze::IoRuntime::write_emergency",
                      SemanticFamily::Io);
    }
    if (contains_any(lowered, {"error"})) {
        return mapped("Error-reporting channel for the active workflow.",
                      "tze::IoRuntime::write_error",
                      SemanticFamily::Io);
    }
    if (starts_with(lowered, "xin")) {
        if (contains_any(lowered, {"prompt"})) {
            return mapped("Prompt-driven input channel for the active workflow.",
                          "tze::IoRuntime::prompt_input",
                          SemanticFamily::Io);
        }
        return mapped("Input channel read for the active workflow.",
                      "tze::IoRuntime::read_input",
                      SemanticFamily::Io);
    }
    if (starts_with(lowered, "xtrans")) {
        return mapped("Translation-oriented I/O step for operator-visible text.",
                      "tze::IoRuntime::translate_text",
                      SemanticFamily::Io);
    }
    if (contains_any(lowered, {"display"})) {
        return mapped("Display text to the active operator-facing channel.",
                      "tze::IoRuntime::display_text",
                      SemanticFamily::Io);
    }
    return mapped("Output text to the active operator-facing channel.",
                  "tze::IoRuntime::write_output",
                  SemanticFamily::Io);
}

SemanticHint infer_language_hint(const SymbolOccurrence& occurrence) {
    const std::string lowered = lowercase(occurrence.normalized_symbol);
    if (contains_any(lowered, {"dnlio"})) {
        return mapped("Run native-language I/O translation and normalization flow.",
                      "tze::LanguageEngine::native_language_io",
                      SemanticFamily::Language);
    }
    if (contains_any(lowered, {"readnativeos"})) {
        return mapped("Read the host operating system identity.",
                      "tze::LanguageEngine::read_native_os",
                      SemanticFamily::Language);
    }
    if (contains_any(lowered, {"detectnativelanguageindexoperating"})) {
        return mapped("Detect native language and index it against the active OS context.",
                      "tze::LanguageEngine::detect_native_language_index_operating",
                      SemanticFamily::Language);
    }
    if (contains_any(lowered, {"detectnativelanguage", "x_dnl"})) {
        return mapped("Detect the native language for the active environment or input.",
                      "tze::LanguageEngine::detect_native_language",
                      SemanticFamily::Language);
    }
    if (contains_any(lowered, {"determineoslanguage", "oslanguage"})) {
        return mapped("Determine the dominant OS-language mapping for the current environment.",
                      "tze::LanguageEngine::determine_os_language",
                      SemanticFamily::Language);
    }
    if (contains_any(lowered, {"map_nativeoslanguage", "nativeoslanguage"})) {
        return mapped("Map the current OS identity to a language profile.",
                      "tze::LanguageEngine::map_native_os_language",
                      SemanticFamily::Language);
    }
    if (contains_any(lowered, {"permitunboundparse"})) {
        return mapped("Permit a safe unbound parse for loosely-typed input.",
                      "tze::LanguageEngine::permit_unbound_parse",
                      SemanticFamily::Language);
    }
    if (contains_any(lowered, {"checkfile"})) {
        return mapped("Check file coherence before language processing continues.",
                      "tze::LanguageEngine::check_file_coherence",
                      SemanticFamily::Language);
    }
    if (contains_any(lowered, {"decompress"})) {
        return mapped("Decompress or unwrap a staged artifact for language analysis.",
                      "tze::LanguageEngine::decompress_artifact",
                      SemanticFamily::Language);
    }
    if (contains_any(lowered, {"postprocessing"})) {
        return mapped("Post-process language-detection output for final storage or display.",
                      "tze::LanguageEngine::postprocess_language_detection",
                      SemanticFamily::Language);
    }
    if (contains_any(lowered, {"translanguageinput"})) {
        return mapped("Capture translated language input from the active operator.",
                      "tze::LanguageEngine::trans_language_input",
                      SemanticFamily::Language);
    }
    return mapped("Translate deep-language parse output into a normalized representation.",
                  "tze::LanguageEngine::translate_deep_language_parse",
                  SemanticFamily::Language);
}

SemanticHint infer_preprocessor_hint(const SymbolOccurrence& occurrence) {
    const std::string lowered = lowercase(occurrence.normalized_symbol);
    if (contains_any(lowered, {"tunnelpair"})) {
        return unsupported("Tunnel-establishment branch from the pseudo source remains intentionally non-executable.",
                           "generated::xpp::support::unsupported_symbol");
    }
    if (contains_any(lowered, {"binarypreprocessor", "x_bpp"})) {
        return mapped("Binary pre-processor stage for generated translation artifacts.",
                      "tze::PreprocessorRuntime::binary_preprocessor",
                      SemanticFamily::Preprocessor);
    }
    if (contains_any(lowered, {"regenx"})) {
        return mapped("Regenerate a GENx token or derivative key.",
                      "tze::PreprocessorRuntime::regenerate_token",
                      SemanticFamily::Preprocessor);
    }
    if (contains_any(lowered, {"genxengine"})) {
        return mapped("Drive the GENx engine over the active preprocessor state.",
                      "tze::PreprocessorRuntime::genx_engine",
                      SemanticFamily::Preprocessor);
    }
    if (contains_any(lowered, {"encode"})) {
        return mapped("Encode a value as part of the preprocessor flow.",
                      "tze::PreprocessorRuntime::encode_value",
                      SemanticFamily::Preprocessor);
    }
    if (contains_any(lowered, {"encrypt"})) {
        return mapped("Encrypt a value as part of the preprocessor flow.",
                      "tze::PreprocessorRuntime::encrypt_value",
                      SemanticFamily::Preprocessor);
    }
    if (contains_any(lowered, {"compression"})) {
        return mapped("Resolve a compression profile for the active artifact flow.",
                      "tze::PreprocessorRuntime::compression_profile",
                      SemanticFamily::Preprocessor);
    }
    if (contains_any(lowered, {"key", "keystore"})) {
        return mapped("Resolve a generated key-store address for preprocessing output.",
                      "tze::PreprocessorRuntime::key_store_address",
                      SemanticFamily::Preprocessor);
    }
    return mapped("Resolve or emit a GENx token for preprocessing and regeneration work.",
                  "tze::PreprocessorRuntime::genx_token",
                  SemanticFamily::Preprocessor);
}

SemanticHint infer_storage_hint(const SymbolOccurrence& occurrence) {
    const std::string lowered = lowercase(occurrence.normalized_symbol);
    if (contains_any(lowered, {"temp"})) {
        return mapped("Temporary storage namespace resolved from the pseudo source.",
                      "tze::NamespaceRegistry::temporary_namespace",
                      SemanticFamily::Storage);
    }
    if (contains_any(lowered, {"core"})) {
        return mapped("Core storage namespace resolved from the pseudo source.",
                      "tze::NamespaceRegistry::core_namespace",
                      SemanticFamily::Storage);
    }
    if (contains_any(lowered, {"perm"})) {
        return mapped("Persistent storage namespace resolved from the pseudo source.",
                      "tze::NamespaceRegistry::persistent_namespace",
                      SemanticFamily::Storage);
    }
    return mapped("Storage namespace resolved from the pseudo source.",
                  "tze::NamespaceRegistry::resolve_namespace",
                  SemanticFamily::Storage);
}

SemanticHint infer_omni_hint(const SymbolOccurrence& occurrence) {
    const std::string lowered = lowercase(occurrence.normalized_symbol);
    if (contains_any(lowered, {"activeping"})) {
        return mapped("Diagnostic reachability probe for the Omni bridge flow.",
                      "tze::OmniBridge::active_ping_placeholder",
                      SemanticFamily::OmniBridge);
    }
    if (contains_any(lowered, {"contextualizecache"})) {
        return mapped("Contextualize or hydrate an Omni-side cache view.",
                      "tze::OmniBridge::contextualize_cache",
                      SemanticFamily::OmniBridge);
    }
    if (contains_any(lowered, {"host"})) {
        return mapped("Bind or describe an Omni host link.", "tze::OmniBridge::host_link", SemanticFamily::OmniBridge);
    }
    if (contains_any(lowered, {"loadnativosbound"})) {
        return mapped("Load native-OS-bound Omni context.", "tze::OmniBridge::load_native_os_bound", SemanticFamily::OmniBridge);
    }
    if (contains_any(lowered, {"match"})) {
        return mapped("Match Omni-side context against another record set.",
                      "tze::OmniBridge::match_context",
                      SemanticFamily::OmniBridge);
    }
    if (contains_any(lowered, {"permit", "premit"})) {
        return mapped("Toggle permit-mode semantics inside the Omni bridge layer.",
                      "tze::OmniBridge::permit_mode",
                      SemanticFamily::OmniBridge);
    }
    if (contains_any(lowered, {"read"})) {
        return mapped("Read data from the Omni bridge context.",
                      "tze::OmniBridge::read_context",
                      SemanticFamily::OmniBridge);
    }
    if (contains_any(lowered, {"request"})) {
        return mapped("Request an Omni bridge operation.", "tze::OmniBridge::request_operation", SemanticFamily::OmniBridge);
    }
    if (contains_any(lowered, {"detect", "detection"})) {
        return mapped("Detect patterns or classifications across the Omni bridge surface.",
                      "tze::OmniBridge::detect_patterns",
                      SemanticFamily::OmniBridge);
    }
    if (contains_any(lowered, {"index"})) {
        return mapped("Index records in the Omni bridge context.", "tze::OmniBridge::index_context", SemanticFamily::OmniBridge);
    }
    if (contains_any(lowered, {"seek"})) {
        return mapped("Seek records inside the Omni bridge context.", "tze::OmniBridge::seek_records", SemanticFamily::OmniBridge);
    }
    if (contains_any(lowered, {"runningprocesslist"})) {
        return mapped("List running processes for Omni-side context inspection.",
                      "tze::OmniBridge::running_process_list",
                      SemanticFamily::OmniBridge);
    }
    if (contains_any(lowered, {"pid_temp_location"})) {
        return mapped("Resolve the PID temp location helper.", "tze::OmniBridge::pid_temp_location", SemanticFamily::OmniBridge);
    }
    if (contains_any(lowered, {"binarydecompresser"})) {
        return mapped("Resolve the Omni-side binary decompressor helper.",
                      "tze::OmniBridge::binary_decompressor",
                      SemanticFamily::OmniBridge);
    }
    if (contains_any(lowered, {"compressionalgoclasses"})) {
        return mapped("Resolve the known native compression algorithm catalogue.",
                      "tze::OmniBridge::compression_algo_classes",
                      SemanticFamily::OmniBridge);
    }
    if (starts_with(lowered, "xccess")) {
        if (contains_any(lowered, {"compare"})) {
            return mapped("Compare contextual candidates through the Omni access bridge.",
                          "tze::OmniBridge::access_compare",
                          SemanticFamily::OmniBridge);
        }
        return mapped("Search contextual candidates through the Omni access bridge.",
                      "tze::OmniBridge::access_search",
                      SemanticFamily::OmniBridge);
    }
    if (contains_any(lowered, {"omnibase"})) {
        return mapped("Resolve the base Omni bridge state handle.", "tze::OmniBridge::omnibase", SemanticFamily::OmniBridge);
    }
    if (contains_any(lowered, {"x2"})) {
        return mapped("Resolve the secondary Omni link identifier.", "tze::OmniBridge::x2_link", SemanticFamily::OmniBridge);
    }
    if (starts_with(lowered, "xxmap")) {
        return mapped("Map Omni-side records into a local index.", "tze::OmniBridge::map_records", SemanticFamily::OmniBridge);
    }
    return mapped("Map Omni-side context into a local representation.", "tze::OmniBridge::map_context", SemanticFamily::OmniBridge);
}

SemanticHint infer_security_safe_hint(const SymbolOccurrence& occurrence) {
    const std::string lowered = lowercase(occurrence.normalized_symbol);
    if (contains_any(lowered, {"detect", "detection"})) {
        return abstracted("Defensive detection step preserved as a safe abstraction.",
                          "tze::SecurityManager::detect_threat",
                          SemanticFamily::SecuritySafe);
    }
    if (contains_any(lowered, {"classify", "identify"})) {
        return abstracted("Defensive classification step preserved as a safe abstraction.",
                          "tze::SecurityManager::classify_threat",
                          SemanticFamily::SecuritySafe);
    }
    if (contains_any(lowered, {"isolate"})) {
        return abstracted("Isolation or containment step preserved as a safe abstraction.",
                          "tze::SecurityManager::isolate_target",
                          SemanticFamily::SecuritySafe);
    }
    if (contains_any(lowered, {"trace", "scope", "legacyscan"})) {
        return abstracted("Trace or scope-analysis step preserved as a safe abstraction.",
                          "tze::SecurityManager::trace_scope",
                          SemanticFamily::SecuritySafe);
    }
    if (contains_any(lowered, {"log"})) {
        return abstracted("Defensive logging step preserved as a safe abstraction.",
                          "tze::SecurityManager::log_event",
                          SemanticFamily::SecuritySafe);
    }
    return abstracted("Defensive security branch preserved as a safe abstraction.",
                      "tze::SecurityManager::abstract_operation",
                      SemanticFamily::SecuritySafe);
}

SemanticHint infer_section_fallback(const SymbolOccurrence& occurrence) {
    if (is_preprocessor_section(occurrence.section_title)) {
        return abstracted("Safe preprocessor symbol preserved as an abstract lowering target.",
                          "tze::PreprocessorRuntime::genx_engine",
                          SemanticFamily::Preprocessor);
    }
    if (is_language_section(occurrence.section_title)) {
        return abstracted("Safe language symbol preserved as an abstract lowering target.",
                          "tze::LanguageEngine::translate_deep_language_parse",
                          SemanticFamily::Language);
    }
    if (is_build_section(occurrence.section_title)) {
        return abstracted("Safe build-flow symbol preserved as an abstract lowering target.",
                          "tze::WorkflowSupport::run_named_step",
                          SemanticFamily::Query);
    }
    if (is_security_section(occurrence.section_title)) {
        return abstracted("Safe security symbol preserved as an abstract lowering target.",
                          "tze::SecurityManager::abstract_operation",
                          SemanticFamily::SecuritySafe);
    }
    return abstracted("Safe X++ symbol preserved as an abstract lowering target.",
                      "tze::WorkflowSupport::run_named_step",
                      SemanticFamily::Unknown);
}

SemanticHint infer_fallback(const SymbolOccurrence& occurrence) {
    if (is_blocked_symbol(occurrence)) {
        return unsupported("Unsupported branch from the pseudo source. Parsed for traceability but intentionally emitted as inert code only.",
                           "generated::xpp::support::unsupported_symbol");
    }
    if (is_security_safe_symbol(occurrence)) {
        return infer_security_safe_hint(occurrence);
    }
    if (is_storage_symbol(occurrence)) {
        return infer_storage_hint(occurrence);
    }
    if (is_omni_symbol(occurrence)) {
        return infer_omni_hint(occurrence);
    }
    if (is_language_symbol(occurrence)) {
        return infer_language_hint(occurrence);
    }
    if (is_preprocessor_symbol(occurrence)) {
        return infer_preprocessor_hint(occurrence);
    }
    if (is_io_symbol(occurrence)) {
        return infer_io_hint(occurrence);
    }
    if (is_query_symbol(occurrence)) {
        return infer_query_hint(occurrence);
    }
    return infer_section_fallback(occurrence);
}

bool should_upgrade(MappingStatus current, MappingStatus candidate) {
    const auto rank = [](MappingStatus value) {
        switch (value) {
            case MappingStatus::Unsupported:
                return 4;
            case MappingStatus::Mapped:
                return 3;
            case MappingStatus::Abstracted:
                return 2;
            case MappingStatus::Stubbed:
                return 1;
        }
        return 0;
    };

    return rank(candidate) > rank(current);
}

}  // namespace

std::string_view to_string(MappingStatus status) {
    switch (status) {
        case MappingStatus::Mapped:
            return "mapped";
        case MappingStatus::Abstracted:
            return "abstracted";
        case MappingStatus::Stubbed:
            return "stubbed";
        case MappingStatus::Unsupported:
            return "unsupported";
    }
    return "stubbed";
}

std::string_view to_string(SemanticFamily family) {
    switch (family) {
        case SemanticFamily::Unknown:
            return "unknown";
        case SemanticFamily::Io:
            return "io";
        case SemanticFamily::Query:
            return "query";
        case SemanticFamily::Storage:
            return "storage";
        case SemanticFamily::Preprocessor:
            return "preprocessor";
        case SemanticFamily::Language:
            return "language";
        case SemanticFamily::OmniBridge:
            return "omni_bridge";
        case SemanticFamily::SecuritySafe:
            return "security_safe";
        case SemanticFamily::SecurityBlocked:
            return "security_blocked";
    }
    return "unknown";
}

std::string normalize_symbol(std::string_view raw_symbol) {
    if (raw_symbol == "<~>") {
        return "contextual_match";
    }
    if (raw_symbol == "~~") {
        return "carry_forward";
    }
    if (raw_symbol == "-~>") {
        return "bounded_transform";
    }
    if (raw_symbol == "~~>") {
        return "carry_forward";
    }

    std::string normalized;
    bool previous_underscore = false;
    for (char c : raw_symbol) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            previous_underscore = false;
            continue;
        }

        if (!normalized.empty() && !previous_underscore) {
            normalized.push_back('_');
            previous_underscore = true;
        }
    }

    while (!normalized.empty() && normalized.back() == '_') {
        normalized.pop_back();
    }

    return normalized;
}

SymbolIndex build_symbol_index(const MappingUnit& unit) {
    SymbolIndex index;
    std::map<std::string, std::size_t> slot_by_symbol;

    auto ingest = [&](const std::string& section_title, const PseudoNode& node) {
        for (const std::string& raw_symbol : node.symbols) {
            SymbolOccurrence occurrence;
            occurrence.raw_symbol = raw_symbol;
            occurrence.normalized_symbol = normalize_symbol(raw_symbol);
            occurrence.section_title = section_title;
            occurrence.line = node.line;
            occurrence.node_kind = node.kind;
            occurrence.harmful = node.harmful;

            const auto hint_it = semantic_hints().find(occurrence.normalized_symbol);
            const SemanticHint hint = hint_it != semantic_hints().end() ? hint_it->second : infer_fallback(occurrence);

            const auto slot_it = slot_by_symbol.find(occurrence.normalized_symbol);
            if (slot_it == slot_by_symbol.end()) {
                const std::size_t slot = index.mappings.size();
                slot_by_symbol.emplace(occurrence.normalized_symbol, slot);

                SymbolMapping mapping;
                mapping.raw_symbol = raw_symbol;
                mapping.normalized_symbol = occurrence.normalized_symbol;
                mapping.inferred_meaning = hint.meaning;
                mapping.mapped_cpp_target = hint.cpp_target;
                mapping.status = hint.status;
                mapping.family = hint.family;
                mapping.occurrences.push_back(occurrence);
                index.mappings.push_back(std::move(mapping));
                continue;
            }

            SymbolMapping& mapping = index.mappings[slot_it->second];
            if (should_upgrade(mapping.status, hint.status) ||
                (mapping.family == SemanticFamily::Unknown && hint.family != SemanticFamily::Unknown)) {
                mapping.inferred_meaning = hint.meaning;
                mapping.mapped_cpp_target = hint.cpp_target;
                mapping.status = hint.status;
                mapping.family = hint.family;
            }
            mapping.occurrences.push_back(std::move(occurrence));
        }
    };

    for (const PseudoNode& node : unit.preamble) {
        ingest("Preamble", node);
    }
    for (const SectionNode& section : unit.sections) {
        for (const PseudoNode& node : section.nodes) {
            ingest(section.title, node);
        }
    }

    std::sort(index.mappings.begin(), index.mappings.end(), [](const SymbolMapping& lhs, const SymbolMapping& rhs) {
        if (lhs.occurrences.size() != rhs.occurrences.size()) {
            return lhs.occurrences.size() > rhs.occurrences.size();
        }
        return lhs.raw_symbol < rhs.raw_symbol;
    });

    return index;
}

const SymbolMapping* find_mapping(const SymbolIndex& index, std::string_view query) {
    const std::string normalized_query = normalize_symbol(query);
    const std::string lowered_query = lowercase(query);

    for (const SymbolMapping& mapping : index.mappings) {
        if (mapping.raw_symbol == query || mapping.normalized_symbol == normalized_query) {
            return &mapping;
        }

        if (lowercase(mapping.raw_symbol) == lowered_query) {
            return &mapping;
        }
    }

    return nullptr;
}

}  // namespace xpp
