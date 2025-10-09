#include "ExpressionUtils.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>

namespace ExpressionUtils {

std::string trim(const std::string& value) {
    auto begin = std::find_if_not(value.begin(), value.end(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    });
    auto end = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    }).base();

    if (begin >= end) {
        return {};
    }

    return std::string(begin, end);
}

std::string removeWhitespace(const std::string& value) {
    std::string cleaned;
    cleaned.reserve(value.size());
    for (unsigned char ch : value) {
        if (std::isspace(ch) == 0) {
            cleaned.push_back(static_cast<char>(ch));
        }
    }
    return cleaned;
}

std::string normalizeWhitespace(const std::string& value) {
    std::string result;
    result.reserve(value.size());

    bool inWhitespace = false;
    for (unsigned char ch : value) {
        if (std::isspace(ch) != 0) {
            if (!inWhitespace) {
                if (!result.empty()) {
                    result.push_back(' ');
                }
                inWhitespace = true;
            }
        } else {
            result.push_back(static_cast<char>(ch));
            inWhitespace = false;
        }
    }

    return result;
}

std::optional<double> evaluateBinaryExpression(const std::string& expression) {
    const std::string cleaned = removeWhitespace(expression);
    if (cleaned.empty()) {
        return std::nullopt;
    }

    std::size_t operatorPos = std::string::npos;
    char op = 0;
    for (std::size_t i = 0; i < cleaned.size(); ++i) {
        char current = cleaned[i];
        const bool isOperator = current == '+' || current == '-' || current == '*' || current == '/' || current == '%';
        if (!isOperator) {
            continue;
        }

        const bool isUnaryMinus = current == '-' && i == 0;
        if (isUnaryMinus) {
            continue;
        }

        operatorPos = i;
        op = current;
        break;
    }

    if (operatorPos == std::string::npos || operatorPos + 1 >= cleaned.size()) {
        return std::nullopt;
    }

    const std::string lhs = cleaned.substr(0, operatorPos);
    const std::string rhs = cleaned.substr(operatorPos + 1);

    if (lhs.empty() || rhs.empty()) {
        return std::nullopt;
    }

    try {
        if (op == '%') {
            long long left = std::stoll(lhs);
            long long right = std::stoll(rhs);
            if (right == 0) {
                return std::nullopt;
            }
            return static_cast<double>(left % right);
        }

        double left = std::stod(lhs);
        double right = std::stod(rhs);

        switch (op) {
            case '+':
                return left + right;
            case '-':
                return left - right;
            case '*':
                return left * right;
            case '/':
                if (areNearlyEqual(right, 0.0)) {
                    return std::nullopt;
                }
                return left / right;
            default:
                return std::nullopt;
        }
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

bool areNearlyEqual(double lhs, double rhs, double epsilon) {
    return std::fabs(lhs - rhs) <= epsilon;
}

} // namespace ExpressionUtils
