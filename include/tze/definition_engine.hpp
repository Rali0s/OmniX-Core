#pragma once

#include <string_view>

#include "tze/types.hpp"

namespace tze {

class DefinitionEngine {
public:
    DefinitionAnswer lookup(std::string_view query,
                            std::string_view source_map_path,
                            const MemorySnapshot& memory) const;
};

}  // namespace tze
