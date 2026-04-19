#pragma once

#include <string_view>

#include "tze/definition_engine.hpp"
#include "tze/memory_store.hpp"
#include "tze/types.hpp"

namespace tze {

class DefinitionFlowInterpreter {
public:
    void run(const RequestProfile& profile,
             std::string_view target,
             MemorySnapshot& memory,
             ProcessingReport& report,
             const DefinitionEngine& definitions,
             const MemoryStore& memory_store) const;
};

}  // namespace tze
