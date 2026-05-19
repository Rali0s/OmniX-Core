#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace tze {

struct InstanceIdentityReport {
    std::string status;
    std::string instance_id;
    std::string fingerprint;
    std::string architecture;
    std::string platform;
    std::string host_hint_hash;
    std::string salt_path;
    std::string mode;
    std::string warning;
    bool generated_new_salt = false;
    std::vector<std::string> components;
};

InstanceIdentityReport resolve_instance_identity(const std::filesystem::path& memory_root = {});

}  // namespace tze
