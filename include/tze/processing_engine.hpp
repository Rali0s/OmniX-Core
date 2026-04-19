#pragma once

#include "tze/build_executor.hpp"
#include "tze/cache_coordinator.hpp"
#include "tze/knowledge_engine.hpp"
#include "tze/session_coordinator.hpp"
#include "tze/security_manager.hpp"
#include "tze/types.hpp"

namespace tze {

class ProcessingEngine {
public:
    ProcessingReport process(const RequestProfile& profile) const;

private:
    SessionCoordinator coordinator_;
};

}  // namespace tze
