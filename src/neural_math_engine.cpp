#include "tze/neural_math_engine.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace tze {
namespace {

struct TrainingSample {
    std::vector<double> inputs;
    int expected = 0;
};

std::string lowercase(std::string_view value) {
    std::string lowered;
    lowered.reserve(value.size());
    for (char c : value) {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return lowered;
}

std::vector<TrainingSample> truth_table(std::string_view dataset) {
    const std::string key = lowercase(dataset);
    if (key == "or") {
        return {{{0.0, 0.0}, 0}, {{0.0, 1.0}, 1}, {{1.0, 0.0}, 1}, {{1.0, 1.0}, 1}};
    }
    if (key == "and") {
        return {{{0.0, 0.0}, 0}, {{0.0, 1.0}, 0}, {{1.0, 0.0}, 0}, {{1.0, 1.0}, 1}};
    }
    if (key == "xor") {
        return {{{0.0, 0.0}, 0}, {{0.0, 1.0}, 1}, {{1.0, 0.0}, 1}, {{1.0, 1.0}, 0}};
    }
    return {};
}

int activate(double value) {
    return value > 0.0 ? 1 : 0;
}

std::string format_double(double value) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(3) << value;
    return out.str();
}

std::string format_vector(const std::vector<double>& values) {
    std::ostringstream out;
    out << "[";
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) {
            out << ", ";
        }
        out << format_double(values[index]);
    }
    out << "]";
    return out.str();
}

double weighted_sum(const std::vector<double>& weights, const std::vector<double>& inputs, double bias) {
    double sum = bias;
    for (std::size_t index = 0; index < std::min(weights.size(), inputs.size()); ++index) {
        sum += weights[index] * inputs[index];
    }
    return sum;
}

}  // namespace

NeuralMathReport NeuralMathEngine::run_perceptron(std::string_view dataset,
                                                  std::size_t epochs,
                                                  double learning_rate) const {
    NeuralMathReport report;
    report.model_type = "single_layer_perceptron";
    report.dataset = lowercase(dataset);
    report.epochs_requested = epochs;
    report.learning_rate = learning_rate;

    const std::vector<TrainingSample> samples = truth_table(report.dataset);
    if (samples.empty()) {
        report.status = "neural_math_invalid_dataset";
        report.summary = "Supported perceptron datasets are `or`, `and`, and `xor`.";
        report.warnings.push_back("Unknown dataset: " + std::string(dataset));
        return report;
    }

    report.weights = {0.0, 0.0};
    report.bias = 0.0;

    if (report.dataset == "xor") {
        report.status = "not_linearly_separable";
        report.summary =
            "XOR is not linearly separable for a single-layer perceptron; it requires a hidden layer / MLP.";
        report.math_trace.push_back("A single perceptron draws one linear decision boundary.");
        report.math_trace.push_back("XOR requires separating opposite corners of the truth table, so one line is insufficient.");
        for (const TrainingSample& sample : samples) {
            report.predictions.push_back({sample.inputs, sample.expected, 0});
        }
        report.warnings.push_back("Simulation stops before fake convergence; this is a known math limitation, not a runtime error.");
        return report;
    }

    const std::size_t bounded_epochs = std::max<std::size_t>(1, epochs);
    for (std::size_t epoch = 1; epoch <= bounded_epochs; ++epoch) {
        int errors = 0;
        for (const TrainingSample& sample : samples) {
            const int predicted = activate(weighted_sum(report.weights, sample.inputs, report.bias));
            const int error = sample.expected - predicted;
            if (error != 0) {
                ++errors;
            }
            for (std::size_t weight_index = 0; weight_index < report.weights.size(); ++weight_index) {
                report.weights[weight_index] += learning_rate * static_cast<double>(error) * sample.inputs[weight_index];
            }
            report.bias += learning_rate * static_cast<double>(error);
        }
        report.epochs_ran = epoch;
        if (epoch <= 4 || errors == 0) {
            report.math_trace.push_back("epoch=" + std::to_string(epoch) + " errors=" + std::to_string(errors) +
                                        " weights=" + format_vector(report.weights) +
                                        " bias=" + format_double(report.bias));
        }
        if (errors == 0) {
            break;
        }
    }

    int correct = 0;
    for (const TrainingSample& sample : samples) {
        const int predicted = activate(weighted_sum(report.weights, sample.inputs, report.bias));
        if (predicted == sample.expected) {
            ++correct;
        }
        report.predictions.push_back({sample.inputs, sample.expected, predicted});
    }
    report.accuracy = samples.empty() ? 0.0 : static_cast<double>(correct) / static_cast<double>(samples.size());
    report.status = report.accuracy >= 1.0 ? "neural_math_complete" : "neural_math_incomplete";
    report.summary = "Trained " + report.model_type + " for `" + report.dataset + "` in " +
        std::to_string(report.epochs_ran) + " epoch(s); accuracy=" + format_double(report.accuracy) +
        ", weights=" + format_vector(report.weights) + ", bias=" + format_double(report.bias) + ".";
    if (report.accuracy < 1.0) {
        report.warnings.push_back("Increase --epochs or adjust --learning-rate to continue simulation.");
    }
    return report;
}

}  // namespace tze
