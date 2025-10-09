#include "FunctionAnalyzer.h"

#include <algorithm>
#include <cctype>

namespace {
bool containsMathOperator(const std::string& expression) {
    return expression.find('+') != std::string::npos ||
           expression.find('-') != std::string::npos ||
           expression.find('*') != std::string::npos ||
           expression.find('/') != std::string::npos ||
           expression.find('%') != std::string::npos;
}

bool hasAlphabeticCharacters(const std::string& expression) {
    return std::any_of(expression.begin(), expression.end(), [](unsigned char ch) {
        return std::isalpha(ch) != 0;
    });
}
} // namespace

OperationType FunctionAnalyzer::identifyOperation(const std::string& expression) {
    if (isMathematical(expression)) {
        return OperationType::Mathematical;
    }
    return OperationType::Other;
}

bool FunctionAnalyzer::isMathematical(const std::string& expression) {
    return containsMathOperator(expression) && !hasAlphabeticCharacters(expression);
}
