#pragma once

#include "tze/build_executor.hpp"
#include "tze/cache_coordinator.hpp"
#include "tze/memory_store.hpp"
#include "tze/reasoning_provider.hpp"
#include "tze/security_manager.hpp"
#include "tze/types.hpp"

namespace tze {

class RecipeAuthoringEngine {
public:
    void run(const RequestProfile& profile,
             MemorySnapshot& memory,
             ProcessingReport& report,
             const BuildExecutor& builder,
             const CacheCoordinator& cache,
             const MemoryStore& memory_store,
             const ReasoningProvider& provider,
             const SecurityManager& security) const;
};

}  // namespace tze
