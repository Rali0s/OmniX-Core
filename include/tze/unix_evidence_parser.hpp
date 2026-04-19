#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "tze/types.hpp"

namespace tze {

class UnixEvidenceParser {
public:
    std::vector<std::string> detect_signals(std::string_view text, std::string_view source_ref) const;
    std::vector<NormalizedObject> parse(const ObservationRecord& observation) const;
};

}  // namespace tze
