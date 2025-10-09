#pragma once

#include <functional>
#include <optional>
#include <string>

#include "FunctionAnalyzer.h"

struct FunctionProfile {
    std::string rawExpression;
    std::string normalizedExpression;
    OperationType type = OperationType::Other;
    std::optional<double> numericValue;

    static FunctionProfile analyze(const std::function<std::string()>& func);
};
