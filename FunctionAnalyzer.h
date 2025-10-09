#pragma once

#include <string>

enum class OperationType {
    Mathematical,
    Other
};

class FunctionAnalyzer {
public:
    static OperationType identifyOperation(const std::string& expression);

private:
    static bool isMathematical(const std::string& expression);
};
