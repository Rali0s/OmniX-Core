#include "AgileWorkflow.h"

#include <utility>

AgileWorkflow::AgileWorkflow(std::string leftLabel,
                             VersusComparator::FuncType leftFunc,
                             std::string rightLabel,
                             VersusComparator::FuncType rightFunc)
    : leftLabel_(std::move(leftLabel)),
      rightLabel_(std::move(rightLabel)),
      comparator_(std::move(leftFunc), std::move(rightFunc)) {}

AnalysisReport AgileWorkflow::runComparison() const {
    const ComparisonResult comparison = comparator_.compareDetailed();

    AnalysisReport report{};
    report.leftLabel = leftLabel_;
    report.rightLabel = rightLabel_;
    report.comparison = comparison;
    report.conclusion = buildConclusion(comparison);
    report.highlights = buildHighlights(comparison);

    return report;
}

std::string AgileWorkflow::buildConclusion(const ComparisonResult& result) const {
    if (!result.areTypesCompatible) {
        return "Functions exhibit incompatible operation types.";
    }

    if (result.areNumericallyComparable) {
        return result.areNumericallyEqual ?
                   "Numeric evaluations align; treat as equivalent." :
                   "Numeric evaluations diverge; further investigation required.";
    }

    if (result.areNormalizedEqual) {
        return "Expressions match after normalization.";
    }

    return "Expressions diverge; investigate textual differences.";
}

std::vector<std::string> AgileWorkflow::buildHighlights(const ComparisonResult& result) const {
    std::vector<std::string> items;
    items.emplace_back("Left expression: " +
                       (result.left.normalizedExpression.empty() ? std::string{"<empty>"}
                                                                 : result.left.normalizedExpression));
    items.emplace_back("Right expression: " +
                       (result.right.normalizedExpression.empty() ? std::string{"<empty>"}
                                                                  : result.right.normalizedExpression));

    if (result.areNumericallyComparable) {
        const std::string leftValue = result.left.numericValue ? std::to_string(*result.left.numericValue)
                                                               : std::string{"n/a"};
        const std::string rightValue = result.right.numericValue ? std::to_string(*result.right.numericValue)
                                                                  : std::string{"n/a"};
        items.emplace_back("Numeric evaluation (left vs right): " + leftValue + " vs " + rightValue);
    } else {
        items.emplace_back("Numeric evaluation skipped; one or more expressions lack deterministic arithmetic form.");
    }

    items.emplace_back("Resolution basis: " + result.resolutionBasis());
    items.emplace_back(result.isMatch() ? "Decision: expressions considered equivalent." :
                                           "Decision: expressions considered distinct.");
    return items;
}
