#pragma once

#include "tze/build_executor.hpp"
#include "tze/cache_coordinator.hpp"
#include "tze/knowledge_engine.hpp"
#include "tze/memory_store.hpp"
#include "tze/native_tool_registry.hpp"
#include "tze/project_alias_registry.hpp"
#include "tze/project_resolver.hpp"
#include "tze/reasoning_provider.hpp"
#include "tze/security_manager.hpp"
#include "tze/types.hpp"

namespace tze {

class BuildFlowInterpreter {
public:
    void run(const RequestProfile& profile,
             MemorySnapshot& memory,
             ProcessingReport& report,
             const BuildExecutor& builder,
             const CacheCoordinator& cache,
             const KnowledgeEngine& knowledge,
             const MemoryStore& memory_store,
             const NativeToolRegistry& tools,
             const ProjectAliasRegistry& aliases,
             const ProjectResolver& projects,
             const ReasoningProvider& provider,
             const SecurityManager& security) const;
};

}  // namespace tze
