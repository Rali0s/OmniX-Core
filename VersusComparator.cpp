#include "VersusComparator.h"

#include <utility>

#include "ExpressionUtils.h"
#include "FunctionProfile.h"

VersusComparator::VersusComparator(FuncType left, FuncType right)
    : left_(std::move(left)), right_(std::move(right)) {}

bool VersusComparator::compare() const {
    return compareDetailed().isMatch();
}

ComparisonResult VersusComparator::compareDetailed() const {
    ComparisonResult result{};
    result.left = FunctionProfile::analyze(left_);
    result.right = FunctionProfile::analyze(right_);

    result.areTypesCompatible = result.left.type == result.right.type;
    result.areNormalizedEqual =
        result.left.normalizedExpression == result.right.normalizedExpression;
    result.areNumericallyComparable =
        result.left.numericValue.has_value() && result.right.numericValue.has_value();

    if (result.areNumericallyComparable) {
        result.areNumericallyEqual = ExpressionUtils::areNearlyEqual(
            *result.left.numericValue, *result.right.numericValue);
    }

    return result;
}
