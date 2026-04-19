#pragma once

#include "tze/build_executor.hpp"
#include "tze/memory_store.hpp"
#include "tze/native_tool_registry.hpp"
#include "tze/types.hpp"

namespace tze {

class ToolFlowInterpreter {
public:
    void run(const RequestProfile& profile,
             MemorySnapshot& memory,
             ProcessingReport& report,
             const BuildExecutor& builder,
             const NativeToolRegistry& tools,
             const MemoryStore& memory_store) const;
};

}  // namespace tze
