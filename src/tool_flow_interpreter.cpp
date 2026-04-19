#include "tze/tool_flow_interpreter.hpp"

#include "tze/unix_evidence_parser.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <sys/wait.h>

namespace tze {
namespace {

struct CommandResult {
    int exit_code = 0;
    std::string output;
};

struct BuiltinToolResult {
    ToolInvocationReport invocation;
    std::string produced_artifact;
    std::string next_action;
    std::string case_id;
};

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

std::string lowercase(std::string_view value) {
    std::string lowered;
    lowered.reserve(value.size());
    for (char c : value) {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return lowered;
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

std::string join_lines(const std::vector<std::string>& lines) {
    std::ostringstream out;
    for (std::size_t index = 0; index < lines.size(); ++index) {
        if (index != 0) {
            out << '\n';
        }
        out << lines[index];
    }
    return out.str();
}

std::vector<std::string> split_lines(std::string_view text, std::size_t max_lines = 20) {
    std::vector<std::string> lines;
    std::stringstream stream{std::string(text)};
    for (std::string line; std::getline(stream, line);) {
        if (!line.empty()) {
            lines.push_back(line);
        }
        if (lines.size() >= max_lines) {
            break;
        }
    }
    return lines;
}

std::string summarize_text(std::string_view text, std::size_t max_len = 180) {
    const std::vector<std::string> lines = split_lines(text, 1);
    const std::string summary = lines.empty() ? std::string("No textual content captured.") : trim(lines.front());
    if (summary.size() <= max_len) {
        return summary;
    }
    return summary.substr(0, max_len - 3) + "...";
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

std::string make_id(std::string_view prefix, std::string_view seed) {
    const std::size_t hash_value = std::hash<std::string>{}(std::string(seed));
    std::ostringstream out;
    out << prefix << "-" << hash_value;
    return out.str();
}

std::string join_arguments(const std::vector<std::string>& values) {
    std::ostringstream out;
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) {
            out << ' ';
        }
        out << values[index];
    }
    return out.str();
}

void append_unique(std::vector<std::string>& values, std::string value) {
    if (value.empty()) {
        return;
    }
    if (std::find(values.begin(), values.end(), value) == values.end()) {
        values.push_back(std::move(value));
    }
}

std::string current_platform() {
    if (const char* override = std::getenv("OMNIX_HOST_INSPECT_PLATFORM"); override != nullptr && *override != '\0') {
        return override;
    }
#if defined(__APPLE__)
    return "macos";
#elif defined(__linux__)
    return "linux";
#else
    return "unsupported";
#endif
}

std::filesystem::path host_root() {
    if (const char* root = std::getenv("OMNIX_HOST_INSPECT_ROOT"); root != nullptr && *root != '\0') {
        return std::filesystem::path(root);
    }
    return "/";
}

std::filesystem::path rooted_path(const std::filesystem::path& root, std::string_view absolute_path) {
    std::filesystem::path target(absolute_path);
    if (root == "/" || root.empty()) {
        return target;
    }
    if (target.is_absolute()) {
        target = target.relative_path();
    }
    return root / target;
}

bool exists_at(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

std::vector<std::string> read_nonempty_lines(const std::filesystem::path& path,
                                             std::size_t max_lines = 64,
                                             bool skip_comments = false) {
    std::vector<std::string> lines;
    std::ifstream input(path);
    if (!input) {
        return lines;
    }

    for (std::string line; std::getline(input, line);) {
        const std::string trimmed = trim(line);
        if (trimmed.empty()) {
            continue;
        }
        if (skip_comments && (trimmed[0] == '#' || trimmed[0] == ';')) {
            continue;
        }
        lines.push_back(trimmed);
        if (lines.size() >= max_lines) {
            break;
        }
    }
    return lines;
}

std::optional<std::string> command_path(std::string_view command) {
    const CommandResult result = run_shell("command -v " + std::string(command));
    if (result.exit_code != 0) {
        return std::nullopt;
    }
    const std::string resolved = trim(result.output);
    if (resolved.empty()) {
        return std::nullopt;
    }
    return resolved.substr(0, resolved.find('\n'));
}

std::vector<std::filesystem::path> list_children(const std::filesystem::path& path,
                                                 std::size_t max_entries = 16) {
    std::vector<std::filesystem::path> entries;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || !std::filesystem::is_directory(path, ec)) {
        return entries;
    }
    for (const auto& entry : std::filesystem::directory_iterator(path, ec)) {
        entries.push_back(entry.path());
        if (entries.size() >= max_entries || ec) {
            break;
        }
    }
    std::sort(entries.begin(), entries.end());
    return entries;
}

NativeToolRecord record_from_resolution(const ToolResolution& resolution) {
    NativeToolRecord record;
    record.logical_name = resolution.logical_name;
    record.provider_type = resolution.provider_type;
    record.executable_path = resolution.executable_path;
    record.applet_name = resolution.applet_name;
    record.version_fingerprint = resolution.version_fingerprint;
    record.capability_flags = resolution.capability_flags;
    record.environment_signature = resolution.environment_signature;
    record.discovery_origin = resolution.cache_origin;
    record.last_verified = now_timestamp();
    std::error_code ec;
    if (!resolution.executable_path.empty() && std::filesystem::exists(resolution.executable_path, ec)) {
        const auto size = std::filesystem::file_size(resolution.executable_path, ec);
        record.size_bytes = ec ? 0 : size;
        ec.clear();
        const auto write_time = std::filesystem::last_write_time(resolution.executable_path, ec);
        record.modified_timestamp = ec
            ? 0
            : static_cast<long long>(
                  std::chrono::duration_cast<std::chrono::seconds>(write_time.time_since_epoch()).count());
    }
    return record;
}

std::vector<std::string> analyst_builtin_tools() {
    return {"inspect-log", "inspect-build", "inspect-host", "report-case", "text-pipeline"};
}

bool is_analyst_builtin_tool(std::string_view logical_name) {
    const std::vector<std::string> tools = analyst_builtin_tools();
    return std::find(tools.begin(), tools.end(), logical_name) != tools.end();
}

ToolResolution builtin_resolution(std::string_view logical_name) {
    ToolResolution resolution;
    resolution.requested_name = std::string(logical_name);
    resolution.logical_name = std::string(logical_name);
    resolution.provider_type = "analyst_module";
    resolution.executable_path = "omnix-runtime";
    resolution.environment_signature = "omnix";
    resolution.cache_origin = "builtin_analyst_module";
    resolution.validation_signature = "builtin";
    resolution.found = true;
    resolution.cache_validated = true;
    resolution.summary = "Resolved built-in OmniX analyst module `" + std::string(logical_name) + "`.";
    return resolution;
}

ToolDoctorReport builtin_doctor(std::string_view logical_name,
                                MemorySnapshot& memory,
                                const BuildExecutor& builder,
                                const NativeToolRegistry& tools) {
    ToolDoctorReport report;
    report.logical_name = std::string(logical_name);
    report.status = "builtin_ready";
    report.selected_provider = "analyst_module";
    report.executable_path = "omnix-runtime";
    report.cache_origin = "builtin_analyst_module";

    if (logical_name == "inspect-log") {
        report.summary = "Structured log inspection is built into OmniX and runs locally without external services.";
        report.capability_notes = {
            "Parses JSON, build logs, SSH/auth logs, shell output, and tool output.",
            "Returns deterministic normalized objects and signal summaries.",
        };
        report.recommended_next_command = "Use `omnix tool inspect-log -- <path>` on a local log or captured output file.";
        return report;
    }

    if (logical_name == "inspect-build") {
        report.summary = "Build inspection is built into OmniX and can inspect build logs or source directories.";
        const std::vector<ToolchainModuleStatus> modules = builder.probe_modules();
        for (const ToolchainModuleStatus& module : modules) {
            report.capability_notes.push_back(module.name + ":" + (module.available ? "available" : "missing"));
        }
        report.recommended_next_command = "Use `omnix tool inspect-build -- <path>` on a project root or build log.";
        return report;
    }

    if (logical_name == "inspect-host") {
        report.summary = "Linux-first host inspection is built into OmniX and inventories users, sudoers, package mirrors, logs, lastlog, crontabs, systemd, initrd, and native tools.";
        report.capability_notes = {
            "Generates a saved host report under the OmniX memory root.",
            "Can be analyzed later through the standard Omni analyst case flow.",
            "Prioritizes Linux host structure, but still returns a local report on other platforms.",
        };
        report.recommended_next_command = "Use `omnix tool inspect-host -- --linux` on a fresh Linux box, then `omnix analyze <produced-report>`.";
        return report;
    }

    if (logical_name == "report-case") {
        report.summary = "Case report generation is built into OmniX and writes a portable analyst summary to the OmniX memory root.";
        report.capability_notes = {
            "Includes case metadata, normalized objects, decisions, and related links.",
            "Produces a saved local artifact for later review or transport.",
        };
        report.recommended_next_command = "Use `omnix tool report-case -- <case-id>` to generate a saved analyst report.";
        return report;
    }

    if (logical_name == "text-pipeline") {
        const ToolResolution grep = tools.resolve("grep", memory, false);
        const ToolResolution sed = tools.resolve("sed", memory, false);
        const ToolResolution awk = tools.resolve("awk", memory, false);
        report.summary = "Safe text pipelines are built into OmniX and can compose grep/sed/awk over a local file.";
        report.capability_notes.push_back("grep=" + std::string(grep.found ? grep.provider_type : "missing"));
        report.capability_notes.push_back("sed=" + std::string(sed.found ? sed.provider_type : "missing"));
        report.capability_notes.push_back("awk=" + std::string(awk.found ? awk.provider_type : "missing"));
        report.recommended_next_command =
            "Use `omnix tool text-pipeline -- <path> --grep '<pattern>' [--sed '<expr>'] [--awk '<program>']`.";
        return report;
    }

    report.summary = "Built-in OmniX analyst module available.";
    report.recommended_next_command = "Use `omnix tool " + std::string(logical_name) + " -- ...`.";
    return report;
}

ObservationRecord capture_observation(std::string_view reference) {
    ObservationRecord observation;
    observation.source_ref = std::string(reference);
    observation.collected_at = now_timestamp();

    const std::filesystem::path path(reference);
    std::error_code ec;
    if (std::filesystem::exists(path, ec) && std::filesystem::is_regular_file(path, ec)) {
        observation.source_kind = "file";
        std::ifstream input(path);
        std::ostringstream buffer;
        buffer << input.rdbuf();
        observation.raw_content = buffer.str();
        observation.summary = "Loaded file `" + path.string() + "` for structured inspection.";
    } else if (std::filesystem::exists(path, ec) && std::filesystem::is_directory(path, ec)) {
        observation.source_kind = "directory";
        observation.summary = "Directory references are better suited for `inspect-build`.";
    } else {
        observation.source_kind = "command";
        const CommandResult command = run_shell(reference);
        observation.raw_content = command.output;
        observation.summary = "Captured command output from `" + std::string(reference) + "` (exit=" +
            std::to_string(command.exit_code) + ").";
    }

    observation.content_hash = std::to_string(std::hash<std::string>{}(observation.raw_content));
    observation.id = make_id("tool-observation", observation.source_ref + "|" + observation.content_hash);
    return observation;
}

const CaseRecord* find_case(const MemorySnapshot& memory, std::string_view id_or_source) {
    for (const CaseRecord& entry : memory.case_records) {
        if (entry.id == id_or_source || entry.primary_source == id_or_source) {
            return &entry;
        }
    }
    return nullptr;
}

std::vector<ObservationRecord> observations_for_case(const MemorySnapshot& memory, std::string_view case_id) {
    std::vector<ObservationRecord> entries;
    for (const ObservationRecord& entry : memory.observations) {
        if (entry.case_id == case_id) {
            entries.push_back(entry);
        }
    }
    return entries;
}

std::vector<NormalizedObject> objects_for_case(const MemorySnapshot& memory, std::string_view case_id) {
    std::vector<NormalizedObject> entries;
    for (const NormalizedObject& entry : memory.normalized_objects) {
        if (entry.case_id == case_id) {
            entries.push_back(entry);
        }
    }
    return entries;
}

std::vector<DecisionCandidate> decisions_for_case(const MemorySnapshot& memory, std::string_view case_id) {
    std::vector<DecisionCandidate> entries;
    for (const DecisionCandidate& entry : memory.decision_candidates) {
        if (entry.case_id == case_id) {
            entries.push_back(entry);
        }
    }
    std::stable_sort(entries.begin(), entries.end(), [](const DecisionCandidate& lhs, const DecisionCandidate& rhs) {
        if (lhs.score != rhs.score) {
            return lhs.score > rhs.score;
        }
        if (lhs.probability_likelihood != rhs.probability_likelihood) {
            return lhs.probability_likelihood > rhs.probability_likelihood;
        }
        return lhs.confidence > rhs.confidence;
    });
    return entries;
}

std::vector<CaseLink> links_for_case(const MemorySnapshot& memory, std::string_view case_id) {
    std::vector<CaseLink> entries;
    for (const CaseLink& link : memory.case_links) {
        if (link.left_case_id == case_id || link.right_case_id == case_id) {
            entries.push_back(link);
        }
    }
    std::stable_sort(entries.begin(), entries.end(), [](const CaseLink& lhs, const CaseLink& rhs) {
        return lhs.strength > rhs.strength;
    });
    return entries;
}

BuiltinToolResult invoke_inspect_host(MemorySnapshot& memory,
                                      const NativeToolRegistry& tools,
                                      bool allow_deep_hunt) {
    BuiltinToolResult result;
    result.invocation.logical_name = "inspect-host";
    result.invocation.selected_provider = "analyst_module";
    result.invocation.cache_origin = "builtin_analyst_module";
    result.invocation.command_line = "omnix::inspect-host";

    const std::string platform = current_platform();
    const std::filesystem::path root = host_root();
    const std::filesystem::path report_dir = memory.paths.root / "host-inspection";
    std::filesystem::create_directories(report_dir);
    const std::filesystem::path report_path = report_dir / ("host-" + platform + "-" + now_timestamp() + ".txt");

    std::vector<std::string> report_lines = {
        "OMNIX_HOST_REPORT",
        "platform=" + platform,
        "root=" + root.string(),
    };

    const auto add_path_if_exists = [&](std::string_view label, std::string_view path_string) {
        const std::filesystem::path path = rooted_path(root, path_string);
        if (exists_at(path)) {
            report_lines.push_back(std::string(label) + "=" + path.string());
        }
    };

    const std::filesystem::path passwd_path = rooted_path(root, "/etc/passwd");
    std::size_t user_count = 0;
    if (exists_at(passwd_path)) {
        for (const std::string& line : read_nonempty_lines(passwd_path, 32, false)) {
            if (line.empty() || line[0] == '#') {
                continue;
            }
            std::stringstream fields(line);
            std::string user;
            std::string password;
            std::string uid;
            std::string gid;
            std::string gecos;
            std::string home;
            std::string shell;
            std::getline(fields, user, ':');
            std::getline(fields, password, ':');
            std::getline(fields, uid, ':');
            std::getline(fields, gid, ':');
            std::getline(fields, gecos, ':');
            std::getline(fields, home, ':');
            std::getline(fields, shell, ':');
            if (!user.empty()) {
                report_lines.push_back("user=" + user + " uid=" + uid + " gid=" + gid + " shell=" + shell);
                ++user_count;
            }
            if (user_count >= 12) {
                break;
            }
        }
    }

    const std::filesystem::path sudoers_path = rooted_path(root, "/etc/sudoers");
    std::size_t sudo_rule_count = 0;
    if (exists_at(sudoers_path)) {
        report_lines.push_back("sudoers_path=" + sudoers_path.string());
        for (const std::string& line : read_nonempty_lines(sudoers_path, 12, true)) {
            report_lines.push_back("sudo_rule=" + line);
            if (++sudo_rule_count >= 6) {
                break;
            }
        }
    }
    const std::filesystem::path sudoers_dir = rooted_path(root, "/etc/sudoers.d");
    for (const std::filesystem::path& path : list_children(sudoers_dir, 8)) {
        if (!std::filesystem::is_regular_file(path)) {
            continue;
        }
        report_lines.push_back("sudoers_include=" + path.string());
        for (const std::string& line : read_nonempty_lines(path, 8, true)) {
            report_lines.push_back("sudo_rule=" + line);
            if (++sudo_rule_count >= 10) {
                break;
            }
        }
        if (sudo_rule_count >= 10) {
            break;
        }
    }

    for (std::string_view manager : {"apt-get", "dnf", "yum", "pacman", "rpm", "pkg"}) {
        if (const std::optional<std::string> resolved = command_path(manager); resolved.has_value()) {
            report_lines.push_back("package_manager=" + std::string(manager) + " path=" + *resolved);
        }
    }
    for (std::string_view mirror_path : {"/etc/apt/sources.list", "/etc/pacman.d/mirrorlist", "/etc/zypp/repos.d",
                                         "/etc/yum.repos.d", "/etc/apt/sources.list.d", "/etc/apk/repositories"}) {
        const std::filesystem::path path = rooted_path(root, mirror_path);
        if (!exists_at(path)) {
            continue;
        }
        report_lines.push_back("mirror_path=" + path.string());
        if (std::filesystem::is_regular_file(path)) {
            for (const std::string& line : read_nonempty_lines(path, 4, true)) {
                report_lines.push_back("mirror_entry=" + line);
            }
        } else if (std::filesystem::is_directory(path)) {
            for (const std::filesystem::path& child : list_children(path, 4)) {
                report_lines.push_back("mirror_entry=" + child.string());
            }
        }
    }

    std::vector<std::filesystem::path> log_paths;
    for (std::string_view candidate : {"/var/log/syslog", "/var/log/messages", "/var/log/auth.log", "/var/log/secure",
                                       "/var/log/kern.log", "/var/log/lastlog", "/var/log/wtmp", "/var/log/btmp"}) {
        const std::filesystem::path path = rooted_path(root, candidate);
        if (exists_at(path)) {
            log_paths.push_back(path);
            report_lines.push_back("log_path=" + path.string());
        }
    }
    const std::filesystem::path journal_path = rooted_path(root, "/var/log/journal");
    if (exists_at(journal_path)) {
        report_lines.push_back("journal_path=" + journal_path.string());
    }

    std::map<std::string, int> pattern_counts;
    for (const std::filesystem::path& log_path : log_paths) {
        if (!std::filesystem::is_regular_file(log_path)) {
            continue;
        }
        for (const std::string& line : read_nonempty_lines(log_path, 256, false)) {
            const std::string lowered = lowercase(line);
            if (lowered.find("error") != std::string::npos) {
                ++pattern_counts["error"];
            }
            if (lowered.find("warn") != std::string::npos) {
                ++pattern_counts["warning"];
            }
            if (lowered.find("auth") != std::string::npos) {
                ++pattern_counts["auth"];
            }
            if (lowered.find("sudo") != std::string::npos) {
                ++pattern_counts["sudo"];
            }
            if (lowered.find("ssh") != std::string::npos) {
                ++pattern_counts["ssh"];
            }
            if (lowered.find("fail") != std::string::npos) {
                ++pattern_counts["failure"];
            }
        }
    }
    for (const auto& [name, count] : pattern_counts) {
        report_lines.push_back("syslog_pattern=" + name + " count=" + std::to_string(count));
    }

    const std::filesystem::path lastlog_path = rooted_path(root, "/var/log/lastlog");
    if (exists_at(lastlog_path)) {
        std::error_code ec;
        const auto size = std::filesystem::file_size(lastlog_path, ec);
        report_lines.push_back("lastlog_path=" + lastlog_path.string() + " size=" + std::to_string(ec ? 0 : size));
    }
    if (const std::optional<std::string> lastlog = command_path("lastlog"); lastlog.has_value()) {
        const CommandResult command = run_shell(shell_quote(*lastlog) + " | head -n 6");
        for (const std::string& line : split_lines(command.output, 6)) {
            report_lines.push_back("lastlog_sample=" + line);
        }
    }

    for (std::string_view cron_candidate : {"/etc/crontab", "/etc/cron.d", "/etc/cron.daily", "/etc/cron.hourly",
                                            "/etc/cron.weekly", "/var/spool/cron", "/var/spool/cron/crontabs"}) {
        const std::filesystem::path path = rooted_path(root, cron_candidate);
        if (!exists_at(path)) {
            continue;
        }
        report_lines.push_back("cron_path=" + path.string());
        if (std::filesystem::is_regular_file(path)) {
            for (const std::string& line : read_nonempty_lines(path, 4, true)) {
                report_lines.push_back("cron_job=" + line);
            }
        } else if (std::filesystem::is_directory(path)) {
            for (const std::filesystem::path& child : list_children(path, 6)) {
                report_lines.push_back("cron_entry=" + child.string());
                if (std::filesystem::is_regular_file(child)) {
                    for (const std::string& line : read_nonempty_lines(child, 3, true)) {
                        report_lines.push_back("cron_job=" + line);
                    }
                }
            }
        }
    }

    const std::filesystem::path systemd_path = rooted_path(root, "/etc/systemd/system");
    const std::filesystem::path systemd_lib_path = rooted_path(root, "/usr/lib/systemd/system");
    add_path_if_exists("systemd_path", "/etc/systemd/system");
    add_path_if_exists("systemd_path", "/usr/lib/systemd/system");
    add_path_if_exists("systemd_path", "/lib/systemd/system");
    if (const std::optional<std::string> systemctl = command_path("systemctl"); systemctl.has_value()) {
        const CommandResult command =
            run_shell(shell_quote(*systemctl) + " list-unit-files --type=service --state=enabled --no-pager --no-legend | head -n 10");
        for (const std::string& line : split_lines(command.output, 10)) {
            if (!trim(line).empty()) {
                report_lines.push_back("systemd_unit=" + trim(line));
            }
        }
    } else {
        for (const std::filesystem::path& dir : {systemd_path, systemd_lib_path}) {
            for (const std::filesystem::path& child : list_children(dir, 8)) {
                if (child.extension() == ".service") {
                    report_lines.push_back("systemd_unit=" + child.filename().string());
                }
            }
        }
    }

    for (std::string_view initrd_candidate : {"/boot/initrd.img", "/boot/initrd", "/boot/initramfs-linux.img",
                                              "/boot/initramfs", "/boot/initrd.img-linux"}) {
        const std::filesystem::path path = rooted_path(root, initrd_candidate);
        if (exists_at(path)) {
            report_lines.push_back("initrd_path=" + path.string());
        }
    }
    const std::filesystem::path boot_dir = rooted_path(root, "/boot");
    for (const std::filesystem::path& child : list_children(boot_dir, 12)) {
        const std::string lowered = lowercase(child.filename().string());
        if (lowered.find("initrd") != std::string::npos || lowered.find("initramfs") != std::string::npos) {
            report_lines.push_back("initrd_path=" + child.string());
        }
    }

    for (std::string_view native_name : {"nmap", "tshark", "wireshark", "dumpcap", "ssh", "busybox", "grep", "sed", "awk"}) {
        ToolResolution resolution = tools.resolve(native_name, memory, allow_deep_hunt);
        if (!resolution.found) {
            continue;
        }
        report_lines.push_back("native_tool=" + resolution.logical_name +
                               " provider=" + resolution.provider_type +
                               " path=" + resolution.executable_path);
        if (!resolution.version_fingerprint.empty()) {
            report_lines.push_back("native_tool_version=" + resolution.logical_name + " " + resolution.version_fingerprint);
        }
    }

    const ToolResolution nmap = tools.resolve("nmap", memory, allow_deep_hunt);
    if (nmap.found) {
        report_lines.push_back("recommended_next=omnix tool nmap -- -V");
    } else {
        report_lines.push_back("recommended_next=omnix doctor nmap");
        report_lines.push_back("recommended_next=omnix build nmap");
    }
    report_lines.push_back("recommended_next=omnix analyze " + report_path.string());

    std::ofstream output(report_path);
    for (const std::string& line : report_lines) {
        output << line << "\n";
    }

    result.produced_artifact = report_path.string();
    result.invocation.status = "ok";
    result.invocation.exit_code = 0;
    result.invocation.summary = "Generated a local host inspection report at `" + report_path.string() + "`.";
    result.invocation.output_excerpt = split_lines(join_lines(report_lines), 20);
    result.next_action = nmap.found
        ? "Use `omnix tool nmap -- -V`, or analyze the report with `omnix analyze " + report_path.string() + "`."
        : "Use `omnix doctor nmap`, then analyze the host report with `omnix analyze " + report_path.string() + "`.";
    return result;
}

BuiltinToolResult invoke_inspect_log(const std::vector<std::string>& arguments) {
    BuiltinToolResult result;
    result.invocation.logical_name = "inspect-log";
    result.invocation.selected_provider = "analyst_module";
    result.invocation.cache_origin = "builtin_analyst_module";
    result.invocation.command_line = "omnix::inspect-log";
    if (arguments.empty()) {
        result.invocation.status = "invalid_arguments";
        result.invocation.summary = "`inspect-log` requires a file path or command reference.";
        result.invocation.exit_code = 1;
        return result;
    }

    const std::string reference = join_arguments(arguments);
    result.invocation.command_line += " " + reference;
    const ObservationRecord observation = capture_observation(reference);
    const UnixEvidenceParser parser;
    const std::vector<NormalizedObject> objects = parser.parse(observation);
    const std::vector<std::string> signals = parser.detect_signals(observation.raw_content, observation.source_ref);

    std::vector<std::string> excerpt = {
        "source=" + observation.source_ref,
        "source_kind=" + observation.source_kind,
        "summary=" + observation.summary,
        "normalized_objects=" + std::to_string(objects.size()),
    };
    if (!signals.empty()) {
        excerpt.push_back("signals=" + join_arguments(signals));
    }
    for (const NormalizedObject& object : objects) {
        excerpt.push_back("[" + object.object_type + "] " + object.summary);
        if (excerpt.size() >= 16) {
            break;
        }
    }

    result.invocation.status = "ok";
    result.invocation.summary = "Structured inspection generated " + std::to_string(objects.size()) +
        " normalized object(s) from `" + reference + "`.";
    result.invocation.exit_code = 0;
    result.invocation.output_excerpt = excerpt;
    result.next_action = "Use `omnix analyze " + shell_quote(reference) + "` to persist the same source into a case if needed.";
    return result;
}

BuiltinToolResult invoke_inspect_build(const std::vector<std::string>& arguments, const BuildExecutor& builder) {
    BuiltinToolResult result;
    result.invocation.logical_name = "inspect-build";
    result.invocation.selected_provider = "analyst_module";
    result.invocation.cache_origin = "builtin_analyst_module";
    result.invocation.command_line = "omnix::inspect-build";
    if (arguments.empty()) {
        result.invocation.status = "invalid_arguments";
        result.invocation.summary = "`inspect-build` requires a source directory or build log path.";
        result.invocation.exit_code = 1;
        return result;
    }

    const std::string reference = join_arguments(arguments);
    result.invocation.command_line += " " + reference;
    const std::filesystem::path path(reference);
    std::error_code ec;
    if (std::filesystem::exists(path, ec) && std::filesystem::is_directory(path, ec)) {
        const SourceInspection inspection = builder.inspect_source(path);
        result.invocation.status = "ok";
        result.invocation.summary = inspection.summary;
        result.invocation.exit_code = 0;
        result.invocation.output_excerpt = {
            "build_system=" + inspection.build_system,
            "ready=" + std::string(inspection.ready ? "true" : "false"),
            "resolved=" + inspection.resolved_source_path,
        };
        for (const std::string& file : inspection.detected_files) {
            result.invocation.output_excerpt.push_back("detected=" + file);
            if (result.invocation.output_excerpt.size() >= 16) {
                break;
            }
        }
        for (const std::string& module : inspection.missing_modules) {
            result.invocation.output_excerpt.push_back("missing_module=" + module);
            if (result.invocation.output_excerpt.size() >= 16) {
                break;
            }
        }
        result.next_action = inspection.ready
            ? "Use `omnix preflight " + shell_quote(reference) + "` or `omnix build " + shell_quote(reference) + "`."
            : "Use `omnix doctor " + shell_quote(reference) + "` for package and module guidance.";
        return result;
    }

    const ObservationRecord observation = capture_observation(reference);
    const UnixEvidenceParser parser;
    const std::vector<NormalizedObject> objects = parser.parse(observation);
    std::vector<NormalizedObject> build_objects;
    for (const NormalizedObject& object : objects) {
        if (object.object_type == "build_log_summary" || object.object_type == "build_issue") {
            build_objects.push_back(object);
        }
    }

    result.invocation.status = "ok";
    result.invocation.summary = build_objects.empty()
        ? "No build-oriented structures were detected in `" + reference + "`."
        : "Detected " + std::to_string(build_objects.size()) + " build-oriented normalized object(s) in `" + reference + "`.";
    result.invocation.exit_code = 0;
    for (const NormalizedObject& object : build_objects) {
        result.invocation.output_excerpt.push_back("[" + object.object_type + "] " + object.summary);
        if (result.invocation.output_excerpt.size() >= 16) {
            break;
        }
    }
    result.next_action = build_objects.empty()
        ? "Inspect a build root with `omnix tool inspect-build -- <directory>` or use `omnix analyze <source>`."
        : "Use `omnix preflight <project>` or `omnix build <project>` once the build root is confirmed.";
    return result;
}

BuiltinToolResult invoke_report_case(const std::vector<std::string>& arguments, const MemorySnapshot& memory) {
    BuiltinToolResult result;
    result.invocation.logical_name = "report-case";
    result.invocation.selected_provider = "analyst_module";
    result.invocation.cache_origin = "builtin_analyst_module";
    result.invocation.command_line = "omnix::report-case";
    if (arguments.empty()) {
        result.invocation.status = "invalid_arguments";
        result.invocation.summary = "`report-case` requires a case id or source reference.";
        result.invocation.exit_code = 1;
        return result;
    }

    std::string case_ref;
    std::optional<std::filesystem::path> explicit_output;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        if (arguments[index] == "--out" && index + 1 < arguments.size()) {
            explicit_output = arguments[++index];
            continue;
        }
        if (case_ref.empty()) {
            case_ref = arguments[index];
        }
    }

    if (case_ref.empty()) {
        result.invocation.status = "invalid_arguments";
        result.invocation.summary = "`report-case` requires a case id before any optional flags.";
        result.invocation.exit_code = 1;
        return result;
    }

    result.invocation.command_line += " " + case_ref;
    const CaseRecord* case_record = find_case(memory, case_ref);
    if (case_record == nullptr) {
        result.invocation.status = "case_not_found";
        result.invocation.summary = "No case matched `" + case_ref + "`.";
        result.invocation.exit_code = 1;
        return result;
    }

    const std::vector<ObservationRecord> observations = observations_for_case(memory, case_record->id);
    const std::vector<NormalizedObject> objects = objects_for_case(memory, case_record->id);
    std::vector<EvidenceLink> evidence_links;
    for (const EvidenceLink& entry : memory.evidence_links) {
        if (entry.case_id == case_record->id) {
            evidence_links.push_back(entry);
        }
    }
    const std::vector<DecisionCandidate> decisions = decisions_for_case(memory, case_record->id);
    const std::vector<CaseLink> links = links_for_case(memory, case_record->id);
    std::vector<AnalystComment> comments;
    for (const AnalystComment& entry : memory.analyst_comments) {
        if (entry.case_id == case_record->id) {
            comments.push_back(entry);
        }
    }

    std::filesystem::path output_path = explicit_output.value_or(memory.paths.root / "reports" / (case_record->id + ".txt"));
    if (!output_path.parent_path().empty()) {
        std::filesystem::create_directories(output_path.parent_path());
    }

    const auto find_observation = [&](std::string_view observation_id) -> const ObservationRecord* {
        for (const ObservationRecord& entry : observations) {
            if (entry.id == observation_id) {
                return &entry;
            }
        }
        return nullptr;
    };

    const auto find_object = [&](std::string_view object_id) -> const NormalizedObject* {
        for (const NormalizedObject& entry : objects) {
            if (entry.id == object_id) {
                return &entry;
            }
        }
        return nullptr;
    };

    const DecisionCandidate* top_decision = decisions.empty() ? nullptr : &decisions.front();
    const std::string executive_summary =
        "Case `" + case_record->id + "` is currently `" + case_record->status + "` and centers on `" +
        case_record->primary_source + "`. Omni captured " + std::to_string(observations.size()) +
        " evidence record(s), normalized " + std::to_string(objects.size()) + " object(s), and tracked " +
        std::to_string(links.size()) + " related case link(s)." +
        (top_decision == nullptr
             ? std::string(" No scored follow-up action has been recorded yet.")
             : std::string(" The top next action is `") + top_decision->title + "` with likelihood " +
                   std::to_string(top_decision->probability_likelihood) + " and confidence " +
                   std::to_string(top_decision->confidence) + ".");

    std::ostringstream report_text;
    report_text << "# OmniX Analyst Report\n\n";
    report_text << "- Case id: " << case_record->id << "\n";
    report_text << "- Title: " << case_record->title << "\n";
    report_text << "- Status: " << case_record->status << "\n";
    report_text << "- Source: " << case_record->primary_source << "\n";
    report_text << "- Updated: " << case_record->updated_at << "\n";
    report_text << "- Permission role: " << case_record->permission.role << "\n\n";

    report_text << "## TZE Run Links\n\n";
    if (case_record->created_by_run_id.empty() && case_record->analyzed_by_run_id.empty() &&
        case_record->decided_by_run_id.empty() && case_record->reported_by_run_id.empty()) {
        report_text << "- No TZE run links have been captured for this case yet.\n";
    } else {
        if (!case_record->created_by_run_id.empty()) {
            report_text << "- Created by: " << case_record->created_by_run_id << "\n";
        }
        if (!case_record->analyzed_by_run_id.empty()) {
            report_text << "- Analyzed by: " << case_record->analyzed_by_run_id << "\n";
        }
        if (!case_record->decided_by_run_id.empty()) {
            report_text << "- Decided by: " << case_record->decided_by_run_id << "\n";
        }
        if (!case_record->reported_by_run_id.empty()) {
            report_text << "- Reported by: " << case_record->reported_by_run_id << "\n";
        }
    }

    report_text << "## Executive Summary\n\n";
    report_text << executive_summary << "\n";
    if (!case_record->latest_summary.empty()) {
        report_text << "\nCurrent case summary: " << case_record->latest_summary << "\n";
    }

    report_text << "\n## Evidence List\n\n";
    if (observations.empty()) {
        report_text << "- No observations have been recorded for this case yet.\n";
    }
    for (const ObservationRecord& observation : observations) {
        report_text << "- Evidence `" << observation.id << "`\n";
        report_text << "  - Source kind: " << observation.source_kind << "\n";
        report_text << "  - Source ref: " << observation.source_ref << "\n";
        report_text << "  - Collected: " << observation.collected_at << "\n";
        report_text << "  - Summary: " << observation.summary << "\n";
        if (!observation.content_hash.empty()) {
            report_text << "  - Content hash: " << observation.content_hash << "\n";
        }
    }

    report_text << "\n## Normalized Objects\n\n";
    if (objects.empty()) {
        report_text << "- No normalized objects have been derived for this case yet.\n";
    }
    for (const NormalizedObject& object : objects) {
        report_text << "- Object `" << object.id << "` [" << object.object_type << "] " << object.title << "\n";
        report_text << "  - Summary: " << object.summary << "\n";
        if (!object.attributes.empty()) {
            report_text << "  - Attributes:\n";
            for (const std::string& attribute : object.attributes) {
                report_text << "    - " << attribute << "\n";
            }
        }
    }

    report_text << "\n## Recommended Next Actions\n\n";
    if (decisions.empty()) {
        report_text << "- No decision candidates have been scored for this case yet.\n";
    }
    for (const DecisionCandidate& decision : decisions) {
        report_text << "- Action `" << decision.title << "`\n";
        report_text << "  - Status: " << decision.status << "\n";
        report_text << "  - Score: " << decision.score << "\n";
        report_text << "  - Likelihood: " << decision.probability_likelihood << "\n";
        report_text << "  - Confidence: " << decision.confidence << "\n";
        report_text << "  - Evidence coverage: " << decision.evidence_coverage << "\n";
        report_text << "  - Prior success score: " << decision.prior_success_score << "\n";
        report_text << "  - Validity: " << (decision.valid ? "valid" : "needs-review")
                    << " (" << decision.validity_score << ")\n";
        report_text << "  - Rationale: " << decision.rationale << "\n";
        if (!decision.recommended_command.empty()) {
            report_text << "  - Command: " << decision.recommended_command << "\n";
        }
        if (!decision.validation_checks.empty()) {
            report_text << "  - Validation checks:\n";
            for (const std::string& check : decision.validation_checks) {
                report_text << "    - " << check << "\n";
            }
        }
        if (!decision.operator_feedback.empty()) {
            report_text << "  - Operator feedback: " << decision.operator_feedback;
            if (!decision.feedback_timestamp.empty()) {
                report_text << " @ " << decision.feedback_timestamp;
            }
            report_text << "\n";
            if (!decision.feedback_note.empty()) {
                report_text << "    - Note: " << decision.feedback_note << "\n";
            }
        }
        if (!decision.outcome_status.empty()) {
            report_text << "  - Outcome: " << decision.outcome_status;
            if (!decision.outcome_timestamp.empty()) {
                report_text << " @ " << decision.outcome_timestamp;
            }
            report_text << "\n";
            if (!decision.outcome_note.empty()) {
                report_text << "    - Note: " << decision.outcome_note << "\n";
            }
        }
        if (!decision.supporting_signals.empty()) {
            report_text << "  - Supporting signals:\n";
            for (const std::string& signal : decision.supporting_signals) {
                report_text << "    - " << signal << "\n";
            }
        }
    }

    report_text << "\n## Provenance Trail\n\n";
    if (evidence_links.empty()) {
        report_text << "- No observation-to-object lineage has been recorded yet.\n";
    }
    for (const EvidenceLink& link : evidence_links) {
        const ObservationRecord* observation = find_observation(link.source_observation_id);
        const NormalizedObject* object = find_object(link.target_object_id);
        report_text << "- " << link.relation << ": "
                    << (observation == nullptr ? link.source_observation_id : observation->source_ref)
                    << " -> "
                    << (object == nullptr ? link.target_object_id : object->title)
                    << "\n";
        if (!link.rationale.empty()) {
            report_text << "  - Rationale: " << link.rationale << "\n";
        }
    }

    if (!links.empty()) {
        report_text << "\nRelated case links:\n";
        for (const CaseLink& link : links) {
            report_text << "- [" << link.strength << "] " << link.left_case_id << " <-> " << link.right_case_id
                        << " | " << link.link_type << "=" << link.link_value << "\n";
            if (!link.rationale.empty()) {
                report_text << "  - Rationale: " << link.rationale << "\n";
            }
        }
    }

    if (!comments.empty()) {
        report_text << "\nAnalyst comments:\n";
        for (const AnalystComment& comment : comments) {
            report_text << "- [" << comment.created_at << "] " << comment.author << ": " << comment.text << "\n";
        }
    }

    std::ofstream output(output_path);
    output << report_text.str();

    result.produced_artifact = output_path.string();
    result.case_id = case_record->id;
    result.invocation.status = "ok";
    result.invocation.summary = "Generated analyst report for `" + case_record->id + "` at `" + output_path.string() + "`.";
    result.invocation.exit_code = 0;
    result.invocation.output_excerpt = split_lines(report_text.str(), 18);
    result.next_action = "Open the saved report at `" + output_path.string() + "` or rerun `omnix case " + case_record->id + "`.";
    return result;
}

std::string stage_command(const ToolResolution& resolution,
                          const std::vector<std::string>& arguments) {
    std::ostringstream out;
    if (resolution.provider_type == "busybox_applet") {
        out << shell_quote(resolution.executable_path) << " " << shell_quote(resolution.applet_name);
    } else {
        out << shell_quote(resolution.executable_path);
    }
    for (const std::string& argument : arguments) {
        out << " " << shell_quote(argument);
    }
    return out.str();
}

BuiltinToolResult invoke_text_pipeline(const std::vector<std::string>& arguments,
                                       const NativeToolRegistry& tools,
                                       MemorySnapshot& memory,
                                       bool allow_deep_hunt) {
    BuiltinToolResult result;
    result.invocation.logical_name = "text-pipeline";
    result.invocation.selected_provider = "analyst_module";
    result.invocation.cache_origin = "builtin_analyst_module";
    result.invocation.command_line = "omnix::text-pipeline";
    if (arguments.empty()) {
        result.invocation.status = "invalid_arguments";
        result.invocation.summary = "`text-pipeline` requires a file path followed by one or more stages.";
        result.invocation.exit_code = 1;
        return result;
    }

    const std::string input_path = arguments.front();
    if (!std::filesystem::exists(input_path) || !std::filesystem::is_regular_file(input_path)) {
        result.invocation.status = "invalid_arguments";
        result.invocation.summary = "`text-pipeline` requires a readable file as its first argument.";
        result.invocation.exit_code = 1;
        return result;
    }

    std::string grep_pattern;
    std::string sed_expression;
    std::string awk_program;
    for (std::size_t index = 1; index < arguments.size(); ++index) {
        if (arguments[index] == "--grep" && index + 1 < arguments.size()) {
            grep_pattern = arguments[++index];
        } else if (arguments[index] == "--sed" && index + 1 < arguments.size()) {
            sed_expression = arguments[++index];
        } else if (arguments[index] == "--awk" && index + 1 < arguments.size()) {
            awk_program = arguments[++index];
        }
    }

    if (grep_pattern.empty() && sed_expression.empty() && awk_program.empty()) {
        result.invocation.status = "invalid_arguments";
        result.invocation.summary = "`text-pipeline` requires at least one stage: --grep, --sed, or --awk.";
        result.invocation.exit_code = 1;
        return result;
    }

    std::vector<std::string> stages;
    if (!grep_pattern.empty()) {
        const ToolResolution grep = tools.resolve("grep", memory, allow_deep_hunt);
        if (!grep.found) {
            result.invocation.status = "tool_missing";
            result.invocation.summary = "No grep-compatible provider was found for the `--grep` stage.";
            result.invocation.exit_code = 1;
            return result;
        }
        stages.push_back(stage_command(grep, {"-n", "-E", "--", grep_pattern}));
    }
    if (!sed_expression.empty()) {
        const ToolResolution sed = tools.resolve("sed", memory, allow_deep_hunt);
        if (!sed.found) {
            result.invocation.status = "tool_missing";
            result.invocation.summary = "No sed-compatible provider was found for the `--sed` stage.";
            result.invocation.exit_code = 1;
            return result;
        }
        stages.push_back(stage_command(sed, {"-E", sed_expression}));
    }
    if (!awk_program.empty()) {
        const ToolResolution awk = tools.resolve("awk", memory, allow_deep_hunt);
        if (!awk.found) {
            result.invocation.status = "tool_missing";
            result.invocation.summary = "No awk-compatible provider was found for the `--awk` stage.";
            result.invocation.exit_code = 1;
            return result;
        }
        stages.push_back(stage_command(awk, {awk_program}));
    }

    std::ostringstream command;
    command << stages.front() << " < " << shell_quote(input_path);
    for (std::size_t index = 1; index < stages.size(); ++index) {
        command << " | " << stages[index];
    }

    result.invocation.command_line = command.str();
    const CommandResult shell = run_shell(command.str());
    result.invocation.exit_code = shell.exit_code;
    result.invocation.output_excerpt = split_lines(shell.output, 24);
    result.invocation.status = shell.exit_code == 0 ? "ok" : "command_failed";
    result.invocation.summary = shell.exit_code == 0
        ? "Executed a safe OmniX text pipeline over `" + input_path + "`."
        : "Safe OmniX text pipeline execution failed.";
    result.next_action = "Adjust the stage pattern or add/remove a stage, then rerun `omnix tool text-pipeline`.";
    return result;
}

BuiltinToolResult invoke_builtin_tool(std::string_view logical_name,
                                      const std::vector<std::string>& arguments,
                                      MemorySnapshot& memory,
                                      const BuildExecutor& builder,
                                      const NativeToolRegistry& tools,
                                      bool allow_deep_hunt) {
    if (logical_name == "inspect-host") {
        return invoke_inspect_host(memory, tools, allow_deep_hunt);
    }
    if (logical_name == "inspect-log") {
        return invoke_inspect_log(arguments);
    }
    if (logical_name == "inspect-build") {
        return invoke_inspect_build(arguments, builder);
    }
    if (logical_name == "report-case") {
        return invoke_report_case(arguments, memory);
    }
    if (logical_name == "text-pipeline") {
        return invoke_text_pipeline(arguments, tools, memory, allow_deep_hunt);
    }

    BuiltinToolResult result;
    result.invocation.logical_name = std::string(logical_name);
    result.invocation.status = "tool_missing";
    result.invocation.summary = "Unknown built-in tool `" + std::string(logical_name) + "`.";
    result.invocation.exit_code = 1;
    return result;
}

}  // namespace

void ToolFlowInterpreter::run(const RequestProfile& profile,
                              MemorySnapshot& memory,
                              ProcessingReport& report,
                              const BuildExecutor& builder,
                              const NativeToolRegistry& tools,
                              const MemoryStore& memory_store) const {
    if (profile.tool_mode == ToolCommandMode::List) {
        const std::vector<NativeToolRecord> records = tools.list(memory);
        std::ostringstream out;
        out << "Known native tool inventory:\n";
        for (const NativeToolRecord& record : records) {
            out << " - " << record.logical_name << " => " << record.provider_type
                << " @ " << record.executable_path;
            if (!record.applet_name.empty()) {
                out << " (applet=" << record.applet_name << ")";
            }
            if (!record.version_fingerprint.empty()) {
                out << " [" << record.version_fingerprint << "]";
            }
            out << "\n";
        }
        out << "Analyst modules:\n";
        for (const std::string& tool_name : analyst_builtin_tools()) {
            out << " - " << tool_name << " => analyst_module @ omnix-runtime\n";
        }
        report.answer_status = "tool_inventory";
        report.answer_explanation = out.str();
        report.next_action =
            "Use `omnix tool locate <name>`, `omnix tool doctor <name>`, or `omnix tool <name> -- <args...>`.";
        return;
    }

    const std::string logical_name = tools.canonical_name(profile.requested_tool_name);
    const bool builtin_tool = is_analyst_builtin_tool(logical_name);
    report.resolved_project = logical_name;

    if (builtin_tool && profile.tool_mode == ToolCommandMode::Locate) {
        report.tool_resolution = builtin_resolution(logical_name);
        report.answer_status = "tool_found";
        report.answer_explanation = report.tool_resolution->summary;
        report.next_action = "Use `omnix tool " + logical_name + " -- ...` to run the module.";
        return;
    }

    if (builtin_tool && profile.tool_mode == ToolCommandMode::Doctor) {
        report.tool_doctor_report = builtin_doctor(logical_name, memory, builder, tools);
        report.answer_status = report.tool_doctor_report->status;
        report.answer_explanation = report.tool_doctor_report->summary;
        report.next_action = report.tool_doctor_report->recommended_next_command;
        return;
    }

    if (builtin_tool && profile.tool_mode == ToolCommandMode::Run) {
        const BuiltinToolResult builtin = invoke_builtin_tool(
            logical_name, profile.tool_arguments, memory, builder, tools, true);
        report.tool_invocation_report = builtin.invocation;
        report.tool_resolution = builtin_resolution(logical_name);
        report.answer_status = builtin.invocation.status;
        report.answer_explanation = builtin.invocation.summary;
        if (!builtin.invocation.output_excerpt.empty()) {
            report.answer_explanation += "\n" + join_lines(builtin.invocation.output_excerpt);
        }
        report.next_action = builtin.next_action.empty()
            ? "Reuse the built-in analyst module directly."
            : builtin.next_action;
        if (!builtin.produced_artifact.empty()) {
            report.produced_artifact = builtin.produced_artifact;
            report.memory_writes.push_back(builtin.produced_artifact);
        }
        if (logical_name == "report-case" && !builtin.case_id.empty()) {
            const auto case_match = std::find_if(memory.case_records.begin(),
                                                 memory.case_records.end(),
                                                 [&builtin](const CaseRecord& entry) {
                                                     return entry.id == builtin.case_id;
                                                 });
            if (case_match != memory.case_records.end()) {
                case_match->reported_by_run_id = report.tze_run_id;
                memory_store.remember_case_record(memory, *case_match);
                report.case_record = *case_match;
                report.memory_writes.push_back(memory.paths.cases_path.string());
            }
        }
        if (logical_name == "inspect-host") {
            for (std::string_view native_name : {"nmap", "ssh", "busybox", "grep", "sed", "awk"}) {
                const ToolResolution resolution = tools.resolve(native_name, memory, true);
                if (!resolution.found) {
                    continue;
                }
                memory_store.remember_native_tool(memory, record_from_resolution(resolution));
            }
            report.memory_writes.push_back(memory.paths.native_tools_path.string());
        }
        return;
    }

    if (profile.tool_mode == ToolCommandMode::Locate) {
        const ToolResolution resolution = tools.resolve(logical_name, memory, true);
        report.tool_resolution = resolution;
        report.answer_status = resolution.found ? "native_found" : "tool_missing";
        report.answer_explanation = resolution.summary;
        report.next_action = resolution.found
            ? "Use `omnix tool " + logical_name + " -- --version` to verify the cached provider."
            : "Run `omnix tool doctor " + logical_name + "` for guidance.";
        if (resolution.found) {
            memory_store.remember_native_tool(memory, record_from_resolution(resolution));
            report.memory_writes.push_back(memory.paths.native_tools_path.string());
        }
        return;
    }

    if (profile.tool_mode == ToolCommandMode::Doctor) {
        const ToolDoctorReport doctor = tools.doctor(logical_name, memory, true);
        report.tool_doctor_report = doctor;
        report.answer_status = doctor.status;
        report.answer_explanation = doctor.summary;
        report.next_action = doctor.recommended_next_command;
        if (doctor.status == "native_ready") {
            const ToolResolution resolution = tools.resolve(logical_name, memory, true);
            report.tool_resolution = resolution;
            if (resolution.found) {
                memory_store.remember_native_tool(memory, record_from_resolution(resolution));
                report.memory_writes.push_back(memory.paths.native_tools_path.string());
            }
        }
        return;
    }

    const ToolInvocationReport invocation = tools.invoke(logical_name, profile.tool_arguments, memory, true);
    report.tool_invocation_report = invocation;
    report.answer_status = invocation.status;
    report.answer_explanation = invocation.summary;
    if (!invocation.output_excerpt.empty()) {
        report.answer_explanation += "\n" + join_lines(invocation.output_excerpt);
    }
    report.next_action = invocation.status == "ok"
        ? "Reuse this tool directly or inspect `omnix memory prefs` for the cached provider."
        : "Run `omnix tool doctor " + logical_name + "` to inspect the native provider and guidance.";

    const ToolResolution resolution = tools.resolve(logical_name, memory, true);
    if (resolution.found) {
        report.tool_resolution = resolution;
        memory_store.remember_native_tool(memory, record_from_resolution(resolution));
        report.memory_writes.push_back(memory.paths.native_tools_path.string());
    }
}

}  // namespace tze
