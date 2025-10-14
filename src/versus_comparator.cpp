#include "versus_comparator.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr double kEpsilon = 1e-9;

bool is_operator(char c) {
    return c == '+' || c == '-' || c == '*' || c == '/' || c == '%';
}
}

VersusComparator::VersusComparator(FunctionType lhs, FunctionType rhs)
    : lhs_(std::move(lhs)), rhs_(std::move(rhs)) {}

bool VersusComparator::compare() {
    const std::string lhs_expression = normalize_expression(lhs_());
    const std::string rhs_expression = normalize_expression(rhs_());

    const OperationType lhs_type = identify_operation(lhs_expression);
    const OperationType rhs_type = identify_operation(rhs_expression);

    if (lhs_type != rhs_type) {
        return false;
    }

    switch (lhs_type) {
        case OperationType::Mathematical:
            return evaluate_mathematical(lhs_expression, rhs_expression);
        default:
            return lhs_expression == rhs_expression;
    }
}

std::string VersusComparator::normalize_expression(const std::string& expression) {
    std::string normalized;
    normalized.reserve(expression.size());

    bool token_active = false;
    for (char c : expression) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (token_active) {
                normalized.push_back(' ');
                token_active = false;
            }
            continue;
        }

        if (is_operator(c) || c == '(' || c == ')') {
            if (!normalized.empty() && normalized.back() != ' ') {
                normalized.push_back(' ');
            }
            normalized.push_back(c);
            normalized.push_back(' ');
            token_active = false;
            continue;
        }

        normalized.push_back(c);
        token_active = true;
    }

    if (!normalized.empty() && normalized.back() == ' ') {
        normalized.pop_back();
    }

    return normalized;
}

OperationType VersusComparator::identify_operation(const std::string& expression) {
    return FunctionAnalyzer::identifyOperation(expression);
}

bool VersusComparator::evaluate_mathematical(const std::string& lhs, const std::string& rhs) {
    try {
        const double lhs_value = evaluate_math_expression(lhs);
        const double rhs_value = evaluate_math_expression(rhs);
        return std::fabs(lhs_value - rhs_value) < kEpsilon;
    } catch (const std::exception&) {
        return lhs == rhs;
    }
}

int VersusComparator::precedence(char op) {
    switch (op) {
        case '+':
        case '-':
            return 1;
        case '*':
        case '/':
        case '%':
            return 2;
        default:
            return 0;
    }
}

void VersusComparator::apply_operator(std::vector<double>& values, std::vector<char>& ops) {
    if (values.size() < 2 || ops.empty()) {
        throw std::runtime_error("Invalid expression");
    }

    const double rhs = values.back();
    values.pop_back();
    const double lhs = values.back();
    values.pop_back();

    const char op = ops.back();
    ops.pop_back();

    double result = 0.0;
    switch (op) {
        case '+':
            result = lhs + rhs;
            break;
        case '-':
            result = lhs - rhs;
            break;
        case '*':
            result = lhs * rhs;
            break;
        case '/':
            if (std::fabs(rhs) < kEpsilon) {
                throw std::runtime_error("Division by zero");
            }
            result = lhs / rhs;
            break;
        case '%': {
            if (std::fabs(rhs) < kEpsilon) {
                throw std::runtime_error("Modulo by zero");
            }
            const double truncated_lhs = std::floor(lhs);
            const double truncated_rhs = std::floor(rhs);
            result = std::fmod(truncated_lhs, truncated_rhs);
            break;
        }
        default:
            throw std::runtime_error("Unsupported operator");
    }

    values.push_back(result);
}

double VersusComparator::evaluate_math_expression(const std::string& expression) {
    std::vector<double> values;
    std::vector<char> ops;

    std::istringstream stream(expression);
    std::string token;

    while (stream >> token) {
        if (token.size() == 1 && is_operator(token[0])) {
            const char current_op = token[0];
            while (!ops.empty() && precedence(ops.back()) >= precedence(current_op)) {
                apply_operator(values, ops);
            }
            ops.push_back(current_op);
        } else if (token == "(") {
            ops.push_back('(');
        } else if (token == ")") {
            while (!ops.empty() && ops.back() != '(') {
                apply_operator(values, ops);
            }
            if (ops.empty() || ops.back() != '(') {
                throw std::runtime_error("Mismatched parentheses");
            }
            ops.pop_back();
        } else {
            size_t processed = 0;
            double value = std::stod(token, &processed);
            if (processed != token.size()) {
                throw std::runtime_error("Invalid numeric token");
            }
            values.push_back(value);
        }
    }

    while (!ops.empty()) {
        if (ops.back() == '(') {
            throw std::runtime_error("Mismatched parentheses");
        }
        apply_operator(values, ops);
    }

    if (values.size() != 1) {
        throw std::runtime_error("Invalid expression evaluation state");
    }

    return values.front();
}
