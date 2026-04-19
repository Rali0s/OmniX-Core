#include "tze/security_manager.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>

#include "tze/query_runtime.hpp"

namespace tze {
namespace {

std::string prefix(std::string_view label, std::string_view value) {
    return std::string(label) + ":" + std::string(value);
}

std::string lowercase(std::string_view value) {
    std::string lowered;
    lowered.reserve(value.size());
    for (char c : value) {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return lowered;
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

bool contains_token(const std::vector<std::string>& tokens, std::string_view needle) {
    return std::find(tokens.begin(), tokens.end(), needle) != tokens.end();
}

std::string now_timestamp() {
    std::time_t now = std::time(nullptr);
    std::tm local_time{};
#if defined(_WIN32)
    localtime_s(&local_time, &now);
#else
    localtime_r(&now, &local_time);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &local_time);
    return buffer;
}

std::string env_or(std::string_view name, std::string_view fallback = {}) {
    const char* value = std::getenv(std::string(name).c_str());
    if (value == nullptr || *value == '\0') {
        return std::string(fallback);
    }
    return value;
}

void push_unique(std::vector<std::string>& values, std::string value) {
    if (value.empty()) {
        return;
    }
    if (std::find(values.begin(), values.end(), value) == values.end()) {
        values.push_back(std::move(value));
    }
}

void merge_unique(std::vector<std::string>& into, const std::vector<std::string>& from) {
    for (const std::string& value : from) {
        push_unique(into, value);
    }
}

void append_trace(SecurityAudit& audit, std::string value) {
    if (!value.empty()) {
        audit.reasoning_trace.push_back(std::move(value));
    }
}

std::string threat_bracket_for(const std::vector<std::string>& tokens) {
    if (contains_token(tokens, "ddos") || contains_token(tokens, "flood") || contains_token(tokens, "intrusion")) {
        return "tier2";
    }
    if (contains_token(tokens, "gateway") || contains_token(tokens, "router")) {
        return "tier1";
    }
    return "tier1";
}

std::string threat_label_for(const std::vector<std::string>& tokens) {
    if (contains_token(tokens, "detection") || contains_token(tokens, "detect")) {
        return "defensive-detection";
    }
    if (contains_token(tokens, "classify") || contains_token(tokens, "identify")) {
        return "defensive-classification";
    }
    if (contains_token(tokens, "isolate")) {
        return "gateway-isolation";
    }
    if (contains_token(tokens, "trace") || contains_token(tokens, "scope")) {
        return "scope-trace";
    }
    return "defensive-security";
}

}  // namespace

SecurityAudit SecurityManager::verify(const RequestProfile& profile) const {
    SecurityAudit audit;
    audit.id = "security-" + std::to_string(std::hash<std::string>{}(
        profile.operator_handle + "|" + profile.raw_prompt + "|" + profile.project_reference));
    audit.query = profile.raw_prompt.empty() ? profile.project_reference : profile.raw_prompt;
    audit.status = "admin-gate";
    audit.behavior_mode = "abstracted";
    audit.phases.push_back("x.Security(" + profile.operator_handle + ")");
    audit.phases.push_back("x.C_P.1()");
    audit.phases.push_back("x.C_P.2()");
    audit.phases.push_back("x.C_P.3()");
    audit.trace_paths.push_back("x.TraceBack(Index&Log)");
    audit.evidence.push_back("operator=" + profile.operator_handle);
    append_trace(audit, "admin-check");

    if (!profile.operator_is_admin) {
        audit.admin_verified = false;
        audit.status = "blocked";
        audit.behavior_mode = "blocked";
        audit.threat_label = "privilege-gate";
        audit.threat_bracket = "tier1";
        audit.mitigations.push_back("xX_Kill.All() -> non-admin access attempt");
        audit.mitigations.push_back("x.lockOut(" + profile.operator_handle + ")");
        append_trace(audit, "blocked=non-admin");
        audit.persisted_at = now_timestamp();
        return audit;
    }

    audit.admin_verified = true;
    audit.communications.push_back("x.Comms(PrioritizeNow)");
    audit.communications.push_back("x.DisplayFeedBackLoop(Admin Confirmed)");
    audit.mitigations.push_back("retain defensive-only execution");
    append_trace(audit, "admin-verified");
    audit.persisted_at = now_timestamp();
    return audit;
}

SecurityAudit SecurityManager::simulate_symbol(std::string_view label,
                                               std::string_view semantic_family,
                                               const MemorySnapshot& memory,
                                               QuerySessionRecord* query_session) const {
    SecurityAudit audit;
    audit.query = std::string(label);
    audit.id = "security-" + std::to_string(std::hash<std::string>{}(
        audit.query + "|" + std::string(semantic_family)));
    audit.persisted_at = now_timestamp();

    const std::vector<std::string> tokens = tokenize(label);
    audit.threat_label = threat_label_for(tokens);
    audit.threat_bracket = threat_bracket_for(tokens);
    audit.phases = {"xXOmni::Detection()", "xXOmni::ScopeDefense()", "measureAgainst::threatBrackets()"};
    audit.trace_paths = {"x.TraceBack(Index&Log)", "xMap_Perm_Thrice_ThreatLog_Epoch.3x"};
    audit.evidence = {
        "symbol=" + std::string(label),
        "semantic-family=" + std::string(semantic_family),
        "threat-bracket=" + audit.threat_bracket,
    };

    if (semantic_family == "security_blocked") {
        audit.status = "blocked";
        audit.behavior_mode = "blocked";
        audit.blocked_paths = {
            "x.Penetration",
            "x.implementExploits",
            "x.obtainLock",
            "x.reverseEngineer",
            "xXXOmni::Inject&Extirpate",
        };
        audit.mitigations = {
            "retain inert generated output",
            "report blocked branch in audit log",
            "prefer scope/detection simulation only",
        };
        append_trace(audit, "mode=blocked");
    } else {
        audit.status = "simulated";
        audit.behavior_mode = "simulated";
        audit.simulated_actions = {
            detect_threat(label),
            classify_threat(audit.threat_bracket),
            trace_scope(label),
            log_event(audit.threat_label),
        };
        if (contains_token(tokens, "isolate") || contains_token(tokens, "gateway") || contains_token(tokens, "intrusion")) {
            audit.simulated_actions.push_back(isolate_target("intrusionGateway"));
        }
        audit.mitigations = {
            "scope defense only",
            "simulate containment",
            "no exploit or persistence branch execution",
        };
        append_trace(audit, "mode=simulated");
    }

    for (auto it = memory.security_audits.rbegin(); it != memory.security_audits.rend(); ++it) {
        if (it->threat_label == audit.threat_label || it->query == audit.query) {
            push_unique(audit.evidence, "memory-hit=" + it->id);
            append_trace(audit, "memory-hit=" + it->id);
            break;
        }
    }

    if (query_session != nullptr) {
        QueryRuntime runtime;
        runtime.index_values(*query_session,
                             "security-evidence",
                             {audit.query,
                              audit.threat_label,
                              audit.threat_bracket,
                              audit.status,
                              audit.behavior_mode});
    }

    return audit;
}

std::string SecurityManager::abstract_operation(std::string_view label) {
    return prefix("security-abstract", label);
}

std::string SecurityManager::detect_threat(std::string_view label) {
    return prefix("security-detect", label);
}

std::string SecurityManager::classify_threat(std::string_view label) {
    return prefix("security-classify", label);
}

std::string SecurityManager::isolate_target(std::string_view label) {
    return prefix("security-isolate", label);
}

std::string SecurityManager::trace_scope(std::string_view label) {
    return prefix("security-trace", label);
}

std::string SecurityManager::log_event(std::string_view label) {
    return prefix("security-log", label);
}

}  // namespace tze
