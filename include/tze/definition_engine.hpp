#pragma once

#include <string_view>

#include "tze/types.hpp"

namespace tze {

class DefinitionEngine {
public:
    DefinitionAnswer lookup(std::string_view query,
                            std::string_view source_map_path,
                            const MemorySnapshot& memory,
                            std::string_view domain_hint = {},
                            std::string_view comparison_rationale = {},
                            bool prefer_general_definition = false) const;
};

}  // namespace tze
