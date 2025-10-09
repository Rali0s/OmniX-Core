#include "ComparisonResult.h"

bool ComparisonResult::isMatch() const {
    if (!areTypesCompatible) {
        return false;
    }

    if (areNumericallyComparable) {
        return areNumericallyEqual;
    }

    return areNormalizedEqual;
}

std::string ComparisonResult::resolutionBasis() const {
    if (!areTypesCompatible) {
        return "type mismatch";
    }

    if (areNumericallyComparable) {
        return areNumericallyEqual ? "numeric parity" : "numeric divergence";
    }

    return areNormalizedEqual ? "textual parity" : "textual divergence";
}
