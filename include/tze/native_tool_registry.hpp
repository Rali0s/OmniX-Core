#pragma once

#include <string_view>
#include <vector>

#include "tze/types.hpp"

namespace tze {

class NativeToolRegistry {
public:
    std::vector<std::string> supported_tools() const;
    bool is_known_tool(std::string_view name) const;
    std::string canonical_name(std::string_view name) const;

    ToolResolution resolve(std::string_view name,
                           MemorySnapshot& memory,
                           bool allow_deep_hunt = true) const;
    ToolDoctorReport doctor(std::string_view name,
                            MemorySnapshot& memory,
                            bool allow_deep_hunt = true) const;
    ToolInvocationReport invoke(std::string_view name,
                                const std::vector<std::string>& arguments,
                                MemorySnapshot& memory,
                                bool allow_deep_hunt = true) const;
    std::vector<NativeToolRecord> list(MemorySnapshot& memory) const;
};

}  // namespace tze
