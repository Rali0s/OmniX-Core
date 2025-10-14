#include "tze/processing_engine.hpp"
#include "versus_comparator.hpp"

#include <cctype>
#include <iostream>
#include <string>
#include <vector>

namespace {

std::string trim(const std::string& value) {
    const std::string::size_type first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }

    const std::string::size_type last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string prompt_line(const std::string& prompt, const std::string& fallback = {}) {
    std::cout << prompt;
    std::string input;
    if (!std::getline(std::cin, input)) {
        return fallback;
    }

    const std::string trimmed = trim(input);
    if (trimmed.empty()) {
        return fallback;
    }

    return trimmed;
}

bool parse_affirmative(const std::string& response) {
    std::string normalized;
    normalized.reserve(response.size());

    for (char c : response) {
        if (!std::isspace(static_cast<unsigned char>(c))) {
            normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
    }

    return normalized == "y" || normalized == "yes" || normalized == "true" || normalized == "t";
}

void print_vector(const std::vector<std::string>& values, const std::string& label) {
    if (values.empty()) {
        return;
    }

    std::cout << label;
    for (const auto& entry : values) {
        std::cout << "  - " << entry << '\n';
    }
}

void print_references(const std::vector<tze::KnowledgeReference>& references) {
    if (references.empty()) {
        return;
    }

    std::cout << "Referenced knowledge:\n";
    for (const auto& reference : references) {
        std::cout << "  - [" << reference.priority << "] " << reference.source
                  << ": " << reference.excerpt << '\n';
    }
}

}  // namespace

int main() {
    const std::string base_expression =
        prompt_line("Enter the baseline expression to analyze (default: 1 + 1): ", "1 + 1");

    const std::string comparison_expression = prompt_line(
        "Enter the expression to compare against the baseline (default: 4 / 2): ", "4 / 2");

    VersusComparator comparator(
        [base_expression]() { return base_expression; },
        [comparison_expression]() { return comparison_expression; });

    const bool expressions_equal = comparator.compare();

    std::cout << '\n'
              << "The expressions are " << (expressions_equal ? "equivalent" : "different")
              << " after normalization and evaluation.\n";

    const bool run_processing_demo = parse_affirmative(
        prompt_line("Run the TZE processing demo for the baseline expression? (y/n): ", "y"));

    if (!run_processing_demo) {
        std::cout << "Skipping TZE processing demo based on your response.\n";
        return 0;
    }

    tze::ProcessingEngine engine;
    tze::RequestProfile profile;
    profile.instruction_slot = base_expression;
    profile.operator_handle = "cli-user";
    profile.operator_is_admin = true;
    profile.first_run = true;
    profile.persist_on_success = expressions_equal;
    profile.estimated_size = base_expression.size() + comparison_expression.size();

    const tze::ProcessingReport report = engine.process(profile);

    std::cout << '\n' << "--- TZE Processing Report ---\n";
    std::cout << "Decoded instruction: " << report.decoded_instruction << '\n';
    std::cout << "Cache cell: " << report.cache.name << " (" << report.cache.size_bytes
              << " bytes, persistent: " << (report.cache.persistent ? "yes" : "no") << ")\n";

    print_vector(report.cache.operations, "Cache operations:\n");
    print_references(report.references);
    print_vector(report.feedback_loop, "Feedback loop events:\n");
    std::cout << "Security admin verified: " << (report.security.admin_verified ? "yes" : "no")
              << '\n';
    print_vector(report.security.phases, "Security phases:\n");
    print_vector(report.security.communications, "Security communications:\n");
    print_vector(report.security.mitigations, "Security mitigations:\n");
    print_vector(report.storage_writes, "Storage writes:\n");

    return 0;
}
