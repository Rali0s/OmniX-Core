#pragma once

#include "FunctionAnalyzer.h"

#include <functional>
#include <string>
#include <utility>
#include <vector>

class VersusComparator {
public:
    using FunctionType = std::function<std::string()>;

    VersusComparator(FunctionType lhs, FunctionType rhs);

    bool compare();

private:
    static std::string normalize_expression(const std::string& expression);
    static OperationType identify_operation(const std::string& expression);
    static bool evaluate_mathematical(const std::string& lhs, const std::string& rhs);
    static double evaluate_math_expression(const std::string& expression);
    static int precedence(char op);
    static void apply_operator(std::vector<double>& values, std::vector<char>& ops);

    FunctionType lhs_;
    FunctionType rhs_;
};
