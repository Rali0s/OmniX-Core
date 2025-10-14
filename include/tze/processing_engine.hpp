#pragma once

#include "tze/cache_coordinator.hpp"
#include "tze/knowledge_engine.hpp"
#include "tze/security_manager.hpp"
#include "tze/types.hpp"

namespace tze {

class ProcessingEngine {
public:
    ProcessingReport process(const RequestProfile& profile) const;

private:
    CacheCoordinator cache_;
    KnowledgeEngine knowledge_;
    SecurityManager security_;
};

}  // namespace tze
