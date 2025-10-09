#pragma once

#include <optional>
#include <string>

namespace ExpressionUtils {

std::string trim(const std::string& value);
std::string removeWhitespace(const std::string& value);
std::optional<double> evaluateBinaryExpression(const std::string& expression);
std::string normalizeWhitespace(const std::string& value);
bool areNearlyEqual(double lhs, double rhs, double epsilon = 1e-9);

} // namespace ExpressionUtils
