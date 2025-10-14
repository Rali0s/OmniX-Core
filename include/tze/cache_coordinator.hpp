#pragma once

#include <string>
#include <vector>

#include "tze/types.hpp"

namespace tze {

class CacheCoordinator {
public:
    CacheCell prepare(const std::string& command, std::size_t estimated_size, bool first_run) const;
    void define(CacheCell& cell, std::vector<std::string> behaviours) const;
    void destroy(CacheCell& cell, bool persist_on_success) const;

private:
    std::size_t align_estimate(std::size_t estimated_size) const;
    std::string select_namespace(bool persistent) const;
};

}  // namespace tze
