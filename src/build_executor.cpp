#include "tze/build_executor.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include <sys/wait.h>

namespace tze {
namespace {

struct ModuleDefinition {
    std::string id;
    std::string name;
    std::vector<std::string> commands;
    std::vector<std::string> capabilities;
};

struct CommandResult {
    int exit_code = 0;
    std::string output;
};

std::string lowercase(std::string_view value) {
    std::string lowered;
    lowered.reserve(value.size());
    for (char c : value) {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return lowered;
}

std::string trim(std::string_view value) {
    std::size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start]))) {
        ++start;
    }

    std::size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }

    return std::string(value.substr(start, end - start));
}

std::string shell_quote(std::string_view value) {
    std::string quoted = "'";
    for (char c : value) {
        if (c == '\'') {
            quoted += "'\\''";
        } else {
            quoted.push_back(c);
        }
    }
    quoted.push_back('\'');
    return quoted;
}

std::vector<std::string> split_path(std::string_view value) {
    std::vector<std::string> paths;
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t end = value.find(':', start);
        const std::size_t length = end == std::string::npos ? value.size() - start : end - start;
        if (length > 0) {
            paths.emplace_back(value.substr(start, length));
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return paths;
}

const std::vector<ModuleDefinition>& module_definitions() {
    static const std::vector<ModuleDefinition> definitions = {
        {"git", "Git", {"git"}, {"probe", "acquire"}},
        {"gcc", "GCC", {"gcc"}, {"probe", "compile"}},
        {"gxx", "G++", {"g++", "c++"}, {"probe", "compile"}},
        {"make", "Make", {"make", "gmake"}, {"probe", "build"}},
        {"cmake", "CMake", {"cmake"}, {"probe", "inspect", "configure", "build", "install"}},
    };
    return definitions;
}

std::optional<std::filesystem::path> which(std::string_view command) {
    const char* env_path = std::getenv("PATH");
    if (env_path == nullptr) {
        return std::nullopt;
    }

    for (const std::string& directory : split_path(env_path)) {
        const std::filesystem::path candidate = std::filesystem::path(directory) / std::string(command);
        std::error_code ec;
        if (!std::filesystem::exists(candidate, ec)) {
            continue;
        }
        const auto perms = std::filesystem::status(candidate, ec).permissions();
        if (ec) {
            continue;
        }
        using perms_t = std::filesystem::perms;
        const bool executable =
            (perms & perms_t::owner_exec) != perms_t::none ||
            (perms & perms_t::group_exec) != perms_t::none ||
            (perms & perms_t::others_exec) != perms_t::none;
        if (executable) {
            return candidate;
        }
    }
    return std::nullopt;
}

CommandResult run_shell(std::string_view command) {
    CommandResult result;
    const std::string full_command = std::string(command) + " 2>&1";
    FILE* pipe = popen(full_command.c_str(), "r");
    if (pipe == nullptr) {
        result.exit_code = -1;
        result.output = "Failed to launch shell command.";
        return result;
    }

    std::array<char, 4096> buffer{};
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        result.output.append(buffer.data());
    }

    const int status = pclose(pipe);
    if (status == -1) {
        result.exit_code = -1;
    } else if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else {
        result.exit_code = status;
    }
    return result;
}

std::optional<std::string> read_version(const std::filesystem::path& executable) {
    static const std::vector<std::string> flags = {"--version", "-V", "-v"};
    for (const std::string& flag : flags) {
        const CommandResult result = run_shell(shell_quote(executable.string()) + " " + flag);
        const std::string output = trim(result.output);
        if (output.empty()) {
            continue;
        }
        const std::string first_line = trim(output.substr(0, output.find('\n')));
        const std::string lowered = [&first_line]() {
            std::string value;
            value.reserve(first_line.size());
            for (char c : first_line) {
                value.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            }
            return value;
        }();
        if (lowered.find("unknown flag") != std::string::npos || lowered.find("illegal option") != std::string::npos) {
            continue;
        }
        return first_line;
    }
    return std::nullopt;
}

std::string slugify(std::string_view value) {
    std::string slug;
    bool previous_dash = false;
    for (char c : value) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '-') {
            slug.push_back(c);
            previous_dash = false;
            continue;
        }
        if (!slug.empty() && !previous_dash) {
            slug.push_back('-');
            previous_dash = true;
        }
    }
    while (!slug.empty() && slug.back() == '-') {
        slug.pop_back();
    }
    return slug.empty() ? "source" : slug;
}

std::string timestamp() {
    using clock = std::chrono::system_clock;
    const auto now = clock::now();
    const std::time_t raw = clock::to_time_t(now);
    std::tm local{};
#if defined(__APPLE__) || defined(__unix__)
    localtime_r(&raw, &local);
#else
    local = *std::localtime(&raw);
#endif
    std::ostringstream oss;
    oss << std::put_time(&local, "%Y%m%d-%H%M%S");
    return oss.str();
}

std::string current_platform() {
#if defined(__APPLE__)
    return "macos";
#elif defined(__linux__)
    return "linux";
#else
    return "unsupported";
#endif
}

std::string current_architecture() {
#if defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)
    return "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#elif defined(__i386__) || defined(_M_IX86)
    return "x86";
#else
    return "unknown";
#endif
}

bool platform_matches(const BuildRecipe& recipe, std::string_view platform) {
    if (recipe.supported_platforms.empty()) {
        return true;
    }
    for (const std::string& candidate : recipe.supported_platforms) {
        if (lowercase(candidate) == lowercase(platform)) {
            return true;
        }
    }
    return false;
}

std::vector<std::string> available_recipe_ids(const std::optional<ProjectAlias>& alias) {
    std::vector<std::string> ids;
    if (!alias.has_value()) {
        return ids;
    }
    for (const BuildRecipe& recipe : alias->recipes) {
        ids.push_back(recipe.id);
    }
    return ids;
}

bool recipe_exists_for_platform(const std::optional<ProjectAlias>& alias,
                                std::string_view recipe_id,
                                std::string_view platform) {
    if (!alias.has_value()) {
        return false;
    }
    for (const BuildRecipe& recipe : alias->recipes) {
        if (recipe.id == recipe_id && platform_matches(recipe, platform)) {
            return true;
        }
    }
    return false;
}

bool recipe_exists_any_platform(const std::optional<ProjectAlias>& alias, std::string_view recipe_id) {
    if (!alias.has_value()) {
        return false;
    }
    for (const BuildRecipe& recipe : alias->recipes) {
        if (recipe.id == recipe_id) {
            return true;
        }
    }
    return false;
}

const BuildRecipe* select_recipe(const std::optional<ProjectAlias>& alias,
                                 std::string_view platform,
                                 std::string_view requested_recipe_id) {
    if (!alias.has_value()) {
        return nullptr;
    }

    if (!requested_recipe_id.empty()) {
        for (const BuildRecipe& recipe : alias->recipes) {
            if (recipe.id == requested_recipe_id && platform_matches(recipe, platform)) {
                return &recipe;
            }
        }
    }

    for (const BuildRecipe& recipe : alias->recipes) {
        if (platform_matches(recipe, platform)) {
            return &recipe;
        }
    }

    return alias->recipes.empty() ? nullptr : &alias->recipes.front();
}

std::vector<std::string> recommended_modules(std::string_view build_system) {
    if (build_system == "cmake") {
        return {"cmake", "gcc", "g++"};
    }
    if (build_system == "configure" || build_system == "make") {
        return {"make", "gcc", "g++"};
    }
    return {};
}

std::vector<std::string> tail_lines(const std::filesystem::path& path, std::size_t max_lines) {
    std::ifstream input(path);
    if (!input) {
        return {};
    }

    std::vector<std::string> lines;
    for (std::string line; std::getline(input, line);) {
        lines.push_back(line);
    }
    if (lines.size() <= max_lines) {
        return lines;
    }
    return std::vector<std::string>(lines.end() - static_cast<std::ptrdiff_t>(max_lines), lines.end());
}

std::string detect_build_system(const std::filesystem::path& root, std::vector<std::string>* detected_files) {
    if (std::filesystem::exists(root / "CMakeLists.txt")) {
        detected_files->push_back("CMakeLists.txt");
        return "cmake";
    }
    if (std::filesystem::exists(root / "configure")) {
        detected_files->push_back("configure");
        return "configure";
    }
    if (std::filesystem::exists(root / "Makefile")) {
        detected_files->push_back("Makefile");
        return "make";
    }
    if (std::filesystem::exists(root / "GNUmakefile")) {
        detected_files->push_back("GNUmakefile");
        return "make";
    }
    return {};
}

bool is_supported_build_system(std::string_view build_system) {
    return build_system == "cmake" || build_system == "configure" || build_system == "make";
}

const ToolchainModuleStatus* find_module(const std::vector<ToolchainModuleStatus>& modules, std::string_view id) {
    for (const ToolchainModuleStatus& module : modules) {
        if (module.id == id) {
            return &module;
        }
    }
    return nullptr;
}

std::vector<std::string> missing_modules(std::string_view build_system,
                                         const std::vector<ToolchainModuleStatus>& modules,
                                         bool needs_git) {
    auto append_unique = [](std::vector<std::string>& values, std::string value) {
        if (std::find(values.begin(), values.end(), value) == values.end()) {
            values.push_back(std::move(value));
        }
    };

    std::vector<std::string> missing;
    const ToolchainModuleStatus* gcc = find_module(modules, "gcc");
    const ToolchainModuleStatus* gxx = find_module(modules, "gxx");
    const bool compiler_ready = (gcc != nullptr && gcc->available) || (gxx != nullptr && gxx->available);

    if (needs_git) {
        const ToolchainModuleStatus* git = find_module(modules, "git");
        if (git == nullptr || !git->available) {
            append_unique(missing, "git");
        }
    }

    if (build_system == "cmake") {
        const ToolchainModuleStatus* cmake = find_module(modules, "cmake");
        if (cmake == nullptr || !cmake->available) {
            append_unique(missing, "cmake");
        }
        if (!compiler_ready) {
            append_unique(missing, "gcc/g++");
        }
    }
    if (build_system == "configure" || build_system == "make") {
        const ToolchainModuleStatus* make = find_module(modules, "make");
        if (make == nullptr || !make->available) {
            append_unique(missing, "make");
        }
        if (!compiler_ready) {
            append_unique(missing, "gcc/g++");
        }
    }
    return missing;
}

std::vector<std::pair<std::string, std::string>> build_env_overrides(const std::vector<ToolchainModuleStatus>& modules) {
    std::vector<std::pair<std::string, std::string>> env_overrides;
    const ToolchainModuleStatus* gcc = find_module(modules, "gcc");
    const ToolchainModuleStatus* gxx = find_module(modules, "gxx");
    if (gcc != nullptr && gcc->available && !gcc->resolved_path.empty()) {
        env_overrides.emplace_back("CC", gcc->resolved_path);
    }
    if (gxx != nullptr && gxx->available && !gxx->resolved_path.empty()) {
        env_overrides.emplace_back("CXX", gxx->resolved_path);
    }
    return env_overrides;
}

std::string format_command(const std::vector<std::string>& command) {
    std::ostringstream oss;
    for (std::size_t i = 0; i < command.size(); ++i) {
        if (i != 0) {
            oss << ' ';
        }
        oss << shell_quote(command[i]);
    }
    return oss.str();
}

std::string build_shell_command(const std::filesystem::path& cwd,
                                const std::vector<std::string>& command,
                                const std::vector<std::pair<std::string, std::string>>& env_overrides) {
    std::ostringstream oss;
    oss << "cd " << shell_quote(cwd.string()) << " && ";
    if (!env_overrides.empty()) {
        for (const auto& [key, value] : env_overrides) {
            oss << key << '=' << shell_quote(value) << ' ';
        }
    }
    oss << format_command(command);
    return oss.str();
}

int parallelism() {
    const unsigned int hardware = std::thread::hardware_concurrency();
    return hardware == 0 ? 4 : static_cast<int>(hardware);
}

std::filesystem::path default_memory_root(const RequestProfile& profile) {
    if (!profile.memory_root_path.empty()) {
        return profile.memory_root_path;
    }
    if (const char* env_home = std::getenv("OMNIX_HOME"); env_home != nullptr && *env_home != '\0') {
        return env_home;
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return std::filesystem::path(home) / ".omnix";
    }
    return std::filesystem::current_path() / ".omnix";
}

std::string detect_environment_signature(const std::vector<ToolchainModuleStatus>& modules,
                                         std::string_view build_system) {
    std::ostringstream oss;
    oss << current_platform() << "-" << current_architecture();
    if (!build_system.empty()) {
        oss << "|" << build_system;
    }
    for (const std::string_view id : {"git", "cmake", "make", "gxx", "gcc"}) {
        const ToolchainModuleStatus* module = find_module(modules, id);
        if (module != nullptr && module->available) {
            oss << "|" << module->id << "=" << module->version;
        }
    }
    return oss.str();
}

std::string deduce_project_name(const RequestProfile& profile,
                                const std::optional<ProjectAlias>& alias,
                                const std::filesystem::path& source_root) {
    if (alias.has_value()) {
        return alias->canonical_name;
    }
    if (!profile.project_alias.empty()) {
        return profile.project_alias;
    }
    if (!profile.project_reference.empty() && std::filesystem::exists(profile.project_reference)) {
        return std::filesystem::path(profile.project_reference).filename().string();
    }
    if (!source_root.empty()) {
        return source_root.filename().string();
    }
    if (!profile.project_reference.empty()) {
        return slugify(profile.project_reference);
    }
    return "project";
}

std::filesystem::path compute_build_dir(const RequestProfile& profile,
                                        const std::string& project_name,
                                        std::string_view recipe_id) {
    if (!profile.build_dir.empty()) {
        return profile.build_dir;
    }
    return default_memory_root(profile) / "workspaces" / project_name / "build" / std::string(recipe_id);
}

std::filesystem::path compute_install_prefix(const RequestProfile& profile,
                                             const std::string& project_name,
                                             std::string_view recipe_id) {
    if (!profile.install_prefix.empty()) {
        return profile.install_prefix;
    }
    return default_memory_root(profile) / "installs" / project_name /
           (current_platform() + "-" + current_architecture()) /
           std::string(recipe_id) /
           "current";
}

std::filesystem::path compute_log_path(const RequestProfile& profile, const std::string& project_name) {
    return default_memory_root(profile) / "logs" / project_name / (timestamp() + ".log");
}

std::vector<std::filesystem::path> existing_from_patterns(const std::vector<std::string>& patterns,
                                                          const std::vector<std::filesystem::path>& roots) {
    std::vector<std::filesystem::path> found;
    for (const std::string& pattern : patterns) {
        const std::filesystem::path pattern_path(pattern);
        if (pattern_path.is_absolute()) {
            std::error_code ec;
            if (std::filesystem::exists(pattern_path, ec) && std::filesystem::is_regular_file(pattern_path, ec)) {
                found.push_back(pattern_path);
            }
            continue;
        }
        for (const std::filesystem::path& root : roots) {
            const std::filesystem::path candidate = root / pattern_path;
            std::error_code ec;
            if (std::filesystem::exists(candidate, ec) && std::filesystem::is_regular_file(candidate, ec)) {
                found.push_back(candidate);
                break;
            }
        }
    }
    return found;
}

std::vector<std::string> stringify_paths(const std::vector<std::filesystem::path>& paths) {
    std::vector<std::string> strings;
    strings.reserve(paths.size());
    for (const std::filesystem::path& path : paths) {
        strings.push_back(path.string());
    }
    return strings;
}

bool copy_stage_outputs(const std::filesystem::path& source_root,
                        const std::filesystem::path& build_dir,
                        const std::filesystem::path& install_prefix,
                        const BuildRecipe& recipe,
                        std::vector<std::string>* copied_paths) {
    if (recipe.install_output_patterns.empty() || recipe.fallback_stage_patterns.empty()) {
        return false;
    }

    const std::vector<std::filesystem::path> roots = {build_dir, build_dir / "src", source_root, source_root / "src"};
    bool copied_any = false;
    for (std::size_t index = 0; index < recipe.install_output_patterns.size() && index < recipe.fallback_stage_patterns.size(); ++index) {
        const std::string& output_pattern = recipe.install_output_patterns[index];
        const std::string& source_pattern = recipe.fallback_stage_patterns[index];
        const std::vector<std::filesystem::path> matches = existing_from_patterns({source_pattern}, roots);
        if (matches.empty()) {
            continue;
        }
        const std::filesystem::path destination = install_prefix / output_pattern;
        std::filesystem::create_directories(destination.parent_path());
        std::error_code ec;
        std::filesystem::copy_file(matches.front(), destination, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            continue;
        }
        copied_any = true;
        copied_paths->push_back(destination.string());
    }
    return copied_any;
}

std::vector<std::string> default_artifact_patterns(const RequestProfile& profile) {
    if (!profile.build_target.empty()) {
        return {profile.build_target};
    }
    return {"omnix", "versus_demo"};
}

std::vector<std::string> default_install_output_patterns(const RequestProfile& profile) {
    if (!profile.build_target.empty()) {
        return {"bin/" + profile.build_target};
    }
    return {};
}

std::string read_os_release_value(std::string_view key) {
    std::ifstream input("/etc/os-release");
    if (!input) {
        return {};
    }

    const std::string needle = std::string(key) + "=";
    for (std::string line; std::getline(input, line);) {
        if (line.rfind(needle, 0) != 0) {
            continue;
        }
        std::string value = line.substr(needle.size());
        if (!value.empty() && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
        }
        return lowercase(value);
    }
    return {};
}

std::vector<std::string> detected_package_managers() {
    std::vector<std::string> ids;
    for (std::string_view id : {"brew", "apt-get", "dnf", "yum", "pacman", "rpm", "pkg"}) {
        if (which(id).has_value()) {
            ids.push_back(std::string(id));
        }
    }
    return ids;
}

bool has_value(const std::vector<std::string>& values, std::string_view needle) {
    return std::find(values.begin(), values.end(), needle) != values.end();
}

std::string choose_primary_package_manager(std::string_view platform,
                                           const std::vector<std::string>& available) {
    if (platform == "macos") {
        return has_value(available, "brew") ? "brew" : std::string();
    }

    const std::string id = read_os_release_value("ID");
    const std::string id_like = read_os_release_value("ID_LIKE");
    const auto matches = [&](std::string_view token) {
        return id == token || id_like.find(std::string(token)) != std::string::npos;
    };

    if (matches("ubuntu") || matches("debian")) {
        if (has_value(available, "apt-get")) {
            return "apt-get";
        }
    }
    if (matches("fedora")) {
        if (has_value(available, "dnf")) {
            return "dnf";
        }
    }
    if (matches("rhel") || matches("centos") || matches("rocky") || matches("almalinux")) {
        if (has_value(available, "dnf")) {
            return "dnf";
        }
        if (has_value(available, "yum")) {
            return "yum";
        }
    }
    if (matches("arch")) {
        if (has_value(available, "pacman")) {
            return "pacman";
        }
    }

    for (std::string_view candidate : {"brew", "apt-get", "dnf", "yum", "pacman", "pkg", "rpm"}) {
        if (has_value(available, candidate)) {
            return std::string(candidate);
        }
    }
    return {};
}

bool command_available(std::string_view command) {
    return which(command).has_value();
}

bool pkg_config_exists(std::string_view package_name) {
    const std::optional<std::filesystem::path> pkg_config = which("pkg-config");
    if (!pkg_config.has_value()) {
        return false;
    }
    const CommandResult result =
        run_shell(shell_quote(pkg_config->string()) + " --exists " + shell_quote(package_name));
    return result.exit_code == 0;
}

std::string archive_url_for_alias(const ProjectAlias& alias) {
    if (alias.upstream_url.find("github.com") == std::string::npos) {
        return {};
    }
    std::string base = alias.upstream_url;
    if (base.size() >= 4 && base.substr(base.size() - 4) == ".git") {
        base.resize(base.size() - 4);
    }
    const std::string ref = alias.default_ref.empty() ? "master" : alias.default_ref;
    return base + "/archive/refs/heads/" + ref + ".tar.gz";
}

std::vector<std::string> packages_for_manager(std::string_view manager,
                                              std::string_view canonical_name,
                                              std::string_view build_system) {
    const std::string project = lowercase(canonical_name);
    if (manager == "brew") {
        if (project == "nmap") {
            return {"git", "pkg-config", "libpcap", "openssl", "curl", "wget"};
        }
        if (project == "tshark") {
            return {"git", "cmake", "flex", "bison", "pkg-config", "glib", "libpcap", "gnutls", "zlib"};
        }
        if (project == "fmt" || project == "tinyxml2") {
            return {"git", "cmake"};
        }
        if (project == "lua") {
            return {"git"};
        }
        if (build_system == "cmake") {
            return {"git", "cmake"};
        }
        return {"git"};
    }
    if (manager == "apt-get") {
        if (project == "nmap") {
            return {"build-essential", "git", "pkg-config", "libpcap-dev", "libssl-dev", "curl", "wget"};
        }
        if (project == "tshark") {
            return {"build-essential", "git", "cmake", "flex", "bison", "pkg-config", "libpcap-dev", "libglib2.0-dev", "libgnutls28-dev", "zlib1g-dev"};
        }
        if (project == "fmt" || project == "tinyxml2") {
            return {"build-essential", "cmake", "git"};
        }
        if (project == "lua") {
            return {"build-essential", "git"};
        }
        if (build_system == "cmake") {
            return {"build-essential", "cmake", "git"};
        }
        return {"build-essential", "git"};
    }
    if (manager == "dnf" || manager == "yum") {
        if (project == "nmap") {
            return {"gcc", "gcc-c++", "make", "git", "pkgconf-pkg-config", "libpcap-devel", "openssl-devel", "curl", "wget"};
        }
        if (project == "tshark") {
            return {"gcc", "gcc-c++", "make", "git", "cmake", "flex", "bison", "pkgconf-pkg-config", "libpcap-devel", "glib2-devel", "gnutls-devel", "zlib-devel"};
        }
        if (project == "fmt" || project == "tinyxml2") {
            return {"gcc", "gcc-c++", "make", "cmake", "git"};
        }
        if (project == "lua") {
            return {"gcc", "gcc-c++", "make", "git"};
        }
        if (build_system == "cmake") {
            return {"gcc", "gcc-c++", "make", "cmake", "git"};
        }
        return {"gcc", "gcc-c++", "make", "git"};
    }
    if (manager == "pacman") {
        if (project == "nmap") {
            return {"base-devel", "git", "pkgconf", "libpcap", "openssl", "curl", "wget"};
        }
        if (project == "tshark") {
            return {"base-devel", "git", "cmake", "flex", "bison", "pkgconf", "libpcap", "glib2", "gnutls", "zlib"};
        }
        if (project == "fmt" || project == "tinyxml2") {
            return {"base-devel", "cmake", "git"};
        }
        if (project == "lua") {
            return {"base-devel", "git"};
        }
        if (build_system == "cmake") {
            return {"base-devel", "cmake", "git"};
        }
        return {"base-devel", "git"};
    }
    if (manager == "pkg") {
        if (project == "nmap") {
            return {"git", "pkgconf", "libpcap", "openssl", "curl", "wget", "gmake", "gcc"};
        }
        if (project == "tshark") {
            return {"git", "cmake", "flex", "bison", "pkgconf", "libpcap", "glib", "gnutls", "zlib", "gmake", "gcc"};
        }
        if (project == "fmt" || project == "tinyxml2") {
            return {"git", "cmake", "gmake", "gcc"};
        }
        if (project == "lua") {
            return {"git", "gmake", "gcc"};
        }
        if (build_system == "cmake") {
            return {"git", "cmake", "gmake", "gcc"};
        }
        return {"git", "gmake", "gcc"};
    }
    if (manager == "rpm") {
        if (project == "nmap") {
            return {"gcc", "gcc-c++", "make", "git", "pkgconf-pkg-config", "libpcap-devel", "openssl-devel", "curl", "wget"};
        }
        if (project == "tshark") {
            return {"gcc", "gcc-c++", "make", "git", "cmake", "flex", "bison", "pkgconf-pkg-config", "libpcap-devel", "glib2-devel", "gnutls-devel", "zlib-devel"};
        }
        if (project == "fmt" || project == "tinyxml2") {
            return {"gcc", "gcc-c++", "make", "cmake", "git"};
        }
        if (project == "lua") {
            return {"gcc", "gcc-c++", "make", "git"};
        }
        if (build_system == "cmake") {
            return {"gcc", "gcc-c++", "make", "cmake", "git"};
        }
        return {"gcc", "gcc-c++", "make", "git"};
    }
    return {};
}

PackageManagerGuidance make_package_guidance(std::string_view manager,
                                             std::string_view canonical_name,
                                             std::string_view build_system,
                                             std::string_view primary_manager) {
    PackageManagerGuidance guidance;
    guidance.id = std::string(manager);
    guidance.primary = manager == primary_manager;
    const std::vector<std::string> packages = packages_for_manager(manager, canonical_name, build_system);
    if (manager == "brew") {
        guidance.label = "Homebrew";
        if (!packages.empty()) {
            guidance.commands.push_back("brew install " + [&packages]() {
                std::ostringstream out;
                for (std::size_t i = 0; i < packages.size(); ++i) {
                    if (i != 0) {
                        out << ' ';
                    }
                    out << packages[i];
                }
                return out.str();
            }());
        }
        guidance.commands.push_back("xcode-select --install");
        return guidance;
    }
    if (manager == "apt-get") {
        guidance.label = "APT";
        if (!packages.empty()) {
            std::ostringstream out;
            for (std::size_t i = 0; i < packages.size(); ++i) {
                if (i != 0) {
                    out << ' ';
                }
                out << packages[i];
            }
            guidance.commands.push_back("sudo apt-get update");
            guidance.commands.push_back("sudo apt-get install -y " + out.str());
        }
        return guidance;
    }
    if (manager == "dnf") {
        guidance.label = "DNF";
        if (!packages.empty()) {
            std::ostringstream out;
            for (std::size_t i = 0; i < packages.size(); ++i) {
                if (i != 0) {
                    out << ' ';
                }
                out << packages[i];
            }
            guidance.commands.push_back("sudo dnf install -y " + out.str());
        }
        return guidance;
    }
    if (manager == "yum") {
        guidance.label = "YUM";
        if (!packages.empty()) {
            std::ostringstream out;
            for (std::size_t i = 0; i < packages.size(); ++i) {
                if (i != 0) {
                    out << ' ';
                }
                out << packages[i];
            }
            guidance.commands.push_back("sudo yum install -y " + out.str());
        }
        return guidance;
    }
    if (manager == "pacman") {
        guidance.label = "Pacman";
        if (!packages.empty()) {
            std::ostringstream out;
            for (std::size_t i = 0; i < packages.size(); ++i) {
                if (i != 0) {
                    out << ' ';
                }
                out << packages[i];
            }
            guidance.commands.push_back("sudo pacman -S --needed " + out.str());
        }
        return guidance;
    }
    if (manager == "pkg") {
        guidance.label = "pkg";
        if (!packages.empty()) {
            std::ostringstream out;
            for (std::size_t i = 0; i < packages.size(); ++i) {
                if (i != 0) {
                    out << ' ';
                }
                out << packages[i];
            }
            guidance.commands.push_back("pkg install " + out.str());
        }
        return guidance;
    }
    if (manager == "rpm") {
        guidance.label = "RPM verification";
        if (!packages.empty()) {
            std::ostringstream out;
            for (std::size_t i = 0; i < packages.size(); ++i) {
                if (i != 0) {
                    out << ' ';
                }
                out << packages[i];
            }
            guidance.commands.push_back("rpm -q " + out.str());
        }
        return guidance;
    }
    return guidance;
}

PackageManagerGuidance make_fetch_guidance(std::string_view fetcher,
                                           const ProjectAlias& alias,
                                           std::string_view archive_url) {
    PackageManagerGuidance guidance;
    guidance.id = std::string(fetcher);
    guidance.label = fetcher == "curl" ? "curl bootstrap" : "wget bootstrap";
    const std::string archive_url_string(archive_url);
    const std::string archive_name =
        alias.canonical_name + "-" + (alias.default_ref.empty() ? std::string("archive") : alias.default_ref) + ".tar.gz";
    if (!archive_url.empty()) {
        if (fetcher == "curl") {
            guidance.commands.push_back("curl -L " + archive_url_string + " -o " + archive_name);
        } else {
            guidance.commands.push_back("wget -O " + archive_name + " " + archive_url_string);
        }
        guidance.commands.push_back("tar -xzf " + archive_name);
    }
    guidance.commands.push_back("git clone " + alias.upstream_url + " " + alias.canonical_name);
    return guidance;
}

void append_unique(std::vector<std::string>& values, std::string value) {
    if (value.empty()) {
        return;
    }
    if (std::find(values.begin(), values.end(), value) == values.end()) {
        values.push_back(std::move(value));
    }
}

std::vector<std::string> dependency_checks_for_alias(const std::optional<ProjectAlias>& alias,
                                                     const std::vector<ToolchainModuleStatus>& modules) {
    std::vector<std::string> checks;
    const auto tool_line = [&](std::string_view label, bool available) {
        return std::string(label) + ": " + (available ? "available" : "missing");
    };
    const ToolchainModuleStatus* gcc = find_module(modules, "gcc");
    const ToolchainModuleStatus* gxx = find_module(modules, "gxx");
    const ToolchainModuleStatus* make = find_module(modules, "make");
    const ToolchainModuleStatus* cmake = find_module(modules, "cmake");
    const bool compiler = (gcc != nullptr && gcc->available) || (gxx != nullptr && gxx->available);

    append_unique(checks, tool_line("compiler", compiler));
    if (!alias.has_value()) {
        return checks;
    }

    const std::string project = lowercase(alias->canonical_name);
    if (project == "nmap") {
        append_unique(checks, tool_line("make", make != nullptr && make->available));
        append_unique(checks, tool_line("pkg-config", command_available("pkg-config")));
        append_unique(checks, std::string("libpcap (pkg-config): ") + (pkg_config_exists("libpcap") ? "detected" : "missing"));
        append_unique(checks, std::string("openssl (pkg-config): ") +
                                  ((pkg_config_exists("openssl") || pkg_config_exists("libssl")) ? "detected" : "missing"));
        append_unique(checks, tool_line("curl", command_available("curl")));
        append_unique(checks, tool_line("wget", command_available("wget")));
        return checks;
    }
    if (project == "tshark") {
        append_unique(checks, tool_line("cmake", cmake != nullptr && cmake->available));
        append_unique(checks, tool_line("git", command_available("git")));
        append_unique(checks, tool_line("flex", command_available("flex")));
        append_unique(checks, tool_line("bison", command_available("bison")));
        append_unique(checks, tool_line("pkg-config", command_available("pkg-config")));
        append_unique(checks, std::string("libpcap (pkg-config): ") + (pkg_config_exists("libpcap") ? "detected" : "missing"));
        append_unique(checks, std::string("glib-2.0 (pkg-config): ") + (pkg_config_exists("glib-2.0") ? "detected" : "missing"));
        append_unique(checks, std::string("gnutls (pkg-config): ") + (pkg_config_exists("gnutls") ? "detected" : "missing"));
        append_unique(checks, std::string("zlib (pkg-config): ") + (pkg_config_exists("zlib") ? "detected" : "missing"));
        return checks;
    }
    if (project == "fmt" || project == "tinyxml2") {
        append_unique(checks, tool_line("cmake", cmake != nullptr && cmake->available));
        append_unique(checks, tool_line("git", command_available("git")));
        return checks;
    }
    if (project == "lua") {
        append_unique(checks, tool_line("make", make != nullptr && make->available));
        append_unique(checks, tool_line("git", command_available("git")));
        return checks;
    }
    return checks;
}

}  // namespace

std::vector<ToolchainModuleStatus> BuildExecutor::probe_modules() const {
    std::vector<ToolchainModuleStatus> modules;
    for (const ModuleDefinition& definition : module_definitions()) {
        ToolchainModuleStatus status;
        status.id = definition.id;
        status.name = definition.name;
        status.capabilities = definition.capabilities;

        for (const std::string& command : definition.commands) {
            const std::optional<std::filesystem::path> resolved = which(command);
            if (!resolved.has_value()) {
                continue;
            }
            status.available = true;
            status.command = command;
            status.resolved_path = resolved->string();
            status.version = read_version(*resolved).value_or("");
            break;
        }

        modules.push_back(std::move(status));
    }
    return modules;
}

PreflightReport BuildExecutor::preflight(const RequestProfile& profile,
                                         const std::optional<ProjectAlias>& alias,
                                         const std::filesystem::path& source_path) const {
    PreflightReport report;
    const std::vector<ToolchainModuleStatus> modules = probe_modules();
    const std::string platform = current_platform();
    report.available_recipe_ids = available_recipe_ids(alias);
    if (!profile.selected_recipe_id.empty() && alias.has_value() &&
        !recipe_exists_for_platform(alias, profile.selected_recipe_id, platform)) {
        report.platform_supported = platform == "macos" || platform == "linux";
        report.canonical_project_name = deduce_project_name(profile, alias, source_path);
        report.recipe_id = profile.selected_recipe_id;
        report.status = "preflight_failed";
        report.summary = recipe_exists_any_platform(alias, profile.selected_recipe_id)
            ? "The requested recipe is defined, but not for this platform. Valid recipes for this alias: " +
                  [&report]() {
                      std::ostringstream out;
                      for (std::size_t i = 0; i < report.available_recipe_ids.size(); ++i) {
                          if (i != 0) {
                              out << ", ";
                          }
                          out << report.available_recipe_ids[i];
                      }
                      return out.str();
                  }()
            : "The requested recipe is not valid for this alias. Valid recipes: " +
                  [&report]() {
                      std::ostringstream out;
                      for (std::size_t i = 0; i < report.available_recipe_ids.size(); ++i) {
                          if (i != 0) {
                              out << ", ";
                          }
                          out << report.available_recipe_ids[i];
                      }
                      return out.str();
                  }();
        return report;
    }
    const BuildRecipe* recipe = select_recipe(alias, platform, profile.selected_recipe_id);
    report.platform_supported = platform == "macos" || platform == "linux";
    report.canonical_project_name = deduce_project_name(profile, alias, source_path);
    report.recipe_id = recipe != nullptr ? recipe->id : profile.selected_recipe_id;
    report.recipe_selection_reason = !profile.selected_recipe_id.empty()
        ? "manual_override"
        : (recipe != nullptr ? "alias_default" : "generic_fallback");

    SourceInspection inspection;
    if (!source_path.empty() && std::filesystem::exists(source_path)) {
        inspection = inspect_source(source_path);
        report.build_system = inspection.build_system;
    } else if (recipe != nullptr) {
        report.build_system = recipe->build_system;
    }

    report.environment_signature = detect_environment_signature(modules, report.build_system);
    report.will_acquire = alias.has_value() &&
        (source_path.empty() || !std::filesystem::exists(source_path)) &&
        !profile.offline &&
        profile.acquisition_policy != AcquisitionPolicy::LocalOnly;
    report.will_install = profile.perform_install;
    if (profile.perform_install) {
        const std::string recipe_id = report.recipe_id.empty() ? "default" : report.recipe_id;
        report.install_prefix = compute_install_prefix(profile, report.canonical_project_name, recipe_id).string();
    }

    if (recipe != nullptr) {
        report.dependency_hints = recipe->dependency_hints;
        report.expected_artifacts = recipe->artifact_patterns;
        report.expected_install_outputs = recipe->install_output_patterns;
    } else {
        report.expected_artifacts = default_artifact_patterns(profile);
        report.expected_install_outputs = default_install_output_patterns(profile);
    }

    if (report.will_acquire) {
        report.expected_steps.push_back("Acquire source via git clone into the managed OmniX workspace.");
    } else if (!source_path.empty()) {
        report.expected_steps.push_back("Reuse the resolved local source tree.");
    }

    if (report.build_system == "cmake") {
        report.expected_steps.push_back("Configure the project with CMake.");
        report.expected_steps.push_back("Build the requested target with CMake.");
    } else if (report.build_system == "configure") {
        report.expected_steps.push_back("Run configure with a staged install prefix.");
        report.expected_steps.push_back("Build the requested target with make.");
    } else if (report.build_system == "make") {
        report.expected_steps.push_back("Build the requested target with make.");
    }
    if (report.will_install) {
        report.expected_steps.push_back("Stage install outputs under the managed OmniX prefix.");
    }
    report.expected_steps.push_back("Verify declared artifacts before promoting the recipe.");

    if (!report.platform_supported) {
        report.status = "preflight_failed";
        report.summary = "OmniX portable builds are supported on macOS and Linux only for this milestone.";
        return report;
    }

    if (report.build_system.empty()) {
        report.status = "preflight_failed";
        report.summary = "OmniX could not determine a supported build recipe for this request.";
        return report;
    }

    report.missing_modules = missing_modules(report.build_system, modules, report.will_acquire);
    if (!inspection.summary.empty() && !inspection.exists) {
        report.status = "preflight_failed";
        report.summary = inspection.summary;
        return report;
    }

    if (!report.missing_modules.empty()) {
        report.status = "preflight_failed";
        report.summary = "The local toolchain is missing modules required for the selected portable build recipe.";
        return report;
    }

    report.ready = true;
    report.status = "ready";
    report.summary = "The portable build recipe is ready to run on this machine.";
    return report;
}

DoctorReport BuildExecutor::doctor(const RequestProfile& profile,
                                   const std::optional<ProjectAlias>& alias,
                                   const std::filesystem::path& source_path) const {
    DoctorReport report;
    const PreflightReport preflight_result = BuildExecutor::preflight(profile, alias, source_path);
    const std::vector<ToolchainModuleStatus> modules = probe_modules();
    const std::vector<std::string> package_managers = detected_package_managers();
    const std::string platform = current_platform();
    const std::string primary_manager = choose_primary_package_manager(platform, package_managers);

    report.status = preflight_result.ready ? "doctor_ready" : "doctor_attention_needed";
    report.summary = preflight_result.ready
        ? "The current machine looks capable of running the selected OmniX build recipe."
        : "The current machine needs attention before the selected OmniX build recipe is likely to succeed.";
    report.canonical_project_name = preflight_result.canonical_project_name;
    report.recipe_id = preflight_result.recipe_id;
    report.detected_platform = platform;
    report.detected_package_manager = primary_manager;
    report.available_package_managers = package_managers;
    report.missing_modules = preflight_result.missing_modules;
    report.dependency_hints = preflight_result.dependency_hints;
    report.dependency_checks = dependency_checks_for_alias(alias, modules);

    const std::string build_system = !preflight_result.build_system.empty()
        ? preflight_result.build_system
        : (alias.has_value() && !alias->recipes.empty() ? alias->recipes.front().build_system : std::string());
    for (std::string_view manager : {"brew", "apt-get", "dnf", "yum", "pacman", "rpm", "pkg"}) {
        report.package_guidance.push_back(
            make_package_guidance(manager, report.canonical_project_name, build_system, primary_manager));
    }

    if (alias.has_value()) {
        const std::string archive_url = archive_url_for_alias(*alias);
        report.package_guidance.push_back(make_fetch_guidance("curl", *alias, archive_url));
        report.package_guidance.push_back(make_fetch_guidance("wget", *alias, archive_url));
        report.bootstrap_guidance.push_back("Managed source workspace: " +
                                            (default_memory_root(profile) / "workspaces" / alias->canonical_name / "source").string());
        if (!archive_url.empty()) {
            report.bootstrap_guidance.push_back("Archive fallback: " + archive_url);
        }
    }

    if (platform == "macos") {
        report.bootstrap_guidance.push_back("Compiler bootstrap: xcode-select --install");
    } else if (platform == "linux") {
        report.bootstrap_guidance.push_back("Compiler bootstrap: install the compiler and build tools with the detected package manager first.");
        report.bootstrap_guidance.push_back("Fresh-box host audit: omnix tool inspect-host -- --linux");
    }

    return report;
}

AcquisitionResult BuildExecutor::acquire_source(const ProjectAlias& alias, const RequestProfile& profile) const {
    AcquisitionResult result;
    result.canonical_project_name = alias.canonical_name;
    result.upstream_url = alias.upstream_url;

    const std::filesystem::path workspaces_root = default_memory_root(profile) / "workspaces";
    const std::filesystem::path target_root = workspaces_root / alias.canonical_name / "source";
    std::filesystem::create_directories(target_root.parent_path());

    if (std::filesystem::exists(target_root) && std::filesystem::is_directory(target_root)) {
        result.status = "reused_workspace";
        result.summary = "Reused an existing managed workspace for the requested project alias.";
        result.resolved_source_path = target_root.string();
        return result;
    }

    const std::vector<ToolchainModuleStatus> modules = probe_modules();
    const ToolchainModuleStatus* git = find_module(modules, "git");
    if (git == nullptr || !git->available) {
        result.status = "missing_modules";
        result.summary = "Git is required to acquire the requested project alias.";
        return result;
    }

    if (std::filesystem::exists(target_root)) {
        std::error_code cleanup_error;
        std::filesystem::remove_all(target_root, cleanup_error);
    }

    const std::string requested_ref = profile.git_ref_override.empty() ? alias.default_ref : profile.git_ref_override;
    std::vector<std::string> clone_cmd = {
        git->resolved_path.empty() ? std::string("git") : git->resolved_path,
        "clone",
        "--depth",
        "1",
    };
    if (!requested_ref.empty()) {
        clone_cmd.push_back("--branch");
        clone_cmd.push_back(requested_ref);
    }
    clone_cmd.push_back(alias.upstream_url);
    clone_cmd.push_back(target_root.string());

    result.commands.push_back(format_command(clone_cmd));
    const CommandResult command_result = run_shell(build_shell_command(workspaces_root, clone_cmd, {}));
    if (command_result.exit_code != 0) {
        result.status = "acquisition_failed";
        result.summary = "Git clone failed while acquiring the requested project alias.";
        return result;
    }

    result.status = "acquired";
    result.summary = "Acquired the requested project alias into the managed OmniX workspace.";
    result.resolved_source_path = target_root.string();
    result.fetched = true;
    return result;
}

SourceInspection BuildExecutor::inspect_source(const std::filesystem::path& source_path) const {
    SourceInspection inspection;
    inspection.source_path = source_path.string();

    std::error_code ec;
    const std::filesystem::path resolved = std::filesystem::weakly_canonical(source_path, ec);
    const std::filesystem::path effective = ec ? source_path : resolved;
    inspection.resolved_source_path = effective.string();
    inspection.exists = std::filesystem::exists(effective);
    if (!inspection.exists) {
        inspection.summary = "Source path does not exist.";
        return inspection;
    }

    inspection.build_system = detect_build_system(effective, &inspection.detected_files);
    inspection.recommended_modules = recommended_modules(inspection.build_system);
    const std::vector<ToolchainModuleStatus> modules = probe_modules();
    inspection.missing_modules = missing_modules(inspection.build_system, modules, false);
    inspection.ready = is_supported_build_system(inspection.build_system) && inspection.missing_modules.empty();

    if (inspection.build_system == "cmake") {
        inspection.summary = inspection.ready
            ? "CMake project detected and ready for a local OmniX-managed build."
            : "CMake project detected, but the local toolchain is incomplete.";
    } else if (inspection.build_system == "configure") {
        inspection.summary = inspection.ready
            ? "Configure-based project detected and ready for a local OmniX-managed build."
            : "Configure-based project detected, but the local toolchain is incomplete.";
    } else if (inspection.build_system == "make") {
        inspection.summary = inspection.ready
            ? "Make-based project detected and ready for a local OmniX-managed build."
            : "Make-based project detected, but the local toolchain is incomplete.";
    } else if (!inspection.build_system.empty()) {
        inspection.summary = "Detected a build system that OmniX does not execute yet.";
    } else {
        inspection.summary = "No supported build system was detected at the source root.";
    }

    return inspection;
}

BuildExecution BuildExecutor::build_source(const RequestProfile& profile,
                                           const std::optional<ProjectAlias>& alias) const {
    BuildExecution execution;
    const std::filesystem::path requested_source = profile.build_source_path.empty()
        ? std::filesystem::current_path()
        : std::filesystem::path(profile.build_source_path);
    const std::string platform = current_platform();
    const BuildRecipe* recipe = select_recipe(alias, platform, profile.selected_recipe_id);
    const std::vector<ToolchainModuleStatus> modules = probe_modules();
    const std::string project_name = deduce_project_name(profile, alias, requested_source);
    const std::string recipe_id = recipe != nullptr
        ? recipe->id
        : (!profile.selected_recipe_id.empty() ? profile.selected_recipe_id : "generic");

    execution.selected_recipe_id = recipe_id;
    execution.environment_signature = detect_environment_signature(modules, recipe != nullptr ? recipe->build_system : "");
    execution.source_path = requested_source.string();

    const SourceInspection inspection = inspect_source(requested_source);
    execution.resolved_source_path = inspection.resolved_source_path;
    execution.build_system = inspection.build_system;
    execution.detected_files = inspection.detected_files;
    execution.missing_modules = inspection.missing_modules;

    if (!inspection.exists) {
        execution.status = "inspect_failed";
        execution.failure_category = "inspect_failed";
        execution.summary = inspection.summary;
        return execution;
    }

    if (recipe != nullptr && !recipe->build_system.empty() && !inspection.build_system.empty() &&
        recipe->build_system != inspection.build_system) {
        execution.status = "inspect_failed";
        execution.failure_category = "inspect_failed";
        execution.summary = "The resolved source tree does not match the selected portable build recipe.";
        return execution;
    }

    const std::string build_system = !inspection.build_system.empty()
        ? inspection.build_system
        : (recipe != nullptr ? recipe->build_system : std::string());
    execution.build_system = build_system;
    execution.environment_signature = detect_environment_signature(modules, build_system);

    if (!is_supported_build_system(build_system)) {
        execution.status = "unsupported_build_system";
        execution.failure_category = "unsupported_build_system";
        execution.summary = "OmniX currently executes local CMake, configure, or make projects only.";
        return execution;
    }

    const std::vector<std::string> missing = missing_modules(build_system, modules, false);
    execution.missing_modules = missing;
    if (!missing.empty()) {
        execution.status = "preflight_failed";
        execution.failure_category = "preflight_failed";
        execution.summary = "The local toolchain is missing required modules for this build.";
        return execution;
    }

    const std::filesystem::path source_root = inspection.resolved_source_path;
    const std::filesystem::path build_dir = compute_build_dir(profile, project_name, recipe_id);
    const std::filesystem::path install_prefix = compute_install_prefix(profile, project_name, recipe_id);
    const std::filesystem::path log_path = compute_log_path(profile, project_name);
    const ToolchainModuleStatus* cmake = find_module(modules, "cmake");
    const ToolchainModuleStatus* make = find_module(modules, "make");
    const std::vector<std::pair<std::string, std::string>> env_overrides = build_env_overrides(modules);
    const std::string effective_target = !profile.build_target.empty()
        ? profile.build_target
        : (recipe != nullptr ? recipe->default_target : std::string());

    execution.build_dir = build_dir.string();
    execution.log_path = log_path.string();
    if (profile.perform_install) {
        execution.install_prefix = install_prefix.string();
    }

    if (profile.clean_build) {
        std::error_code build_cleanup_error;
        std::filesystem::remove_all(build_dir, build_cleanup_error);
        if (profile.perform_install) {
            std::error_code install_cleanup_error;
            std::filesystem::remove_all(install_prefix, install_cleanup_error);
        }
    }
    std::filesystem::create_directories(build_dir);
    std::filesystem::create_directories(log_path.parent_path());
    if (profile.perform_install) {
        std::filesystem::create_directories(install_prefix);
    }

    std::filesystem::path command_cwd = source_root;
    std::vector<std::vector<std::string>> command_plan;
    std::optional<std::vector<std::string>> install_command;

    if (build_system == "cmake") {
        std::vector<std::string> configure_cmd = {
            cmake != nullptr && !cmake->resolved_path.empty() ? cmake->resolved_path : "cmake",
            "-S",
            source_root.string(),
            "-B",
            build_dir.string(),
            "-DCMAKE_BUILD_TYPE=" + (profile.build_type.empty() ? std::string("Release") : profile.build_type),
            "-DCMAKE_INSTALL_PREFIX=" + install_prefix.string(),
        };
        if (recipe != nullptr && !recipe->configure_arguments.empty()) {
            configure_cmd.insert(configure_cmd.end(), recipe->configure_arguments.begin(), recipe->configure_arguments.end());
        }
        command_plan.push_back(std::move(configure_cmd));

        std::vector<std::string> build_cmd = {
            cmake != nullptr && !cmake->resolved_path.empty() ? cmake->resolved_path : "cmake",
            "--build",
            build_dir.string(),
            "--parallel",
            std::to_string(parallelism()),
        };
        if (!effective_target.empty()) {
            build_cmd.push_back("--target");
            build_cmd.push_back(effective_target);
        }
        command_plan.push_back(build_cmd);

        if (profile.perform_install) {
            install_command = std::vector<std::string>{
                cmake != nullptr && !cmake->resolved_path.empty() ? cmake->resolved_path : "cmake",
                "--install",
                build_dir.string(),
            };
        }
        execution.summary = "Local CMake project configured and built successfully through OmniX.";
    } else if (build_system == "configure") {
        command_cwd = build_dir;
        std::vector<std::string> configure_cmd = {
            (source_root / "configure").string(),
            "--prefix=" + install_prefix.string(),
        };
        if (recipe != nullptr && !recipe->configure_arguments.empty()) {
            configure_cmd.insert(configure_cmd.end(), recipe->configure_arguments.begin(), recipe->configure_arguments.end());
        }
        command_plan.push_back(std::move(configure_cmd));

        std::vector<std::string> build_cmd = {
            make != nullptr && !make->resolved_path.empty() ? make->resolved_path : "make",
            "-j",
            std::to_string(parallelism()),
        };
        if (!effective_target.empty()) {
            build_cmd.push_back(effective_target);
        }
        command_plan.push_back(build_cmd);

        if (profile.perform_install) {
            install_command = std::vector<std::string>{
                make != nullptr && !make->resolved_path.empty() ? make->resolved_path : "make",
                recipe != nullptr && !recipe->install_target.empty() ? recipe->install_target : "install",
            };
        }
        execution.summary = "Local configure-based project configured and built successfully through OmniX.";
    } else if (build_system == "make") {
        std::vector<std::string> build_cmd = {
            make != nullptr && !make->resolved_path.empty() ? make->resolved_path : "make",
            "-j",
            std::to_string(parallelism()),
        };
        if (!effective_target.empty()) {
            build_cmd.push_back(effective_target);
        }
        command_plan.push_back(build_cmd);

        if (profile.perform_install && recipe != nullptr && recipe->supports_install && !recipe->install_target.empty()) {
            install_command = std::vector<std::string>{
                make != nullptr && !make->resolved_path.empty() ? make->resolved_path : "make",
                recipe->install_target,
            };
        }
        execution.summary = "Local make-based project built successfully through OmniX.";
    }

    for (const std::vector<std::string>& command : command_plan) {
        execution.commands.push_back(format_command(command));
    }
    if (install_command.has_value()) {
        execution.commands.push_back(format_command(*install_command));
    }

    std::ofstream log_handle(log_path);
    if (!log_handle) {
        execution.status = "log_open_failed";
        execution.failure_category = "log_open_failed";
        execution.summary = "Unable to create build log file.";
        return execution;
    }

    for (const std::vector<std::string>& command : command_plan) {
        log_handle << "$ " << format_command(command) << '\n';
        log_handle.flush();

        const CommandResult result = run_shell(build_shell_command(command_cwd, command, env_overrides));
        log_handle << result.output << "\n";
        log_handle.flush();
        execution.log_excerpt = tail_lines(log_path, 20);
        if (result.exit_code != 0) {
            execution.status = "build_failed";
            execution.failure_category = "build_failed";
            execution.summary = "Build failed with exit code " + std::to_string(result.exit_code) + ".";
            execution.artifact_hint = build_dir.string();
            return execution;
        }
    }

    if (profile.perform_install) {
        if (install_command.has_value()) {
            log_handle << "$ " << format_command(*install_command) << '\n';
            log_handle.flush();

            const CommandResult result = run_shell(build_shell_command(command_cwd, *install_command, env_overrides));
            log_handle << result.output << "\n";
            log_handle.flush();
            execution.log_excerpt = tail_lines(log_path, 20);
            if (result.exit_code != 0) {
                execution.status = "install_failed";
                execution.failure_category = "install_failed";
                execution.install_status = "install_failed";
                execution.summary = "Install failed with exit code " + std::to_string(result.exit_code) + ".";
                execution.artifact_hint = install_prefix.string();
                return execution;
            }
            execution.install_status = "installed";
        } else if (recipe != nullptr && recipe->copy_artifacts_on_install) {
            std::vector<std::string> copied_paths;
            if (!copy_stage_outputs(source_root, build_dir, install_prefix, *recipe, &copied_paths)) {
                execution.status = "install_failed";
                execution.failure_category = "install_failed";
                execution.install_status = "install_failed";
                execution.summary = "The staged copy step could not materialize the expected install outputs.";
                execution.artifact_hint = install_prefix.string();
                return execution;
            }
            execution.install_status = "copied";
            execution.verified_install_outputs = std::move(copied_paths);
        } else {
            execution.install_status = "not_requested";
        }
    } else {
        execution.install_status = "not_requested";
    }

    const std::vector<std::string> artifact_patterns = recipe != nullptr && !recipe->artifact_patterns.empty()
        ? recipe->artifact_patterns
        : default_artifact_patterns(profile);
    const std::vector<std::filesystem::path> artifact_roots = {
        build_dir,
        build_dir / "src",
        build_dir / "bin",
        source_root,
        source_root / "src",
        source_root / "bin",
    };
    execution.verified_artifacts = stringify_paths(existing_from_patterns(artifact_patterns, artifact_roots));

    if (profile.perform_install && execution.verified_install_outputs.empty()) {
        const std::vector<std::string> install_patterns = recipe != nullptr
            ? recipe->install_output_patterns
            : default_install_output_patterns(profile);
        execution.verified_install_outputs = stringify_paths(existing_from_patterns(install_patterns, {install_prefix}));
    }

    if (execution.verified_artifacts.empty() && execution.verified_install_outputs.empty()) {
        execution.status = "artifact_missing";
        execution.failure_category = "artifact_missing";
        execution.summary = "Build completed, but OmniX could not verify any declared artifacts.";
        execution.artifact_hint = profile.perform_install ? install_prefix.string() : build_dir.string();
        return execution;
    }

    execution.built = true;
    if (profile.perform_install) {
        execution.installed = true;
        execution.status = "installed";
        execution.summary = "Portable build and staged install completed successfully through OmniX.";
        execution.artifact_hint = !execution.verified_install_outputs.empty()
            ? execution.verified_install_outputs.front()
            : execution.verified_artifacts.front();
    } else {
        execution.status = "built";
        execution.summary = "Portable build completed successfully through OmniX.";
        execution.artifact_hint = execution.verified_artifacts.front();
    }
    execution.log_excerpt = tail_lines(log_path, 20);
    return execution;
}

}  // namespace tze
