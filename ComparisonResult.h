#pragma once

#include <string>

#include "FunctionProfile.h"

struct ComparisonResult {
    FunctionProfile left;
    FunctionProfile right;
    bool areTypesCompatible = false;
    bool areNormalizedEqual = false;
    bool areNumericallyComparable = false;
    bool areNumericallyEqual = false;

    bool isMatch() const;
    std::string resolutionBasis() const;
};
