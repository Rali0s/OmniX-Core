#include "tze/knowledge_engine.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace tze {
namespace {
using Dictionary = std::map<std::string, std::string>;

Dictionary build_instruction_dictionary() {
    return {
        {"aZ::1", "Build"},
        {"aZ::2", "Prioritize"},
        {"aZ::3", "SecurityAudit"},
        {"aZ::99", "Investigate"},
    };
}

const Dictionary& instruction_dictionary() {
    static const Dictionary dict = build_instruction_dictionary();
    return dict;
}

std::vector<KnowledgeReference> default_references(std::string_view command) {
    if (command == "Build") {
        return {
            {"Wikipedia", "Overview of CMake build systems and cache behaviours.", 1},
            {"Oxford", "Definition of \"build\" in the context of constructing artefacts.", 2},
            {"Webster", "Historical usage of the term with references to compilation.", 3},
        };
    }
    if (command == "Prioritize") {
        return {
            {"Wikipedia", "Concept of priority queues in computer science.", 1},
            {"PMBOK", "Guidance on ranking work streams in project management.", 2},
        };
    }
    if (command == "SecurityAudit") {
        return {
            {"NIST", "Checklist for privileged account verification.", 1},
            {"OWASP", "Guidelines for administrator workflow hardening.", 2},
        };
    }
    return {{"OmniX-Core", "No canonical references yet. Record future discoveries here.", 1}};
}

std::vector<std::string> default_feedback(std::string_view command) {
    if (command == "Build") {
        return {
            "Previous run compiled 142 targets in 95 seconds.",
            "Admin preference: always consult Wikipedia before Oxford.",
        };
    }
    if (command == "Prioritize") {
        return {"Admin set priority to Web search > Internal docs on last run."};
    }
    if (command == "SecurityAudit") {
        return {"Last audit flagged delayed MFA confirmation from operator al3x."};
    }
    return {"No historical feedback available."};
}
}  // namespace

std::string KnowledgeEngine::decode_instruction(std::string_view slot) const {
    const auto it = instruction_dictionary().find(std::string(slot));
    if (it != instruction_dictionary().end()) {
        return it->second;
    }
    return "Unknown";
}

std::vector<KnowledgeReference> KnowledgeEngine::prioritize(std::string_view command) const {
    auto references = default_references(command);
    std::stable_sort(references.begin(), references.end(), [](const KnowledgeReference& lhs, const KnowledgeReference& rhs) {
        return lhs.priority < rhs.priority;
    });
    return references;
}

std::vector<std::string> KnowledgeEngine::replay_feedback(std::string_view command) const {
    return default_feedback(command);
}

}  // namespace tze
