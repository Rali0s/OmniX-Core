#include "tze/types.hpp"

namespace tze {

std::string_view to_string(RequestIntent intent) {
    switch (intent) {
        case RequestIntent::Unknown:
            return "unknown";
        case RequestIntent::IngestData:
            return "ingest_data";
        case RequestIntent::AnalyzeCase:
            return "analyze_case";
        case RequestIntent::DecideAction:
            return "decide_action";
        case RequestIntent::InspectCase:
            return "inspect_case";
        case RequestIntent::CaseTimeline:
            return "case_timeline";
        case RequestIntent::ReplayTzeRun:
            return "replay_tze_run";
        case RequestIntent::ChainTzeRun:
            return "chain_tze_run";
        case RequestIntent::DiffTzeRuns:
            return "diff_tze_runs";
        case RequestIntent::ExplainTzeChange:
            return "explain_tze_change";
        case RequestIntent::ReportTzeRun:
            return "report_tze_run";
        case RequestIntent::DiffReportTzeRuns:
            return "diff_report_tze_runs";
        case RequestIntent::ExportTzeBundle:
            return "export_tze_bundle";
        case RequestIntent::ImportTzeBundle:
            return "import_tze_bundle";
        case RequestIntent::PruneTzeRuns:
            return "prune_tze_runs";
        case RequestIntent::PruneMemory:
            return "prune_memory";
        case RequestIntent::MarkTzeRunOutcome:
            return "mark_tze_run_outcome";
        case RequestIntent::MarkDecisionFeedback:
            return "mark_decision_feedback";
        case RequestIntent::MarkDecisionOutcome:
            return "mark_decision_outcome";
        case RequestIntent::ExportCaseBundle:
            return "export_case_bundle";
        case RequestIntent::ImportCaseBundle:
            return "import_case_bundle";
        case RequestIntent::ListIncidents:
            return "list_incidents";
        case RequestIntent::InspectIncident:
            return "inspect_incident";
        case RequestIntent::ReportIncident:
            return "report_incident";
        case RequestIntent::BuildProject:
            return "build_project";
        case RequestIntent::DoctorProject:
            return "doctor_project";
        case RequestIntent::ToolAction:
            return "tool_action";
        case RequestIntent::ProbeProvider:
            return "probe_provider";
        case RequestIntent::DefineSymbol:
            return "define_symbol";
        case RequestIntent::ExplainCommand:
            return "explain_command";
        case RequestIntent::InspectToolchain:
            return "inspect_toolchain";
        case RequestIntent::ShowMemory:
            return "show_memory";
    }
    return "unknown";
}

std::string_view to_string(AcquisitionPolicy policy) {
    switch (policy) {
        case AcquisitionPolicy::PreferLocal:
            return "prefer_local";
        case AcquisitionPolicy::FetchIfMissing:
            return "fetch_if_missing";
        case AcquisitionPolicy::LocalOnly:
            return "local_only";
    }
    return "fetch_if_missing";
}

std::string_view to_string(ToolCommandMode mode) {
    switch (mode) {
        case ToolCommandMode::None:
            return "none";
        case ToolCommandMode::List:
            return "list";
        case ToolCommandMode::Locate:
            return "locate";
        case ToolCommandMode::Doctor:
            return "doctor";
        case ToolCommandMode::Run:
            return "run";
    }
    return "none";
}

}  // namespace tze
