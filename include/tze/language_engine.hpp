#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "tze/types.hpp"

namespace tze {

class LanguageEngine {
public:
    static LanguageResolutionRecord resolve_context(std::string_view query,
                                                    std::string_view source_map_path,
                                                    std::string_view confirmation_mode,
                                                    const MemorySnapshot& memory,
                                                    QuerySessionRecord* query_session = nullptr);
    static std::string read_native_os();
    static std::string detect_native_language();
    static std::string detect_native_language_index_operating();
    static std::string determine_os_language();
    static std::string map_native_os_language();
    static std::string native_language_io(std::string_view label);
    static bool permit_unbound_parse();
    static std::string check_file_coherence(const std::filesystem::path& path);
    static std::string decompress_artifact(std::string_view label);
    static std::string postprocess_language_detection(std::string_view label);
    static std::string store_os_type(std::string_view os_type);
    static std::string translate_deep_language_parse(std::string_view label);
    static std::string trans_language_input(std::string_view input);
    static std::string encrypt_marker(std::string_view label);
};

}  // namespace tze
