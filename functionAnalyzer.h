// FunctionAnalyzer.h

#include <string>
#include <functional>

enum class OperationType {
    Mathematical,
    Other // Add more as needed
};

class FunctionAnalyzer {
public:
    static OperationType identifyOperation(const std::string& expression);

private:
    static bool isMathematical(const std::string& expression);
};
