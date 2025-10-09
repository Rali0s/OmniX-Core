#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace tze {

enum class OperandCategory {
    Cache,
    Query,
    Security,
    DataMap,
};

struct OperandDefinition {
    std::string name;
    OperandCategory category;
    std::string summary;
    std::string detail;
};

const std::vector<OperandDefinition>& operand_catalogue();

std::string_view to_string(OperandCategory category);

}  // namespace tze

