#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "tze/types.hpp"

namespace tze {

struct IntentResolution {
    RequestIntent intent = RequestIntent::Unknown;
    std::string normalized_prompt;
    std::string primary_target;
    std::string memory_view;
    double confidence = 0.0;
    std::vector<std::string> suggestions;
};

class IntentResolver {
public:
    IntentResolution resolve(std::string_view prompt) const;
};

}  // namespace tze
