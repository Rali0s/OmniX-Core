#pragma once

#include <string>
#include <string_view>

#include "tze/types.hpp"

namespace tze {

class PreprocessorRuntime {
public:
    static UacStateRecord resolve_uac_state(std::string_view query,
                                            const MemorySnapshot& memory,
                                            QuerySessionRecord* query_session = nullptr);
    static std::string genx_token(std::string_view label = "GENx");
    static std::string genx_engine(std::string_view label);
    static std::string binary_preprocessor(std::string_view label);
    static std::string regenerate_token(std::string_view label);
    static std::string encode_value(std::string_view label);
    static std::string encrypt_value(std::string_view label);
    static std::string compression_profile(std::string_view label);
    static std::string key_store_address(std::string_view label);
    static std::string key_budget(std::string_view label);
};

}  // namespace tze
