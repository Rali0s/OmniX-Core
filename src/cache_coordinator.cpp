#include "tze/cache_coordinator.hpp"

#include <sstream>

namespace tze {
namespace {
constexpr std::size_t kAlignment = 64 * 1024;

std::size_t round_up(std::size_t value, std::size_t alignment) {
    if (alignment == 0) {
        return value;
    }
    const std::size_t remainder = value % alignment;
    return remainder == 0 ? value : value + (alignment - remainder);
}
}  // namespace

CacheCell CacheCoordinator::prepare(const std::string& command, std::size_t estimated_size, bool first_run) const {
    CacheCell cell;
    cell.persistent = !first_run;
    cell.name = select_namespace(cell.persistent);
    cell.operations.push_back("xProcessingCache(" + command + ")");

    const std::size_t aligned = align_estimate(estimated_size);
    cell.size_bytes = aligned;

    std::ostringstream oss;
    oss << "xize(" << estimated_size << ") -> " << aligned << " bytes";
    cell.operations.push_back(oss.str());

    const std::string create_target = first_run ? "xMap_Temp" : "xMap_Perm";
    cell.operations.push_back("xCell_Create(" + create_target + ")");

    return cell;
}

void CacheCoordinator::define(CacheCell& cell, std::vector<std::string> behaviours) const {
    std::ostringstream oss;
    oss << "xProcessingDefine(";
    for (std::size_t i = 0; i < behaviours.size(); ++i) {
        oss << behaviours[i];
        if (i + 1 < behaviours.size()) {
            oss << ", ";
        }
    }
    oss << ")";
    cell.operations.push_back(oss.str());
}

void CacheCoordinator::destroy(CacheCell& cell, bool persist_on_success) const {
    const std::string mode = persist_on_success ? "PostSuccess" : "PostFail";
    cell.operations.push_back("x.Destroy(" + mode + ")");
}

std::size_t CacheCoordinator::align_estimate(std::size_t estimated_size) const {
    if (estimated_size == 0) {
        return 5ull * 1024 * 1024 * 1024;
    }
    return round_up(estimated_size, kAlignment);
}

std::string CacheCoordinator::select_namespace(bool persistent) const {
    return persistent ? "xMap_Perm" : "xMap_Temp";
}

}  // namespace tze
