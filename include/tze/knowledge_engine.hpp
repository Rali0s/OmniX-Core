#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "tze/types.hpp"

namespace tze {

class KnowledgeEngine {
public:
    std::string decode_instruction(std::string_view slot) const;
    std::vector<KnowledgeReference> prioritize(std::string_view command) const;
    std::vector<std::string> replay_feedback(std::string_view command) const;
};

}  // namespace tze
