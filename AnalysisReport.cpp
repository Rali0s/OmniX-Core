#include "AnalysisReport.h"

#include <iomanip>
#include <sstream>

std::string AnalysisReport::summary() const {
    std::ostringstream oss;
    oss << "=== OmniX Agile Comparison Report ===\n";
    oss << "Left  (" << leftLabel << "): "
        << (comparison.left.normalizedExpression.empty() ? "<empty>"
                                                          : comparison.left.normalizedExpression)
        << "\n";
    oss << "Right (" << rightLabel << "): "
        << (comparison.right.normalizedExpression.empty() ? "<empty>"
                                                           : comparison.right.normalizedExpression)
        << "\n\n";

    oss << std::boolalpha;
    oss << "Type compatible: " << comparison.areTypesCompatible << "\n";
    oss << "Textual parity: " << comparison.areNormalizedEqual << "\n";
    oss << "Numeric comparable: " << comparison.areNumericallyComparable << "\n";
    if (comparison.areNumericallyComparable) {
        oss << "Numeric parity: " << comparison.areNumericallyEqual << "\n";
    }
    oss << "Resolution basis: " << comparison.resolutionBasis() << "\n\n";

    if (!highlights.empty()) {
        oss << "Highlights:" << "\n";
        for (const std::string& line : highlights) {
            oss << " - " << line << "\n";
        }
        oss << "\n";
    }

    oss << "Conclusion: " << conclusion << "\n";
    return oss.str();
}
