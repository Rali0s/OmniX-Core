#pragma once

#include <functional>
#include <string>

#include "FunctionAnalyzer.h"

class VersusComparator {
public:
    using FuncType = std::function<std::string()>;

    VersusComparator(FuncType left, FuncType right);

    bool compare() const;

private:
    static std::string getExpression(FuncType func);

    FuncType left_;
    FuncType right_;
};
