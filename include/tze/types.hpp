#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace tze {

struct CacheCell {
    std::string name;
    std::size_t size_bytes = 0;
    bool persistent = false;
    std::vector<std::string> operations;
};

struct KnowledgeReference {
    std::string source;
    std::string excerpt;
    int priority = 0;
};

struct SecurityAudit {
    bool admin_verified = false;
    std::vector<std::string> phases;
    std::vector<std::string> communications;
    std::vector<std::string> mitigations;
};

struct ProcessingReport {
    CacheCell cache;
    std::string decoded_instruction;
    std::vector<KnowledgeReference> references;
    std::vector<std::string> feedback_loop;
    SecurityAudit security;
    std::vector<std::string> storage_writes;
};

struct RequestProfile {
    std::string instruction_slot;
    std::string operator_handle;
    bool operator_is_admin = false;
    bool first_run = false;
    bool persist_on_success = true;
    std::size_t estimated_size = 0;
};

}  // namespace tze
