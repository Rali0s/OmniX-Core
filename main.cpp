#include <iostream>
#include <string>

#include "AgileWorkflow.h"

std::string expressionOne() {
    return "1 + 1";
}

std::string expressionTwo() {
    return "3 - 1";
}

int main() {
    AgileWorkflow workflow("Expression One", expressionOne, "Expression Two", expressionTwo);
    const AnalysisReport report = workflow.runComparison();

    std::cout << report.summary() << std::endl;

    return 0;
}
