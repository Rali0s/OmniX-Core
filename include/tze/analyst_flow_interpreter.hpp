#pragma once

#include <string_view>

#include "tze/memory_store.hpp"
#include "tze/native_tool_registry.hpp"
#include "tze/types.hpp"

namespace tze {

class AnalystFlowInterpreter {
public:
    void run(const RequestProfile& profile,
             std::string_view target,
             MemorySnapshot& memory,
             ProcessingReport& report,
             const NativeToolRegistry& tools,
             const MemoryStore& memory_store) const;
};

}  // namespace tze
