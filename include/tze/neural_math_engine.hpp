#pragma once

#include "tze/types.hpp"

#include <string_view>

namespace tze {

class NeuralMathEngine {
public:
    NeuralMathReport run_perceptron(std::string_view dataset,
                                    std::size_t epochs,
                                    double learning_rate) const;
};

}  // namespace tze
