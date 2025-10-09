#include "FunctionProfile.h"

#include "ExpressionUtils.h"

FunctionProfile FunctionProfile::analyze(const std::function<std::string()>& func) {
    FunctionProfile profile{};

    if (!func) {
        return profile;
    }

    profile.rawExpression = func();
    profile.normalizedExpression = ExpressionUtils::normalizeWhitespace(profile.rawExpression);
    profile.type = FunctionAnalyzer::identifyOperation(profile.rawExpression);

    if (profile.type == OperationType::Mathematical) {
        if (const auto value = ExpressionUtils::evaluateBinaryExpression(profile.rawExpression)) {
            profile.numericValue = *value;
        }
    }

    return profile;
}
