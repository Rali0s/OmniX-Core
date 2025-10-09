#include "VersusComparator.h"

#include <utility>

#include "ExpressionUtils.h"

VersusComparator::VersusComparator(FuncType left, FuncType right)
    : left_(std::move(left)), right_(std::move(right)) {}

bool VersusComparator::compare() const {
    const std::string leftExpression = getExpression(left_);
    const std::string rightExpression = getExpression(right_);

    const OperationType leftType = FunctionAnalyzer::identifyOperation(leftExpression);
    const OperationType rightType = FunctionAnalyzer::identifyOperation(rightExpression);

    if (leftType != rightType) {
        return false;
    }

    if (leftType == OperationType::Mathematical) {
        const auto leftValue = ExpressionUtils::evaluateBinaryExpression(leftExpression);
        const auto rightValue = ExpressionUtils::evaluateBinaryExpression(rightExpression);

        if (leftValue && rightValue) {
            return ExpressionUtils::areNearlyEqual(*leftValue, *rightValue);
        }
    }

    return ExpressionUtils::normalizeWhitespace(leftExpression) ==
           ExpressionUtils::normalizeWhitespace(rightExpression);
}

std::string VersusComparator::getExpression(FuncType func) {
    return func ? func() : std::string{};
}
