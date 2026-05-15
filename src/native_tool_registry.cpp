#include "tze/native_tool_registry.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <sys/wait.h>

namespace tze {
namespace {

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

bool is_executable_file(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || !std::filesystem::is_regular_file(path, ec)) {
        return false;
    }
    const auto perms = std::filesystem::status(path, ec).permissions();
    if (ec) {
        return false;
    }
    using perms_t = std::filesystem::perms;
    return (perms & perms_t::owner_exec) != perms_t::none ||
        (perms & perms_t::group_exec) != perms_t::none ||
        (perms & perms_t::others_exec) != perms_t::none;
}

std::optional<std::filesystem::path> find_on_path(const std::vector<std::string>& commands) {
    const char* env_path = std::getenv("PATH");
    if (env_path == nullptr) {
        return std::nullopt;
    }

    for (const std::string& directory : split_path(env_path)) {
        for (const std::string& command : commands) {
            const std::filesystem::path candidate = std::filesystem::path(directory) / command;
            if (is_executable_file(candidate)) {
                return candidate;
            }
        }
    }
    return std::nullopt;
}

std::optional<std::filesystem::path> find_in_roots(const std::vector<std::filesystem::path>& roots,
                                                   const std::vector<std::string>& commands) {
    for (const std::filesystem::path& root : roots) {
        for (const std::string& command : commands) {
            const std::filesystem::path candidate = root / command;
            if (is_executable_file(candidate)) {
                return candidate;
            }
        }
    }
    return std::nullopt;
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

std::string now_timestamp() {
    using clock = std::chrono::system_clock;
    const std::time_t raw = clock::to_time_t(clock::now());
    std::tm local{};
#if defined(__APPLE__) || defined(__unix__)
    localtime_r(&raw, &local);
#else
    local = *std::localtime(&raw);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &local);
    return buffer;
}

std::string environment_signature() {
    return current_platform() + "-" + current_architecture();
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
        const std::string lowered = lowercase(first_line);
        if (lowered.find("unknown flag") != std::string::npos || lowered.find("illegal option") != std::string::npos) {
            continue;
        }
        return first_line;
    }
    return std::nullopt;
}

std::optional<std::string> run_help_probe(const std::filesystem::path& executable) {
    const CommandResult result = run_shell(shell_quote(executable.string()) + " -h");
    const std::string output = trim(result.output);
    if (output.empty()) {
        return std::nullopt;
    }
    return trim(output.substr(0, output.find('\n')));
}

long long modified_timestamp(const std::filesystem::path& path) {
    std::error_code ec;
    const auto value = std::filesystem::last_write_time(path, ec);
    if (ec) {
        return 0;
    }
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(value.time_since_epoch()).count();
    return static_cast<long long>(seconds);
}

std::uintmax_t file_size_or_zero(const std::filesystem::path& path) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    return ec ? 0 : size;
}

std::string validation_signature(const std::filesystem::path& path) {
    std::ostringstream out;
    out << "size=" << file_size_or_zero(path) << ",mtime=" << modified_timestamp(path);
    return out.str();
}

std::vector<std::string> split_lines(std::string_view text, std::size_t max_lines = 20) {
    std::vector<std::string> lines;
    std::stringstream stream{std::string(text)};
    for (std::string line; std::getline(stream, line);) {
        lines.push_back(line);
        if (lines.size() >= max_lines) {
            break;
        }
    }
    return lines;
}

std::vector<std::string> tool_commands(std::string_view logical_name) {
    if (logical_name == "awk") {
        return {"awk", "gawk", "mawk"};
    }
    return {std::string(logical_name)};
}

std::vector<std::string> supported_tool_names() {
    return {"nmap", "tshark", "wireshark", "dumpcap", "grep", "sed", "awk", "ruby", "perl", "busybox", "ssh", "regex-search", "deep-grep"};
}

bool is_virtual_tool(std::string_view logical_name) {
    return logical_name == "regex-search" || logical_name == "deep-grep";
}

std::string canonical_tool_name(std::string_view name) {
    std::string lowered = lowercase(trim(name));
    for (char& c : lowered) {
        if (c == '_' || c == ' ') {
            c = '-';
        }
    }
    if (lowered == "pearl") {
        return "perl";
    }
    if (lowered == "t-shark") {
        return "tshark";
    }
    if (lowered == "wire-shark") {
        return "wireshark";
    }
    if (lowered == "regexsearch") {
        return "regex-search";
    }
    if (lowered == "deepgrep") {
        return "deep-grep";
    }
    return lowered;
}

std::vector<std::filesystem::path> common_search_roots() {
    if (const char* env = std::getenv("OMNIX_NATIVE_SEARCH_ROOTS"); env != nullptr && *env != '\0') {
        std::vector<std::filesystem::path> roots;
        for (const std::string& value : split_path(env)) {
            roots.emplace_back(value);
        }
        return roots;
    }

    std::vector<std::filesystem::path> roots = {
        "/bin",
        "/sbin",
        "/usr/bin",
        "/usr/sbin",
        "/usr/local/bin",
        "/usr/local/sbin",
        "/opt",
        "/snap/bin",
    };
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        roots.emplace_back(std::filesystem::path(home) / "bin");
        roots.emplace_back(std::filesystem::path(home) / ".local" / "bin");
    }
    return roots;
}

long long deep_hunt_timeout_seconds() {
    if (const char* env = std::getenv("OMNIX_NATIVE_HUNT_TIMEOUT_SECONDS"); env != nullptr && *env != '\0') {
        try {
            return std::max(1L, std::stol(env));
        } catch (const std::exception&) {
        }
    }
    return 300;
}

bool should_skip_directory(const std::filesystem::path& path) {
    static const std::set<std::string> kExcludedNames = {
        "proc", "sys", "dev", "run", "tmp", "private", "Volumes", "build", "workspaces", "installs",
    };

    const std::string name = path.filename().string();
    return kExcludedNames.find(name) != kExcludedNames.end();
}

std::optional<std::filesystem::path> deep_scan_for_tool(const std::vector<std::filesystem::path>& roots,
                                                        const std::vector<std::string>& commands) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(deep_hunt_timeout_seconds());
    const std::set<std::string> command_set(commands.begin(), commands.end());

    for (const std::filesystem::path& root : roots) {
        std::error_code ec;
        if (!std::filesystem::exists(root, ec)) {
            continue;
        }

        std::filesystem::recursive_directory_iterator it(
            root,
            std::filesystem::directory_options::skip_permission_denied,
            ec);
        std::filesystem::recursive_directory_iterator end;
        for (; !ec && it != end; it.increment(ec)) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return std::nullopt;
            }
            const std::filesystem::path current = it->path();
            if (it->is_directory(ec) && should_skip_directory(current)) {
                it.disable_recursion_pending();
                continue;
            }
            if (!it->is_regular_file(ec)) {
                continue;
            }
            if (command_set.find(current.filename().string()) == command_set.end()) {
                continue;
            }
            if (is_executable_file(current)) {
                return current;
            }
        }
    }
    return std::nullopt;
}

std::optional<std::filesystem::path> validate_record_path(const NativeToolRecord& record) {
    const std::filesystem::path path(record.executable_path);
    if (!is_executable_file(path)) {
        return std::nullopt;
    }
    if (record.size_bytes != 0 && file_size_or_zero(path) != record.size_bytes) {
        return std::nullopt;
    }
    if (record.modified_timestamp != 0 && modified_timestamp(path) != record.modified_timestamp) {
        return std::nullopt;
    }
    return path;
}

NativeToolRecord make_record(std::string_view logical_name,
                             std::string_view provider_type,
                             const std::filesystem::path& executable_path,
                             std::string_view applet_name,
                             std::string_view version,
                             const std::vector<std::string>& capabilities,
                             std::string_view discovery_origin) {
    NativeToolRecord record;
    record.logical_name = std::string(logical_name);
    record.provider_type = std::string(provider_type);
    record.executable_path = executable_path.string();
    record.applet_name = std::string(applet_name);
    record.version_fingerprint = std::string(version);
    record.capability_flags = capabilities;
    record.environment_signature = environment_signature();
    record.size_bytes = file_size_or_zero(executable_path);
    record.modified_timestamp = modified_timestamp(executable_path);
    record.discovery_origin = std::string(discovery_origin);
    record.last_verified = now_timestamp();
    return record;
}

ToolResolution resolution_from_record(std::string_view requested_name,
                                      const NativeToolRecord& record,
                                      std::string_view cache_origin) {
    ToolResolution resolution;
    resolution.requested_name = std::string(requested_name);
    resolution.logical_name = record.logical_name;
    resolution.provider_type = record.provider_type;
    resolution.executable_path = record.executable_path;
    resolution.applet_name = record.applet_name;
    resolution.version_fingerprint = record.version_fingerprint;
    resolution.capability_flags = record.capability_flags;
    resolution.environment_signature = record.environment_signature;
    resolution.cache_origin = std::string(cache_origin);
    resolution.validation_signature = validation_signature(record.executable_path);
    resolution.found = true;
    resolution.cache_validated = cache_origin == "cache_hit";
    resolution.summary = "Resolved `" + record.logical_name + "` via " + std::string(cache_origin) +
        " at " + record.executable_path + ".";
    if (!record.version_fingerprint.empty()) {
        resolution.summary += " " + record.version_fingerprint;
    }
    return resolution;
}

std::vector<std::string> enumerate_busybox_applets(const std::filesystem::path& busybox_path) {
    const CommandResult list_result = run_shell(shell_quote(busybox_path.string()) + " --list");
    if (list_result.exit_code != 0) {
        return {};
    }

    std::vector<std::string> applets;
    for (const std::string& line : split_lines(list_result.output, 512)) {
        const std::string applet = trim(line);
        if (!applet.empty()) {
            applets.push_back(applet);
        }
    }
    return applets;
}

std::optional<NativeToolRecord> resolve_busybox_applet(std::string_view requested_name,
                                                       std::string_view logical_name,
                                                       MemorySnapshot& memory,
                                                       bool allow_deep_hunt) {
    (void)requested_name;
    if (!(logical_name == "grep" || logical_name == "sed" || logical_name == "awk")) {
        return std::nullopt;
    }

    const std::string env = environment_signature();
    for (const NativeToolRecord& entry : memory.native_tools) {
        if (entry.logical_name != "busybox" || entry.environment_signature != env) {
            continue;
        }
        const auto busybox_path = validate_record_path(entry);
        if (!busybox_path.has_value()) {
            continue;
        }
        const std::vector<std::string> applets = enumerate_busybox_applets(*busybox_path);
        if (std::find(applets.begin(), applets.end(), logical_name) != applets.end()) {
            return make_record(logical_name,
                               "busybox_applet",
                               *busybox_path,
                               logical_name,
                               entry.version_fingerprint,
                               {"applet", std::string(logical_name)},
                               "busybox_applet");
        }
    }

    NativeToolRegistry registry;
    ToolResolution busybox = registry.resolve("busybox", memory, allow_deep_hunt);
    if (!busybox.found) {
        return std::nullopt;
    }

    const std::vector<std::string> applets = enumerate_busybox_applets(busybox.executable_path);
    if (std::find(applets.begin(), applets.end(), logical_name) == applets.end()) {
        return std::nullopt;
    }

    return make_record(logical_name,
                       "busybox_applet",
                       busybox.executable_path,
                       logical_name,
                       busybox.version_fingerprint,
                       {"applet", std::string(logical_name)},
                       "busybox_applet");
}

std::optional<NativeToolRecord> find_cached_record(std::string_view logical_name, const MemorySnapshot& memory) {
    const std::string env = environment_signature();
    const NativeToolRecord* best = nullptr;
    for (const NativeToolRecord& entry : memory.native_tools) {
        if (entry.logical_name != logical_name || entry.environment_signature != env) {
            continue;
        }
        if (!validate_record_path(entry).has_value()) {
            continue;
        }
        if (best == nullptr || entry.last_verified > best->last_verified) {
            best = &entry;
        }
    }
    if (best == nullptr) {
        return std::nullopt;
    }
    return *best;
}

ToolResolution make_virtual_resolution(std::string_view requested_name,
                                       std::string_view logical_name,
                                       const ToolResolution& backing) {
    ToolResolution resolution;
    resolution.requested_name = std::string(requested_name);
    resolution.logical_name = std::string(logical_name);
    resolution.provider_type = "virtual_tool";
    resolution.executable_path = backing.executable_path;
    resolution.applet_name = backing.applet_name;
    resolution.version_fingerprint = backing.version_fingerprint;
    resolution.environment_signature = backing.environment_signature;
    resolution.cache_origin = "virtual_tool";
    resolution.validation_signature = backing.validation_signature;
    resolution.found = backing.found;
    resolution.summary = "Resolved virtual OmniX tool `" + std::string(logical_name) +
        "` using provider `" + backing.logical_name + "` at " + backing.executable_path + ".";
    resolution.provider_notes.push_back("Backed by `" + backing.logical_name + "`.");
    return resolution;
}

std::string format_command(const std::vector<std::string>& command) {
    std::ostringstream out;
    for (std::size_t index = 0; index < command.size(); ++index) {
        if (index != 0) {
            out << ' ';
        }
        out << shell_quote(command[index]);
    }
    return out.str();
}

ToolInvocationReport make_not_found_report(std::string_view logical_name) {
    ToolInvocationReport report;
    report.logical_name = std::string(logical_name);
    report.status = "tool_missing";
    report.summary = "No runnable provider was found for `" + std::string(logical_name) + "`.";
    report.exit_code = 1;
    return report;
}

ToolInvocationReport invoke_with_resolution(const ToolResolution& resolution,
                                            const std::vector<std::string>& command_parts) {
    ToolInvocationReport report;
    report.logical_name = resolution.logical_name;
    report.selected_provider = resolution.provider_type;
    report.executable_path = resolution.executable_path;
    report.cache_origin = resolution.cache_origin;
    report.command_line = format_command(command_parts);
    const CommandResult command = run_shell(report.command_line);
    report.exit_code = command.exit_code;
    report.output_excerpt = split_lines(command.output, 24);
    report.status = command.exit_code == 0 ? "ok" : "command_failed";
    report.summary = command.exit_code == 0
        ? "Executed `" + resolution.logical_name + "` successfully."
        : "Execution failed for `" + resolution.logical_name + "`.";
    return report;
}

std::vector<std::string> virtual_backing_tools(std::string_view logical_name) {
    if (logical_name == "regex-search") {
        return {"grep", "perl"};
    }
    if (logical_name == "deep-grep") {
        return {"grep"};
    }
    return {};
}

}  // namespace

std::vector<std::string> NativeToolRegistry::supported_tools() const {
    return supported_tool_names();
}

bool NativeToolRegistry::is_known_tool(std::string_view name) const {
    const std::string canonical = canonical_name(name);
    const std::vector<std::string> names = supported_tool_names();
    return std::find(names.begin(), names.end(), canonical) != names.end();
}

std::string NativeToolRegistry::canonical_name(std::string_view name) const {
    return canonical_tool_name(name);
}

ToolResolution NativeToolRegistry::resolve(std::string_view name,
                                           MemorySnapshot& memory,
                                           bool allow_deep_hunt) const {
    const std::string requested = std::string(trim(name));
    const std::string logical_name = canonical_tool_name(name);
    ToolResolution resolution;
    resolution.requested_name = requested;
    resolution.logical_name = logical_name;
    resolution.environment_signature = environment_signature();

    if (!is_known_tool(logical_name)) {
        resolution.summary = "Unknown tool `" + requested + "`.";
        return resolution;
    }

    if (is_virtual_tool(logical_name)) {
        for (const std::string& backing_name : virtual_backing_tools(logical_name)) {
            ToolResolution backing = resolve(backing_name, memory, allow_deep_hunt);
            if (backing.found) {
                return make_virtual_resolution(requested, logical_name, backing);
            }
        }
        resolution.summary = "No provider was available for virtual OmniX tool `" + logical_name + "`.";
        return resolution;
    }

    if (const std::optional<NativeToolRecord> cached = find_cached_record(logical_name, memory); cached.has_value()) {
        return resolution_from_record(requested, *cached, "cache_hit");
    }

    const std::vector<std::string> commands = tool_commands(logical_name);
    if (const auto on_path = find_on_path(commands); on_path.has_value()) {
        const NativeToolRecord record = make_record(logical_name,
                                                    "native_binary",
                                                    *on_path,
                                                    {},
                                                    read_version(*on_path).value_or(""),
                                                    {"path_lookup"},
                                                    "path_lookup");
        ToolResolution found = resolution_from_record(requested, record, "path_lookup");
        found.candidate_paths.push_back(on_path->string());
        return found;
    }

    const std::vector<std::filesystem::path> roots = common_search_roots();
    if (const auto in_roots = find_in_roots(roots, commands); in_roots.has_value()) {
        const NativeToolRecord record = make_record(logical_name,
                                                    "native_binary",
                                                    *in_roots,
                                                    {},
                                                    read_version(*in_roots).value_or(""),
                                                    {"root_scan"},
                                                    "root_scan");
        ToolResolution found = resolution_from_record(requested, record, "root_scan");
        found.candidate_paths.push_back(in_roots->string());
        return found;
    }

    const bool forced_roots = std::getenv("OMNIX_NATIVE_SEARCH_ROOTS") != nullptr;
    if (allow_deep_hunt && (current_platform() == "linux" || forced_roots)) {
        if (const auto deep_match = deep_scan_for_tool(roots, commands); deep_match.has_value()) {
            const NativeToolRecord record = make_record(logical_name,
                                                        "native_binary",
                                                        *deep_match,
                                                        {},
                                                        read_version(*deep_match).value_or(""),
                                                        {"deep_scan"},
                                                        "deep_scan");
            ToolResolution found = resolution_from_record(requested, record, "deep_scan");
            found.candidate_paths.push_back(deep_match->string());
            return found;
        }
    }

    if (const auto busybox = resolve_busybox_applet(requested, logical_name, memory, allow_deep_hunt); busybox.has_value()) {
        ToolResolution found = resolution_from_record(requested, *busybox, "busybox_applet");
        found.provider_notes.push_back("Resolved via BusyBox applet `" + busybox->applet_name + "`.");
        return found;
    }

    resolution.summary = "No native binary or BusyBox applet was found for `" + logical_name + "`.";
    return resolution;
}

ToolDoctorReport NativeToolRegistry::doctor(std::string_view name,
                                            MemorySnapshot& memory,
                                            bool allow_deep_hunt) const {
    ToolDoctorReport report;
    const ToolResolution resolution = resolve(name, memory, allow_deep_hunt);
    report.logical_name = resolution.logical_name.empty() ? canonical_tool_name(name) : resolution.logical_name;
    if (resolution.found) {
        report.status = "native_ready";
        report.summary = "Native provider available for `" + report.logical_name + "`.";
        report.selected_provider = resolution.provider_type;
        report.executable_path = resolution.executable_path;
        report.cache_origin = resolution.cache_origin;
        report.capability_notes = resolution.capability_flags;
        report.discovered_paths = resolution.candidate_paths;
        report.recommended_next_command = report.logical_name == "nmap"
            ? "Use the native binary now, or run `omnix tool nmap -- -V` to verify it."
            : "Use `omnix tool " + report.logical_name + " -- --version` to verify the provider.";
        if (resolution.provider_type == "busybox_applet") {
            report.busybox_applets.push_back(resolution.applet_name);
        }
        return report;
    }

    report.status = "tool_missing";
    report.summary = resolution.summary.empty()
        ? "No native provider was found."
        : resolution.summary;
    report.missing_tools.push_back(report.logical_name);
    report.recommended_next_command = "Use `omnix doctor " + report.logical_name +
        "` for package guidance or build fallback information.";
    return report;
}

ToolInvocationReport NativeToolRegistry::invoke(std::string_view name,
                                                const std::vector<std::string>& arguments,
                                                MemorySnapshot& memory,
                                                bool allow_deep_hunt) const {
    const std::string logical_name = canonical_tool_name(name);
    if (!is_known_tool(logical_name)) {
        return make_not_found_report(logical_name);
    }

    if (logical_name == "regex-search") {
        if (arguments.size() < 2) {
            ToolInvocationReport report = make_not_found_report(logical_name);
            report.status = "invalid_arguments";
            report.summary = "`regex-search` requires a pattern and one or more paths.";
            return report;
        }
        ToolResolution grep = resolve("grep", memory, allow_deep_hunt);
        if (grep.found) {
            bool has_directory_target = false;
            for (std::size_t index = 1; index < arguments.size(); ++index) {
                std::error_code ec;
                if (std::filesystem::is_directory(arguments[index], ec)) {
                    has_directory_target = true;
                    break;
                }
            }
            std::vector<std::string> command;
            if (grep.provider_type == "busybox_applet") {
                command = {grep.executable_path, grep.applet_name};
            } else {
                command = {grep.executable_path};
            }
            if (has_directory_target) {
                command.push_back("-R");
            }
            command.insert(command.end(), {"-E", "-n", "--"});
            command.insert(command.end(), arguments.begin(), arguments.end());
            ToolInvocationReport report = invoke_with_resolution(make_virtual_resolution(logical_name, logical_name, grep), command);
            report.logical_name = logical_name;
            return report;
        }

        ToolResolution perl = resolve("perl", memory, allow_deep_hunt);
        if (!perl.found) {
            return make_not_found_report(logical_name);
        }
        std::vector<std::string> command = {
            perl.executable_path,
            "-ne",
            "BEGIN{$re=shift @ARGV} print \"$ARGV:$.:$_\" if /$re/",
        };
        command.insert(command.end(), arguments.begin(), arguments.end());
        ToolInvocationReport report = invoke_with_resolution(make_virtual_resolution(logical_name, logical_name, perl), command);
        report.logical_name = logical_name;
        return report;
    }

    if (logical_name == "deep-grep") {
        if (arguments.size() < 2) {
            ToolInvocationReport report = make_not_found_report(logical_name);
            report.status = "invalid_arguments";
            report.summary = "`deep-grep` requires a pattern and a root path.";
            return report;
        }
        ToolResolution grep = resolve("grep", memory, allow_deep_hunt);
        const std::string pattern = arguments.front();
        const std::string root = arguments.back();
        if (grep.found) {
            std::vector<std::string> command;
            if (grep.provider_type == "busybox_applet") {
                command = {grep.executable_path, grep.applet_name, "-R", "-n", "-E", "--", pattern, root};
            } else {
                command = {grep.executable_path, "-R", "-n", "-E", "--", pattern, root};
            }
            ToolInvocationReport report = invoke_with_resolution(make_virtual_resolution(logical_name, logical_name, grep), command);
            report.logical_name = logical_name;
            return report;
        }

        ToolInvocationReport report;
        report.logical_name = logical_name;
        report.selected_provider = "virtual_tool";
        report.cache_origin = "virtual_tool";
        report.command_line = "find " + shell_quote(root) + " -type f -print0 | xargs -0 grep -n -E -- " + shell_quote(pattern);
        const CommandResult result = run_shell(report.command_line);
        report.exit_code = result.exit_code;
        report.output_excerpt = split_lines(result.output, 24);
        report.status = result.exit_code == 0 ? "ok" : "command_failed";
        report.summary = result.exit_code == 0
            ? "Executed `deep-grep` using the `find + grep` fallback."
            : "Fallback `deep-grep` execution failed.";
        return report;
    }

    const ToolResolution resolution = resolve(logical_name, memory, allow_deep_hunt);
    if (!resolution.found) {
        return make_not_found_report(logical_name);
    }

    std::vector<std::string> command;
    if (resolution.provider_type == "busybox_applet") {
        command = {resolution.executable_path, resolution.applet_name};
    } else {
        command = {resolution.executable_path};
    }
    command.insert(command.end(), arguments.begin(), arguments.end());
    return invoke_with_resolution(resolution, command);
}

std::vector<NativeToolRecord> NativeToolRegistry::list(MemorySnapshot& memory) const {
    std::vector<NativeToolRecord> records = memory.native_tools;
    const std::string env = environment_signature();
    std::set<std::string> present;
    for (const NativeToolRecord& record : records) {
        if (record.environment_signature == env) {
            present.insert(record.logical_name);
        }
    }

    for (const std::string& tool : supported_tool_names()) {
        if (is_virtual_tool(tool) || present.find(tool) != present.end()) {
            continue;
        }
        ToolResolution resolution = resolve(tool, memory, false);
        if (!resolution.found) {
            continue;
        }
        records.push_back(make_record(tool,
                                      resolution.provider_type,
                                      resolution.executable_path,
                                      resolution.applet_name,
                                      resolution.version_fingerprint,
                                      resolution.capability_flags,
                                      resolution.cache_origin));
    }

    std::sort(records.begin(), records.end(), [](const NativeToolRecord& lhs, const NativeToolRecord& rhs) {
        if (lhs.logical_name == rhs.logical_name) {
            return lhs.last_verified > rhs.last_verified;
        }
        return lhs.logical_name < rhs.logical_name;
    });
    return records;
}

}  // namespace tze
