#pragma once

#include "tze/types.hpp"

#include <string_view>

namespace tze {

class NeuralSignalRouter {
public:
    NeuralRouteReport route_tview_jsonl(std::string_view path) const;
    bool export_jsonl(const NeuralRouteReport& report, std::string_view path, std::string* error = nullptr) const;
};

}  // namespace tze
