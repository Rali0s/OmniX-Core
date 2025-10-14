#pragma once

#include "tze/types.hpp"

namespace tze {

class SecurityManager {
public:
    SecurityAudit verify(const RequestProfile& profile) const;
};

}  // namespace tze
