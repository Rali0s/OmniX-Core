#pragma once

#include <string>
#include <vector>

#include "ComparisonResult.h"

struct AnalysisReport {
    std::string leftLabel;
    std::string rightLabel;
    ComparisonResult comparison;
    std::string conclusion;
    std::vector<std::string> highlights;

    std::string summary() const;
};
