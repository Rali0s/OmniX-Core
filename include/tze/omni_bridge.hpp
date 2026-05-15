#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "tze/types.hpp"

namespace tze {

class OmniBridge {
public:
    static std::string active_ping_placeholder(std::string_view target);
    static std::string index_context(std::string_view label);
    static std::string map_context(std::string_view label);
    static std::string seek_records(std::string_view label);
    static std::string contextualize_cache(std::string_view label);
    static std::string host_link(std::string_view label);
    static std::string load_native_os_bound(std::string_view label);
    static std::string match_context(std::string_view label);
    static std::string permit_mode(std::string_view label);
    static std::string read_context(std::string_view label);
    static std::string request_operation(std::string_view label);
    static std::string detect_patterns(std::string_view label);
    static std::string map_records(std::string_view label);
    static std::string ccd_test_placeholder(std::string_view label);
    static std::string omnibase();
    static std::string x2_link(std::string_view label);
    static std::vector<std::string> running_process_list();
    static std::string pid_temp_location();
    static std::string binary_decompressor();
    static std::string compression_algo_classes();
    static std::string access_search(std::string_view label);
    static std::string access_compare(std::string_view lhs, std::string_view rhs);
    static LegacyBridgeReport recover_legacy_bridge(std::string_view label,
                                                    const MemorySnapshot& memory,
                                                    QuerySessionRecord* query_session = nullptr);
};

}  // namespace tze
