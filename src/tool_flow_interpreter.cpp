#include "tze/tool_flow_interpreter.hpp"

#include "tze/unix_evidence_parser.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

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

CommandResult run_command_parts(const std::vector<std::string>& command_parts) {
    CommandResult result;
    if (command_parts.empty()) {
        result.exit_code = -1;
        result.output = "No command was provided.";
        return result;
    }

    int pipe_fd[2];
    if (pipe(pipe_fd) != 0) {
        result.exit_code = -1;
        result.output = "Failed to create command pipe.";
        return result;
    }

    const pid_t child = fork();
    if (child == -1) {
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        result.exit_code = -1;
        result.output = "Failed to fork command process.";
        return result;
    }

    if (child == 0) {
        close(pipe_fd[0]);
        dup2(pipe_fd[1], STDOUT_FILENO);
        dup2(pipe_fd[1], STDERR_FILENO);
        close(pipe_fd[1]);

        std::vector<char*> argv;
        argv.reserve(command_parts.size() + 1);
        for (const std::string& part : command_parts) {
            argv.push_back(const_cast<char*>(part.c_str()));
        }
        argv.push_back(nullptr);
        execv(argv.front(), argv.data());
        _exit(127);
    }

    close(pipe_fd[1]);
    std::array<char, 4096> buffer{};
    ssize_t bytes_read = 0;
    while ((bytes_read = read(pipe_fd[0], buffer.data(), buffer.size())) > 0) {
        result.output.append(buffer.data(), static_cast<std::size_t>(bytes_read));
    }
    close(pipe_fd[0]);

    int status = 0;
    waitpid(child, &status, 0);
    if (WIFEXITED(status)) {
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

std::string escape_json(std::string_view value) {
    std::string escaped;
    for (char c : value) {
        switch (c) {
            case '\\': escaped += "\\\\"; break;
            case '"': escaped += "\\\""; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    std::ostringstream code;
                    code << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                         << static_cast<int>(static_cast<unsigned char>(c));
                    escaped += code.str();
                } else {
                    escaped.push_back(c);
                }
                break;
        }
    }
    return escaped;
}

std::string format_number(double value) {
    if (std::abs(value) < 0.0000005) {
        value = 0.0;
    }
    std::ostringstream out;
    out << std::fixed << std::setprecision(6) << value;
    std::string text = out.str();
    while (text.size() > 1 && text.back() == '0') {
        text.pop_back();
    }
    if (!text.empty() && text.back() == '.') {
        text.pop_back();
    }
    return text.empty() ? "0" : text;
}

std::string render_number_array(const std::vector<double>& values) {
    std::ostringstream out;
    out << "[";
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) {
            out << ",";
        }
        out << format_number(values[index]);
    }
    out << "]";
    return out.str();
}

std::string render_number_matrix(const std::vector<std::vector<double>>& values) {
    std::ostringstream out;
    out << "[";
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) {
            out << ",";
        }
        out << render_number_array(values[index]);
    }
    out << "]";
    return out.str();
}

std::string render_size_array(const std::vector<std::size_t>& values) {
    std::ostringstream out;
    out << "[";
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) {
            out << ",";
        }
        out << values[index];
    }
    out << "]";
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
    return {"inspect-log", "inspect-build", "inspect-host", "report-case", "text-pipeline", "mlp-lens", "thresholds", "symlink", "gg"};
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

    if (logical_name == "mlp-lens") {
        report.summary = "Transformer MLP lens is built into OmniX with demo mode and tiny loaded-tensor-bundle mode.";
        report.capability_notes = {
            "Traces MLP(x) = W2 * activation(W1 * x + b1) + b2.",
            "Demo mode uses deterministic toy embeddings and demo weights for teaching.",
            "Tensor bundle mode loads tokenizer-like metadata plus W1/b1/W2/b2 from disk.",
            "Loaded tensor bundle mode is real external tensor tracing, but not a full LLM or production model trace.",
            "Future inactive adapter targets: safetensors, ONNX, GGUF, extracted small transformer weights.",
        };
        report.recommended_next_command =
            "Use `omnix tool mlp-lens -- --tensor-bundle res/mlp_lens/tiny_mlp_bundle.json \"Michael Jordan plays basketball\"`.";
        return report;
    }

    if (logical_name == "thresholds") {
        report.summary = "Proactive Infrastructure Thresholds is built into OmniX for local-first operational triage.";
        report.capability_notes = {
            "Evaluates alarm scenarios using assets, signals, ownership maps, baselines, seasonal overrides, error signatures, decision policies, runbooks, and evidence bundles.",
            "Starts with RabbitMQ Queue XXB, but the model is global enough for apps, DBs, mounts, storage, hosts, CI/CD, and outage-window workflows.",
            "GSMg mode evaluates generic assets[], signals[], policies[], and runbooks[] so RabbitMQ, DB connection pools, mounts, ingress, queues, and outage-window gates share one signal ground.",
            "Generates machine-readable evidence JSON plus Jira-ready Markdown without external APIs.",
            "Default mode recommends only; `--execute` requires exact interactive confirmation before an allowlisted local runbook action can run.",
        };
        report.recommended_next_command =
            "Use `omnix tool thresholds -- gsmg res/thresholds/gsmg-rabbitmq-xxb.json` or `omnix tool thresholds -- evaluate res/thresholds/rabbitmq-xxb-incident.json`.";
        return report;
    }

    if (logical_name == "gg") {
        report.summary = "Ghostline Gate is vendored into OmniX as `gg` for explicit queue/packet transit observation and mutation workflows.";
        report.capability_notes = {
            "Safe Original Priority: original delivery wins unless modified release is proven safe.",
            "Active mutation today: raw-live, byte-window, and MQTT PUBLISH reframe paths.",
            "RabbitMQ, AMQP, Kafka, ActiveMQ, and Azure Service Bus are detection/audit profiles until protocol-owned mutation is implemented.",
            "Bridge mode converts Ghostline audit JSONL into TView-compatible `omnix.tview.packet.v1` evidence for neural routing.",
            "No mutation happens from doctor, search, audit, or actions; mutation requires explicit `gg run` arguments.",
        };
        report.recommended_next_command =
            "Use `omnix gg audit res/ghostline/ghostline_audit_fixture.jsonl --out /tmp/gg-tview.jsonl` or `omnix gg run <listen-port> <upstream-host> <upstream-port> ...`.";
        return report;
    }

    if (logical_name == "symlink") {
        report.summary = "Filesystem symlink and namespace-shim creation is built into OmniX for local operator convenience.";
        report.capability_notes = {
            "Creates filesystem symlinks with `create <target> <link-path>` using C++ filesystem APIs.",
            "Writes small POSIX shell namespace shims with `shim <name> <namespace>` for commands such as `tze -> omnix tze`.",
            "Refuses to replace existing paths unless `--force` is explicit.",
            "Does not execute generated shell scripts during creation.",
        };
        report.recommended_next_command =
            "Use `omnix tool symlink -- create ./build/omnix ~/.local/bin/omnix` or `omnix tool symlink -- shim tze tze --prefix ~/.local/bin --bin ./build/omnix`.";
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

double dot_product(const std::vector<double>& a, const std::vector<double>& b) {
    double total = 0.0;
    const std::size_t size = std::min(a.size(), b.size());
    for (std::size_t index = 0; index < size; ++index) {
        total += a[index] * b[index];
    }
    return total;
}

double vector_norm(const std::vector<double>& values) {
    return std::sqrt(dot_product(values, values));
}

std::vector<double> mat_vec(const std::vector<std::vector<double>>& matrix, const std::vector<double>& input) {
    std::vector<double> output;
    output.reserve(matrix.size());
    for (const std::vector<double>& row : matrix) {
        output.push_back(dot_product(row, input));
    }
    return output;
}

std::vector<double> add_vectors(std::vector<double> lhs, const std::vector<double>& rhs) {
    const std::size_t size = std::min(lhs.size(), rhs.size());
    for (std::size_t index = 0; index < size; ++index) {
        lhs[index] += rhs[index];
    }
    return lhs;
}

double gelu_activation(double value) {
    const double c = std::sqrt(2.0 / 3.14159265358979323846);
    return 0.5 * value * (1.0 + std::tanh(c * (value + 0.044715 * value * value * value)));
}

std::vector<double> softmax_values(const std::vector<double>& logits) {
    if (logits.empty()) {
        return {};
    }
    const double max_value = *std::max_element(logits.begin(), logits.end());
    std::vector<double> exps;
    exps.reserve(logits.size());
    double denom = 0.0;
    for (double value : logits) {
        const double exp_value = std::exp(value - max_value);
        exps.push_back(exp_value);
        denom += exp_value;
    }
    if (denom == 0.0) {
        return std::vector<double>(logits.size(), 0.0);
    }
    for (double& value : exps) {
        value /= denom;
    }
    return exps;
}

std::vector<double> toy_embedding(std::string_view text) {
    std::vector<double> values = {0.0, 0.0, 0.0, 0.0};
    for (std::size_t index = 0; index < text.size(); ++index) {
        const unsigned char c = static_cast<unsigned char>(text[index]);
        values[index % values.size()] += (static_cast<double>(c % 31) - 15.0) / 15.0;
    }
    const double denom = vector_norm(values);
    if (denom == 0.0) {
        return values;
    }
    for (double& value : values) {
        value /= denom;
    }
    return values;
}

std::string render_string_array_json(const std::vector<std::string>& values) {
    std::ostringstream out;
    out << "[";
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) {
            out << ",";
        }
        out << "\"" << escape_json(values[index]) << "\"";
    }
    out << "]";
    return out.str();
}

std::string render_shape_json(const std::vector<std::size_t>& values) {
    std::ostringstream out;
    out << "[";
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) {
            out << ",";
        }
        out << values[index];
    }
    out << "]";
    return out.str();
}

struct MlpJsonValue {
    enum class Type {
        Null,
        Boolean,
        Number,
        String,
        Array,
        Object,
    };

    Type type = Type::Null;
    bool boolean_value = false;
    double number_value = 0.0;
    std::string string_value;
    std::vector<MlpJsonValue> array_values;
    std::map<std::string, MlpJsonValue> object_values;
};

class MlpJsonParser {
public:
    explicit MlpJsonParser(std::string_view text) : text_(text) {}

    MlpJsonValue parse() {
        MlpJsonValue value = parse_value();
        skip_space();
        if (cursor_ != text_.size()) {
            fail("unexpected trailing JSON content");
        }
        return value;
    }

private:
    [[noreturn]] void fail(std::string_view message) const {
        throw std::runtime_error("tensor bundle JSON parse error: " + std::string(message));
    }

    void skip_space() {
        while (cursor_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[cursor_]))) {
            ++cursor_;
        }
    }

    bool consume(char expected) {
        skip_space();
        if (cursor_ < text_.size() && text_[cursor_] == expected) {
            ++cursor_;
            return true;
        }
        return false;
    }

    void expect_literal(std::string_view literal) {
        if (text_.substr(cursor_, literal.size()) != literal) {
            fail("expected `" + std::string(literal) + "`");
        }
        cursor_ += literal.size();
    }

    MlpJsonValue parse_value() {
        skip_space();
        if (cursor_ >= text_.size()) {
            fail("unexpected end of input");
        }
        const char c = text_[cursor_];
        if (c == '"') {
            MlpJsonValue value;
            value.type = MlpJsonValue::Type::String;
            value.string_value = parse_string();
            return value;
        }
        if (c == '{') {
            return parse_object();
        }
        if (c == '[') {
            return parse_array();
        }
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) {
            return parse_number();
        }
        if (text_.substr(cursor_, 4) == "true") {
            cursor_ += 4;
            MlpJsonValue value;
            value.type = MlpJsonValue::Type::Boolean;
            value.boolean_value = true;
            return value;
        }
        if (text_.substr(cursor_, 5) == "false") {
            cursor_ += 5;
            MlpJsonValue value;
            value.type = MlpJsonValue::Type::Boolean;
            value.boolean_value = false;
            return value;
        }
        if (text_.substr(cursor_, 4) == "null") {
            cursor_ += 4;
            return {};
        }
        fail("unsupported JSON value");
    }

    std::string parse_string() {
        if (!consume('"')) {
            fail("expected string");
        }
        std::string value;
        while (cursor_ < text_.size()) {
            const char c = text_[cursor_++];
            if (c == '"') {
                return value;
            }
            if (c != '\\') {
                value.push_back(c);
                continue;
            }
            if (cursor_ >= text_.size()) {
                fail("unterminated escape sequence");
            }
            const char escaped = text_[cursor_++];
            switch (escaped) {
                case '"': value.push_back('"'); break;
                case '\\': value.push_back('\\'); break;
                case '/': value.push_back('/'); break;
                case 'b': value.push_back('\b'); break;
                case 'f': value.push_back('\f'); break;
                case 'n': value.push_back('\n'); break;
                case 'r': value.push_back('\r'); break;
                case 't': value.push_back('\t'); break;
                default:
                    fail("unsupported string escape");
            }
        }
        fail("unterminated string");
    }

    MlpJsonValue parse_number() {
        const std::size_t start = cursor_;
        if (cursor_ < text_.size() && text_[cursor_] == '-') {
            ++cursor_;
        }
        while (cursor_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[cursor_]))) {
            ++cursor_;
        }
        if (cursor_ < text_.size() && text_[cursor_] == '.') {
            ++cursor_;
            while (cursor_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[cursor_]))) {
                ++cursor_;
            }
        }
        if (cursor_ < text_.size() && (text_[cursor_] == 'e' || text_[cursor_] == 'E')) {
            ++cursor_;
            if (cursor_ < text_.size() && (text_[cursor_] == '+' || text_[cursor_] == '-')) {
                ++cursor_;
            }
            while (cursor_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[cursor_]))) {
                ++cursor_;
            }
        }
        MlpJsonValue value;
        value.type = MlpJsonValue::Type::Number;
        value.number_value = std::stod(std::string(text_.substr(start, cursor_ - start)));
        return value;
    }

    MlpJsonValue parse_array() {
        if (!consume('[')) {
            fail("expected array");
        }
        MlpJsonValue value;
        value.type = MlpJsonValue::Type::Array;
        if (consume(']')) {
            return value;
        }
        while (true) {
            value.array_values.push_back(parse_value());
            if (consume(']')) {
                return value;
            }
            if (!consume(',')) {
                fail("expected array comma");
            }
        }
    }

    MlpJsonValue parse_object() {
        if (!consume('{')) {
            fail("expected object");
        }
        MlpJsonValue value;
        value.type = MlpJsonValue::Type::Object;
        if (consume('}')) {
            return value;
        }
        while (true) {
            skip_space();
            const std::string key = parse_string();
            if (!consume(':')) {
                fail("expected object colon");
            }
            value.object_values[key] = parse_value();
            if (consume('}')) {
                return value;
            }
            if (!consume(',')) {
                fail("expected object comma");
            }
        }
    }

    std::string_view text_;
    std::size_t cursor_ = 0;
};

const MlpJsonValue& required_object_field(const MlpJsonValue& object,
                                          std::string_view key) {
    if (object.type != MlpJsonValue::Type::Object) {
        throw std::runtime_error("tensor bundle field lookup expected an object");
    }
    const auto found = object.object_values.find(std::string(key));
    if (found == object.object_values.end()) {
        throw std::runtime_error("tensor bundle is missing required field `" + std::string(key) + "`");
    }
    return found->second;
}

std::optional<std::reference_wrapper<const MlpJsonValue>> optional_object_field(const MlpJsonValue& object,
                                                                                std::string_view key) {
    if (object.type != MlpJsonValue::Type::Object) {
        return std::nullopt;
    }
    const auto found = object.object_values.find(std::string(key));
    if (found == object.object_values.end()) {
        return std::nullopt;
    }
    return std::cref(found->second);
}

std::string json_string_field(const MlpJsonValue& object, std::string_view key) {
    const MlpJsonValue& value = required_object_field(object, key);
    if (value.type != MlpJsonValue::Type::String) {
        throw std::runtime_error("tensor bundle field `" + std::string(key) + "` must be a string");
    }
    return value.string_value;
}

std::vector<std::string> json_string_array_field(const MlpJsonValue& object, std::string_view key) {
    const MlpJsonValue& value = required_object_field(object, key);
    if (value.type != MlpJsonValue::Type::Array) {
        throw std::runtime_error("tensor bundle field `" + std::string(key) + "` must be an array");
    }
    std::vector<std::string> result;
    result.reserve(value.array_values.size());
    for (const MlpJsonValue& entry : value.array_values) {
        if (entry.type != MlpJsonValue::Type::String) {
            throw std::runtime_error("tensor bundle field `" + std::string(key) + "` must contain strings");
        }
        result.push_back(entry.string_value);
    }
    return result;
}

std::vector<double> json_number_array(const MlpJsonValue& value, std::string_view field_name) {
    if (value.type != MlpJsonValue::Type::Array) {
        throw std::runtime_error("tensor bundle field `" + std::string(field_name) + "` must be a numeric array");
    }
    std::vector<double> result;
    result.reserve(value.array_values.size());
    for (const MlpJsonValue& entry : value.array_values) {
        if (entry.type != MlpJsonValue::Type::Number) {
            throw std::runtime_error("tensor bundle field `" + std::string(field_name) + "` must contain numbers");
        }
        result.push_back(entry.number_value);
    }
    return result;
}

std::vector<double> json_number_array_field(const MlpJsonValue& object, std::string_view key) {
    return json_number_array(required_object_field(object, key), key);
}

std::vector<std::vector<double>> json_number_matrix_field(const MlpJsonValue& object, std::string_view key) {
    const MlpJsonValue& value = required_object_field(object, key);
    if (value.type != MlpJsonValue::Type::Array) {
        throw std::runtime_error("tensor bundle field `" + std::string(key) + "` must be a matrix");
    }
    std::vector<std::vector<double>> result;
    result.reserve(value.array_values.size());
    for (const MlpJsonValue& row : value.array_values) {
        result.push_back(json_number_array(row, key));
    }
    return result;
}

struct MlpTokenizerSpec {
    std::string source = "toy_embedding";
    std::vector<std::string> vocabulary;
    std::map<std::string, std::size_t> token_to_id;
    std::string unknown_token = "<unk>";
    std::size_t unknown_id = 0;
    std::vector<std::vector<double>> embeddings;
};

struct MlpLayerSpec {
    std::vector<std::vector<double>> w1;
    std::vector<double> b1;
    std::vector<std::vector<double>> w2;
    std::vector<double> b2;
    std::string activation = "gelu";
};

struct MlpTensorBundle {
    std::string path;
    std::string format;
    std::string model_name;
    std::string model_source;
    MlpTokenizerSpec tokenizer;
    MlpLayerSpec layer;
    std::vector<std::string> output_labels;
};

void validate_matrix_width(const std::vector<std::vector<double>>& matrix,
                           std::size_t width,
                           std::string_view name) {
    if (matrix.empty()) {
        throw std::runtime_error("tensor bundle `" + std::string(name) + "` must not be empty");
    }
    for (const std::vector<double>& row : matrix) {
        if (row.size() != width) {
            throw std::runtime_error("tensor bundle `" + std::string(name) + "` has inconsistent dimensions");
        }
    }
}

MlpTensorBundle load_mlp_tensor_bundle(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("unable to open tensor bundle `" + path.string() + "`");
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    const MlpJsonValue root = MlpJsonParser(buffer.str()).parse();
    if (root.type != MlpJsonValue::Type::Object) {
        throw std::runtime_error("tensor bundle root must be a JSON object");
    }

    MlpTensorBundle bundle;
    bundle.path = path.string();
    bundle.format = json_string_field(root, "format");
    bundle.model_name = json_string_field(root, "modelName");
    bundle.model_source = json_string_field(root, "modelSource");
    bundle.output_labels = json_string_array_field(root, "outputLabels");

    const MlpJsonValue& tokenizer = required_object_field(root, "tokenizer");
    bundle.tokenizer.source = json_string_field(tokenizer, "source");
    bundle.tokenizer.unknown_token = json_string_field(tokenizer, "unknownToken");
    bundle.tokenizer.vocabulary = json_string_array_field(tokenizer, "vocabulary");
    bundle.tokenizer.embeddings = json_number_matrix_field(tokenizer, "embeddings");

    if (bundle.tokenizer.vocabulary.empty()) {
        throw std::runtime_error("tensor bundle vocabulary must not be empty");
    }
    if (bundle.tokenizer.vocabulary.size() != bundle.tokenizer.embeddings.size()) {
        throw std::runtime_error("tensor bundle vocabulary and embedding row counts differ");
    }
    const std::size_t embedding_width = bundle.tokenizer.embeddings.front().size();
    if (embedding_width == 0) {
        throw std::runtime_error("tensor bundle embeddings must have a non-zero width");
    }
    validate_matrix_width(bundle.tokenizer.embeddings, embedding_width, "embeddings");

    for (std::size_t index = 0; index < bundle.tokenizer.vocabulary.size(); ++index) {
        bundle.tokenizer.token_to_id[lowercase(bundle.tokenizer.vocabulary[index])] = index;
    }
    const auto unknown = bundle.tokenizer.token_to_id.find(lowercase(bundle.tokenizer.unknown_token));
    if (unknown == bundle.tokenizer.token_to_id.end()) {
        throw std::runtime_error("tensor bundle unknownToken must exist in vocabulary");
    }
    bundle.tokenizer.unknown_id = unknown->second;

    const MlpJsonValue& layer = required_object_field(root, "mlp");
    bundle.layer.activation = lowercase(json_string_field(layer, "activation"));
    if (bundle.layer.activation != "relu" && bundle.layer.activation != "gelu" && bundle.layer.activation != "linear") {
        throw std::runtime_error("tensor bundle activation must be relu, gelu, or linear");
    }
    bundle.layer.w1 = json_number_matrix_field(layer, "W1");
    bundle.layer.b1 = json_number_array_field(layer, "b1");
    bundle.layer.w2 = json_number_matrix_field(layer, "W2");
    bundle.layer.b2 = json_number_array_field(layer, "b2");

    validate_matrix_width(bundle.layer.w1, embedding_width, "W1");
    if (bundle.layer.b1.size() != bundle.layer.w1.size()) {
        throw std::runtime_error("tensor bundle b1 length must match W1 row count");
    }
    validate_matrix_width(bundle.layer.w2, bundle.layer.w1.size(), "W2");
    if (bundle.layer.b2.size() != bundle.layer.w2.size()) {
        throw std::runtime_error("tensor bundle b2 length must match W2 row count");
    }
    if (!bundle.output_labels.empty() && bundle.output_labels.size() != bundle.layer.b2.size()) {
        throw std::runtime_error("tensor bundle outputLabels length must match output width");
    }
    return bundle;
}

struct MlpLensInput {
    std::string prompt;
    std::string tensor_bundle_path;
};

MlpLensInput parse_mlp_lens_input(const std::vector<std::string>& arguments) {
    MlpLensInput parsed;
    std::vector<std::string> prompt_parts;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        if (arguments[index] == "--tensor-bundle") {
            if (index + 1 >= arguments.size()) {
                throw std::runtime_error("`--tensor-bundle` requires a path");
            }
            parsed.tensor_bundle_path = arguments[++index];
            continue;
        }
        prompt_parts.push_back(arguments[index]);
    }
    if (parsed.tensor_bundle_path.empty()) {
        if (const char* env_path = std::getenv("OMNIX_MLP_LENS_TENSOR_BUNDLE");
            env_path != nullptr && *env_path != '\0') {
            parsed.tensor_bundle_path = env_path;
        }
    }
    parsed.prompt = trim(join_arguments(prompt_parts));
    return parsed;
}

std::vector<std::string> tokenize_for_bundle(std::string_view prompt) {
    std::vector<std::string> tokens;
    std::string current;
    for (char c : prompt) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc)) {
            current.push_back(static_cast<char>(std::tolower(uc)));
            continue;
        }
        if (!current.empty()) {
            tokens.push_back(current);
            current.clear();
        }
    }
    if (!current.empty()) {
        tokens.push_back(current);
    }
    return tokens;
}

std::vector<double> activate_vector(const std::vector<double>& values, std::string_view activation_name) {
    std::vector<double> result;
    result.reserve(values.size());
    for (double value : values) {
        if (activation_name == "relu") {
            result.push_back(std::max(0.0, value));
        } else if (activation_name == "linear") {
            result.push_back(value);
        } else {
            result.push_back(gelu_activation(value));
        }
    }
    return result;
}

std::vector<double> average_vectors(const std::vector<std::vector<double>>& vectors, std::size_t width) {
    std::vector<double> result(width, 0.0);
    if (vectors.empty()) {
        return result;
    }
    for (const std::vector<double>& vector : vectors) {
        for (std::size_t index = 0; index < std::min(width, vector.size()); ++index) {
            result[index] += vector[index];
        }
    }
    for (double& value : result) {
        value /= static_cast<double>(vectors.size());
    }
    return result;
}

BuiltinToolResult invoke_mlp_lens(const std::vector<std::string>& arguments, bool verbose) {
    BuiltinToolResult result;
    result.invocation.logical_name = "mlp-lens";
    result.invocation.selected_provider = "analyst_module";
    result.invocation.cache_origin = "builtin_analyst_module";
    result.invocation.command_line = "omnix::mlp-lens";

    MlpLensInput parsed_input;
    try {
        parsed_input = parse_mlp_lens_input(arguments);
    } catch (const std::exception& error) {
        result.invocation.status = "invalid_arguments";
        result.invocation.summary = error.what();
        result.invocation.exit_code = 1;
        result.next_action = "Use `omnix tool mlp-lens -- --tensor-bundle res/mlp_lens/tiny_mlp_bundle.json \"text\"`.";
        return result;
    }

    const std::string prompt = parsed_input.prompt;
    if (prompt.empty()) {
        result.invocation.status = "invalid_arguments";
        result.invocation.summary = "`mlp-lens` requires text input after `--`.";
        result.invocation.exit_code = 1;
        result.next_action = "Use `omnix tool mlp-lens -- \"Michael Jordan plays basketball\"`.";
        return result;
    }

    std::string model_source = "demo_weights";
    std::string model_name = "omnix-demo-mlp";
    std::string tensor_bundle_path;
    std::string tokenizer_source = "toy_embedding";
    std::vector<std::string> tokens;
    std::vector<std::size_t> token_ids;
    std::vector<std::vector<double>> embedding_vectors;
    std::vector<std::string> output_labels;
    std::vector<std::vector<double>> w1 = {
        {0.8, -0.2, 0.1, 0.4},
        {-0.1, 0.9, 0.3, -0.7},
        {0.5, 0.5, -0.4, 0.2},
        {-0.3, 0.1, 0.8, 0.6},
        {0.2, -0.6, 0.7, -0.1},
        {0.9, 0.1, -0.2, 0.3},
    };
    std::vector<double> b1 = {0.1, -0.2, 0.0, 0.3, -0.1, 0.2};
    std::vector<std::vector<double>> w2 = {
        {0.3, -0.1, 0.6, 0.2, -0.2, 0.5},
        {-0.4, 0.7, 0.1, -0.3, 0.8, 0.2},
        {0.2, 0.3, -0.5, 0.9, 0.1, -0.4},
        {0.6, -0.2, 0.4, 0.1, -0.7, 0.3},
    };
    std::vector<double> b2 = {0.0, 0.1, -0.1, 0.0};
    std::string activation_name = "gelu";
    std::vector<double> input;

    if (!parsed_input.tensor_bundle_path.empty()) {
        try {
            const MlpTensorBundle bundle = load_mlp_tensor_bundle(parsed_input.tensor_bundle_path);
            model_source = "loaded_tensor_bundle";
            model_name = bundle.model_name;
            tensor_bundle_path = bundle.path;
            tokenizer_source = bundle.tokenizer.source;
            w1 = bundle.layer.w1;
            b1 = bundle.layer.b1;
            w2 = bundle.layer.w2;
            b2 = bundle.layer.b2;
            activation_name = bundle.layer.activation;
            output_labels = bundle.output_labels;
            tokens = tokenize_for_bundle(prompt);
            if (tokens.empty()) {
                tokens.push_back(bundle.tokenizer.unknown_token);
            }
            for (const std::string& token : tokens) {
                const auto found = bundle.tokenizer.token_to_id.find(token);
                const std::size_t token_id =
                    found == bundle.tokenizer.token_to_id.end() ? bundle.tokenizer.unknown_id : found->second;
                token_ids.push_back(token_id);
                embedding_vectors.push_back(bundle.tokenizer.embeddings[token_id]);
            }
            input = average_vectors(embedding_vectors, bundle.tokenizer.embeddings.front().size());
        } catch (const std::exception& error) {
            result.invocation.status = "tensor_bundle_invalid";
            result.invocation.summary = error.what();
            result.invocation.exit_code = 1;
            result.next_action = "Validate the bundle schema and dimensions, then rerun `omnix tool mlp-lens`.";
            return result;
        }
    } else {
        input = toy_embedding(prompt);
    }

    const std::vector<double> z1 = add_vectors(mat_vec(w1, input), b1);
    const std::vector<double> hidden = activate_vector(z1, activation_name);
    const std::vector<double> output = add_vectors(mat_vec(w2, hidden), b2);
    const std::vector<double> probabilities = softmax_values(output);

    struct NeuronView {
        std::size_t index = 0;
        double pre_activation = 0.0;
        double activation = 0.0;
        double bias = 0.0;
        double input_weight_norm = 0.0;
        double output_weight_norm = 0.0;
    };
    std::vector<NeuronView> neurons;
    for (std::size_t index = 0; index < z1.size(); ++index) {
        std::vector<double> output_column;
        output_column.reserve(w2.size());
        for (const std::vector<double>& row : w2) {
            output_column.push_back(index < row.size() ? row[index] : 0.0);
        }
        neurons.push_back({
            index,
            z1[index],
            hidden[index],
            b1[index],
            vector_norm(w1[index]),
            vector_norm(output_column),
        });
    }
    std::stable_sort(neurons.begin(), neurons.end(), [](const NeuronView& lhs, const NeuronView& rhs) {
        return std::abs(lhs.activation) > std::abs(rhs.activation);
    });
    if (neurons.size() > 6) {
        neurons.resize(6);
    }

    std::ostringstream top_neurons;
    top_neurons << "[";
    for (std::size_t index = 0; index < neurons.size(); ++index) {
        if (index != 0) {
            top_neurons << ",";
        }
        const NeuronView& neuron = neurons[index];
        top_neurons << "{\"index\":" << neuron.index
                    << ",\"preActivation\":" << format_number(neuron.pre_activation)
                    << ",\"activation\":" << format_number(neuron.activation);
        if (verbose) {
            top_neurons << ",\"bias\":" << format_number(neuron.bias)
                        << ",\"inputWeightNorm\":" << format_number(neuron.input_weight_norm)
                        << ",\"outputWeightNorm\":" << format_number(neuron.output_weight_norm);
        }
        top_neurons << "}";
    }
    top_neurons << "]";

    const std::string warning = model_source == "loaded_tensor_bundle"
        ? "Loaded external tensor bundle and tokenizer-like metadata for a tiny MLP trace; this is still not a full LLM, production model, or transformer-layer activation trace."
        : "Educational toy embedding and demo weights only; this is not a real LLM or real model-weight trace unless real tensors are loaded.";

    std::ostringstream json;
    json << "{\"tool\":\"mlp-lens\""
         << ",\"prompt\":\"" << escape_json(prompt) << "\""
         << ",\"mode\":\"" << (verbose ? "verbose" : "compact") << "\""
         << ",\"warning\":\"" << escape_json(warning) << "\""
         << ",\"modelName\":\"" << escape_json(model_name) << "\""
         << ",\"modelSource\":\"" << escape_json(model_source) << "\""
         << ",\"tensorBundlePath\":\"" << escape_json(tensor_bundle_path) << "\""
         << ",\"tokenizerSource\":\"" << escape_json(tokenizer_source) << "\""
         << ",\"tokens\":" << render_string_array_json(tokens)
         << ",\"tokenIds\":" << render_size_array(token_ids)
         << ",\"embeddingVectors\":" << render_number_matrix(embedding_vectors)
         << ",\"inputVector\":" << render_number_array(input)
         << ",\"z1PreActivations\":" << render_number_array(z1)
         << ",\"hiddenActivations\":" << render_number_array(hidden)
         << ",\"outputVector\":" << render_number_array(output)
         << ",\"softmaxProbabilities\":" << render_number_array(probabilities)
         << ",\"topActivatedNeurons\":" << top_neurons.str()
         << ",\"outputLabels\":" << render_string_array_json(output_labels)
         << ",\"adapterStatus\":\"inactive: safetensors, ONNX, GGUF, and extracted transformer weights are future adapters\"";
    if (verbose) {
        json << ",\"trace\":{"
             << "\"formula\":\"MLP(x) = W2 * activation(W1 * x + b1) + b2\""
             << ",\"activation\":\"" << activation_name << "\""
             << ",\"tokenizer\":{\"source\":\"" << escape_json(tokenizer_source)
             << "\",\"tokenCount\":" << tokens.size() << ",\"tokenIds\":" << render_size_array(token_ids) << "}"
             << ",\"layer\":{\"modelName\":\"" << escape_json(model_name)
             << "\",\"modelSource\":\"" << escape_json(model_source)
             << "\",\"tensorBundlePath\":\"" << escape_json(tensor_bundle_path) << "\"}"
             << ",\"matrixShapes\":{\"W1\":" << render_shape_json({w1.size(), w1.front().size()})
             << ",\"b1\":" << render_shape_json({b1.size()})
             << ",\"W2\":" << render_shape_json({w2.size(), w2.front().size()})
             << ",\"b2\":" << render_shape_json({b2.size()}) << "}"
             << ",\"formulaSteps\":" << render_string_array_json({
                    model_source == "loaded_tensor_bundle"
                        ? "inputVector = average(loaded token embedding vectors)"
                        : "inputVector = deterministic toy embedding(text)",
                    "z1PreActivations = W1 * inputVector + b1",
                    "hiddenActivations = activation(z1PreActivations)",
                    "outputVector = W2 * hiddenActivations + b2",
                    "softmaxProbabilities = softmax(outputVector)",
                })
             << "}";
    }
    json << "}";

    result.invocation.status = "ok";
    result.invocation.summary = model_source == "loaded_tensor_bundle"
        ? "Traced a tiny loaded tensor bundle through the OmniX MLP lens."
        : "Traced a toy transformer MLP lens over educational demo weights.";
    result.invocation.exit_code = 0;
    result.invocation.output_excerpt = {json.str()};
    result.next_action = model_source == "loaded_tensor_bundle"
        ? "Use `--verbose` before `--` to inspect tokenizer and loaded tensor metadata."
        : "Use `--tensor-bundle res/mlp_lens/tiny_mlp_bundle.json` after `--` to trace loaded tensors.";
    return result;
}

double json_number_field(const MlpJsonValue& object, std::string_view key) {
    const MlpJsonValue& value = required_object_field(object, key);
    if (value.type != MlpJsonValue::Type::Number) {
        throw std::runtime_error("threshold scenario field `" + std::string(key) + "` must be a number");
    }
    return value.number_value;
}

bool json_bool_field(const MlpJsonValue& object, std::string_view key) {
    const MlpJsonValue& value = required_object_field(object, key);
    if (value.type != MlpJsonValue::Type::Boolean) {
        throw std::runtime_error("threshold scenario field `" + std::string(key) + "` must be a boolean");
    }
    return value.boolean_value;
}

std::string optional_json_string_field(const MlpJsonValue& object,
                                       std::string_view key,
                                       std::string fallback = {}) {
    const auto value = optional_object_field(object, key);
    if (!value.has_value()) {
        return fallback;
    }
    if (value->get().type != MlpJsonValue::Type::String) {
        throw std::runtime_error("threshold scenario field `" + std::string(key) + "` must be a string");
    }
    return value->get().string_value;
}

double optional_json_number_field(const MlpJsonValue& object,
                                  std::string_view key,
                                  double fallback = 0.0) {
    const auto value = optional_object_field(object, key);
    if (!value.has_value()) {
        return fallback;
    }
    if (value->get().type != MlpJsonValue::Type::Number) {
        throw std::runtime_error("threshold scenario field `" + std::string(key) + "` must be a number");
    }
    return value->get().number_value;
}

bool optional_json_bool_field(const MlpJsonValue& object,
                              std::string_view key,
                              bool fallback = false) {
    const auto value = optional_object_field(object, key);
    if (!value.has_value()) {
        return fallback;
    }
    if (value->get().type != MlpJsonValue::Type::Boolean) {
        throw std::runtime_error("threshold scenario field `" + std::string(key) + "` must be a boolean");
    }
    return value->get().boolean_value;
}

std::vector<std::string> optional_json_string_array_field(const MlpJsonValue& object,
                                                          std::string_view key) {
    const auto value = optional_object_field(object, key);
    if (!value.has_value()) {
        return {};
    }
    if (value->get().type != MlpJsonValue::Type::Array) {
        throw std::runtime_error("threshold scenario field `" + std::string(key) + "` must be an array");
    }
    std::vector<std::string> result;
    for (const MlpJsonValue& entry : value->get().array_values) {
        if (entry.type != MlpJsonValue::Type::String) {
            throw std::runtime_error("threshold scenario field `" + std::string(key) + "` must contain strings");
        }
        result.push_back(entry.string_value);
    }
    return result;
}

struct ThresholdAsset {
    std::string server;
    std::string server_class;
    std::string customer;
    std::string site;
    std::string queue;
    std::string rabbitmq_node;
    std::string app;
    std::string service_name;
};

struct ThresholdMetrics {
    double cpu_usage = 0.0;
    double ram_usage = 0.0;
    bool mount_healthy = true;
    double storage_usage = 0.0;
    bool data_brick_healthy = true;
    bool rabbitmq_node_healthy = true;
    double queue_depth = 0.0;
    double previous_queue_depth = 0.0;
    double consumer_count = 0.0;
    double previous_consumer_count = 0.0;
    double publish_rate = 0.0;
    double ack_rate = 0.0;
    double oldest_message_age_seconds = 0.0;
    std::string app_service_status;
    bool messages_persistent = false;
    double estimated_restart_loss_packets = 0.0;
};

struct ThresholdBaseline {
    double normal_depth_max = 40.0;
    double warning_depth = 100.0;
    double abnormal_depth = 200.0;
    double critical_depth = 500.0;
    double p1_depth = 1000.0;
    double normal_consumers = 1.0;
    double max_oldest_message_age_seconds = 180.0;
};

struct ThresholdSeasonalOverride {
    std::string name;
    bool active = false;
    double expected_depth_max = 0.0;
    double abnormal_depth = 0.0;
    double critical_depth = 0.0;
};

struct ThresholdErrorSignature {
    std::string code;
    std::string pattern;
    std::string likely_cause;
    std::string recommended_action;
    std::string developer_recommendation;
};

struct ThresholdOwnership {
    std::string fed_by;
    std::string consumed_by;
    std::string dev_owner;
    std::string infra_owner;
    std::vector<std::string> log_paths;
};

struct ThresholdRunbookAction {
    std::string id;
    std::string type;
    std::string service_name;
    std::string display_command;
    std::vector<std::string> validation_checks;
};

struct ThresholdPolicy {
    double max_restart_loss_packets = 3.0;
    bool require_rabbitmq_healthy = true;
    bool require_persistent_messages = true;
};

struct ThresholdLogSnippet {
    std::string path;
    std::string snippet;
};

struct ThresholdScenario {
    std::string path;
    std::string id;
    std::string alarm_id;
    std::string alarm_severity;
    std::string alarm_timestamp;
    std::string evaluation_timestamp;
    ThresholdAsset asset;
    ThresholdMetrics metrics;
    ThresholdBaseline baseline;
    ThresholdSeasonalOverride seasonal;
    ThresholdOwnership ownership;
    ThresholdPolicy policy;
    ThresholdRunbookAction runbook;
    std::vector<ThresholdErrorSignature> signatures;
    std::vector<ThresholdLogSnippet> logs;
};

struct ThresholdCliInput {
    std::string mode;
    std::string scenario_path;
    std::string evidence_out;
    std::string jira_out;
    bool execute = false;
};

struct ThresholdEvaluation {
    std::string threshold_status = "thresholds_evaluated";
    std::string severity;
    std::string likely_cause;
    std::string decision;
    std::string recommendation;
    std::string action_id;
    std::string action_status = "not_requested";
    std::string action_output;
    bool seasonal_override_applied = false;
    bool restart_recommended = false;
    bool escalation_required = false;
    std::vector<std::string> matched_signatures;
    std::vector<std::string> rationale;
};

struct GenericSignalAsset {
    std::string id;
    std::string kind;
    std::string name;
    std::string role;
    std::string tier;
    std::string owner;
    std::string customer;
    std::string site;
};

struct GenericSignal {
    std::string id;
    std::string asset;
    std::string unit;
    std::string trend;
    std::string status;
    std::string text_value;
    double number_value = 0.0;
    double previous_number_value = 0.0;
    bool has_number = false;
    bool has_previous_number = false;
};

struct GenericSignalCondition {
    std::string signal_id;
    std::string op;
    std::string trend;
    std::string status;
    std::string text_value;
    double number_value = 0.0;
    bool has_number = false;
    bool has_text = false;
};

struct GenericSignalPolicy {
    std::string id;
    std::string description;
    std::vector<GenericSignalCondition> conditions;
    std::string likely_cause;
    std::string severity;
    std::string decision;
    std::string recommendation;
    std::string runbook_id;
    std::string rationale;
    bool escalation_required = false;
};

struct GenericSignalRunbook {
    std::string id;
    std::string type;
    std::string target;
    std::string display_command;
    std::vector<std::string> validation_checks;
};

struct GenericSignalScenario {
    std::string path;
    std::string id;
    std::string workflow;
    std::string evaluation_timestamp;
    std::vector<GenericSignalAsset> assets;
    std::vector<GenericSignal> signals;
    std::vector<GenericSignalPolicy> policies;
    std::vector<GenericSignalRunbook> runbooks;
    std::vector<std::string> doctrine_notes;
};

struct GenericSignalPolicyMatch {
    GenericSignalPolicy policy;
    std::vector<std::string> matched_conditions;
};

struct GenericSignalEvaluation {
    std::string status = "gsmg_evaluated";
    std::string severity = "normal";
    std::string likely_cause = "no_policy_match";
    std::string decision = "monitor_or_wait";
    std::string recommendation = "No generic policy matched; continue collecting signal evidence.";
    std::string action_id;
    bool escalation_required = false;
    std::vector<GenericSignalPolicyMatch> matched_policies;
    std::vector<std::string> rationale;
};

ThresholdCliInput parse_thresholds_input(const std::vector<std::string>& arguments) {
    ThresholdCliInput input;
    if (arguments.empty() || (arguments.front() != "evaluate" && arguments.front() != "gsmg")) {
        throw std::runtime_error("`thresholds` supports `evaluate <scenario.json>` or `gsmg <scenario.json>`.");
    }
    input.mode = arguments.front();
    for (std::size_t index = 1; index < arguments.size(); ++index) {
        const std::string& arg = arguments[index];
        if (arg == "--out") {
            if (index + 1 >= arguments.size()) {
                throw std::runtime_error("`--out` requires a path");
            }
            input.evidence_out = arguments[++index];
            continue;
        }
        if (arg == "--jira-out") {
            if (index + 1 >= arguments.size()) {
                throw std::runtime_error("`--jira-out` requires a path");
            }
            input.jira_out = arguments[++index];
            continue;
        }
        if (arg == "--execute") {
            input.execute = true;
            continue;
        }
        if (input.scenario_path.empty()) {
            input.scenario_path = arg;
            continue;
        }
        throw std::runtime_error("unexpected thresholds argument `" + arg + "`");
    }
    if (input.scenario_path.empty()) {
        throw std::runtime_error("`thresholds " + input.mode + "` requires a scenario JSON path");
    }
    return input;
}

std::vector<ThresholdErrorSignature> parse_threshold_signatures(const MlpJsonValue& root) {
    const auto value = optional_object_field(root, "errorSignatures");
    if (!value.has_value()) {
        return {};
    }
    if (value->get().type != MlpJsonValue::Type::Array) {
        throw std::runtime_error("threshold scenario field `errorSignatures` must be an array");
    }
    std::vector<ThresholdErrorSignature> signatures;
    for (const MlpJsonValue& entry : value->get().array_values) {
        ThresholdErrorSignature signature;
        signature.code = json_string_field(entry, "code");
        signature.pattern = json_string_field(entry, "pattern");
        signature.likely_cause = json_string_field(entry, "likelyCause");
        signature.recommended_action = json_string_field(entry, "recommendedAction");
        signature.developer_recommendation = json_string_field(entry, "developerRecommendation");
        signatures.push_back(std::move(signature));
    }
    return signatures;
}

std::vector<ThresholdLogSnippet> parse_threshold_logs(const MlpJsonValue& root) {
    const auto value = optional_object_field(root, "logs");
    if (!value.has_value()) {
        return {};
    }
    if (value->get().type != MlpJsonValue::Type::Array) {
        throw std::runtime_error("threshold scenario field `logs` must be an array");
    }
    std::vector<ThresholdLogSnippet> logs;
    for (const MlpJsonValue& entry : value->get().array_values) {
        ThresholdLogSnippet log;
        log.path = json_string_field(entry, "path");
        log.snippet = json_string_field(entry, "snippet");
        logs.push_back(std::move(log));
    }
    return logs;
}

std::string json_value_to_string(const MlpJsonValue& value) {
    switch (value.type) {
        case MlpJsonValue::Type::String:
            return value.string_value;
        case MlpJsonValue::Type::Number:
            return format_number(value.number_value);
        case MlpJsonValue::Type::Boolean:
            return value.boolean_value ? "true" : "false";
        case MlpJsonValue::Type::Null:
            return "null";
        default:
            throw std::runtime_error("GSMg scalar field must be a string, number, boolean, or null");
    }
}

std::vector<GenericSignalAsset> parse_gsmg_assets(const MlpJsonValue& root) {
    const MlpJsonValue& value = required_object_field(root, "assets");
    if (value.type != MlpJsonValue::Type::Array) {
        throw std::runtime_error("GSMg field `assets` must be an array");
    }
    std::vector<GenericSignalAsset> assets;
    for (const MlpJsonValue& entry : value.array_values) {
        GenericSignalAsset asset;
        asset.id = json_string_field(entry, "id");
        asset.kind = json_string_field(entry, "kind");
        asset.name = optional_json_string_field(entry, "name", asset.id);
        asset.role = optional_json_string_field(entry, "role");
        asset.tier = optional_json_string_field(entry, "tier");
        asset.owner = optional_json_string_field(entry, "owner");
        asset.customer = optional_json_string_field(entry, "customer");
        asset.site = optional_json_string_field(entry, "site");
        assets.push_back(std::move(asset));
    }
    return assets;
}

std::vector<GenericSignal> parse_gsmg_signals(const MlpJsonValue& root) {
    const MlpJsonValue& value = required_object_field(root, "signals");
    if (value.type != MlpJsonValue::Type::Array) {
        throw std::runtime_error("GSMg field `signals` must be an array");
    }
    std::vector<GenericSignal> signals;
    for (const MlpJsonValue& entry : value.array_values) {
        GenericSignal signal;
        signal.id = json_string_field(entry, "id");
        signal.asset = json_string_field(entry, "asset");
        signal.unit = optional_json_string_field(entry, "unit");
        signal.trend = optional_json_string_field(entry, "trend");
        signal.status = optional_json_string_field(entry, "status");
        const MlpJsonValue& scalar = required_object_field(entry, "value");
        if (scalar.type == MlpJsonValue::Type::Number) {
            signal.has_number = true;
            signal.number_value = scalar.number_value;
        } else {
            signal.text_value = json_value_to_string(scalar);
        }
        if (const auto previous = optional_object_field(entry, "previousValue")) {
            if (previous->get().type == MlpJsonValue::Type::Number) {
                signal.has_previous_number = true;
                signal.previous_number_value = previous->get().number_value;
                if (signal.trend.empty()) {
                    if (signal.number_value > signal.previous_number_value) {
                        signal.trend = "rising";
                    } else if (signal.number_value < signal.previous_number_value) {
                        signal.trend = "falling";
                    } else {
                        signal.trend = "flat";
                    }
                }
            } else if (signal.trend.empty()) {
                signal.trend = "unknown";
            }
        }
        signals.push_back(std::move(signal));
    }
    return signals;
}

GenericSignalCondition parse_gsmg_condition(const MlpJsonValue& entry) {
    GenericSignalCondition condition;
    condition.signal_id = json_string_field(entry, "signal");
    condition.op = optional_json_string_field(entry, "operator", "==");
    condition.trend = optional_json_string_field(entry, "trend");
    condition.status = optional_json_string_field(entry, "status");
    if (const auto scalar = optional_object_field(entry, "value")) {
        if (scalar->get().type == MlpJsonValue::Type::Number) {
            condition.has_number = true;
            condition.number_value = scalar->get().number_value;
        } else {
            condition.has_text = true;
            condition.text_value = json_value_to_string(scalar->get());
        }
    }
    return condition;
}

std::vector<GenericSignalPolicy> parse_gsmg_policies(const MlpJsonValue& root) {
    const MlpJsonValue& value = required_object_field(root, "policies");
    if (value.type != MlpJsonValue::Type::Array) {
        throw std::runtime_error("GSMg field `policies` must be an array");
    }
    std::vector<GenericSignalPolicy> policies;
    for (const MlpJsonValue& entry : value.array_values) {
        GenericSignalPolicy policy;
        policy.id = json_string_field(entry, "id");
        policy.description = optional_json_string_field(entry, "description");
        const MlpJsonValue& conditions = required_object_field(entry, "if");
        if (conditions.type != MlpJsonValue::Type::Array) {
            throw std::runtime_error("GSMg policy `if` must be an array");
        }
        for (const MlpJsonValue& condition : conditions.array_values) {
            policy.conditions.push_back(parse_gsmg_condition(condition));
        }
        const MlpJsonValue& then = required_object_field(entry, "then");
        policy.likely_cause = json_string_field(then, "likelyCause");
        policy.severity = json_string_field(then, "severity");
        policy.decision = optional_json_string_field(then, "decision");
        policy.recommendation = json_string_field(then, "recommendation");
        policy.runbook_id = optional_json_string_field(then, "runbookId");
        policy.rationale = optional_json_string_field(then, "rationale");
        policy.escalation_required = optional_json_bool_field(then, "escalationRequired", false);
        policies.push_back(std::move(policy));
    }
    return policies;
}

std::vector<GenericSignalRunbook> parse_gsmg_runbooks(const MlpJsonValue& root) {
    const auto value = optional_object_field(root, "runbooks");
    if (!value.has_value()) {
        return {};
    }
    if (value->get().type != MlpJsonValue::Type::Array) {
        throw std::runtime_error("GSMg field `runbooks` must be an array");
    }
    std::vector<GenericSignalRunbook> runbooks;
    for (const MlpJsonValue& entry : value->get().array_values) {
        GenericSignalRunbook runbook;
        runbook.id = json_string_field(entry, "id");
        runbook.type = json_string_field(entry, "type");
        runbook.target = optional_json_string_field(entry, "target");
        runbook.display_command = optional_json_string_field(entry, "displayCommand");
        runbook.validation_checks = optional_json_string_array_field(entry, "validationChecks");
        runbooks.push_back(std::move(runbook));
    }
    return runbooks;
}

GenericSignalScenario load_gsmg_scenario(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("unable to open GSMg scenario `" + path.string() + "`");
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    const MlpJsonValue root = MlpJsonParser(buffer.str()).parse();
    if (root.type != MlpJsonValue::Type::Object) {
        throw std::runtime_error("GSMg scenario root must be an object");
    }
    GenericSignalScenario scenario;
    scenario.path = path.string();
    scenario.id = json_string_field(root, "id");
    scenario.workflow = optional_json_string_field(root, "workflow", "generic-signal-model-ground");
    scenario.evaluation_timestamp = optional_json_string_field(root, "evaluationTimestamp", now_timestamp());
    scenario.assets = parse_gsmg_assets(root);
    scenario.signals = parse_gsmg_signals(root);
    scenario.policies = parse_gsmg_policies(root);
    scenario.runbooks = parse_gsmg_runbooks(root);
    scenario.doctrine_notes = optional_json_string_array_field(root, "doctrineNotes");
    return scenario;
}

const GenericSignal* find_gsmg_signal(const std::vector<GenericSignal>& signals,
                                      std::string_view id) {
    for (const GenericSignal& signal : signals) {
        if (signal.id == id) {
            return &signal;
        }
    }
    return nullptr;
}

bool compare_gsmg_number(double actual, std::string_view op, double expected) {
    if (op == ">=") {
        return actual >= expected;
    }
    if (op == ">") {
        return actual > expected;
    }
    if (op == "<=") {
        return actual <= expected;
    }
    if (op == "<") {
        return actual < expected;
    }
    if (op == "!=") {
        return actual != expected;
    }
    return actual == expected;
}

bool gsmg_condition_matches(const GenericSignalCondition& condition,
                            const std::vector<GenericSignal>& signals,
                            std::string* explanation) {
    const GenericSignal* signal = find_gsmg_signal(signals, condition.signal_id);
    if (signal == nullptr) {
        return false;
    }
    if (!condition.trend.empty() && signal->trend != condition.trend) {
        return false;
    }
    if (!condition.status.empty() && signal->status != condition.status) {
        return false;
    }
    if (condition.has_number) {
        if (!signal->has_number || !compare_gsmg_number(signal->number_value, condition.op, condition.number_value)) {
            return false;
        }
    }
    if (condition.has_text) {
        const std::string actual = !signal->status.empty() ? signal->status : signal->text_value;
        if (condition.op == "!=") {
            if (actual == condition.text_value) {
                return false;
            }
        } else if (actual != condition.text_value) {
            return false;
        }
    }
    if (explanation != nullptr) {
        std::ostringstream out;
        out << condition.signal_id << "=";
        if (signal->has_number) {
            out << format_number(signal->number_value);
        } else if (!signal->status.empty()) {
            out << signal->status;
        } else {
            out << signal->text_value;
        }
        if (!signal->unit.empty()) {
            out << " " << signal->unit;
        }
        if (!signal->trend.empty()) {
            out << " trend=" << signal->trend;
        }
        *explanation = out.str();
    }
    return true;
}

int gsmg_severity_rank(std::string_view severity) {
    const std::string lowered = lowercase(severity);
    if (lowered == "blocker") {
        return 6;
    }
    if (lowered == "p1") {
        return 5;
    }
    if (lowered == "critical") {
        return 4;
    }
    if (lowered == "abnormal") {
        return 3;
    }
    if (lowered == "warning") {
        return 2;
    }
    if (lowered == "normal") {
        return 1;
    }
    return 0;
}

GenericSignalEvaluation evaluate_gsmg_scenario(const GenericSignalScenario& scenario) {
    GenericSignalEvaluation evaluation;
    for (const GenericSignalPolicy& policy : scenario.policies) {
        GenericSignalPolicyMatch match;
        match.policy = policy;
        bool matched = true;
        for (const GenericSignalCondition& condition : policy.conditions) {
            std::string explanation;
            if (!gsmg_condition_matches(condition, scenario.signals, &explanation)) {
                matched = false;
                break;
            }
            match.matched_conditions.push_back(explanation);
        }
        if (!matched) {
            continue;
        }
        evaluation.matched_policies.push_back(match);
        if (gsmg_severity_rank(policy.severity) >= gsmg_severity_rank(evaluation.severity)) {
            evaluation.severity = policy.severity;
            evaluation.likely_cause = policy.likely_cause;
            evaluation.decision = policy.decision.empty()
                ? (policy.severity == "blocker" ? "block_unsafe_action" : "recommend_runbook")
                : policy.decision;
            evaluation.recommendation = policy.recommendation;
            evaluation.action_id = policy.runbook_id;
            evaluation.escalation_required = policy.escalation_required;
        }
        if (!policy.rationale.empty()) {
            evaluation.rationale.push_back(policy.rationale);
        }
        for (const std::string& condition_text : match.matched_conditions) {
            evaluation.rationale.push_back("Matched `" + policy.id + "` using " + condition_text + ".");
        }
    }
    if (evaluation.matched_policies.empty()) {
        evaluation.rationale.push_back("No generic signal policy matched this scenario.");
    }
    return evaluation;
}

std::string render_gsmg_assets_json(const std::vector<GenericSignalAsset>& assets) {
    std::ostringstream out;
    out << "[";
    for (std::size_t index = 0; index < assets.size(); ++index) {
        if (index != 0) {
            out << ",";
        }
        const GenericSignalAsset& asset = assets[index];
        out << "{\"id\":\"" << escape_json(asset.id)
            << "\",\"kind\":\"" << escape_json(asset.kind)
            << "\",\"name\":\"" << escape_json(asset.name)
            << "\",\"role\":\"" << escape_json(asset.role)
            << "\",\"tier\":\"" << escape_json(asset.tier)
            << "\",\"owner\":\"" << escape_json(asset.owner)
            << "\",\"customer\":\"" << escape_json(asset.customer)
            << "\",\"site\":\"" << escape_json(asset.site) << "\"}";
    }
    out << "]";
    return out.str();
}

std::string render_gsmg_signals_json(const std::vector<GenericSignal>& signals) {
    std::ostringstream out;
    out << "[";
    for (std::size_t index = 0; index < signals.size(); ++index) {
        if (index != 0) {
            out << ",";
        }
        const GenericSignal& signal = signals[index];
        out << "{\"id\":\"" << escape_json(signal.id)
            << "\",\"asset\":\"" << escape_json(signal.asset) << "\",\"value\":";
        if (signal.has_number) {
            out << format_number(signal.number_value);
        } else {
            out << "\"" << escape_json(signal.text_value) << "\"";
        }
        if (signal.has_previous_number) {
            out << ",\"previousValue\":" << format_number(signal.previous_number_value);
        }
        out << ",\"unit\":\"" << escape_json(signal.unit)
            << "\",\"trend\":\"" << escape_json(signal.trend)
            << "\",\"status\":\"" << escape_json(signal.status) << "\"}";
    }
    out << "]";
    return out.str();
}

std::string render_gsmg_matches_json(const std::vector<GenericSignalPolicyMatch>& matches) {
    std::ostringstream out;
    out << "[";
    for (std::size_t index = 0; index < matches.size(); ++index) {
        if (index != 0) {
            out << ",";
        }
        const GenericSignalPolicyMatch& match = matches[index];
        out << "{\"id\":\"" << escape_json(match.policy.id)
            << "\",\"description\":\"" << escape_json(match.policy.description)
            << "\",\"severity\":\"" << escape_json(match.policy.severity)
            << "\",\"likelyCause\":\"" << escape_json(match.policy.likely_cause)
            << "\",\"matchedConditions\":" << render_string_array_json(match.matched_conditions) << "}";
    }
    out << "]";
    return out.str();
}

std::string render_gsmg_runbooks_json(const std::vector<GenericSignalRunbook>& runbooks) {
    std::ostringstream out;
    out << "[";
    for (std::size_t index = 0; index < runbooks.size(); ++index) {
        if (index != 0) {
            out << ",";
        }
        const GenericSignalRunbook& runbook = runbooks[index];
        out << "{\"id\":\"" << escape_json(runbook.id)
            << "\",\"type\":\"" << escape_json(runbook.type)
            << "\",\"target\":\"" << escape_json(runbook.target)
            << "\",\"displayCommand\":\"" << escape_json(runbook.display_command)
            << "\",\"validationChecks\":" << render_string_array_json(runbook.validation_checks) << "}";
    }
    out << "]";
    return out.str();
}

std::string render_gsmg_evidence_json(const GenericSignalScenario& scenario,
                                      const GenericSignalEvaluation& evaluation,
                                      std::string_view evidence_path,
                                      std::string_view jira_path) {
    std::ostringstream out;
    out << "{\"event_type\":\"omnix.threshold.gsmg.v1\""
        << ",\"gsmgStatus\":\"" << escape_json(evaluation.status) << "\""
        << ",\"scenarioId\":\"" << escape_json(scenario.id) << "\""
        << ",\"workflow\":\"" << escape_json(scenario.workflow) << "\""
        << ",\"evaluationTimestamp\":\"" << escape_json(scenario.evaluation_timestamp) << "\""
        << ",\"assets\":" << render_gsmg_assets_json(scenario.assets)
        << ",\"signals\":" << render_gsmg_signals_json(scenario.signals)
        << ",\"matchedPolicies\":" << render_gsmg_matches_json(evaluation.matched_policies)
        << ",\"decision\":{\"severity\":\"" << escape_json(evaluation.severity)
        << "\",\"likelyCause\":\"" << escape_json(evaluation.likely_cause)
        << "\",\"decision\":\"" << escape_json(evaluation.decision)
        << "\",\"recommendation\":\"" << escape_json(evaluation.recommendation)
        << "\",\"actionId\":\"" << escape_json(evaluation.action_id)
        << "\",\"escalationRequired\":" << (evaluation.escalation_required ? "true" : "false")
        << ",\"rationale\":" << render_string_array_json(evaluation.rationale) << "}"
        << ",\"runbooks\":" << render_gsmg_runbooks_json(scenario.runbooks)
        << ",\"doctrineNotes\":" << render_string_array_json(scenario.doctrine_notes)
        << ",\"artifacts\":{\"evidencePath\":\"" << escape_json(evidence_path)
        << "\",\"jiraPath\":\"" << escape_json(jira_path) << "\"}}";
    return out.str();
}

std::string render_gsmg_jira_markdown(const GenericSignalScenario& scenario,
                                      const GenericSignalEvaluation& evaluation) {
    std::ostringstream out;
    out << "# GSMg Triage - " << scenario.id << "\n\n"
        << "## Decision\n"
        << "- Workflow: `" << scenario.workflow << "`\n"
        << "- Severity: " << evaluation.severity << "\n"
        << "- Likely cause: " << evaluation.likely_cause << "\n"
        << "- Decision: " << evaluation.decision << "\n"
        << "- Recommendation: " << evaluation.recommendation << "\n"
        << "- Escalation required: " << (evaluation.escalation_required ? "yes" : "no") << "\n\n"
        << "## Matched Policies\n";
    for (const GenericSignalPolicyMatch& match : evaluation.matched_policies) {
        out << "- `" << match.policy.id << "`: " << match.policy.description << "\n";
        for (const std::string& condition : match.matched_conditions) {
            out << "  - " << condition << "\n";
        }
    }
    out << "\n## Signals\n";
    for (const GenericSignal& signal : scenario.signals) {
        out << "- `" << signal.id << "` on `" << signal.asset << "` = ";
        if (signal.has_number) {
            out << format_number(signal.number_value);
        } else {
            out << signal.text_value;
        }
        if (!signal.unit.empty()) {
            out << " " << signal.unit;
        }
        if (!signal.trend.empty()) {
            out << " (" << signal.trend << ")";
        }
        out << "\n";
    }
    out << "\n## Doctrine Notes\n";
    for (const std::string& note : scenario.doctrine_notes) {
        out << "- " << note << "\n";
    }
    return out.str();
}

ThresholdScenario load_threshold_scenario(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("unable to open threshold scenario `" + path.string() + "`");
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    const MlpJsonValue root = MlpJsonParser(buffer.str()).parse();
    if (root.type != MlpJsonValue::Type::Object) {
        throw std::runtime_error("threshold scenario root must be an object");
    }

    ThresholdScenario scenario;
    scenario.path = path.string();
    scenario.id = json_string_field(root, "id");
    scenario.evaluation_timestamp = optional_json_string_field(root, "evaluationTimestamp", now_timestamp());

    const MlpJsonValue& alarm = required_object_field(root, "alarm");
    scenario.alarm_id = json_string_field(alarm, "id");
    scenario.alarm_severity = json_string_field(alarm, "severity");
    scenario.alarm_timestamp = json_string_field(alarm, "timestamp");

    const MlpJsonValue& assets = required_object_field(root, "assets");
    scenario.asset.server = json_string_field(assets, "server");
    scenario.asset.server_class = optional_json_string_field(assets, "serverClass");
    scenario.asset.customer = json_string_field(assets, "customer");
    scenario.asset.site = json_string_field(assets, "site");
    scenario.asset.queue = json_string_field(assets, "queue");
    scenario.asset.rabbitmq_node = json_string_field(assets, "rabbitmqNode");
    scenario.asset.app = json_string_field(assets, "app");
    scenario.asset.service_name = json_string_field(assets, "serviceName");

    const MlpJsonValue& ownership = required_object_field(root, "ownership");
    scenario.ownership.fed_by = json_string_field(ownership, "fedBy");
    scenario.ownership.consumed_by = json_string_field(ownership, "consumedBy");
    scenario.ownership.dev_owner = json_string_field(ownership, "devOwner");
    scenario.ownership.infra_owner = json_string_field(ownership, "infraOwner");
    scenario.ownership.log_paths = optional_json_string_array_field(ownership, "logPaths");

    const MlpJsonValue& metrics = required_object_field(root, "metrics");
    scenario.metrics.cpu_usage = json_number_field(metrics, "cpuUsage");
    scenario.metrics.ram_usage = json_number_field(metrics, "ramUsage");
    scenario.metrics.mount_healthy = json_bool_field(metrics, "mountHealthy");
    scenario.metrics.storage_usage = json_number_field(metrics, "storageUsage");
    scenario.metrics.data_brick_healthy = json_bool_field(metrics, "dataBrickHealthy");
    scenario.metrics.rabbitmq_node_healthy = json_bool_field(metrics, "rabbitmqNodeHealthy");
    scenario.metrics.queue_depth = json_number_field(metrics, "queueDepth");
    scenario.metrics.previous_queue_depth = optional_json_number_field(metrics, "previousQueueDepth", scenario.metrics.queue_depth);
    scenario.metrics.consumer_count = json_number_field(metrics, "consumerCount");
    scenario.metrics.previous_consumer_count = optional_json_number_field(metrics, "previousConsumerCount", scenario.metrics.consumer_count);
    scenario.metrics.publish_rate = json_number_field(metrics, "publishRate");
    scenario.metrics.ack_rate = json_number_field(metrics, "ackRate");
    scenario.metrics.oldest_message_age_seconds = json_number_field(metrics, "oldestMessageAgeSeconds");
    scenario.metrics.app_service_status = json_string_field(metrics, "appServiceStatus");
    scenario.metrics.messages_persistent = json_bool_field(metrics, "messagesPersistent");
    scenario.metrics.estimated_restart_loss_packets = json_number_field(metrics, "estimatedRestartLossPackets");

    const MlpJsonValue& baseline = required_object_field(root, "baseline");
    scenario.baseline.normal_depth_max = json_number_field(baseline, "normalDepthMax");
    scenario.baseline.warning_depth = json_number_field(baseline, "warningDepth");
    scenario.baseline.abnormal_depth = json_number_field(baseline, "abnormalDepth");
    scenario.baseline.critical_depth = json_number_field(baseline, "criticalDepth");
    scenario.baseline.p1_depth = json_number_field(baseline, "p1Depth");
    scenario.baseline.normal_consumers = json_number_field(baseline, "normalConsumers");
    scenario.baseline.max_oldest_message_age_seconds = json_number_field(baseline, "maxOldestMessageAgeSeconds");

    if (const auto seasonal = optional_object_field(root, "seasonalOverride")) {
        scenario.seasonal.name = optional_json_string_field(seasonal->get(), "name");
        scenario.seasonal.active = optional_json_bool_field(seasonal->get(), "active");
        scenario.seasonal.expected_depth_max = optional_json_number_field(seasonal->get(), "expectedDepthMax");
        scenario.seasonal.abnormal_depth = optional_json_number_field(seasonal->get(), "abnormalDepth");
        scenario.seasonal.critical_depth = optional_json_number_field(seasonal->get(), "criticalDepth");
    }

    if (const auto policy = optional_object_field(root, "decisionPolicy")) {
        scenario.policy.max_restart_loss_packets = optional_json_number_field(policy->get(), "maxRestartLossPackets", 3.0);
        scenario.policy.require_rabbitmq_healthy = optional_json_bool_field(policy->get(), "requireRabbitmqHealthy", true);
        scenario.policy.require_persistent_messages = optional_json_bool_field(policy->get(), "requirePersistentMessages", true);
    }

    if (const auto runbook = optional_object_field(root, "runbookAction")) {
        scenario.runbook.id = json_string_field(runbook->get(), "id");
        scenario.runbook.type = json_string_field(runbook->get(), "type");
        scenario.runbook.service_name = json_string_field(runbook->get(), "serviceName");
        scenario.runbook.display_command = json_string_field(runbook->get(), "displayCommand");
        scenario.runbook.validation_checks = optional_json_string_array_field(runbook->get(), "validationChecks");
    }

    scenario.signatures = parse_threshold_signatures(root);
    scenario.logs = parse_threshold_logs(root);
    return scenario;
}

std::string threshold_depth_severity(const ThresholdScenario& scenario, bool* seasonal_applied) {
    const double depth = scenario.metrics.queue_depth;
    double warning = scenario.baseline.warning_depth;
    double abnormal = scenario.baseline.abnormal_depth;
    double critical = scenario.baseline.critical_depth;
    if (scenario.seasonal.active) {
        *seasonal_applied = true;
        if (scenario.seasonal.abnormal_depth > 0) {
            abnormal = scenario.seasonal.abnormal_depth;
        }
        if (scenario.seasonal.critical_depth > 0) {
            critical = scenario.seasonal.critical_depth;
        }
        if (scenario.seasonal.expected_depth_max > 0) {
            warning = scenario.seasonal.expected_depth_max;
        }
    }
    if (depth >= scenario.baseline.p1_depth) {
        return "p1";
    }
    if (depth >= critical) {
        return "critical";
    }
    if (depth >= abnormal) {
        return "abnormal";
    }
    if (depth > warning) {
        return "warning";
    }
    return "normal";
}

ThresholdEvaluation evaluate_threshold_scenario(const ThresholdScenario& scenario) {
    ThresholdEvaluation evaluation;
    evaluation.severity = threshold_depth_severity(scenario, &evaluation.seasonal_override_applied);
    const bool queue_rising = scenario.metrics.queue_depth > scenario.metrics.previous_queue_depth;
    const bool consumers_down = scenario.metrics.consumer_count <= 0.0;
    const bool memory_high = scenario.metrics.ram_usage >= 85.0;
    const bool rabbit_ok = scenario.metrics.rabbitmq_node_healthy;
    const bool persistent = scenario.metrics.messages_persistent;
    const bool low_loss = scenario.metrics.estimated_restart_loss_packets <= scenario.policy.max_restart_loss_packets;

    for (const ThresholdErrorSignature& signature : scenario.signatures) {
        const std::string code = lowercase(signature.code);
        const std::string pattern = lowercase(signature.pattern);
        for (const ThresholdLogSnippet& log : scenario.logs) {
            const std::string snippet = lowercase(log.snippet);
            if ((!code.empty() && snippet.find(code) != std::string::npos) ||
                (!pattern.empty() && snippet.find(pattern) != std::string::npos)) {
                append_unique(evaluation.matched_signatures, signature.code);
                evaluation.likely_cause = signature.likely_cause;
                evaluation.recommendation = signature.recommended_action;
            }
        }
    }

    if (evaluation.seasonal_override_applied) {
        evaluation.rationale.push_back("Seasonal override `" + scenario.seasonal.name + "` adjusted queue-depth boundaries.");
    }
    if (queue_rising) {
        evaluation.rationale.push_back("Queue depth is rising from " +
            format_number(scenario.metrics.previous_queue_depth) + " to " + format_number(scenario.metrics.queue_depth) + ".");
    }
    if (consumers_down) {
        evaluation.rationale.push_back("Consumer count is 0 for queue `" + scenario.asset.queue + "`.");
    }
    if (memory_high) {
        evaluation.rationale.push_back("RAM usage is above 85% at " + format_number(scenario.metrics.ram_usage) + "%.");
    }
    if (rabbit_ok) {
        evaluation.rationale.push_back("RabbitMQ node `" + scenario.asset.rabbitmq_node + "` is healthy, so node failure is less likely.");
    }
    if (!scenario.metrics.mount_healthy || !scenario.metrics.data_brick_healthy || scenario.metrics.storage_usage >= 90.0) {
        evaluation.likely_cause = "storage_or_mount";
        evaluation.escalation_required = true;
        evaluation.rationale.push_back("Storage, mount, or data-brick health is outside safe bounds.");
    }

    if (evaluation.likely_cause.empty()) {
        if (!rabbit_ok) {
            evaluation.likely_cause = "rabbitmq_node";
            evaluation.escalation_required = true;
            evaluation.recommendation = "Escalate RabbitMQ infrastructure incident; do not restart the app worker first.";
        } else if (consumers_down && memory_high) {
            evaluation.likely_cause = "app_worker_memory_exhaustion";
            evaluation.recommendation = "Restart the app worker if messages are persistent and data-loss risk is acceptable.";
        } else if (evaluation.seasonal_override_applied && scenario.metrics.queue_depth <= scenario.seasonal.expected_depth_max) {
            evaluation.likely_cause = "seasonal_load";
            evaluation.recommendation = "Continue monitoring; current depth is expected for the configured seasonal window.";
        } else {
            evaluation.likely_cause = "unknown_manual_review";
            evaluation.escalation_required = true;
            evaluation.recommendation = "Capture logs and metrics, then contact the developer owner.";
        }
    }

    const bool restart_allowed =
        scenario.runbook.type == "systemd_restart_service" &&
        rabbit_ok == scenario.policy.require_rabbitmq_healthy &&
        persistent == scenario.policy.require_persistent_messages &&
        consumers_down &&
        low_loss &&
        (evaluation.likely_cause.find("app_worker") != std::string::npos ||
         evaluation.likely_cause.find("memory") != std::string::npos);

    if (restart_allowed) {
        evaluation.restart_recommended = true;
        evaluation.action_id = scenario.runbook.id;
        evaluation.decision = "recommend_runbook";
        evaluation.rationale.push_back("Decision policy allows `" + scenario.runbook.id +
            "` because RabbitMQ is healthy, messages are persistent, consumers are down, and estimated loss is <= " +
            format_number(scenario.policy.max_restart_loss_packets) + " packets.");
    } else if (evaluation.escalation_required) {
        evaluation.decision = "escalate_to_developer_or_infra_owner";
    } else {
        evaluation.decision = "monitor_or_wait";
    }
    if (evaluation.matched_signatures.empty() && !evaluation.restart_recommended &&
        evaluation.likely_cause != "seasonal_load") {
        evaluation.escalation_required = true;
        evaluation.decision = "escalate_to_developer_or_infra_owner";
    }
    return evaluation;
}

std::string render_threshold_evidence_json(const ThresholdScenario& scenario,
                                           const ThresholdEvaluation& evaluation,
                                           std::string_view evidence_path,
                                           std::string_view jira_path) {
    std::ostringstream out;
    out << "{"
        << "\"event_type\":\"omnix.threshold.evidence.v1\""
        << ",\"thresholdStatus\":\"" << escape_json(evaluation.threshold_status) << "\""
        << ",\"scenarioId\":\"" << escape_json(scenario.id) << "\""
        << ",\"alarm\":{\"id\":\"" << escape_json(scenario.alarm_id)
        << "\",\"severity\":\"" << escape_json(scenario.alarm_severity)
        << "\",\"timestamp\":\"" << escape_json(scenario.alarm_timestamp) << "\"}"
        << ",\"assets\":{\"server\":\"" << escape_json(scenario.asset.server)
        << "\",\"serverClass\":\"" << escape_json(scenario.asset.server_class)
        << "\",\"customer\":\"" << escape_json(scenario.asset.customer)
        << "\",\"site\":\"" << escape_json(scenario.asset.site)
        << "\",\"queue\":\"" << escape_json(scenario.asset.queue)
        << "\",\"rabbitmqNode\":\"" << escape_json(scenario.asset.rabbitmq_node)
        << "\",\"app\":\"" << escape_json(scenario.asset.app)
        << "\",\"serviceName\":\"" << escape_json(scenario.asset.service_name) << "\"}"
        << ",\"ownership\":{\"fedBy\":\"" << escape_json(scenario.ownership.fed_by)
        << "\",\"consumedBy\":\"" << escape_json(scenario.ownership.consumed_by)
        << "\",\"devOwner\":\"" << escape_json(scenario.ownership.dev_owner)
        << "\",\"infraOwner\":\"" << escape_json(scenario.ownership.infra_owner)
        << "\",\"logPaths\":" << render_string_array_json(scenario.ownership.log_paths) << "}"
        << ",\"metrics\":{\"cpuUsage\":" << format_number(scenario.metrics.cpu_usage)
        << ",\"ramUsage\":" << format_number(scenario.metrics.ram_usage)
        << ",\"mountHealthy\":" << (scenario.metrics.mount_healthy ? "true" : "false")
        << ",\"storageUsage\":" << format_number(scenario.metrics.storage_usage)
        << ",\"dataBrickHealthy\":" << (scenario.metrics.data_brick_healthy ? "true" : "false")
        << ",\"rabbitmqNodeHealthy\":" << (scenario.metrics.rabbitmq_node_healthy ? "true" : "false")
        << ",\"queueDepth\":" << format_number(scenario.metrics.queue_depth)
        << ",\"previousQueueDepth\":" << format_number(scenario.metrics.previous_queue_depth)
        << ",\"consumerCount\":" << format_number(scenario.metrics.consumer_count)
        << ",\"previousConsumerCount\":" << format_number(scenario.metrics.previous_consumer_count)
        << ",\"publishRate\":" << format_number(scenario.metrics.publish_rate)
        << ",\"ackRate\":" << format_number(scenario.metrics.ack_rate)
        << ",\"oldestMessageAgeSeconds\":" << format_number(scenario.metrics.oldest_message_age_seconds)
        << ",\"appServiceStatus\":\"" << escape_json(scenario.metrics.app_service_status)
        << "\",\"messagesPersistent\":" << (scenario.metrics.messages_persistent ? "true" : "false")
        << ",\"estimatedRestartLossPackets\":" << format_number(scenario.metrics.estimated_restart_loss_packets) << "}"
        << ",\"baseline\":{\"normalDepthMax\":" << format_number(scenario.baseline.normal_depth_max)
        << ",\"warningDepth\":" << format_number(scenario.baseline.warning_depth)
        << ",\"abnormalDepth\":" << format_number(scenario.baseline.abnormal_depth)
        << ",\"criticalDepth\":" << format_number(scenario.baseline.critical_depth)
        << ",\"p1Depth\":" << format_number(scenario.baseline.p1_depth) << "}"
        << ",\"seasonalOverride\":{\"name\":\"" << escape_json(scenario.seasonal.name)
        << "\",\"active\":" << (scenario.seasonal.active ? "true" : "false")
        << ",\"applied\":" << (evaluation.seasonal_override_applied ? "true" : "false") << "}"
        << ",\"matchedSignatures\":" << render_string_array_json(evaluation.matched_signatures)
        << ",\"decision\":{\"severity\":\"" << escape_json(evaluation.severity)
        << "\",\"likelyCause\":\"" << escape_json(evaluation.likely_cause)
        << "\",\"decision\":\"" << escape_json(evaluation.decision)
        << "\",\"recommendation\":\"" << escape_json(evaluation.recommendation)
        << "\",\"restartRecommended\":" << (evaluation.restart_recommended ? "true" : "false")
        << ",\"escalationRequired\":" << (evaluation.escalation_required ? "true" : "false")
        << ",\"actionId\":\"" << escape_json(evaluation.action_id)
        << "\",\"actionStatus\":\"" << escape_json(evaluation.action_status)
        << "\",\"rationale\":" << render_string_array_json(evaluation.rationale) << "}"
        << ",\"runbook\":{\"id\":\"" << escape_json(scenario.runbook.id)
        << "\",\"type\":\"" << escape_json(scenario.runbook.type)
        << "\",\"displayCommand\":\"" << escape_json(scenario.runbook.display_command)
        << "\",\"validationChecks\":" << render_string_array_json(scenario.runbook.validation_checks) << "}"
        << ",\"logs\":[";
    for (std::size_t index = 0; index < scenario.logs.size(); ++index) {
        if (index != 0) {
            out << ",";
        }
        out << "{\"path\":\"" << escape_json(scenario.logs[index].path)
            << "\",\"snippet\":\"" << escape_json(scenario.logs[index].snippet) << "\"}";
    }
    out << "]"
        << ",\"artifacts\":{\"evidencePath\":\"" << escape_json(evidence_path)
        << "\",\"jiraPath\":\"" << escape_json(jira_path) << "\"}";
    if (!evaluation.action_output.empty()) {
        out << ",\"actionOutput\":\"" << escape_json(evaluation.action_output) << "\"";
    }
    out << "}";
    return out.str();
}

std::string render_threshold_jira_markdown(const ThresholdScenario& scenario,
                                           const ThresholdEvaluation& evaluation) {
    std::ostringstream out;
    out << "# " << scenario.alarm_id << " - " << scenario.asset.queue << " Threshold Escalation\n\n"
        << "## Summary\n"
        << "- Server: `" << scenario.asset.server << "`\n"
        << "- Queue: `" << scenario.asset.queue << "`\n"
        << "- RabbitMQ node: `" << scenario.asset.rabbitmq_node << "`\n"
        << "- Application: `" << scenario.asset.app << "`\n"
        << "- Service: `" << scenario.asset.service_name << "`\n"
        << "- App owner: " << scenario.ownership.dev_owner << "\n"
        << "- Infra owner: " << scenario.ownership.infra_owner << "\n"
        << "- Decision: " << evaluation.decision << "\n"
        << "- Likely cause: " << evaluation.likely_cause << "\n\n"
        << "## Metric Snapshot\n"
        << "- Queue depth: " << format_number(scenario.metrics.queue_depth)
        << " (previous " << format_number(scenario.metrics.previous_queue_depth) << ")\n"
        << "- Consumer count: " << format_number(scenario.metrics.consumer_count)
        << " (previous " << format_number(scenario.metrics.previous_consumer_count) << ")\n"
        << "- RAM usage: " << format_number(scenario.metrics.ram_usage) << "%\n"
        << "- CPU usage: " << format_number(scenario.metrics.cpu_usage) << "%\n"
        << "- Oldest message age: " << format_number(scenario.metrics.oldest_message_age_seconds) << "s\n"
        << "- RabbitMQ healthy: " << (scenario.metrics.rabbitmq_node_healthy ? "yes" : "no") << "\n"
        << "- Messages persistent: " << (scenario.metrics.messages_persistent ? "yes" : "no") << "\n"
        << "- Estimated restart loss: " << format_number(scenario.metrics.estimated_restart_loss_packets) << " packets\n\n"
        << "## Evidence\n";
    for (const std::string& signature : evaluation.matched_signatures) {
        out << "- Matched error signature: `" << signature << "`\n";
    }
    for (const ThresholdLogSnippet& log : scenario.logs) {
        out << "- `" << log.path << "`: " << log.snippet << "\n";
    }
    out << "\n## Recommended Runbook\n"
        << "- Action ID: `" << scenario.runbook.id << "`\n"
        << "- Command: `" << scenario.runbook.display_command << "`\n"
        << "- Action status: " << evaluation.action_status << "\n\n"
        << "## Validation Checklist\n";
    for (const std::string& check : scenario.runbook.validation_checks) {
        out << "- " << check << "\n";
    }
    out << "\n## Developer Recommendation\n"
        << evaluation.recommendation << "\n\n"
        << "## Rationale\n";
    for (const std::string& item : evaluation.rationale) {
        out << "- " << item << "\n";
    }
    return out.str();
}

bool write_text_file(const std::string& path, const std::string& contents, std::string* error) {
    if (path.empty()) {
        return true;
    }
    std::filesystem::path target(path);
    std::error_code ec;
    if (target.has_parent_path()) {
        std::filesystem::create_directories(target.parent_path(), ec);
        if (ec) {
            if (error != nullptr) {
                *error = "Unable to create parent directory for `" + path + "`.";
            }
            return false;
        }
    }
    std::ofstream output(target);
    if (!output) {
        if (error != nullptr) {
            *error = "Unable to open output path `" + path + "`.";
        }
        return false;
    }
    output << contents;
    return true;
}

std::filesystem::path expand_operator_path(std::string_view raw_path) {
    std::string text(raw_path);
    if (text == "~") {
        if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
            return std::filesystem::path(home);
        }
    }
    if (text.rfind("~/", 0) == 0) {
        if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
            return std::filesystem::path(home) / text.substr(2);
        }
    }
    return std::filesystem::path(text);
}

bool remove_existing_link_target(const std::filesystem::path& path, std::string* error) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) && !std::filesystem::is_symlink(std::filesystem::symlink_status(path, ec))) {
        return true;
    }
    std::filesystem::remove(path, ec);
    if (ec) {
        if (error != nullptr) {
            *error = "Unable to remove existing path `" + path.string() + "`.";
        }
        return false;
    }
    return true;
}

bool safe_shim_token(std::string_view token) {
    if (token.empty()) {
        return false;
    }
    for (char c : token) {
        const unsigned char byte = static_cast<unsigned char>(c);
        if (!(std::isalnum(byte) || c == '-' || c == '_' || c == '.')) {
            return false;
        }
    }
    return true;
}

BuiltinToolResult invoke_symlink_tool(const std::vector<std::string>& arguments) {
    BuiltinToolResult result;
    result.invocation.logical_name = "symlink";
    result.invocation.selected_provider = "omnix-runtime";
    result.invocation.executable_path = "omnix-runtime";
    result.invocation.cache_origin = "builtin_analyst_module";
    result.invocation.command_line = "omnix::symlink " + join_arguments(arguments);

    if (arguments.empty() || arguments[0] == "doctor") {
        result.invocation.status = "ok";
        result.invocation.summary = "OmniX can create local symlinks and POSIX namespace shims without arbitrary shell execution.";
        result.invocation.output_excerpt = {
            "{\"event_type\":\"omnix.symlink.doctor.v1\",\"status\":\"builtin_ready\","
            "\"commands\":[\"create <target> <link-path>\",\"shim <name> <namespace> --prefix <dir> --bin <omnix-bin>\"],"
            "\"safety\":\"existing paths require --force; generated shims are written but not executed\"}",
        };
        result.next_action = "Use `omnix tool symlink -- create ./build/omnix ~/.local/bin/omnix` or `omnix tool symlink -- shim tze tze --prefix ~/.local/bin`.";
        return result;
    }

    const std::string mode = arguments[0];
    if (mode == "create") {
        if (arguments.size() < 3) {
            result.invocation.status = "invalid_arguments";
            result.invocation.summary = "`symlink create` requires `<target> <link-path>`.";
            result.invocation.exit_code = 1;
            return result;
        }

        bool force = false;
        bool allow_missing = false;
        for (std::size_t index = 3; index < arguments.size(); ++index) {
            if (arguments[index] == "--force") {
                force = true;
            } else if (arguments[index] == "--allow-missing") {
                allow_missing = true;
            } else {
                result.invocation.status = "invalid_arguments";
                result.invocation.summary = "Unknown `symlink create` option `" + arguments[index] + "`.";
                result.invocation.exit_code = 1;
                return result;
            }
        }

        const std::filesystem::path target = expand_operator_path(arguments[1]);
        const std::filesystem::path link_path = expand_operator_path(arguments[2]);
        std::error_code ec;
        if (!allow_missing && !std::filesystem::exists(target, ec)) {
            result.invocation.status = "target_missing";
            result.invocation.summary = "Symlink target does not exist: `" + target.string() + "`.";
            result.invocation.exit_code = 1;
            result.next_action = "Build or create the target first, or rerun with `--allow-missing` for intentional future targets.";
            return result;
        }

        if ((std::filesystem::exists(link_path, ec) || std::filesystem::is_symlink(std::filesystem::symlink_status(link_path, ec))) && !force) {
            result.invocation.status = "path_exists";
            result.invocation.summary = "Refusing to replace existing path `" + link_path.string() + "` without `--force`.";
            result.invocation.exit_code = 1;
            return result;
        }

        if (link_path.has_parent_path()) {
            std::filesystem::create_directories(link_path.parent_path(), ec);
            if (ec) {
                result.invocation.status = "link_failed";
                result.invocation.summary = "Unable to create parent directory `" + link_path.parent_path().string() + "`.";
                result.invocation.exit_code = 1;
                return result;
            }
        }

        std::string remove_error;
        if (force && !remove_existing_link_target(link_path, &remove_error)) {
            result.invocation.status = "link_failed";
            result.invocation.summary = remove_error;
            result.invocation.exit_code = 1;
            return result;
        }

        std::filesystem::create_symlink(target, link_path, ec);
        if (ec) {
            result.invocation.status = "link_failed";
            result.invocation.summary = "Unable to create symlink `" + link_path.string() + "` -> `" + target.string() + "`: " + ec.message();
            result.invocation.exit_code = 1;
            return result;
        }

        result.invocation.status = "ok";
        result.invocation.summary = "Created filesystem symlink.";
        result.invocation.output_excerpt = {
            "{\"event_type\":\"omnix.symlink.created.v1\",\"linkPath\":\"" + escape_json(link_path.string()) +
            "\",\"targetPath\":\"" + escape_json(target.string()) + "\",\"force\":" + std::string(force ? "true" : "false") + "}",
        };
        result.next_action = "Use the linked command directly if its parent directory is on PATH.";
        return result;
    }

    if (mode == "shim") {
        if (arguments.size() < 3) {
            result.invocation.status = "invalid_arguments";
            result.invocation.summary = "`symlink shim` requires `<name> <namespace>`.";
            result.invocation.exit_code = 1;
            return result;
        }

        const std::string name = arguments[1];
        const std::string namespace_name = arguments[2];
        if (!safe_shim_token(name) || !safe_shim_token(namespace_name)) {
            result.invocation.status = "invalid_arguments";
            result.invocation.summary = "Shim name and namespace may only contain letters, numbers, dot, dash, and underscore.";
            result.invocation.exit_code = 1;
            return result;
        }
        std::filesystem::path prefix = expand_operator_path("~/.local/bin");
        std::filesystem::path omnix_bin = expand_operator_path("./build/omnix");
        bool force = false;
        for (std::size_t index = 3; index < arguments.size(); ++index) {
            const std::string& arg = arguments[index];
            if (arg == "--prefix") {
                if (index + 1 >= arguments.size()) {
                    result.invocation.status = "invalid_arguments";
                    result.invocation.summary = "`symlink shim --prefix` requires a directory.";
                    result.invocation.exit_code = 1;
                    return result;
                }
                prefix = expand_operator_path(arguments[++index]);
            } else if (arg == "--bin") {
                if (index + 1 >= arguments.size()) {
                    result.invocation.status = "invalid_arguments";
                    result.invocation.summary = "`symlink shim --bin` requires an OmniX binary path.";
                    result.invocation.exit_code = 1;
                    return result;
                }
                omnix_bin = expand_operator_path(arguments[++index]);
            } else if (arg == "--force") {
                force = true;
            } else {
                result.invocation.status = "invalid_arguments";
                result.invocation.summary = "Unknown `symlink shim` option `" + arg + "`.";
                result.invocation.exit_code = 1;
                return result;
            }
        }

        std::error_code ec;
        if (!std::filesystem::exists(omnix_bin, ec)) {
            result.invocation.status = "target_missing";
            result.invocation.summary = "OmniX binary for shim does not exist: `" + omnix_bin.string() + "`.";
            result.invocation.exit_code = 1;
            return result;
        }

        std::filesystem::create_directories(prefix, ec);
        if (ec) {
            result.invocation.status = "shim_failed";
            result.invocation.summary = "Unable to create shim directory `" + prefix.string() + "`.";
            result.invocation.exit_code = 1;
            return result;
        }

        const std::filesystem::path shim_path = prefix / name;
        if ((std::filesystem::exists(shim_path, ec) || std::filesystem::is_symlink(std::filesystem::symlink_status(shim_path, ec))) && !force) {
            result.invocation.status = "path_exists";
            result.invocation.summary = "Refusing to replace existing shim `" + shim_path.string() + "` without `--force`.";
            result.invocation.exit_code = 1;
            return result;
        }

        std::string remove_error;
        if (force && !remove_existing_link_target(shim_path, &remove_error)) {
            result.invocation.status = "shim_failed";
            result.invocation.summary = remove_error;
            result.invocation.exit_code = 1;
            return result;
        }

        const std::string shim =
            "#!/usr/bin/env sh\n"
            "# OmniX-managed shim. Recreate with `omnix tool symlink -- shim`.\n"
            "exec " + shell_quote(omnix_bin.string()) + " " + namespace_name + " \"$@\"\n";
        std::string write_error;
        if (!write_text_file(shim_path.string(), shim, &write_error)) {
            result.invocation.status = "shim_failed";
            result.invocation.summary = write_error;
            result.invocation.exit_code = 1;
            return result;
        }
        std::filesystem::permissions(shim_path,
                                     std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec |
                                         std::filesystem::perms::others_exec,
                                     std::filesystem::perm_options::add,
                                     ec);
        if (ec) {
            result.invocation.status = "shim_failed";
            result.invocation.summary = "Wrote shim but could not mark it executable: `" + shim_path.string() + "`.";
            result.invocation.exit_code = 1;
            return result;
        }

        result.invocation.status = "ok";
        result.invocation.summary = "Created POSIX namespace shim.";
        result.invocation.output_excerpt = {
            "{\"event_type\":\"omnix.symlink.shim.v1\",\"shimPath\":\"" + escape_json(shim_path.string()) +
            "\",\"omnixBin\":\"" + escape_json(omnix_bin.string()) +
            "\",\"namespace\":\"" + escape_json(namespace_name) + "\"}",
        };
        result.next_action = "Run `" + name + " ...` directly if `" + prefix.string() + "` is on PATH.";
        return result;
    }

    result.invocation.status = "invalid_arguments";
    result.invocation.summary = "`symlink` supports `doctor`, `create`, and `shim`.";
    result.invocation.exit_code = 1;
    result.next_action = "Use `omnix tool doctor symlink` for examples.";
    return result;
}

std::string resolve_systemctl_path() {
    if (const char* override = std::getenv("OMNIX_THRESHOLDS_SYSTEMCTL");
        override != nullptr && *override != '\0') {
        return override;
    }
    if (const std::optional<std::string> resolved = command_path("systemctl")) {
        return *resolved;
    }
    return "/bin/systemctl";
}

void maybe_execute_threshold_action(const ThresholdScenario& scenario,
                                    ThresholdEvaluation& evaluation,
                                    bool execute_requested) {
    if (!execute_requested) {
        evaluation.action_status = "recommendation_only";
        return;
    }
    if (!evaluation.restart_recommended || scenario.runbook.type != "systemd_restart_service") {
        evaluation.action_status = "action_not_allowed";
        evaluation.action_output = "No allowlisted runbook action was eligible for execution.";
        return;
    }

    const std::string expected = "EXECUTE " + scenario.runbook.id;
    std::cout << "Confirm runbook action by typing `" << expected << "`: " << std::flush;
    std::string response;
    std::getline(std::cin, response);
    if (response != expected) {
        evaluation.action_status = "action_not_executed";
        evaluation.action_output = "Interactive confirmation did not match `" + expected + "`.";
        return;
    }

    const std::string systemctl = resolve_systemctl_path();
    const CommandResult command = run_command_parts({systemctl, "restart", scenario.runbook.service_name});
    evaluation.action_output = command.output.empty() ? "(no command output)" : command.output;
    evaluation.action_status = command.exit_code == 0 ? "remediation_unvalidated" : "action_failed";
    evaluation.rationale.push_back("Executed allowlisted systemd restart for `" + scenario.runbook.service_name +
        "`; validation evidence is still required before declaring recovery.");
}

BuiltinToolResult invoke_thresholds(const std::vector<std::string>& arguments, bool verbose) {
    BuiltinToolResult result;
    result.invocation.logical_name = "thresholds";
    result.invocation.selected_provider = "analyst_module";
    result.invocation.cache_origin = "builtin_analyst_module";
    result.invocation.command_line = "omnix::thresholds";

    ThresholdCliInput input;
    try {
        input = parse_thresholds_input(arguments);
    } catch (const std::exception& error) {
        result.invocation.status = "invalid_arguments";
        result.invocation.summary = error.what();
        result.invocation.exit_code = 1;
        result.next_action = "Use `omnix tool thresholds -- evaluate res/thresholds/rabbitmq-xxb-incident.json` or `omnix tool thresholds -- gsmg res/thresholds/gsmg-rabbitmq-xxb.json`.";
        return result;
    }

    try {
        if (input.mode == "gsmg") {
            if (input.execute) {
                result.invocation.status = "action_not_allowed";
                result.invocation.summary = "GSMg mode is evaluation-only in this phase; surgical `evaluate` keeps prompt-gated execution.";
                result.invocation.exit_code = 1;
                result.next_action = "Run `omnix tool thresholds -- gsmg <scenario.json>` without `--execute`.";
                return result;
            }
            const GenericSignalScenario scenario = load_gsmg_scenario(input.scenario_path);
            const GenericSignalEvaluation evaluation = evaluate_gsmg_scenario(scenario);
            const std::string evidence_json = render_gsmg_evidence_json(
                scenario, evaluation, input.evidence_out, input.jira_out);
            const std::string jira_markdown = render_gsmg_jira_markdown(scenario, evaluation);
            std::string write_error;
            if (!write_text_file(input.evidence_out, evidence_json, &write_error) ||
                !write_text_file(input.jira_out, jira_markdown, &write_error)) {
                result.invocation.status = "artifact_write_failed";
                result.invocation.summary = write_error;
                result.invocation.exit_code = 1;
                result.next_action = "Choose writable paths for `--out` and `--jira-out`.";
                return result;
            }
            result.invocation.status = "ok";
            result.invocation.summary = "gsmg_evaluated: " + evaluation.decision +
                " cause=" + evaluation.likely_cause + " severity=" + evaluation.severity;
            result.invocation.exit_code = 0;
            result.invocation.output_excerpt = {evidence_json};
            if (!input.evidence_out.empty()) {
                result.produced_artifact = input.evidence_out;
            } else if (!input.jira_out.empty()) {
                result.produced_artifact = input.jira_out;
            }
            result.next_action = "Backtrace matched policies and signal boundaries before turning GSMg recommendations into surgical runbooks.";
            if (verbose) {
                result.invocation.output_excerpt.push_back(jira_markdown);
            }
            return result;
        }

        const ThresholdScenario scenario = load_threshold_scenario(input.scenario_path);
        ThresholdEvaluation evaluation = evaluate_threshold_scenario(scenario);
        maybe_execute_threshold_action(scenario, evaluation, input.execute);

        const std::string evidence_json = render_threshold_evidence_json(
            scenario, evaluation, input.evidence_out, input.jira_out);
        const std::string jira_markdown = render_threshold_jira_markdown(scenario, evaluation);
        std::string write_error;
        if (!write_text_file(input.evidence_out, evidence_json, &write_error) ||
            !write_text_file(input.jira_out, jira_markdown, &write_error)) {
            result.invocation.status = "artifact_write_failed";
            result.invocation.summary = write_error;
            result.invocation.exit_code = 1;
            result.next_action = "Choose writable paths for `--out` and `--jira-out`.";
            return result;
        }

        result.invocation.status = evaluation.action_status == "action_not_executed" ? "action_not_executed" : "ok";
        result.invocation.summary = "thresholds_evaluated: " + evaluation.decision +
            " cause=" + evaluation.likely_cause + " severity=" + evaluation.severity;
        if (evaluation.action_status == "remediation_unvalidated") {
            result.invocation.summary += " action=remediation_unvalidated";
        }
        result.invocation.exit_code = result.invocation.status == "ok" ? 0 : 1;
        result.invocation.output_excerpt = {evidence_json};
        if (!input.evidence_out.empty()) {
            result.produced_artifact = input.evidence_out;
        } else if (!input.jira_out.empty()) {
            result.produced_artifact = input.jira_out;
        }
        result.next_action = evaluation.restart_recommended
            ? "Review evidence, then run with `--execute` only if you intend to restart the declared service."
            : "Escalate with the generated Jira packet or collect more black-box evidence.";
        if (verbose) {
            result.invocation.output_excerpt.push_back(jira_markdown);
        }
        return result;
    } catch (const std::exception& error) {
        result.invocation.status = "threshold_scenario_invalid";
        result.invocation.summary = error.what();
        result.invocation.exit_code = 1;
        result.next_action = "Validate the threshold or GSMg scenario JSON and required domain fields.";
        return result;
    }
}

std::filesystem::path resolve_ghostline_cli_path() {
    if (const char* override = std::getenv("OMNIX_GHOSTLINE_CLI");
        override != nullptr && *override != '\0') {
        return override;
    }
    std::vector<std::filesystem::path> candidates;
#ifdef OMNIX_BUILD_DIR
    candidates.push_back(std::filesystem::path(OMNIX_BUILD_DIR) / "vendor" / "ghostline-gate" / "ghostline_cli");
    candidates.push_back(std::filesystem::path(OMNIX_BUILD_DIR) / "ghostline_cli");
#endif
#ifdef OMNIX_SOURCE_DIR
    candidates.push_back(std::filesystem::path(OMNIX_SOURCE_DIR) / "vendor" / "ghostline-gate" / "build-local" / "ghostline_cli");
#endif
    candidates.push_back(std::filesystem::current_path() / "build" / "vendor" / "ghostline-gate" / "ghostline_cli");
    candidates.push_back(std::filesystem::current_path() / "vendor" / "ghostline-gate" / "build-local" / "ghostline_cli");
    for (const std::filesystem::path& candidate : candidates) {
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec) && !std::filesystem::is_directory(candidate, ec)) {
            return candidate;
        }
    }
    return {};
}

std::string ghostline_analysis_code(std::string_view type,
                                    std::string_view stage,
                                    std::string_view message) {
    const std::string lowered = lowercase(std::string(type) + " " + std::string(stage) + " " + std::string(message));
    if (lowered.find("review") != std::string::npos) {
        return "NET.GHOSTLINE.REVIEW_REQUIRED";
    }
    if (lowered.find("parse") != std::string::npos ||
        lowered.find("risk") != std::string::npos ||
        lowered.find("blocked") != std::string::npos ||
        lowered.find("failed") != std::string::npos) {
        return "NET.GHOSTLINE.PARSE_OR_VALIDATION_RISK";
    }
    const std::size_t fallback = lowered.find("fallback=");
    if (fallback != std::string::npos) {
        const std::string fallback_value = trim(lowered.substr(fallback + std::string("fallback=").size()));
        if (!fallback_value.empty() && fallback_value != "\"\"") {
            return "NET.GHOSTLINE.FALLBACK_ORIGINAL";
        }
    }
    if (stage == "released-modified" || type == "candidate-release-modified") {
        return "NET.GHOSTLINE.MODIFIED_RELEASED";
    }
    if (stage == "released-original" || type == "candidate-release-original") {
        return "NET.GHOSTLINE.ORIGINAL_RELEASED";
    }
    if (type == "framed-packet" || type == "plugin-detect") {
        return "NET.GHOSTLINE.FRAME_DETECTED";
    }
    return "NET.GHOSTLINE.FRAME_DETECTED";
}

std::string ghostline_classification(std::string_view code) {
    if (code == "NET.GHOSTLINE.MODIFIED_RELEASED") {
        return "ghostline_modified_release";
    }
    if (code == "NET.GHOSTLINE.ORIGINAL_RELEASED") {
        return "ghostline_original_release";
    }
    if (code == "NET.GHOSTLINE.FALLBACK_ORIGINAL") {
        return "ghostline_fallback_original";
    }
    if (code == "NET.GHOSTLINE.REVIEW_REQUIRED") {
        return "ghostline_review_required";
    }
    if (code == "NET.GHOSTLINE.PARSE_OR_VALIDATION_RISK") {
        return "ghostline_parse_or_validation_risk";
    }
    return "ghostline_frame_detected";
}

std::size_t hex_payload_length(std::string_view hex) {
    return hex.size() / 2;
}

std::string optional_json_scalar_string(const MlpJsonValue& object, std::string_view key) {
    const auto value = optional_object_field(object, key);
    if (!value.has_value()) {
        return {};
    }
    return json_value_to_string(value->get());
}

std::string render_ghostline_tview_event(const MlpJsonValue& event) {
    const std::string type = optional_json_scalar_string(event, "type");
    const std::string stage = optional_json_scalar_string(event, "stage");
    const std::string message = optional_json_scalar_string(event, "message");
    const std::string code = ghostline_analysis_code(type, stage, message);
    const std::string original_hex = optional_json_scalar_string(event, "original");
    const std::string modified_hex = optional_json_scalar_string(event, "modified");
    const std::string payload_hex = modified_hex.empty() ? original_hex : modified_hex;
    const std::string flow = optional_json_scalar_string(event, "flow");
    const std::string seq = optional_json_scalar_string(event, "seq");
    const std::string direction = optional_json_scalar_string(event, "dir");
    const std::string plugin = optional_json_scalar_string(event, "plugin");
    const std::size_t payload_length = hex_payload_length(payload_hex);

    std::ostringstream out;
    out << "{\"event_type\":\"omnix.tview.packet.v1\""
        << ",\"source\":\"ghostline\""
        << ",\"timestamp\":\"" << escape_json(optional_json_scalar_string(event, "ts")) << "\""
        << ",\"src_ip\":\"0.0.0.0\""
        << ",\"src_port\":0"
        << ",\"dst_ip\":\"0.0.0.0\""
        << ",\"dst_port\":0"
        << ",\"tcp_flags\":\"\""
        << ",\"payload_length\":" << payload_length
        << ",\"classification\":\"" << ghostline_classification(code) << "\""
        << ",\"analysis_code\":\"" << code << "\""
        << ",\"hex_preview\":\"" << escape_json(payload_hex.substr(0, std::min<std::size_t>(payload_hex.size(), 192))) << "\""
        << ",\"ghostline\":{\"flow\":\"" << escape_json(flow)
        << "\",\"seq\":\"" << escape_json(seq)
        << "\",\"direction\":\"" << escape_json(direction)
        << "\",\"plugin\":\"" << escape_json(plugin)
        << "\",\"type\":\"" << escape_json(type)
        << "\",\"stage\":\"" << escape_json(stage)
        << "\",\"message\":\"" << escape_json(message)
        << "\",\"originalHex\":\"" << escape_json(original_hex)
        << "\",\"modifiedHex\":\"" << escape_json(modified_hex) << "\"}}";
    return out.str();
}

struct GhostlineAuditBridgeResult {
    std::string status = "gg_audit_converted";
    std::size_t events = 0;
    std::size_t modified = 0;
    std::size_t fallback = 0;
    std::size_t review = 0;
    std::size_t risk = 0;
    std::vector<std::string> output_lines;
};

GhostlineAuditBridgeResult convert_ghostline_audit_to_tview(const std::filesystem::path& input_path) {
    std::ifstream input(input_path);
    if (!input) {
        throw std::runtime_error("Unable to open Ghostline audit JSONL `" + input_path.string() + "`.");
    }
    GhostlineAuditBridgeResult result;
    for (std::string line; std::getline(input, line);) {
        if (trim(line).empty()) {
            continue;
        }
        const MlpJsonValue event = MlpJsonParser(line).parse();
        if (event.type != MlpJsonValue::Type::Object) {
            continue;
        }
        const std::string type = optional_json_scalar_string(event, "type");
        const std::string stage = optional_json_scalar_string(event, "stage");
        const std::string message = optional_json_scalar_string(event, "message");
        const std::string code = ghostline_analysis_code(type, stage, message);
        if (code == "NET.GHOSTLINE.MODIFIED_RELEASED") {
            ++result.modified;
        } else if (code == "NET.GHOSTLINE.FALLBACK_ORIGINAL") {
            ++result.fallback;
        } else if (code == "NET.GHOSTLINE.REVIEW_REQUIRED") {
            ++result.review;
        } else if (code == "NET.GHOSTLINE.PARSE_OR_VALIDATION_RISK") {
            ++result.risk;
        }
        result.output_lines.push_back(render_ghostline_tview_event(event));
        ++result.events;
    }
    if (result.events == 0) {
        result.status = "gg_audit_empty";
    }
    return result;
}

std::string render_ghostline_audit_summary_json(const GhostlineAuditBridgeResult& bridge,
                                                std::string_view input_path,
                                                std::string_view output_path) {
    std::ostringstream out;
    out << "{\"event_type\":\"omnix.gg.audit_bridge.v1\""
        << ",\"status\":\"" << bridge.status << "\""
        << ",\"inputPath\":\"" << escape_json(input_path)
        << "\",\"outputPath\":\"" << escape_json(output_path)
        << "\",\"events\":" << bridge.events
        << ",\"modifiedReleased\":" << bridge.modified
        << ",\"fallbackOriginal\":" << bridge.fallback
        << ",\"reviewRequired\":" << bridge.review
        << ",\"parseOrValidationRisk\":" << bridge.risk << "}";
    return out.str();
}

std::string join_jsonl_lines(const std::vector<std::string>& lines) {
    std::ostringstream out;
    for (const std::string& line : lines) {
        out << line << "\n";
    }
    return out.str();
}

std::string summarize_ghostline_actions_jsonl(const std::filesystem::path& input_path) {
    std::ifstream input(input_path);
    if (!input) {
        throw std::runtime_error("Unable to open Ghostline actions JSONL `" + input_path.string() + "`.");
    }
    std::size_t actions = 0;
    std::vector<std::string> plugins;
    std::vector<std::string> titles;
    for (std::string line; std::getline(input, line);) {
        if (trim(line).empty()) {
            continue;
        }
        const MlpJsonValue event = MlpJsonParser(line).parse();
        if (event.type != MlpJsonValue::Type::Object) {
            continue;
        }
        ++actions;
        append_unique(plugins, optional_json_scalar_string(event, "plugin"));
        append_unique(titles, optional_json_scalar_string(event, "title"));
    }
    std::ostringstream out;
    out << "{\"event_type\":\"omnix.gg.actions.v1\""
        << ",\"status\":\"gg_actions_read\""
        << ",\"inputPath\":\"" << escape_json(input_path.string())
        << "\",\"actions\":" << actions
        << ",\"plugins\":" << render_string_array_json(plugins)
        << ",\"titles\":" << render_string_array_json(titles) << "}";
    return out.str();
}

std::string quote_command_part(std::string_view value) {
    if (value.find_first_of(" \t\n'\"\\") == std::string_view::npos) {
        return std::string(value);
    }
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

std::string render_command_line(const std::vector<std::string>& parts) {
    std::ostringstream out;
    for (std::size_t index = 0; index < parts.size(); ++index) {
        if (index != 0) {
            out << ' ';
        }
        out << quote_command_part(parts[index]);
    }
    return out.str();
}

BuiltinToolResult invoke_gg(const std::vector<std::string>& arguments) {
    BuiltinToolResult result;
    result.invocation.logical_name = "gg";
    result.invocation.selected_provider = "analyst_module";
    result.invocation.cache_origin = "builtin_analyst_module";
    result.invocation.command_line = "omnix::gg";

    if (arguments.empty() || arguments.front() == "doctor") {
        const std::filesystem::path cli = resolve_ghostline_cli_path();
        const bool ready = !cli.empty();
        result.invocation.status = ready ? "ok" : "gg_unavailable";
        result.invocation.summary = ready
            ? "Ghostline Gate is vendored and ready; mutation requires explicit `gg run` arguments."
            : "Ghostline Gate CLI was not found in the OmniX build.";
        result.invocation.exit_code = ready ? 0 : 1;
        result.invocation.output_excerpt = {
            std::string("{\"event_type\":\"omnix.gg.doctor.v1\",\"status\":\"") +
            (ready ? "gg_ready" : "gg_unavailable") +
            "\",\"ghostlineCli\":\"" + escape_json(cli.string()) +
            "\",\"safety\":\"original delivery wins unless modified release is proven safe\","
            "\"mutation\":\"explicit run only\",\"bridge\":\"audit JSONL -> omnix.tview.packet.v1\"}",
        };
        result.next_action = ready
            ? "Use `omnix gg search --port 1883 --listen-only` or `omnix gg audit <audit.jsonl> --out /tmp/gg-tview.jsonl`."
            : "Build OmniX so the vendored `ghostline_cli` target is available.";
        return result;
    }

    const std::string mode = arguments.front();
    if (mode == "audit") {
        if (arguments.size() < 2) {
            result.invocation.status = "invalid_arguments";
            result.invocation.summary = "`gg audit` requires a Ghostline audit JSONL path.";
            result.invocation.exit_code = 1;
            result.next_action = "Use `omnix gg audit <ghostline_audit.jsonl> --out /tmp/gg-tview.jsonl`.";
            return result;
        }
        std::string input_path = arguments[1];
        std::string output_path;
        for (std::size_t index = 2; index < arguments.size(); ++index) {
            if (arguments[index] == "--out") {
                if (index + 1 >= arguments.size()) {
                    result.invocation.status = "invalid_arguments";
                    result.invocation.summary = "`--out` requires a path.";
                    result.invocation.exit_code = 1;
                    return result;
                }
                output_path = arguments[++index];
                continue;
            }
            result.invocation.status = "invalid_arguments";
            result.invocation.summary = "Unexpected `gg audit` argument `" + arguments[index] + "`.";
            result.invocation.exit_code = 1;
            return result;
        }
        try {
            const GhostlineAuditBridgeResult bridge = convert_ghostline_audit_to_tview(input_path);
            const std::string output_jsonl = join_jsonl_lines(bridge.output_lines);
            std::string write_error;
            if (!output_path.empty() && !write_text_file(output_path, output_jsonl, &write_error)) {
                result.invocation.status = "artifact_write_failed";
                result.invocation.summary = write_error;
                result.invocation.exit_code = 1;
                return result;
            }
            result.invocation.status = bridge.status == "gg_audit_empty" ? "gg_audit_empty" : "ok";
            result.invocation.summary = "Converted Ghostline audit JSONL into TView-compatible packet/transit evidence.";
            result.invocation.exit_code = 0;
            result.invocation.output_excerpt = {render_ghostline_audit_summary_json(bridge, input_path, output_path)};
            if (output_path.empty() && !bridge.output_lines.empty()) {
                result.invocation.output_excerpt.push_back(bridge.output_lines.front());
            }
            result.produced_artifact = output_path;
            result.next_action = output_path.empty()
                ? "Rerun with `--out <file>.jsonl`, then route it with `omnix nn route tview <file>.jsonl`."
                : "Route the converted evidence with `omnix nn route tview " + output_path + "`.";
            return result;
        } catch (const std::exception& error) {
            result.invocation.status = "gg_audit_invalid";
            result.invocation.summary = error.what();
            result.invocation.exit_code = 1;
            result.next_action = "Provide a Ghostline audit JSONL file produced with `--audit-json`.";
            return result;
        }
    }

    if (mode == "actions") {
        if (arguments.size() < 2) {
            result.invocation.status = "invalid_arguments";
            result.invocation.summary = "`gg actions` requires a Ghostline actions JSONL path.";
            result.invocation.exit_code = 1;
            return result;
        }
        try {
            result.invocation.status = "ok";
            result.invocation.summary = "Read Ghostline review/action JSONL without executing any action.";
            result.invocation.exit_code = 0;
            result.invocation.output_excerpt = {summarize_ghostline_actions_jsonl(arguments[1])};
            result.next_action = "Review action items in Ghostline before approving or replaying them.";
            return result;
        } catch (const std::exception& error) {
            result.invocation.status = "gg_actions_invalid";
            result.invocation.summary = error.what();
            result.invocation.exit_code = 1;
            return result;
        }
    }

    const std::filesystem::path cli = resolve_ghostline_cli_path();
    if (cli.empty()) {
        result.invocation.status = "gg_unavailable";
        result.invocation.summary = "Ghostline Gate CLI was not found in the OmniX build.";
        result.invocation.exit_code = 1;
        result.next_action = "Run `cmake --build build -j4` to build the vendored Ghostline CLI.";
        return result;
    }

    std::vector<std::string> command = {cli.string()};
    if (mode == "search") {
        for (std::size_t index = 1; index < arguments.size(); ++index) {
            const std::string& arg = arguments[index];
            if (arg == "--pid") {
                if (index + 1 >= arguments.size()) {
                    result.invocation.status = "invalid_arguments";
                    result.invocation.summary = "`gg search --pid` requires a term.";
                    result.invocation.exit_code = 1;
                    return result;
                }
                command.push_back("--search-pid");
                command.push_back(arguments[++index]);
            } else if (arg == "--port") {
                if (index + 1 >= arguments.size()) {
                    result.invocation.status = "invalid_arguments";
                    result.invocation.summary = "`gg search --port` requires a port.";
                    result.invocation.exit_code = 1;
                    return result;
                }
                command.push_back("--search-port");
                command.push_back(arguments[++index]);
            } else {
                command.push_back(arg);
            }
        }
        if (command.size() == 1) {
            result.invocation.status = "invalid_arguments";
            result.invocation.summary = "`gg search` requires `--pid <term>` or `--port <port>`.";
            result.invocation.exit_code = 1;
            return result;
        }
    } else if (mode == "run") {
        if (arguments.size() < 4) {
            result.invocation.status = "invalid_arguments";
            result.invocation.summary = "`gg run` requires `<listen-port> <upstream-host> <upstream-port>`.";
            result.invocation.exit_code = 1;
            result.next_action = "Use `omnix gg run 7777 127.0.0.1 1883 --protocol-hint mqtt --audit-json /tmp/gg.jsonl`.";
            return result;
        }
        command.insert(command.end(), arguments.begin() + 1, arguments.end());
    } else {
        result.invocation.status = "invalid_arguments";
        result.invocation.summary = "`gg` supports doctor, search, run, audit, and actions.";
        result.invocation.exit_code = 1;
        result.next_action = "Use `omnix gg doctor` for examples and safety notes.";
        return result;
    }

    result.invocation.command_line = render_command_line(command);
    const CommandResult executed = run_command_parts(command);
    const bool empty_search_ok =
        mode == "search" && executed.output.find("No TCP PID matches found") != std::string::npos;
    result.invocation.exit_code = empty_search_ok ? 0 : executed.exit_code;
    result.invocation.status = result.invocation.exit_code == 0 ? "ok" : "command_failed";
    result.invocation.summary = mode == "run"
        ? "Executed explicit Ghostline run command."
        : "Executed Ghostline search command.";
    result.invocation.output_excerpt = split_lines(executed.output, 24);
    result.next_action = mode == "run"
        ? "Inspect `--audit-json` / `--actions-json` outputs, then convert audit evidence with `omnix gg audit <file> --out <tview.jsonl>`."
        : "Use a target profile or explicit `gg run` only when you intend to place Ghostline in the traffic path.";
    return result;
}

BuiltinToolResult invoke_builtin_tool(std::string_view logical_name,
                                      const std::vector<std::string>& arguments,
                                      MemorySnapshot& memory,
                                      const BuildExecutor& builder,
                                      const NativeToolRegistry& tools,
                                      bool allow_deep_hunt,
                                      bool verbose_output) {
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
    if (logical_name == "mlp-lens") {
        return invoke_mlp_lens(arguments, verbose_output);
    }
    if (logical_name == "thresholds") {
        return invoke_thresholds(arguments, verbose_output);
    }
    if (logical_name == "symlink") {
        return invoke_symlink_tool(arguments);
    }
    if (logical_name == "gg") {
        return invoke_gg(arguments);
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
            logical_name, profile.tool_arguments, memory, builder, tools, true, profile.verbose_output);
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
