#include "tze/unix_evidence_parser.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace tze {
namespace {

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

std::vector<std::string> split_lines(std::string_view text, std::size_t max_lines = 128) {
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

std::string summarize_text(std::string_view text, std::size_t max_len = 180) {
    const std::vector<std::string> lines = split_lines(text, 1);
    const std::string summary = lines.empty() ? std::string("No textual content captured.") : trim(lines.front());
    if (summary.size() <= max_len) {
        return summary;
    }
    return summary.substr(0, max_len - 3) + "...";
}

std::size_t line_count(std::string_view text) {
    if (text.empty()) {
        return 0;
    }
    return static_cast<std::size_t>(std::count(text.begin(), text.end(), '\n')) + 1;
}

std::string sanitize_token(std::string_view value) {
    std::string token;
    for (char c : value) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            token.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        } else if (!token.empty() && token.back() != '-') {
            token.push_back('-');
        }
    }
    while (!token.empty() && token.back() == '-') {
        token.pop_back();
    }
    return token.empty() ? "item" : token;
}

std::string make_id(std::string_view prefix, std::string_view seed) {
    const std::size_t hash_value = std::hash<std::string>{}(std::string(seed));
    std::ostringstream out;
    out << prefix << "-" << hash_value;
    return out.str();
}

bool contains_token(const std::vector<std::string>& values, std::string_view candidate) {
    return std::find(values.begin(), values.end(), candidate) != values.end();
}

void append_unique(std::vector<std::string>& values, std::string value) {
    if (std::find(values.begin(), values.end(), value) == values.end()) {
        values.push_back(std::move(value));
    }
}

bool looks_like_json(std::string_view text) {
    const std::string trimmed = trim(text);
    return !trimmed.empty() && (trimmed.front() == '{' || trimmed.front() == '[');
}

std::string detect_command_token(std::string_view source_ref) {
    std::string token = trim(source_ref);
    if (token.empty()) {
        return {};
    }
    const std::size_t first_space = token.find_first_of(" \t\r\n");
    if (first_space != std::string::npos) {
        token = token.substr(0, first_space);
    }
    if (token == "sh" || token == "bash" || token == "zsh") {
        return token;
    }
    std::filesystem::path path(token);
    if (!path.filename().empty()) {
        token = path.filename().string();
    }
    return lowercase(token);
}

std::vector<std::string> extract_json_keys(std::string_view text) {
    std::vector<std::string> keys;
    const std::regex key_pattern("\"([^\"\\n\\\\]+)\"\\s*:");
    const std::string owned(text);
    for (std::sregex_iterator it(owned.begin(), owned.end(), key_pattern), end; it != end; ++it) {
        const std::string key = (*it)[1].str();
        if (!key.empty()) {
            append_unique(keys, key);
        }
        if (keys.size() >= 16) {
            break;
        }
    }
    return keys;
}

std::vector<NormalizedObject> build_primary_objects(const ObservationRecord& observation,
                                                    const std::vector<std::string>& signals) {
    std::vector<NormalizedObject> objects;

    const bool json_like = looks_like_json(observation.raw_content);
    const std::string primary_type = observation.source_kind == "directory"
        ? "directory_snapshot"
        : (json_like ? "json_document" : observation.source_kind == "command" ? "command_output" : "log_document");

    NormalizedObject primary;
    primary.id = observation.id + "-primary";
    primary.case_id = observation.case_id;
    primary.observation_id = observation.id;
    primary.object_type = primary_type;
    primary.title = observation.source_ref;
    primary.summary = observation.summary;
    primary.attributes = {
        "source_kind=" + observation.source_kind,
        "line_count=" + std::to_string(line_count(observation.raw_content)),
        "content_hash=" + observation.content_hash,
    };
    objects.push_back(primary);

    if (!signals.empty()) {
        NormalizedObject signal_object;
        signal_object.id = observation.id + "-signals";
        signal_object.case_id = observation.case_id;
        signal_object.observation_id = observation.id;
        signal_object.object_type = "signal_summary";
        signal_object.title = "Detected signals";
        std::ostringstream summary;
        summary << "Detected deterministic signals: ";
        for (std::size_t index = 0; index < signals.size(); ++index) {
            if (index != 0) {
                summary << ", ";
            }
            summary << signals[index];
        }
        signal_object.summary = summary.str();
        for (const std::string& signal : signals) {
            signal_object.attributes.push_back("signal=" + signal);
        }
        objects.push_back(signal_object);
    }

    return objects;
}

std::vector<NormalizedObject> parse_json_objects(const ObservationRecord& observation) {
    std::vector<NormalizedObject> objects;
    const bool json_like = looks_like_json(observation.raw_content);

    std::size_t json_line_count = 0;
    if (!json_like) {
        for (const std::string& line : split_lines(observation.raw_content, 64)) {
            const std::string trimmed = trim(line);
            if (!trimmed.empty() && (trimmed.front() == '{' || trimmed.front() == '[')) {
                ++json_line_count;
            }
        }
    }

    if (!json_like && json_line_count < 2) {
        return objects;
    }

    const std::vector<std::string> keys = extract_json_keys(observation.raw_content);
    if (!keys.empty()) {
        NormalizedObject key_summary;
        key_summary.id = observation.id + "-json-keys";
        key_summary.case_id = observation.case_id;
        key_summary.observation_id = observation.id;
        key_summary.object_type = "json_field_summary";
        key_summary.title = "JSON field summary";
        std::ostringstream summary;
        summary << "Extracted top-level JSON keys: ";
        for (std::size_t index = 0; index < keys.size(); ++index) {
            if (index != 0) {
                summary << ", ";
            }
            summary << keys[index];
        }
        key_summary.summary = summary.str();
        for (const std::string& key : keys) {
            key_summary.attributes.push_back("key=" + key);
        }
        objects.push_back(std::move(key_summary));
    }

    if (json_line_count >= 2) {
        NormalizedObject stream_summary;
        stream_summary.id = observation.id + "-json-stream";
        stream_summary.case_id = observation.case_id;
        stream_summary.observation_id = observation.id;
        stream_summary.object_type = "json_record_stream";
        stream_summary.title = "JSON record stream";
        stream_summary.summary = "Detected a JSON-like line stream with " + std::to_string(json_line_count) + " candidate records.";
        stream_summary.attributes = {"record_count=" + std::to_string(json_line_count)};
        objects.push_back(std::move(stream_summary));
    }

    return objects;
}

std::vector<NormalizedObject> parse_build_objects(const ObservationRecord& observation) {
    std::vector<NormalizedObject> objects;
    const std::vector<std::string> lines = split_lines(observation.raw_content, 256);
    int error_count = 0;
    int warning_count = 0;
    std::vector<std::string> issues;
    std::vector<std::string> targets;
    bool build_like = false;
    std::regex built_target(R"(Built target ([A-Za-z0-9_.+\-]+))", std::regex::icase);
    std::regex linking_target(R"(Linking [A-Za-z ]+ ([^ ]+))", std::regex::icase);

    for (const std::string& raw_line : lines) {
        const std::string line = trim(raw_line);
        if (line.empty()) {
            continue;
        }
        const std::string lowered = lowercase(line);
        const bool line_is_build = lowered.find("cmake") != std::string::npos ||
            lowered.find("make") != std::string::npos ||
            lowered.find("ninja") != std::string::npos ||
            lowered.find("g++") != std::string::npos ||
            lowered.find("gcc") != std::string::npos ||
            lowered.find("clang") != std::string::npos ||
            lowered.find("build") != std::string::npos ||
            lowered.find("linking ") != std::string::npos ||
            lowered.find("built target") != std::string::npos ||
            lowered.find("undefined reference") != std::string::npos;
        if (!line_is_build) {
            continue;
        }

        build_like = true;
        if (lowered.find("error") != std::string::npos || lowered.find("undefined reference") != std::string::npos) {
            ++error_count;
            if (issues.size() < 6) {
                issues.push_back(line);
            }
        } else if (lowered.find("warn") != std::string::npos) {
            ++warning_count;
            if (issues.size() < 6) {
                issues.push_back(line);
            }
        }

        std::smatch match;
        if (std::regex_search(line, match, built_target) && match.size() > 1) {
            append_unique(targets, match[1].str());
        } else if (std::regex_search(line, match, linking_target) && match.size() > 1) {
            append_unique(targets, match[1].str());
        }
    }

    if (!build_like) {
        return objects;
    }

    NormalizedObject summary;
    summary.id = observation.id + "-build-summary";
    summary.case_id = observation.case_id;
    summary.observation_id = observation.id;
    summary.object_type = "build_log_summary";
    summary.title = "Build log summary";
    summary.summary = "Detected build-oriented output with " + std::to_string(error_count) + " errors and " +
        std::to_string(warning_count) + " warnings.";
    summary.attributes = {
        "error_count=" + std::to_string(error_count),
        "warning_count=" + std::to_string(warning_count),
    };
    for (const std::string& target : targets) {
        summary.attributes.push_back("target=" + target);
    }
    objects.push_back(std::move(summary));

    for (std::size_t index = 0; index < issues.size(); ++index) {
        NormalizedObject issue;
        issue.id = make_id("build-issue", observation.id + "|" + std::to_string(index) + "|" + issues[index]);
        issue.case_id = observation.case_id;
        issue.observation_id = observation.id;
        issue.object_type = "build_issue";
        issue.title = "Build issue";
        issue.summary = issues[index];
        issue.attributes = {
            "severity=" + std::string(lowercase(issues[index]).find("warn") != std::string::npos ? "warning" : "error"),
        };
        objects.push_back(std::move(issue));
    }

    return objects;
}

std::vector<NormalizedObject> parse_ssh_auth_objects(const ObservationRecord& observation) {
    std::vector<NormalizedObject> objects;
    const std::regex event_pattern(R"((Accepted|Failed|Invalid|Disconnected))", std::regex::icase);
    const std::regex user_pattern(R"(for(?: invalid user)? ([A-Za-z0-9_.-]+))", std::regex::icase);
    const std::regex ip_pattern(R"(from ([0-9A-Fa-f:.]+))", std::regex::icase);

    std::size_t count = 0;
    for (const std::string& raw_line : split_lines(observation.raw_content, 96)) {
        const std::string line = trim(raw_line);
        if (line.empty()) {
            continue;
        }
        const std::string lowered = lowercase(line);
        if (lowered.find("ssh") == std::string::npos &&
            lowered.find("sshd") == std::string::npos &&
            lowered.find("auth") == std::string::npos &&
            lowered.find("password") == std::string::npos &&
            lowered.find("login") == std::string::npos &&
            lowered.find("denied") == std::string::npos) {
            continue;
        }

        NormalizedObject event;
        event.id = make_id("ssh-auth", observation.id + "|" + std::to_string(count) + "|" + line);
        event.case_id = observation.case_id;
        event.observation_id = observation.id;
        event.object_type = "ssh_auth_event";
        event.title = "SSH/Auth event";
        event.summary = summarize_text(line, 200);
        std::smatch match;
        if (std::regex_search(line, match, event_pattern) && match.size() > 1 && match[1].matched) {
            event.attributes.push_back("event=" + lowercase(match[1].str()));
        }
        if (std::regex_search(line, match, user_pattern) && match.size() > 1 && match[1].matched) {
            event.attributes.push_back("user=" + match[1].str());
        }
        if (std::regex_search(line, match, ip_pattern) && match.size() > 1 && match[1].matched) {
            event.attributes.push_back("ip=" + match[1].str());
        }
        objects.push_back(std::move(event));
        if (++count >= 6) {
            break;
        }
    }

    return objects;
}

std::vector<NormalizedObject> parse_shell_objects(const ObservationRecord& observation) {
    std::vector<NormalizedObject> objects;
    if (observation.source_kind != "command") {
        return objects;
    }

    const std::string command = detect_command_token(observation.source_ref);
    if (command.empty()) {
        return objects;
    }

    NormalizedObject shell;
    shell.id = observation.id + "-shell-command";
    shell.case_id = observation.case_id;
    shell.observation_id = observation.id;
    shell.object_type = "shell_command_summary";
    shell.title = "Shell command output";
    shell.summary = "Captured shell output from command `" + command + "` with " +
        std::to_string(line_count(observation.raw_content)) + " lines.";
    shell.attributes = {
        "command=" + command,
        "source_ref=" + observation.source_ref,
    };

    const std::regex exit_pattern(R"(exit=([0-9\-]+))");
    std::smatch exit_match;
    if (std::regex_search(observation.summary, exit_match, exit_pattern) && exit_match.size() > 1) {
        shell.attributes.push_back("exit_code=" + exit_match[1].str());
    }
    objects.push_back(std::move(shell));

    std::vector<std::string> assignments;
    const std::regex assign_pattern(R"(^([A-Za-z_][A-Za-z0-9_]*)=(.+)$)");
    for (const std::string& raw_line : split_lines(observation.raw_content, 24)) {
        const std::string line = trim(raw_line);
        std::smatch match;
        if (std::regex_match(line, match, assign_pattern) && match.size() > 2) {
            assignments.push_back(match[1].str() + "=" + summarize_text(match[2].str(), 48));
        }
    }
    if (!assignments.empty()) {
        NormalizedObject env;
        env.id = observation.id + "-shell-assignments";
        env.case_id = observation.case_id;
        env.observation_id = observation.id;
        env.object_type = "shell_assignment_summary";
        env.title = "Shell assignment summary";
        env.summary = "Detected " + std::to_string(assignments.size()) + " assignment-style output lines.";
        for (const std::string& assignment : assignments) {
            env.attributes.push_back("assignment=" + assignment);
        }
        objects.push_back(std::move(env));
    }

    return objects;
}

std::vector<NormalizedObject> parse_tool_output_objects(const ObservationRecord& observation) {
    std::vector<NormalizedObject> objects;
    const std::string command = detect_command_token(observation.source_ref);
    const std::string lowered = lowercase(observation.raw_content);

    std::string tool_name;
    if (command == "nmap" || command == "grep" || command == "sed" || command == "awk" ||
        command == "perl" || command == "ruby" || command == "busybox" || command == "ssh") {
        tool_name = command;
    } else if (lowered.find("nmap version") != std::string::npos || lowered.find("nmap scan report") != std::string::npos) {
        tool_name = "nmap";
    } else if (lowered.find("openssh") != std::string::npos) {
        tool_name = "ssh";
    }

    if (tool_name.empty()) {
        return objects;
    }

    NormalizedObject summary;
    summary.id = observation.id + "-tool-summary";
    summary.case_id = observation.case_id;
    summary.observation_id = observation.id;
    summary.object_type = "tool_output_summary";
    summary.title = "Tool output summary";
    summary.summary = "Detected tool output for `" + tool_name + "`.";
    summary.attributes = {
        "tool=" + tool_name,
        "line_count=" + std::to_string(line_count(observation.raw_content)),
    };
    objects.push_back(std::move(summary));

    if (tool_name == "nmap") {
        const std::size_t host_count = static_cast<std::size_t>(std::count(lowered.begin(), lowered.end(), '\n'));
        std::size_t report_count = 0;
        std::size_t cursor = 0;
        while ((cursor = lowered.find("nmap scan report for", cursor)) != std::string::npos) {
            ++report_count;
            cursor += 20;
        }
        NormalizedObject nmap;
        nmap.id = observation.id + "-nmap-summary";
        nmap.case_id = observation.case_id;
        nmap.observation_id = observation.id;
        nmap.object_type = "nmap_output_summary";
        nmap.title = "Nmap summary";
        nmap.summary = report_count > 0
            ? "Detected Nmap scan output with " + std::to_string(report_count) + " host reports."
            : "Detected Nmap version or banner output.";
        nmap.attributes = {
            "tool=nmap",
            "host_reports=" + std::to_string(report_count),
            "line_count=" + std::to_string(host_count),
        };
        objects.push_back(std::move(nmap));
    }

    return objects;
}

std::vector<NormalizedObject> parse_host_inventory_objects(const ObservationRecord& observation) {
    std::vector<NormalizedObject> objects;
    if (observation.raw_content.find("OMNIX_HOST_REPORT") == std::string::npos) {
        return objects;
    }

    int users = 0;
    int sudoers = 0;
    int mirrors = 0;
    int logs = 0;
    int crons = 0;
    int services = 0;
    int initrds = 0;
    int native_tools = 0;
    std::string platform;

    std::vector<std::string> user_examples;
    std::vector<std::string> package_managers;
    std::vector<std::string> tool_names;

    for (const std::string& raw_line : split_lines(observation.raw_content, 256)) {
        const std::string line = trim(raw_line);
        if (line.empty()) {
            continue;
        }
        if (line.rfind("platform=", 0) == 0) {
            platform = line.substr(9);
        } else if (line.rfind("user=", 0) == 0) {
            ++users;
            if (user_examples.size() < 5) {
                user_examples.push_back(line.substr(5));
            }
        } else if (line.rfind("sudoers_path=", 0) == 0 || line.rfind("sudoers_include=", 0) == 0 ||
                   line.rfind("sudo_rule=", 0) == 0) {
            ++sudoers;
        } else if (line.rfind("mirror_path=", 0) == 0 || line.rfind("mirror_entry=", 0) == 0) {
            ++mirrors;
        } else if (line.rfind("log_path=", 0) == 0 || line.rfind("journal_path=", 0) == 0 ||
                   line.rfind("syslog_pattern=", 0) == 0 || line.rfind("lastlog_path=", 0) == 0 ||
                   line.rfind("lastlog_sample=", 0) == 0) {
            ++logs;
        } else if (line.rfind("cron_path=", 0) == 0 || line.rfind("cron_entry=", 0) == 0 ||
                   line.rfind("cron_job=", 0) == 0) {
            ++crons;
        } else if (line.rfind("systemd_path=", 0) == 0 || line.rfind("systemd_unit=", 0) == 0) {
            ++services;
        } else if (line.rfind("initrd_path=", 0) == 0) {
            ++initrds;
        } else if (line.rfind("package_manager=", 0) == 0) {
            const std::string manager = line.substr(std::string("package_manager=").size());
            package_managers.push_back(manager);
        } else if (line.rfind("native_tool=", 0) == 0) {
            ++native_tools;
            const std::string tool = line.substr(std::string("native_tool=").size());
            tool_names.push_back(tool);
        }
    }

    NormalizedObject summary;
    summary.id = observation.id + "-host-summary";
    summary.case_id = observation.case_id;
    summary.observation_id = observation.id;
    summary.object_type = "host_inventory_summary";
    summary.title = "Host inventory summary";
    summary.summary = "Detected OmniX host inspection output for `" + (platform.empty() ? std::string("unknown") : platform) +
        "` with " + std::to_string(users) + " user entries, " + std::to_string(native_tools) + " native tools, and " +
        std::to_string(logs) + " log-related findings.";
    summary.attributes = {
        "platform=" + platform,
        "user_count=" + std::to_string(users),
        "sudoers_count=" + std::to_string(sudoers),
        "mirror_count=" + std::to_string(mirrors),
        "log_count=" + std::to_string(logs),
        "cron_count=" + std::to_string(crons),
        "service_count=" + std::to_string(services),
        "initrd_count=" + std::to_string(initrds),
        "native_tool_count=" + std::to_string(native_tools),
    };
    objects.push_back(std::move(summary));

    if (!user_examples.empty()) {
        NormalizedObject user_summary;
        user_summary.id = observation.id + "-host-users";
        user_summary.case_id = observation.case_id;
        user_summary.observation_id = observation.id;
        user_summary.object_type = "linux_user_summary";
        user_summary.title = "Linux user summary";
        user_summary.summary = "Sampled " + std::to_string(user_examples.size()) + " user entries from the host report.";
        for (const std::string& user : user_examples) {
            user_summary.attributes.push_back("user=" + user);
        }
        objects.push_back(std::move(user_summary));
    }

    if (!package_managers.empty() || !tool_names.empty()) {
        NormalizedObject runtime_summary;
        runtime_summary.id = observation.id + "-host-runtime";
        runtime_summary.case_id = observation.case_id;
        runtime_summary.observation_id = observation.id;
        runtime_summary.object_type = "linux_runtime_summary";
        runtime_summary.title = "Linux runtime summary";
        runtime_summary.summary = "Detected package-manager and native-tool details in the host report.";
        for (const std::string& manager : package_managers) {
            runtime_summary.attributes.push_back("package_manager=" + manager);
        }
        for (const std::string& tool : tool_names) {
            runtime_summary.attributes.push_back("tool=" + tool);
        }
        objects.push_back(std::move(runtime_summary));
    }

    return objects;
}

}  // namespace

std::vector<std::string> UnixEvidenceParser::detect_signals(std::string_view text, std::string_view source_ref) const {
    const std::string lowered = lowercase(std::string(text) + "\n" + std::string(source_ref));
    std::vector<std::string> signals;
    auto append_if = [&signals, &lowered](std::string_view needle, std::string_view signal) {
        if (lowered.find(needle) != std::string::npos && !contains_token(signals, signal)) {
            signals.push_back(std::string(signal));
        }
    };

    append_if("error", "error");
    append_if("warn", "warning");
    append_if("fail", "failure");
    append_if("critical", "critical");
    append_if("auth", "auth");
    append_if("access", "access");
    append_if("denied", "denied");
    append_if("login", "login");
    append_if("ssh", "ssh");
    append_if("password", "auth");
    append_if("nmap", "nmap");
    append_if("port", "network");
    append_if("tcp", "network");
    append_if("packet", "network");
    append_if("cmake", "build");
    append_if("make", "build");
    append_if("build", "build");
    append_if("compile", "build");
    append_if("linker", "build");
    append_if("undefined reference", "build");
    append_if("json", "json");
    append_if("grep", "tool");
    append_if("sed", "tool");
    append_if("awk", "tool");
    append_if("perl", "tool");
    append_if("ruby", "tool");
    append_if("busybox", "tool");
    append_if("openssh", "ssh");
    append_if("omnix_host_report", "host");
    append_if("user=", "host");
    append_if("sudoers", "access");
    append_if("crontab", "schedule");
    append_if("cron_", "schedule");
    append_if("systemd", "service");
    append_if("initrd", "build");
    append_if("package_manager", "tool");
    return signals;
}

std::vector<NormalizedObject> UnixEvidenceParser::parse(const ObservationRecord& observation) const {
    const std::vector<std::string> signals = detect_signals(observation.raw_content, observation.source_ref);
    std::vector<NormalizedObject> objects = build_primary_objects(observation, signals);

    auto append_objects = [&objects](std::vector<NormalizedObject> extra) {
        objects.insert(objects.end(),
                       std::make_move_iterator(extra.begin()),
                       std::make_move_iterator(extra.end()));
    };

    append_objects(parse_json_objects(observation));
    append_objects(parse_build_objects(observation));
    append_objects(parse_ssh_auth_objects(observation));
    append_objects(parse_shell_objects(observation));
    append_objects(parse_tool_output_objects(observation));
    append_objects(parse_host_inventory_objects(observation));

    if (objects.size() > 24) {
        objects.resize(24);
    }
    return objects;
}

}  // namespace tze
