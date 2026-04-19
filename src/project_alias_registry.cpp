#include "tze/project_alias_registry.hpp"

#include <algorithm>
#include <cctype>

namespace tze {
namespace {

std::string lowercase(std::string_view value) {
    std::string lowered;
    lowered.reserve(value.size());
    for (char c : value) {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return lowered;
}

bool platform_matches(const BuildRecipe& recipe, std::string_view platform) {
    if (recipe.supported_platforms.empty()) {
        return true;
    }

    const std::string lowered = lowercase(platform);
    for (const std::string& candidate : recipe.supported_platforms) {
        if (lowercase(candidate) == lowered) {
            return true;
        }
    }
    return false;
}

}  // namespace

const std::vector<ProjectAlias>& ProjectAliasRegistry::aliases() const {
    static const std::vector<ProjectAlias> kAliases = {
        {
            "nmap",
            {"nmap", "build nmap", "network mapper"},
            "https://github.com/nmap/nmap.git",
            "master",
            {
                {
                    "nmap-configure",
                    "git",
                    "configure",
                    {"macos", "linux"},
                    "nmap",
                    "install",
                    {"nmap", "bin/nmap"},
                    {"bin/nmap"},
                    {},
                    {
                        "Install libpcap development headers before building if packet capture support is required.",
                        "Install OpenSSL or LibreSSL development headers for full TLS and NSE features.",
                    },
                    {},
                    true,
                    false,
                },
            },
        },
        {
            "tinyxml2",
            {"tinyxml2", "build tinyxml2", "tiny xml"},
            "https://github.com/leethomason/tinyxml2.git",
            "master",
            {
                {
                    "tinyxml2-cmake",
                    "git",
                    "cmake",
                    {"macos", "linux"},
                    {},
                    "install",
                    {"libtinyxml2.a", "libtinyxml2.dylib", "libtinyxml2.so", "tinyxml2.h"},
                    {"lib/libtinyxml2.a", "lib/libtinyxml2.dylib", "lib/libtinyxml2.so", "include/tinyxml2.h"},
                    {},
                    {"No extra system dependencies expected beyond CMake and a C++ compiler."},
                    {},
                    true,
                    false,
                },
            },
        },
        {
            "fmt",
            {"fmt", "build fmt", "fmtlib"},
            "https://github.com/fmtlib/fmt.git",
            "master",
            {
                {
                    "fmt-cmake",
                    "git",
                    "cmake",
                    {"macos", "linux"},
                    {},
                    "install",
                    {"libfmt.a", "libfmt.dylib", "libfmt.so", "fmt/core.h"},
                    {"lib/libfmt.a", "lib/libfmt.dylib", "lib/libfmt.so", "include/fmt/core.h"},
                    {},
                    {"No extra system dependencies expected beyond CMake and a C++ compiler."},
                    {},
                    true,
                    false,
                },
            },
        },
        {
            "lua",
            {"lua", "build lua", "lua language"},
            "https://github.com/lua/lua.git",
            "master",
            {
                {
                    "lua-make-macos",
                    "git",
                    "make",
                    {"macos"},
                    "macosx",
                    {},
                    {"lua", "src/lua", "liblua.a", "src/liblua.a"},
                    {"bin/lua", "lib/liblua.a"},
                    {"src/lua", "src/liblua.a"},
                    {"Install an ANSI C toolchain and standard development headers before building Lua."},
                    {},
                    false,
                    true,
                },
                {
                    "lua-make-linux",
                    "git",
                    "make",
                    {"linux"},
                    "linux",
                    {},
                    {"lua", "src/lua", "liblua.a", "src/liblua.a"},
                    {"bin/lua", "lib/liblua.a"},
                    {"src/lua", "src/liblua.a"},
                    {"Install an ANSI C toolchain and standard development headers before building Lua."},
                    {},
                    false,
                    true,
                },
            },
        },
        {
            "tshark",
            {"tshark", "build tshark", "wireshark", "build wireshark", "wireshark cli", "wire shark"},
            "https://gitlab.com/wireshark/wireshark.git",
            "master",
            {
                {
                    "tshark-cmake",
                    "git",
                    "cmake",
                    {"macos", "linux"},
                    "tshark",
                    "install",
                    {"tshark", "run/tshark", "bin/tshark"},
                    {"bin/tshark"},
                    {},
                    {
                        "Install libpcap, GLib 2, Flex, Bison, and a TLS/zlib development stack before building TShark.",
                        "This recipe disables the full Wireshark GUI and targets the CLI capture path only.",
                    },
                    {
                        "-DBUILD_wireshark=OFF",
                        "-DBUILD_tshark=ON",
                        "-DBUILD_dumpcap=ON",
                    },
                    true,
                    false,
                },
            },
        },
    };
    return kAliases;
}

std::optional<ProjectAlias> ProjectAliasRegistry::find(std::string_view name) const {
    const std::string lowered = lowercase(name);
    for (const ProjectAlias& alias : aliases()) {
        if (lowered == lowercase(alias.canonical_name)) {
            return alias;
        }
        for (const std::string& alt : alias.aliases) {
            if (lowered == lowercase(alt)) {
                return alias;
            }
        }
    }
    return std::nullopt;
}

std::vector<std::string> ProjectAliasRegistry::suggest(std::string_view query) const {
    const std::string lowered = lowercase(query);
    std::vector<std::string> suggestions;
    for (const ProjectAlias& alias : aliases()) {
        if (lowercase(alias.canonical_name).find(lowered) != std::string::npos) {
            suggestions.push_back(alias.canonical_name);
        }
    }
    return suggestions;
}

const BuildRecipe* ProjectAliasRegistry::select_recipe(const ProjectAlias& alias,
                                                       std::string_view platform,
                                                       std::string_view requested_recipe_id) const {
    if (!requested_recipe_id.empty()) {
        for (const BuildRecipe& recipe : alias.recipes) {
            if (recipe.id == requested_recipe_id && platform_matches(recipe, platform)) {
                return &recipe;
            }
        }
    }

    for (const BuildRecipe& recipe : alias.recipes) {
        if (platform_matches(recipe, platform)) {
            return &recipe;
        }
    }
    return alias.recipes.empty() ? nullptr : &alias.recipes.front();
}

}  // namespace tze
