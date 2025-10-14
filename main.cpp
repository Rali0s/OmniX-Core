#include "versus_comparator.hpp"

#include <iostream>
#include <string>

namespace {

std::string build_simple_sum() {
    return "1 + 1";
}

std::string build_equivalent_expression() {
    return "4 / 2";
}

std::string build_different_expression() {
    return "2 + 3";
}

}  // namespace

int main() {
    VersusComparator equivalent_case(build_simple_sum, build_equivalent_expression);
    const bool equivalent = equivalent_case.compare();

    VersusComparator different_case(build_simple_sum, build_different_expression);
    const bool different = different_case.compare();

    std::cout << "Equivalent expressions compare as "
              << (equivalent ? "equal" : "different") << "\n";
    std::cout << "Different expressions compare as "
              << (different ? "equal" : "different") << "\n";

    return 0;
}
