// FunctionAnalyzer.cpp

#include "FunctionAnalyzer.h"

OperationType FunctionAnalyzer::identifyOperation(const std::string& expression) {
    if (isMathematical(expression)) {
        return OperationType::Mathematical;
    }
    return OperationType::Other;
}

bool FunctionAnalyzer::isMathematical(const std::string& expression) {
    // Check if the expression contains mathematical operators
    return expression.find('+') != std::string::npos ||
           expression.find('-') != std::string::npos ||
           expression.find('*') != std::string::npos ||
           expression.find('/') != std::string::npos ||
           expression.find('%') != std::string::npos;
}
