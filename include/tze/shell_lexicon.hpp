#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "tze/types.hpp"

namespace tze {

class ShellLexicon {
public:
    std::optional<ShellLexiconEntry> normalize(std::string_view input,
                                               const MemorySnapshot& memory,
                                               bool shell_mode) const;
};

}  // namespace tze
