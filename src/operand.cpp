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
            "Legacy name for Cache.PrepareWorkspace.",
            "Source-history alias for the runtime step that prepares cache context, budget, and working cells before request execution.",
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
            "Legacy name for Intent.DecodeInstruction.",
            "Source-history alias for decoding indexed pseudo instructions such as aZ::1 into concrete runtime verbs like Build.",
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
            "Legacy name for Knowledge.EvidenceRanking.",
            "Source-history alias for ranking references, source preferences, and module evidence for the active request.",
        },
        {
            "x.DisplayFeedBackLoop",
            OperandCategory::Query,
            "Legacy name for Memory.FeedbackReview.",
            "Source-history alias for reviewing cached history and learned outcomes before final execution.",
        },
        {
            "x.Store",
            OperandCategory::Query,
            "Legacy name for Memory.StoreArtifact.",
            "Source-history alias for persisting retained artifacts into Storage.Temporary or Storage.Permanent.",
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
            "Sends prompts such as PrioritizeNow and coordinates HumanReadable workflow stages translated from the legacy x.C_P map.",
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
            "Legacy name for SourceIntake.",
            "Historical phase label now represented by HumanReadable runtime names such as RecipeAuthoring.SourceIntake.",
        },
        {
            "x.C_P.2",
            OperandCategory::Security,
            "Legacy name for EvidenceRanking.",
            "Historical phase label now represented by HumanReadable runtime names such as RecipeAuthoring.EvidenceRanking.",
        },
        {
            "x.C_P.3",
            OperandCategory::Security,
            "Legacy name for RecipeDraft.",
            "Historical phase label now represented by HumanReadable runtime names such as RecipeAuthoring.RecipeDraft.",
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
            "Legacy name for Storage.Temporary.",
            "Used for ephemeral storage during active processing, including preference rankings and generated keys.",
        },
        {
            "xMap_Perm",
            OperandCategory::DataMap,
            "Legacy name for Storage.Permanent.",
            "Holds long-lived knowledge structures (AdminProcessingGate, Prioritys.SearchExtranet, etc.).",
        },
        {
            "xMap_Core",
            OperandCategory::DataMap,
            "Legacy name for Storage.Core.",
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
