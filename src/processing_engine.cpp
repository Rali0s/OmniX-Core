#include "tze/processing_engine.hpp"

namespace tze {

ProcessingReport ProcessingEngine::process(const RequestProfile& profile) const {
    return coordinator_.run(profile);
}

}  // namespace tze
