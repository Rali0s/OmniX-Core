#pragma once

#include <string>
#include <string_view>

#include "tze/types.hpp"

namespace tze {

class SecurityManager {
public:
    SecurityAudit verify(const RequestProfile& profile) const;
    SecurityAudit simulate_symbol(std::string_view label,
                                  std::string_view semantic_family,
                                  const MemorySnapshot& memory,
                                  QuerySessionRecord* query_session = nullptr) const;
    static std::string abstract_operation(std::string_view label);
    static std::string detect_threat(std::string_view label);
    static std::string classify_threat(std::string_view label);
    static std::string isolate_target(std::string_view label);
    static std::string trace_scope(std::string_view label);
    static std::string log_event(std::string_view label);
};

}  // namespace tze
