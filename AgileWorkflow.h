#pragma once

#include <string>
#include <vector>

#include "AnalysisReport.h"
#include "VersusComparator.h"

class AgileWorkflow {
public:
    AgileWorkflow(std::string leftLabel,
                  VersusComparator::FuncType leftFunc,
                  std::string rightLabel,
                  VersusComparator::FuncType rightFunc);

    AnalysisReport runComparison() const;

private:
    std::string leftLabel_;
    std::string rightLabel_;
    VersusComparator comparator_;

    std::string buildConclusion(const ComparisonResult& result) const;
    std::vector<std::string> buildHighlights(const ComparisonResult& result) const;
};
