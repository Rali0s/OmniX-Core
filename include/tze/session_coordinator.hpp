#pragma once

#include <memory>

#include "tze/analyst_flow_interpreter.hpp"
#include "tze/build_flow_interpreter.hpp"
#include "tze/build_executor.hpp"
#include "tze/cache_coordinator.hpp"
#include "tze/definition_engine.hpp"
#include "tze/definition_flow_interpreter.hpp"
#include "tze/intent_resolver.hpp"
#include "tze/knowledge_engine.hpp"
#include "tze/memory_store.hpp"
#include "tze/native_tool_registry.hpp"
#include "tze/project_alias_registry.hpp"
#include "tze/project_resolver.hpp"
#include "tze/reasoning_provider.hpp"
#include "tze/security_manager.hpp"
#include "tze/tool_flow_interpreter.hpp"
#include "tze/types.hpp"

namespace tze {

class SessionCoordinator {
public:
    SessionCoordinator();
    ProcessingReport run(const RequestProfile& profile) const;

private:
    AnalystFlowInterpreter analyst_flow_;
    BuildExecutor builder_;
    BuildFlowInterpreter build_flow_;
    CacheCoordinator cache_;
    DefinitionEngine definitions_;
    DefinitionFlowInterpreter definition_flow_;
    IntentResolver intents_;
    KnowledgeEngine knowledge_;
    MemoryStore memory_;
    NativeToolRegistry tools_;
    std::unique_ptr<ReasoningProvider> provider_;
    ProjectAliasRegistry aliases_;
    ProjectResolver projects_;
    SecurityManager security_;
    ToolFlowInterpreter tool_flow_;
};

}  // namespace tze
