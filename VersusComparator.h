#pragma once

#include <functional>
#include <string>

#include "ComparisonResult.h"

class VersusComparator {
public:
    using FuncType = std::function<std::string()>;

    VersusComparator(FuncType left, FuncType right);

    bool compare() const;
    ComparisonResult compareDetailed() const;

private:
    FuncType left_;
    FuncType right_;
};
