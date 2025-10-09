#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace tze {

/// High-level grouping for pseudo-language operands discovered in `tze.cpp`.
enum class OperandCategory {
    Cache,
    Query,
    Security,
    DataMap,
};

/// Human readable description of a single operand.
struct OperandDefinition {
    std::string name;              ///< Raw identifier as it appears in the pseudo code.
    OperandCategory category;      ///< Functional area the operand belongs to.
    std::string summary;           ///< One-line summary of what the operand does.
    std::string detailed_context;  ///< Longer explanation with observed usage notes.
};

/// Returns the seed catalogue that was reverse-engineered from the pseudo code.
///
/// The catalogue is intentionally small. It focuses on the operations that the
/// early "Build Cmake" workflow depends on so that we can map them to real
/// library entry points in subsequent refactors.
const std::vector<OperandDefinition>& operand_catalogue();

/// Utility that turns an OperandCategory into a friendly string.
std::string_view to_string(OperandCategory category);

}  // namespace tze
