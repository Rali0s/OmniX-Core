#include <iostream>
#include <string>

#include "VersusComparator.h"

std::string expressionOne() {
    return "1 + 1";
}

std::string expressionTwo() {
    return "3 - 1";
}

int main() {
    VersusComparator comparator(expressionOne, expressionTwo);
    const bool areEqual = comparator.compare();

    std::cout << "The functions are " << (areEqual ? "equal." : "not equal.") << std::endl;

    return 0;
}
