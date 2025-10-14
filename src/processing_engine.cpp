#include "tze/processing_engine.hpp"

#include <string>
#include <vector>

namespace tze {

ProcessingReport ProcessingEngine::process(const RequestProfile& profile) const {
    ProcessingReport report;

    report.decoded_instruction = knowledge_.decode_instruction(profile.instruction_slot);
    report.cache = cache_.prepare(report.decoded_instruction, profile.estimated_size, profile.first_run);

    cache_.define(report.cache, {"seek_Unbound", profile.persist_on_success ? "retain.Success" : "retain.Fail"});

    report.references = knowledge_.prioritize(report.decoded_instruction);
    report.feedback_loop = knowledge_.replay_feedback(report.decoded_instruction);
    report.security = security_.verify(profile);

    if (report.security.admin_verified) {
        report.storage_writes.push_back("x.Store(summary -> " + report.cache.name + ")");
        report.storage_writes.push_back("x.Store(preferences -> xMap_Perm_Prioritys.SearchExtranet)");
    }

    cache_.destroy(report.cache, profile.persist_on_success);

    return report;
}

}  // namespace tze
