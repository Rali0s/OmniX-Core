#include "tze/operand.hpp"

#include <array>

namespace tze {

namespace {

constexpr std::string_view kCacheSummary =
    "Cache coordination primitives used at the start of the Build CMake workflow.";
constexpr std::string_view kQuerySummary =
    "Knowledge lookup and preference management helpers.";
constexpr std::string_view kSecuritySummary =
    "Administrative guard rails that gate sensitive flows.";
constexpr std::string_view kDataMapSummary =
    "Logical storage namespaces referenced by the pseudo code.";

}  // namespace

const std::vector<OperandDefinition>& operand_catalogue() {
    static const std::vector<OperandDefinition> kCatalogue = {
        {
            "xProcessingCache",
            OperandCategory::Cache,
            "Prepare cache context for a new request.",
            "Entry point that sizes and instantiates temporary working cells before any other operand runs.",
        },
        {
            "xize",
            OperandCategory::Cache,
            "Estimate how much storage a request needs.",
            "Called immediately after xProcessingCache to derive the amount of space required for the temporary map.",
        },
        {
            "xCell_Create",
            OperandCategory::Cache,
            "Provision a cache cell on disk or in memory.",
            "Handles both the first-run bootstrap (5 GB temp cell) and subsequent runs that reuse persistent maps.",
        },
        {
            "xProcessingDefine",
            OperandCategory::Cache,
            "Configure retention rules for the active cache cell.",
            "Declares behaviours such as seek_Unbound and destruction policies once success/failure is known.",
        },
        {
            "x.Destroy",
            OperandCategory::Cache,
            "Tear down cache data after completion.",
            "Accepts modes like PostSuccess/PostFail so that high-value artefacts can survive failures when needed.",
        },
        {
            "x.Define.Low",
            OperandCategory::Query,
            "Resolve a symbolic instruction slot (aZ::n).",
            "Translates indexed pseudo instructions (e.g., aZ::1) into concrete verbs like Build before downstream lookups.",
        },
        {
            "x3m",
            OperandCategory::Query,
            "Invoke an external knowledge source.",
            "Acts as a bridge to search engines or encyclopaedias (Google, Wikipedia) for contextual definitions.",
        },
        {
            "x.DisplayPriorityProcessingGate",
            OperandCategory::Query,
            "Present ranked references and capture preferences.",
            "Displays sources such as Wikipedia/Oxford/Webster and records administrator ranking for future runs.",
        },
        {
            "x.DisplayFeedBackLoop",
            OperandCategory::Query,
            "Replay past answers for similar questions.",
            "Reads cached history (lMC::Cache) to improve recall efficiency and detect anomalies in stored knowledge.",
        },
        {
            "x.Store",
            OperandCategory::Query,
            "Persist structured data into a map.",
            "Writes values into xMap_Temp or xMap_Perm with rich key paths (aZ_Int, aZ_Prime, epoch metadata).",
        },
        {
            "x.Return",
            OperandCategory::Query,
            "Return computed values to the caller.",
            "Wraps results from sizing and search routines so that subsequent operands can consume them.",
        },
        {
            "x.Comms",
            OperandCategory::Security,
            "Request administrator involvement.",
            "Sends prompts such as PrioritizeNow and coordinates the three-phase communication pipeline (x.C_P.*).",
        },
        {
            "x.Security",
            OperandCategory::Security,
            "Validate elevated access.",
            "Checks whether the operator is an admin and triggers kill-switch behaviour when authentication fails.",
        },
        {
            "x.C_P.1",
            OperandCategory::Security,
            "Administrative workflow phase 1.",
            "Initial step in the admin communication pipeline; pairs with x.C_P.2 and x.C_P.3 for full processing.",
        },
        {
            "x.C_P.2",
            OperandCategory::Security,
            "Administrative workflow phase 2.",
            "Handles the preference query/record loop before feedback analysis runs.",
        },
        {
            "x.C_P.3",
            OperandCategory::Security,
            "Administrative workflow phase 3.",
            "Executes the feedback loop analysis and memory checks once preferences are stored.",
        },
        {
            "xX_Kill.All",
            OperandCategory::Security,
            "Emergency cache and session purge.",
            "Invoked when admin authentication fails repeatedly; clears temporary state and logs investigative data.",
        },
        {
            "x.superAdmin",
            OperandCategory::Security,
            "Escalate into lockdown mode.",
            "Activated when attribution ranking scores exceed thresholds, leading to SSH prompts and log destruction options.",
        },
        {
            "x.lockOut",
            OperandCategory::Security,
            "Engage lockout procedures.",
            "Companion to x.superAdmin that ensures further actions require out-of-band administrator confirmation.",
        },
        {
            "xMap_Temp",
            OperandCategory::DataMap,
            "Temporary working map namespace.",
            "Used for ephemeral storage during active processing, including preference rankings and generated keys.",
        },
        {
            "xMap_Perm",
            OperandCategory::DataMap,
            "Persistent canonical map namespace.",
            "Holds long-lived knowledge structures (AdminProcessingGate, Prioritys.SearchExtranet, etc.).",
        },
        {
            "xMap_Core",
            OperandCategory::DataMap,
            "Core system datastore.",
            "Accessed only in elevated contexts such as super-admin investigations and deep threat sweeps.",
        },
    };
    return kCatalogue;
}

std::string_view to_string(OperandCategory category) {
    switch (category) {
        case OperandCategory::Cache:
            return kCacheSummary;
        case OperandCategory::Query:
            return kQuerySummary;
        case OperandCategory::Security:
            return kSecuritySummary;
        case OperandCategory::DataMap:
            return kDataMapSummary;
    }
    return "";
}

}  // namespace tze
