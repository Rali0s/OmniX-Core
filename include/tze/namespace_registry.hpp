#pragma once

#include <string>
#include <string_view>

namespace tze {

class NamespaceRegistry {
public:
    static std::string root_symbol();
    static std::string base_namespace();
    static std::string resolve_namespace(std::string_view name);
    static std::string temporary_namespace(std::string_view name);
    static std::string persistent_namespace(std::string_view name);
    static std::string core_namespace(std::string_view name);
    static std::string map_value(std::string_view name, std::string_view value);
    static std::string store_value(std::string_view name);
};

}  // namespace tze
