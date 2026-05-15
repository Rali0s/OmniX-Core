#include "tze/omni_bridge.hpp"

#include <string>
#include <vector>

namespace tze {
namespace {

std::string prefix(std::string_view label, std::string_view value) {
    return std::string(label) + ":" + std::string(value);
}

std::string join_values(const std::vector<std::string>& values) {
    std::string joined;
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) {
            joined += ",";
        }
        joined += values[index];
    }
    return joined;
}

}  // namespace

std::string OmniBridge::active_ping_placeholder(std::string_view target) {
    return prefix("active-ping", target);
}

std::string OmniBridge::index_context(std::string_view label) {
    return prefix("index", label);
}

std::string OmniBridge::map_context(std::string_view label) {
    return prefix("map", label);
}

std::string OmniBridge::seek_records(std::string_view label) {
    return prefix("seek", label);
}

std::string OmniBridge::contextualize_cache(std::string_view label) {
    return prefix("context-cache", label);
}

std::string OmniBridge::host_link(std::string_view label) {
    return prefix("host", label);
}

std::string OmniBridge::load_native_os_bound(std::string_view label) {
    return prefix("load-native-os", label);
}

std::string OmniBridge::match_context(std::string_view label) {
    return prefix("match", label);
}

std::string OmniBridge::permit_mode(std::string_view label) {
    return prefix("permit", label);
}

std::string OmniBridge::read_context(std::string_view label) {
    return prefix("read", label);
}

std::string OmniBridge::request_operation(std::string_view label) {
    return prefix("request", label);
}

std::string OmniBridge::detect_patterns(std::string_view label) {
    return prefix("detect", label);
}

std::string OmniBridge::map_records(std::string_view label) {
    return prefix("map-records", label);
}

std::string OmniBridge::ccd_test_placeholder(std::string_view label) {
    return prefix("ccd-test", label);
}

std::string OmniBridge::omnibase() {
    return "xXOmniBase";
}

std::string OmniBridge::x2_link(std::string_view label) {
    return prefix("x2", label);
}

std::vector<std::string> OmniBridge::running_process_list() {
    return {"omnix", "cmake", "c++"};
}

std::string OmniBridge::pid_temp_location() {
    return "pid-temp-location";
}

std::string OmniBridge::binary_decompressor() {
    return "binary-decompressor";
}

std::string OmniBridge::compression_algo_classes() {
    return "native-compression-algorithms";
}

std::string OmniBridge::access_search(std::string_view label) {
    return prefix("search", label);
}

std::string OmniBridge::access_compare(std::string_view lhs, std::string_view rhs) {
    return std::string(lhs) + "<=>" + std::string(rhs);
}

LegacyBridgeReport OmniBridge::recover_legacy_bridge(std::string_view label,
                                                     const MemorySnapshot& memory,
                                                     QuerySessionRecord* query_session) {
    LegacyBridgeReport report;
    report.query = std::string(label);
    report.id = "legacy-bridge-" + std::to_string(std::hash<std::string>{}(report.query));
    report.status = "safe-research-bridge";
    report.bridge_mode = "bounded-correlation";
    report.summary =
        "Recovered the xXOmni legacy branch as a safe bridge/correlation path without remote orchestration.";
    report.bridge_steps = {
        "RunSilentRunDeep -> bounded research mode",
        "Map&Index(UserAdmin_Traffic)",
        "LoadNativeOSBound(xXOmniBase)",
        "RequestSecureKeyTunnel -> simulated",
        "LegacyEngine <-> UFS correlation",
        "ContextualizeCache(UFS / LegacyEngine)",
    };
    report.safe_actions = {
        permit_mode("RSRD"),
        map_context("UserAdmin_Traffic"),
        load_native_os_bound(omnibase()),
        seek_records("All_UFSDataMaps"),
        contextualize_cache("LegacyEngine"),
        detect_patterns("Legacy<->UFS"),
    };
    report.research_actions = {
        request_operation("SecureKeyTunnel"),
        host_link("LegacyCommLink"),
        read_context("LegacyCommData"),
        match_context("Legacy<->UFS"),
    };
    report.blocked_actions = {
        "autonomous remote orchestration",
        "hidden network pivoting",
        "unbounded tunnel establishment",
    };
    report.correlation_signals = {
        "LegacyCommLink",
        "LegacyCommData",
        "UnknownSystem<~>LegacyEngine",
        "All_UFSDataMaps",
    };
    report.reasoning_trace.push_back("mode=legacy-bridge-recovery");
    report.reasoning_trace.push_back("networking=simulated-only");
    report.reasoning_trace.push_back("ufs-correlation=enabled");

    if (!memory.legacy_sources.empty()) {
        report.reasoning_trace.push_back("legacy-source=" + memory.legacy_sources.back().source_label);
    }
    if (query_session != nullptr) {
        query_session->indexed_values.push_back("legacy-bridge:" + report.query);
        query_session->indexed_values.push_back("legacy-bridge-status:" + report.status);
        query_session->indexed_values.push_back("legacy-bridge-signals:" + join_values(report.correlation_signals));
    }
    return report;
}

}  // namespace tze
