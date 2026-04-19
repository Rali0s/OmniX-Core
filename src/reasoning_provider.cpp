#include "tze/reasoning_provider.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace tze {
namespace {

struct ParsedUrl {
    std::string scheme;
    std::string host;
    int port = 0;
    std::string path;
    std::string error;
    bool valid = false;
};

struct HttpResponse {
    bool ok = false;
    int status_code = 0;
    std::string error;
    std::string body;
};

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

bool ends_with(std::string_view value, std::string_view suffix) {
    return value.size() >= suffix.size() &&
        value.substr(value.size() - suffix.size(), suffix.size()) == suffix;
}

std::string env_or_default(const char* key, std::string_view fallback) {
    const char* value = std::getenv(key);
    if (value == nullptr || *value == '\0') {
        return std::string(fallback);
    }
    return value;
}

std::string escape_json(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size() + 16);
    for (char c : value) {
        switch (c) {
            case '\\':
                escaped += "\\\\";
                break;
            case '"':
                escaped += "\\\"";
                break;
            case '\n':
                escaped += "\\n";
                break;
            case '\r':
                escaped += "\\r";
                break;
            case '\t':
                escaped += "\\t";
                break;
            default:
                escaped.push_back(c);
                break;
        }
    }
    return escaped;
}

std::string unescape_json(std::string_view value) {
    std::string unescaped;
    unescaped.reserve(value.size());
    bool escaping = false;
    for (char c : value) {
        if (!escaping) {
            if (c == '\\') {
                escaping = true;
                continue;
            }
            unescaped.push_back(c);
            continue;
        }
        escaping = false;
        switch (c) {
            case 'n':
                unescaped.push_back('\n');
                break;
            case 'r':
                unescaped.push_back('\r');
                break;
            case 't':
                unescaped.push_back('\t');
                break;
            default:
                unescaped.push_back(c);
                break;
        }
    }
    return unescaped;
}

std::string extract_json_string(std::string_view text, std::string_view key) {
    const std::string needle = "\"" + std::string(key) + "\"";
    const std::size_t key_start = text.find(needle);
    if (key_start == std::string::npos) {
        return {};
    }
    std::size_t cursor = key_start + needle.size();
    while (cursor < text.size() && std::isspace(static_cast<unsigned char>(text[cursor]))) {
        ++cursor;
    }
    if (cursor >= text.size() || text[cursor] != ':') {
        return {};
    }
    ++cursor;
    while (cursor < text.size() && std::isspace(static_cast<unsigned char>(text[cursor]))) {
        ++cursor;
    }
    if (cursor >= text.size() || text[cursor] != '"') {
        return {};
    }
    ++cursor;
    std::string value;
    bool escaping = false;
    while (cursor < text.size()) {
        const char c = text[cursor++];
        if (!escaping && c == '"') {
            break;
        }
        if (!escaping && c == '\\') {
            escaping = true;
            value.push_back(c);
            continue;
        }
        escaping = false;
        value.push_back(c);
    }
    return unescape_json(value);
}

std::vector<std::string> extract_json_string_array(std::string_view text, std::string_view key) {
    std::vector<std::string> values;
    const std::string needle = "\"" + std::string(key) + "\"";
    const std::size_t key_start = text.find(needle);
    if (key_start == std::string::npos) {
        return values;
    }
    std::size_t cursor = key_start + needle.size();
    while (cursor < text.size() && std::isspace(static_cast<unsigned char>(text[cursor]))) {
        ++cursor;
    }
    if (cursor >= text.size() || text[cursor] != ':') {
        return values;
    }
    ++cursor;
    while (cursor < text.size() && std::isspace(static_cast<unsigned char>(text[cursor]))) {
        ++cursor;
    }
    if (cursor >= text.size() || text[cursor] != '[') {
        return values;
    }
    ++cursor;
    while (cursor < text.size() && text[cursor] != ']') {
        while (cursor < text.size() && (text[cursor] == ' ' || text[cursor] == '\n' || text[cursor] == ',')) {
            ++cursor;
        }
        if (cursor >= text.size() || text[cursor] != '"') {
            break;
        }
        ++cursor;
        std::string value;
        bool escaping = false;
        while (cursor < text.size()) {
            const char c = text[cursor++];
            if (!escaping && c == '"') {
                break;
            }
            if (!escaping && c == '\\') {
                escaping = true;
                value.push_back(c);
                continue;
            }
            escaping = false;
            value.push_back(c);
        }
        values.push_back(unescape_json(value));
        while (cursor < text.size() && text[cursor] != ',' && text[cursor] != ']') {
            ++cursor;
        }
    }
    return values;
}

double extract_json_double(std::string_view text, std::string_view key, double default_value = 0.0) {
    const std::string needle = "\"" + std::string(key) + "\"";
    const std::size_t key_start = text.find(needle);
    if (key_start == std::string::npos) {
        return default_value;
    }
    std::size_t cursor = key_start + needle.size();
    while (cursor < text.size() && std::isspace(static_cast<unsigned char>(text[cursor]))) {
        ++cursor;
    }
    if (cursor >= text.size() || text[cursor] != ':') {
        return default_value;
    }
    ++cursor;
    while (cursor < text.size() && std::isspace(static_cast<unsigned char>(text[cursor]))) {
        ++cursor;
    }
    const std::size_t value_start = cursor;
    while (cursor < text.size() &&
           (std::isdigit(static_cast<unsigned char>(text[cursor])) || text[cursor] == '-' ||
            text[cursor] == '+' || text[cursor] == '.' || text[cursor] == 'e' || text[cursor] == 'E')) {
        ++cursor;
    }
    if (cursor == value_start) {
        return default_value;
    }
    try {
        return std::stod(std::string(text.substr(value_start, cursor - value_start)));
    } catch (const std::exception&) {
        return default_value;
    }
}

bool extract_json_bool(std::string_view text, std::string_view key, bool default_value = false) {
    const std::string needle = "\"" + std::string(key) + "\"";
    const std::size_t key_start = text.find(needle);
    if (key_start == std::string::npos) {
        return default_value;
    }
    std::size_t cursor = key_start + needle.size();
    while (cursor < text.size() && std::isspace(static_cast<unsigned char>(text[cursor]))) {
        ++cursor;
    }
    if (cursor >= text.size() || text[cursor] != ':') {
        return default_value;
    }
    ++cursor;
    while (cursor < text.size() && std::isspace(static_cast<unsigned char>(text[cursor]))) {
        ++cursor;
    }
    if (text.substr(cursor, 4) == "true") {
        return true;
    }
    if (text.substr(cursor, 5) == "false") {
        return false;
    }
    return default_value;
}

ParsedUrl parse_http_url(std::string_view url, int default_port = 11434) {
    ParsedUrl parsed;
    const std::string prefix = "http://";
    if (url.empty()) {
        parsed.error = "base URL is empty";
        return parsed;
    }
    if (url.substr(0, prefix.size()) != prefix) {
        parsed.error = "only http:// URLs are supported for the dormant Ollama probe";
        return parsed;
    }

    std::string remainder(url.substr(prefix.size()));
    std::string authority = remainder;
    parsed.path = "/";
    const std::size_t slash = remainder.find('/');
    if (slash != std::string::npos) {
        authority = remainder.substr(0, slash);
        parsed.path = remainder.substr(slash);
    }

    if (authority.empty()) {
        parsed.error = "missing host";
        return parsed;
    }

    parsed.scheme = "http";
    parsed.port = default_port;
    const std::size_t colon = authority.rfind(':');
    if (colon != std::string::npos && authority.find(']') == std::string::npos) {
        parsed.host = authority.substr(0, colon);
        const std::string port_text = authority.substr(colon + 1);
        if (parsed.host.empty() || port_text.empty()) {
            parsed.error = "invalid host:port pair";
            return parsed;
        }
        try {
            parsed.port = std::stoi(port_text);
        } catch (const std::exception&) {
            parsed.error = "invalid numeric port";
            return parsed;
        }
    } else {
        parsed.host = authority;
    }

    if (parsed.host.empty() || parsed.port <= 0 || parsed.port > 65535) {
        parsed.error = "invalid host or port";
        return parsed;
    }

    if (parsed.path.empty()) {
        parsed.path = "/";
    }
    parsed.valid = true;
    return parsed;
}

std::string normalize_path(std::string path) {
    if (path.empty()) {
        return "/";
    }
    while (path.size() > 1 && path.back() == '/') {
        path.pop_back();
    }
    if (path.front() != '/') {
        path.insert(path.begin(), '/');
    }
    return path;
}

std::string model_listing_path(std::string base_path) {
    base_path = normalize_path(std::move(base_path));
    if (base_path == "/") {
        return "/api/tags";
    }
    if (ends_with(base_path, "/v1")) {
        return base_path + "/models";
    }
    if (ends_with(base_path, "/api/tags")) {
        return base_path;
    }
    if (ends_with(base_path, "/api")) {
        return base_path + "/tags";
    }
    return base_path + "/api/tags";
}

std::string generate_path(std::string base_path) {
    base_path = normalize_path(std::move(base_path));
    if (base_path == "/" || ends_with(base_path, "/v1")) {
        return "/api/generate";
    }
    if (ends_with(base_path, "/api/generate")) {
        return base_path;
    }
    if (ends_with(base_path, "/api")) {
        return base_path + "/generate";
    }
    return "/api/generate";
}

int connect_with_timeout(const ParsedUrl& parsed, int timeout_ms, std::string* error) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* results = nullptr;
    const std::string port_text = std::to_string(parsed.port);
    const int rc = getaddrinfo(parsed.host.c_str(), port_text.c_str(), &hints, &results);
    if (rc != 0) {
        if (error != nullptr) {
            *error = gai_strerror(rc);
        }
        return -1;
    }

    int socket_fd = -1;
    std::string last_error = "connection failed";
    for (addrinfo* candidate = results; candidate != nullptr; candidate = candidate->ai_next) {
        socket_fd = ::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
        if (socket_fd < 0) {
            last_error = std::strerror(errno);
            continue;
        }

        const int flags = fcntl(socket_fd, F_GETFL, 0);
        if (flags < 0 || fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            last_error = std::strerror(errno);
            ::close(socket_fd);
            socket_fd = -1;
            continue;
        }

        const int connect_rc = ::connect(socket_fd, candidate->ai_addr, candidate->ai_addrlen);
        if (connect_rc == 0) {
            (void)fcntl(socket_fd, F_SETFL, flags);
            break;
        }
        if (errno != EINPROGRESS) {
            last_error = std::strerror(errno);
            ::close(socket_fd);
            socket_fd = -1;
            continue;
        }

        fd_set write_set;
        FD_ZERO(&write_set);
        FD_SET(socket_fd, &write_set);
        timeval timeout{};
        timeout.tv_sec = timeout_ms / 1000;
        timeout.tv_usec = (timeout_ms % 1000) * 1000;
        const int select_rc = ::select(socket_fd + 1, nullptr, &write_set, nullptr, &timeout);
        if (select_rc <= 0) {
            last_error = select_rc == 0 ? "connection timed out" : std::strerror(errno);
            ::close(socket_fd);
            socket_fd = -1;
            continue;
        }

        int socket_error = 0;
        socklen_t socket_error_size = sizeof(socket_error);
        if (::getsockopt(socket_fd, SOL_SOCKET, SO_ERROR, &socket_error, &socket_error_size) != 0 ||
            socket_error != 0) {
            last_error = socket_error == 0 ? std::strerror(errno) : std::strerror(socket_error);
            ::close(socket_fd);
            socket_fd = -1;
            continue;
        }

        (void)fcntl(socket_fd, F_SETFL, flags);
        break;
    }

    freeaddrinfo(results);
    if (socket_fd >= 0) {
        timeval timeout{};
        timeout.tv_sec = timeout_ms / 1000;
        timeout.tv_usec = (timeout_ms % 1000) * 1000;
        (void)setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        (void)setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        return socket_fd;
    }

    if (error != nullptr) {
        *error = last_error;
    }
    return -1;
}

HttpResponse http_get_json(const ParsedUrl& parsed, std::string_view request_path) {
    HttpResponse response;
    std::string connect_error;
    const int socket_fd = connect_with_timeout(parsed, 1200, &connect_error);
    if (socket_fd < 0) {
        response.error = connect_error;
        return response;
    }

    const std::string request =
        "GET " + std::string(request_path) + " HTTP/1.1\r\n"
        "Host: " + parsed.host + "\r\n"
        "Connection: close\r\n"
        "Accept: application/json\r\n\r\n";
    const ssize_t sent = ::send(socket_fd, request.data(), request.size(), 0);
    if (sent < 0) {
        response.error = std::strerror(errno);
        ::close(socket_fd);
        return response;
    }

    std::string raw;
    char buffer[4096];
    while (true) {
        const ssize_t received = ::recv(socket_fd, buffer, sizeof(buffer), 0);
        if (received == 0) {
            break;
        }
        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            response.error = std::strerror(errno);
            ::close(socket_fd);
            return response;
        }
        raw.append(buffer, static_cast<std::size_t>(received));
    }
    ::close(socket_fd);

    if (raw.empty()) {
        response.error = "no response body received";
        return response;
    }

    const std::size_t line_end = raw.find("\r\n");
    if (line_end == std::string::npos) {
        response.error = "invalid HTTP response";
        return response;
    }
    const std::string status_line = raw.substr(0, line_end);
    const std::size_t status_start = status_line.find(' ');
    if (status_start != std::string::npos) {
        const std::size_t status_end = status_line.find(' ', status_start + 1);
        const std::string status_text = status_line.substr(
            status_start + 1,
            status_end == std::string::npos ? std::string::npos : status_end - status_start - 1);
        try {
            response.status_code = std::stoi(status_text);
        } catch (const std::exception&) {
            response.status_code = 0;
        }
    }

    const std::size_t header_end = raw.find("\r\n\r\n");
    response.body = header_end == std::string::npos ? raw : raw.substr(header_end + 4);
    response.ok = response.status_code >= 200 && response.status_code < 300;
    if (!response.ok && response.error.empty()) {
        response.error = "HTTP " + std::to_string(response.status_code);
    }
    return response;
}

HttpResponse http_post_json(const ParsedUrl& parsed, std::string_view request_path, std::string_view json_body) {
    HttpResponse response;
    std::string connect_error;
    const int socket_fd = connect_with_timeout(parsed, 2500, &connect_error);
    if (socket_fd < 0) {
        response.error = connect_error;
        return response;
    }

    std::ostringstream request;
    request << "POST " << request_path << " HTTP/1.1\r\n"
            << "Host: " << parsed.host << "\r\n"
            << "Connection: close\r\n"
            << "Content-Type: application/json\r\n"
            << "Accept: application/json\r\n"
            << "Content-Length: " << json_body.size() << "\r\n\r\n"
            << json_body;
    const std::string payload = request.str();

    const ssize_t sent = ::send(socket_fd, payload.data(), payload.size(), 0);
    if (sent < 0) {
        response.error = std::strerror(errno);
        ::close(socket_fd);
        return response;
    }

    std::string raw;
    char buffer[4096];
    while (true) {
        const ssize_t received = ::recv(socket_fd, buffer, sizeof(buffer), 0);
        if (received == 0) {
            break;
        }
        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            response.error = std::strerror(errno);
            ::close(socket_fd);
            return response;
        }
        raw.append(buffer, static_cast<std::size_t>(received));
    }
    ::close(socket_fd);

    if (raw.empty()) {
        response.error = "no response body received";
        return response;
    }

    const std::size_t line_end = raw.find("\r\n");
    if (line_end == std::string::npos) {
        response.error = "invalid HTTP response";
        return response;
    }
    const std::string status_line = raw.substr(0, line_end);
    const std::size_t status_start = status_line.find(' ');
    if (status_start != std::string::npos) {
        const std::size_t status_end = status_line.find(' ', status_start + 1);
        const std::string status_text = status_line.substr(
            status_start + 1,
            status_end == std::string::npos ? std::string::npos : status_end - status_start - 1);
        try {
            response.status_code = std::stoi(status_text);
        } catch (const std::exception&) {
            response.status_code = 0;
        }
    }
    const std::size_t header_end = raw.find("\r\n\r\n");
    response.body = header_end == std::string::npos ? raw : raw.substr(header_end + 4);
    response.ok = response.status_code >= 200 && response.status_code < 300;
    if (!response.ok && response.error.empty()) {
        response.error = "HTTP " + std::to_string(response.status_code);
    }
    return response;
}

std::vector<std::string> extract_model_names(std::string_view body) {
    std::vector<std::string> names;
    const auto collect = [&names, body](std::string_view token) {
        std::size_t cursor = 0;
        while (true) {
            const std::size_t start = body.find(token, cursor);
            if (start == std::string::npos) {
                return;
            }
            const std::size_t value_start = start + token.size();
            const std::size_t value_end = body.find('"', value_start);
            if (value_end == std::string::npos) {
                return;
            }
            const std::string candidate(body.substr(value_start, value_end - value_start));
            if (!candidate.empty() &&
                std::find(names.begin(), names.end(), candidate) == names.end()) {
                names.push_back(candidate);
            }
            cursor = value_end + 1;
        }
    };

    collect("\"name\":\"");
    collect("\"id\":\"");
    return names;
}

std::string strip_code_fences(std::string text) {
    text = trim(text);
    if (text.rfind("```", 0) != 0) {
        return text;
    }
    const std::size_t first_newline = text.find('\n');
    if (first_newline == std::string::npos) {
        return text;
    }
    const std::size_t closing = text.rfind("```");
    if (closing == std::string::npos || closing <= first_newline) {
        return text;
    }
    return trim(text.substr(first_newline + 1, closing - first_newline - 1));
}

std::string render_assist_schema() {
    return
        "{\"type\":\"object\",\"properties\":{"
        "\"summary\":{\"type\":\"string\"},"
        "\"highlights\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},"
        "\"operator_takeaway\":{\"type\":\"string\"}"
        "},\"required\":[\"summary\",\"highlights\",\"operator_takeaway\"]}";
}

std::string build_assist_system_prompt() {
    return "You are an assistive summarizer for OmniX. Use only the deterministic text provided by the user. "
           "Do not invent new facts, tools, verdicts, or actions. Return strict JSON that matches the provided schema.";
}

std::string build_assist_prompt(const AssistRequest& request) {
    std::ostringstream prompt;
    prompt << "Task: " << request.task_id << "\n";
    if (!request.target_label.empty()) {
        prompt << "Target: " << request.target_label << "\n";
    }
    prompt << "Instructions:\n"
           << "- Summarize the deterministic OmniX output without adding new claims.\n"
           << "- Keep highlights concrete and brief.\n"
           << "- Keep the operator takeaway aligned with the existing next action or result.\n"
           << "- Output JSON only.\n\n"
           << "Deterministic OmniX text follows:\n"
           << request.deterministic_text << "\n";
    return prompt.str();
}

std::string render_tool_plan_schema() {
    return
        "{\"type\":\"object\",\"properties\":{"
        "\"tool_name\":{\"type\":\"string\"},"
        "\"arguments\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},"
        "\"rationale\":{\"type\":\"string\"},"
        "\"safety_notes\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}"
        "},\"required\":[\"tool_name\",\"arguments\",\"rationale\",\"safety_notes\"]}";
}

std::string build_tool_plan_system_prompt() {
    return "You are a guarded planner for OmniX. Choose at most one tool from the provided allowlist. "
           "Do not invent tools outside the allowlist. Keep all actions local, read-only, and safe. "
           "If no allowlisted tool is appropriate, return an empty tool_name and no arguments. "
           "Return strict JSON that matches the provided schema.";
}

std::string build_tool_plan_prompt(std::string_view prompt_text,
                                   const std::vector<std::string>& allowlisted_tools) {
    std::ostringstream prompt;
    prompt << "Allowlisted tools:\n";
    for (const std::string& tool : allowlisted_tools) {
        prompt << "- " << tool << "\n";
    }
    prompt << "\nInstructions:\n"
           << "- Prefer the most direct read-only local inspection tool.\n"
           << "- Do not propose shell operators, pipelines, redirection, or multiple tools.\n"
           << "- Only propose literal arguments needed for the chosen tool.\n"
           << "- Use an empty tool_name if the request is not a safe fit.\n\n"
           << "User request:\n"
           << prompt_text << "\n";
    return prompt.str();
}

std::optional<AssistAnnotation> parse_assist_annotation(std::string_view payload,
                                                        std::string_view provider_id,
                                                        std::string_view model,
                                                        std::string_view task_id) {
    const std::string normalized = strip_code_fences(trim(payload));
    if (normalized.empty() || normalized.front() != '{') {
        return std::nullopt;
    }

    AssistAnnotation annotation;
    annotation.task_id = std::string(task_id);
    annotation.provider_id = std::string(provider_id);
    annotation.model = std::string(model);
    annotation.status = "assist_used";
    annotation.summary = trim(extract_json_string(normalized, "summary"));
    annotation.highlights = extract_json_string_array(normalized, "highlights");
    annotation.operator_takeaway = trim(extract_json_string(normalized, "operator_takeaway"));

    if (annotation.summary.empty() || annotation.operator_takeaway.empty()) {
        return std::nullopt;
    }
    if (annotation.highlights.empty()) {
        annotation.highlights.push_back(annotation.summary);
    }
    if (annotation.highlights.size() > 5) {
        annotation.highlights.resize(5);
    }
    return annotation;
}

std::optional<ToolAssistPlan> parse_tool_assist_plan(std::string_view payload,
                                                     std::string_view provider_id,
                                                     std::string_view model) {
    const std::string normalized = strip_code_fences(trim(payload));
    if (normalized.empty() || normalized.front() != '{') {
        return std::nullopt;
    }

    ToolAssistPlan plan;
    plan.task_id = "tool_action";
    plan.provider_id = std::string(provider_id);
    plan.model = std::string(model);
    plan.status = "planned";
    plan.tool_name = trim(extract_json_string(normalized, "tool_name"));
    plan.arguments = extract_json_string_array(normalized, "arguments");
    plan.rationale = trim(extract_json_string(normalized, "rationale"));
    plan.safety_notes = extract_json_string_array(normalized, "safety_notes");
    if (plan.tool_name.empty() || plan.rationale.empty()) {
        return std::nullopt;
    }
    return plan;
}

std::string render_build_plan_schema() {
    return
        "{\"type\":\"object\",\"properties\":{"
        "\"selected_recipe_id\":{\"type\":\"string\"},"
        "\"fallback_recipe_id\":{\"type\":\"string\"},"
        "\"rationale\":{\"type\":\"string\"},"
        "\"confidence\":{\"type\":\"number\"},"
        "\"safety_notes\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}"
        "},\"required\":[\"selected_recipe_id\",\"fallback_recipe_id\",\"rationale\",\"confidence\",\"safety_notes\"]}";
}

std::string build_build_plan_system_prompt() {
    return "You are a guarded build selector for OmniX. Choose only from the provided recipe ids. "
           "Do not invent shell commands, package-manager steps, or recipes outside the provided set. "
           "Return strict JSON that matches the provided schema.";
}

std::string build_build_plan_prompt(const BuildAssistRequest& request) {
    std::ostringstream prompt;
    prompt << "Task: " << request.task_id << "\n";
    prompt << "Project: " << request.canonical_project_name << "\n";
    if (!request.prompt.empty()) {
        prompt << "User intent: " << request.prompt << "\n";
    }
    prompt << "Build system: " << request.build_system << "\n";
    prompt << "Environment signature: " << request.environment_signature << "\n";
    prompt << "Native status: " << request.native_status << "\n";
    prompt << "Will acquire source: " << (request.will_acquire ? "yes" : "no") << "\n";
    prompt << "Will install: " << (request.will_install ? "yes" : "no") << "\n";
    prompt << "Available recipe ids:\n";
    for (const std::string& recipe : request.available_recipe_ids) {
        prompt << "- " << recipe << "\n";
    }
    if (!request.learned_recipe_summaries.empty()) {
        prompt << "Learned recipe history:\n";
        for (const std::string& summary : request.learned_recipe_summaries) {
            prompt << "- " << summary << "\n";
        }
    }
    prompt << "\nInstructions:\n"
           << "- Select exactly one recipe id from the provided recipe ids.\n"
           << "- Use fallback_recipe_id only if another provided recipe is a sensible deterministic fallback.\n"
           << "- Do not invent new recipe ids.\n"
           << "- Confidence must be between 0.0 and 1.0.\n"
           << "- Keep safety_notes focused on why this remains guarded and deterministic.\n";
    return prompt.str();
}

std::optional<BuildAssistPlan> parse_build_assist_plan(std::string_view payload,
                                                       std::string_view provider_id,
                                                       std::string_view model) {
    const std::string normalized = strip_code_fences(trim(payload));
    if (normalized.empty() || normalized.front() != '{') {
        return std::nullopt;
    }

    BuildAssistPlan plan;
    plan.task_id = "build_recipe";
    plan.provider_id = std::string(provider_id);
    plan.model = std::string(model);
    plan.status = "planned";
    plan.selected_recipe_id = trim(extract_json_string(normalized, "selected_recipe_id"));
    plan.fallback_recipe_id = trim(extract_json_string(normalized, "fallback_recipe_id"));
    plan.rationale = trim(extract_json_string(normalized, "rationale"));
    plan.confidence = extract_json_double(normalized, "confidence", -1.0);
    plan.safety_notes = extract_json_string_array(normalized, "safety_notes");
    if (plan.selected_recipe_id.empty() || plan.rationale.empty()) {
        return std::nullopt;
    }
    return plan;
}

std::string render_command_plan_schema() {
    return
        "{\"type\":\"object\",\"properties\":{"
        "\"canonical_command\":{\"type\":\"string\"},"
        "\"command_family\":{\"type\":\"string\"},"
        "\"rationale\":{\"type\":\"string\"},"
        "\"confidence\":{\"type\":\"number\"},"
        "\"requires_confirmation\":{\"type\":\"boolean\"},"
        "\"safety_notes\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}"
        "},\"required\":[\"canonical_command\",\"command_family\",\"rationale\",\"confidence\",\"requires_confirmation\",\"safety_notes\"]}";
}

std::string build_command_plan_system_prompt() {
    return "You are a guarded command router for OmniX. Map the user's request into exactly one canonical OmniX command "
           "chosen from the provided allowlisted command patterns. Do not invent shell commands, pipelines, package-manager "
           "commands, or arguments outside the OmniX command surface. Return strict JSON that matches the provided schema.";
}

std::string build_command_plan_prompt(std::string_view prompt_text,
                                      const std::vector<std::string>& allowlisted_commands) {
    std::ostringstream prompt;
    prompt << "Allowlisted canonical command patterns:\n";
    for (const std::string& command : allowlisted_commands) {
        prompt << "- " << command << "\n";
    }
    prompt << "\nInstructions:\n"
           << "- Return one canonical OmniX command only.\n"
           << "- Prefer read-only or diagnostic commands when they fit the request.\n"
           << "- Keep arguments literal and local.\n"
           << "- Do not add shell quoting, pipes, redirection, or chained commands.\n"
           << "- Confidence must be between 0.0 and 1.0.\n\n"
           << "User request:\n"
           << prompt_text << "\n";
    return prompt.str();
}

std::optional<CommandAssistPlan> parse_command_assist_plan(std::string_view payload,
                                                           std::string_view provider_id,
                                                           std::string_view model) {
    const std::string normalized = strip_code_fences(trim(payload));
    if (normalized.empty() || normalized.front() != '{') {
        return std::nullopt;
    }

    CommandAssistPlan plan;
    plan.task_id = "command_route";
    plan.provider_id = std::string(provider_id);
    plan.model = std::string(model);
    plan.status = "planned";
    plan.canonical_command = trim(extract_json_string(normalized, "canonical_command"));
    plan.command_family = trim(extract_json_string(normalized, "command_family"));
    plan.rationale = trim(extract_json_string(normalized, "rationale"));
    plan.confidence = extract_json_double(normalized, "confidence", -1.0);
    plan.requires_confirmation = extract_json_bool(normalized, "requires_confirmation", false);
    plan.safety_notes = extract_json_string_array(normalized, "safety_notes");
    if (plan.canonical_command.empty() || plan.command_family.empty() || plan.rationale.empty()) {
        return std::nullopt;
    }
    return plan;
}

ProviderProbeReport make_null_probe() {
    ProviderProbeReport report;
    report.provider_id = "null";
    report.status = "inactive";
    report.summary = "Provider `null` is inactive; OmniX remains deterministic-only.";
    report.configured = false;
    report.available = false;
    report.checks = {
        "selected provider: null",
        "no assistive model provider is configured",
    };
    return report;
}

ProviderProbeReport make_unsupported_probe(std::string_view selected_id) {
    ProviderProbeReport report;
    report.provider_id = std::string(selected_id);
    report.status = "unsupported_provider";
    report.summary = "Provider `" + std::string(selected_id) +
        "` is not supported yet. OmniX currently supports only `null` and dormant `ollama` probe configuration.";
    report.configured = true;
    report.available = false;
    report.checks = {
        "selected provider: " + std::string(selected_id),
    };
    report.warnings = {
        "Use `OMNIX_REASONING_PROVIDER=ollama` or unset the provider selection.",
    };
    return report;
}

ProviderProbeReport make_ollama_probe(std::string base_url, std::string model) {
    ProviderProbeReport report;
    report.provider_id = "ollama";
    report.configured = true;
    report.available = false;
    report.base_url = std::move(base_url);
    report.model = std::move(model);
    report.checks.push_back("selected provider: ollama");

    if (report.base_url.empty()) {
        report.base_url = "http://127.0.0.1:11434";
    }
    report.checks.push_back("base URL: " + report.base_url);

    if (report.model.empty()) {
        report.status = "config_incomplete";
        report.summary = "Ollama is selected, but `OMNIX_OLLAMA_MODEL` is not set.";
        report.warnings.push_back("Set `OMNIX_OLLAMA_MODEL` to an installed local model, then rerun `omnix provider probe`.");
        return report;
    }

    report.checks.push_back("requested model: " + report.model);
    const ParsedUrl parsed = parse_http_url(report.base_url);
    if (!parsed.valid) {
        report.status = "invalid_config";
        report.summary = "Ollama base URL `" + report.base_url + "` is invalid: " + parsed.error + ".";
        report.warnings.push_back("Use an HTTP URL such as `http://127.0.0.1:11434` or `http://127.0.0.1:11434/v1`.");
        return report;
    }

    const std::string request_path = model_listing_path(parsed.path);
    report.checks.push_back("model listing path: " + request_path);
    const HttpResponse response = http_get_json(parsed, request_path);
    if (response.status_code > 0) {
        report.checks.push_back("endpoint responded with HTTP " + std::to_string(response.status_code));
    }
    if (!response.ok) {
        report.status = response.status_code > 0 ? "endpoint_error" : "endpoint_unreachable";
        report.summary = response.status_code > 0
            ? "Reached Ollama at `" + report.base_url + "`, but the model listing request failed with HTTP " +
                std::to_string(response.status_code) + "."
            : "Could not reach Ollama at `" + report.base_url + "`.";
        if (!response.error.empty()) {
            report.warnings.push_back(response.error);
        }
        report.warnings.push_back("Start Ollama locally or correct `OMNIX_OLLAMA_BASE_URL`, then rerun `omnix provider probe`.");
        return report;
    }

    report.checks.push_back("endpoint reachable");
    const std::vector<std::string> model_names = extract_model_names(response.body);
    if (!model_names.empty()) {
        std::ostringstream listed;
        listed << "detected models: ";
        for (std::size_t index = 0; index < model_names.size(); ++index) {
            if (index != 0) {
                listed << ", ";
            }
            listed << model_names[index];
            if (index >= 4 && model_names.size() > 5) {
                listed << ", ...";
                break;
            }
        }
        report.checks.push_back(listed.str());
    }

    const bool model_found = std::find(model_names.begin(), model_names.end(), report.model) != model_names.end() ||
        response.body.find(report.model) != std::string::npos;
    if (!model_found) {
        report.status = "model_missing";
        report.summary = "Ollama is reachable, but model `" + report.model + "` is not currently available.";
        report.warnings.push_back("Pull or load the requested model in Ollama, then rerun `omnix provider probe`.");
        return report;
    }

    report.status = "ready";
    report.available = true;
    report.summary = "Ollama is configured and model `" + report.model +
        "` is available. OmniX will still remain deterministic-only until assist is explicitly enabled.";
    return report;
}

std::optional<AssistAnnotation> request_ollama_annotation(std::string_view base_url,
                                                          std::string_view model,
                                                          const AssistRequest& request,
                                                          std::vector<std::string>* warnings) {
    const ParsedUrl parsed = parse_http_url(base_url);
    if (!parsed.valid) {
        if (warnings != nullptr) {
            warnings->push_back("Invalid Ollama base URL for assist: " + parsed.error);
        }
        return std::nullopt;
    }

    std::ostringstream body;
    body << "{\"model\":\"" << escape_json(model)
         << "\",\"system\":\"" << escape_json(build_assist_system_prompt())
         << "\",\"prompt\":\"" << escape_json(build_assist_prompt(request))
         << "\",\"format\":" << render_assist_schema()
         << ",\"stream\":false}";
    const HttpResponse response = http_post_json(parsed, generate_path(parsed.path), body.str());
    if (!response.ok) {
        if (warnings != nullptr) {
            warnings->push_back(response.error.empty() ? "Ollama assist request failed." : response.error);
        }
        return std::nullopt;
    }

    const std::string generated = extract_json_string(response.body, "response");
    if (generated.empty()) {
        if (warnings != nullptr) {
            warnings->push_back("Ollama assist response did not include a `response` field.");
        }
        return std::nullopt;
    }

    return parse_assist_annotation(generated, "ollama", model, request.task_id);
}

std::optional<ToolAssistPlan> request_ollama_tool_plan(std::string_view base_url,
                                                       std::string_view model,
                                                       std::string_view prompt_text,
                                                       const std::vector<std::string>& allowlisted_tools,
                                                       std::vector<std::string>* warnings) {
    if (const char* fixture_path = std::getenv("OMNIX_OLLAMA_TOOL_PLAN_FILE");
        fixture_path != nullptr && *fixture_path != '\0') {
        std::ifstream input(fixture_path);
        if (!input) {
            if (warnings != nullptr) {
                warnings->push_back("Unable to read OMNIX_OLLAMA_TOOL_PLAN_FILE fixture.");
            }
            return std::nullopt;
        }
        std::ostringstream buffer;
        buffer << input.rdbuf();
        return parse_tool_assist_plan(buffer.str(), "ollama", model);
    }

    const ParsedUrl parsed = parse_http_url(base_url);
    if (!parsed.valid) {
        if (warnings != nullptr) {
            warnings->push_back("Invalid Ollama base URL for tool planning: " + parsed.error);
        }
        return std::nullopt;
    }

    std::ostringstream body;
    body << "{\"model\":\"" << escape_json(model)
         << "\",\"system\":\"" << escape_json(build_tool_plan_system_prompt())
         << "\",\"prompt\":\"" << escape_json(build_tool_plan_prompt(prompt_text, allowlisted_tools))
         << "\",\"format\":" << render_tool_plan_schema()
         << ",\"stream\":false}";
    const HttpResponse response = http_post_json(parsed, generate_path(parsed.path), body.str());
    if (!response.ok) {
        if (warnings != nullptr) {
            warnings->push_back(response.error.empty() ? "Ollama tool-plan request failed." : response.error);
        }
        return std::nullopt;
    }

    const std::string generated = extract_json_string(response.body, "response");
    if (generated.empty()) {
        if (warnings != nullptr) {
            warnings->push_back("Ollama tool-plan response did not include a `response` field.");
        }
        return std::nullopt;
    }

    return parse_tool_assist_plan(generated, "ollama", model);
}

std::optional<BuildAssistPlan> request_ollama_build_plan(std::string_view base_url,
                                                         std::string_view model,
                                                         const BuildAssistRequest& request,
                                                         std::vector<std::string>* warnings) {
    if (const char* fixture_path = std::getenv("OMNIX_OLLAMA_BUILD_PLAN_FILE");
        fixture_path != nullptr && *fixture_path != '\0') {
        std::ifstream input(fixture_path);
        if (!input) {
            if (warnings != nullptr) {
                warnings->push_back("Unable to read OMNIX_OLLAMA_BUILD_PLAN_FILE fixture.");
            }
            return std::nullopt;
        }
        std::ostringstream buffer;
        buffer << input.rdbuf();
        return parse_build_assist_plan(buffer.str(), "ollama", model);
    }

    const ParsedUrl parsed = parse_http_url(base_url);
    if (!parsed.valid) {
        if (warnings != nullptr) {
            warnings->push_back("Invalid Ollama base URL for build planning: " + parsed.error);
        }
        return std::nullopt;
    }

    std::ostringstream body;
    body << "{\"model\":\"" << escape_json(model)
         << "\",\"system\":\"" << escape_json(build_build_plan_system_prompt())
         << "\",\"prompt\":\"" << escape_json(build_build_plan_prompt(request))
         << "\",\"format\":" << render_build_plan_schema()
         << ",\"stream\":false}";
    const HttpResponse response = http_post_json(parsed, generate_path(parsed.path), body.str());
    if (!response.ok) {
        if (warnings != nullptr) {
            warnings->push_back(response.error.empty() ? "Ollama build-plan request failed." : response.error);
        }
        return std::nullopt;
    }

    const std::string generated = extract_json_string(response.body, "response");
    if (generated.empty()) {
        if (warnings != nullptr) {
            warnings->push_back("Ollama build-plan response did not include a `response` field.");
        }
        return std::nullopt;
    }

    return parse_build_assist_plan(generated, "ollama", model);
}

std::optional<CommandAssistPlan> request_ollama_command_plan(std::string_view base_url,
                                                             std::string_view model,
                                                             std::string_view prompt_text,
                                                             const std::vector<std::string>& allowlisted_commands,
                                                             std::vector<std::string>* warnings) {
    if (const char* fixture_path = std::getenv("OMNIX_OLLAMA_COMMAND_PLAN_FILE");
        fixture_path != nullptr && *fixture_path != '\0') {
        std::ifstream input(fixture_path);
        if (!input) {
            if (warnings != nullptr) {
                warnings->push_back("Unable to read OMNIX_OLLAMA_COMMAND_PLAN_FILE fixture.");
            }
            return std::nullopt;
        }
        std::ostringstream buffer;
        buffer << input.rdbuf();
        return parse_command_assist_plan(buffer.str(), "ollama", model);
    }

    const ParsedUrl parsed = parse_http_url(base_url);
    if (!parsed.valid) {
        if (warnings != nullptr) {
            warnings->push_back("Invalid Ollama base URL for command routing: " + parsed.error);
        }
        return std::nullopt;
    }

    std::ostringstream body;
    body << "{\"model\":\"" << escape_json(model)
         << "\",\"system\":\"" << escape_json(build_command_plan_system_prompt())
         << "\",\"prompt\":\"" << escape_json(build_command_plan_prompt(prompt_text, allowlisted_commands))
         << "\",\"format\":" << render_command_plan_schema()
         << ",\"stream\":false}";
    const HttpResponse response = http_post_json(parsed, generate_path(parsed.path), body.str());
    if (!response.ok) {
        if (warnings != nullptr) {
            warnings->push_back(response.error.empty() ? "Ollama command-plan request failed." : response.error);
        }
        return std::nullopt;
    }

    const std::string generated = extract_json_string(response.body, "response");
    if (generated.empty()) {
        if (warnings != nullptr) {
            warnings->push_back("Ollama command-plan response did not include a `response` field.");
        }
        return std::nullopt;
    }

    return parse_command_assist_plan(generated, "ollama", model);
}

}  // namespace

std::string_view NullProvider::id() const {
    return "null";
}

bool NullProvider::configured() const {
    return false;
}

bool NullProvider::available() const {
    return false;
}

ProviderProbeReport NullProvider::probe() const {
    return make_null_probe();
}

std::optional<AssistAnnotation> NullProvider::assist_annotation(const AssistRequest&) const {
    return std::nullopt;
}

std::optional<ToolAssistPlan> NullProvider::propose_tool_action(std::string_view,
                                                                const std::vector<std::string>&) const {
    return std::nullopt;
}

std::optional<BuildAssistPlan> NullProvider::propose_build_recipe(const BuildAssistRequest&) const {
    return std::nullopt;
}

std::optional<CommandAssistPlan> NullProvider::propose_command_route(std::string_view,
                                                                     const std::vector<std::string>&) const {
    return std::nullopt;
}

std::optional<std::string> NullProvider::resolve_freeform(std::string_view) const {
    return std::nullopt;
}

OllamaProvider::OllamaProvider(std::string base_url, std::string model)
    : base_url_(std::move(base_url)), model_(std::move(model)) {}

std::string_view OllamaProvider::id() const {
    return "ollama";
}

bool OllamaProvider::configured() const {
    return true;
}

bool OllamaProvider::available() const {
    return false;
}

ProviderProbeReport OllamaProvider::probe() const {
    return make_ollama_probe(base_url_, model_);
}

std::optional<AssistAnnotation> OllamaProvider::assist_annotation(const AssistRequest& request) const {
    const ProviderProbeReport probe_report = probe();
    if (probe_report.status != "ready") {
        return std::nullopt;
    }
    std::vector<std::string> warnings;
    std::optional<AssistAnnotation> annotation =
        request_ollama_annotation(base_url_, model_, request, &warnings);
    if (!annotation.has_value()) {
        return std::nullopt;
    }
    annotation->warnings = warnings;
    return annotation;
}

std::optional<ToolAssistPlan> OllamaProvider::propose_tool_action(std::string_view prompt,
                                                                  const std::vector<std::string>& allowlisted_tools) const {
    const char* fixture_path = std::getenv("OMNIX_OLLAMA_TOOL_PLAN_FILE");
    if (fixture_path == nullptr || *fixture_path == '\0') {
        const ProviderProbeReport probe_report = probe();
        if (probe_report.status != "ready") {
            return std::nullopt;
        }
    }
    std::vector<std::string> warnings;
    std::optional<ToolAssistPlan> plan =
        request_ollama_tool_plan(base_url_, model_, prompt, allowlisted_tools, &warnings);
    if (!plan.has_value()) {
        return std::nullopt;
    }
    if (!warnings.empty()) {
        plan->safety_notes.insert(plan->safety_notes.end(), warnings.begin(), warnings.end());
    }
    return plan;
}

std::optional<BuildAssistPlan> OllamaProvider::propose_build_recipe(const BuildAssistRequest& request) const {
    const char* fixture_path = std::getenv("OMNIX_OLLAMA_BUILD_PLAN_FILE");
    if (fixture_path == nullptr || *fixture_path == '\0') {
        const ProviderProbeReport probe_report = probe();
        if (probe_report.status != "ready") {
            return std::nullopt;
        }
    }
    std::vector<std::string> warnings;
    std::optional<BuildAssistPlan> plan =
        request_ollama_build_plan(base_url_, model_, request, &warnings);
    if (!plan.has_value()) {
        return std::nullopt;
    }
    if (!warnings.empty()) {
        plan->safety_notes.insert(plan->safety_notes.end(), warnings.begin(), warnings.end());
    }
    return plan;
}

std::optional<CommandAssistPlan> OllamaProvider::propose_command_route(
    std::string_view prompt,
    const std::vector<std::string>& allowlisted_commands) const {
    const char* fixture_path = std::getenv("OMNIX_OLLAMA_COMMAND_PLAN_FILE");
    if (fixture_path == nullptr || *fixture_path == '\0') {
        const ProviderProbeReport probe_report = probe();
        if (probe_report.status != "ready") {
            return std::nullopt;
        }
    }
    std::vector<std::string> warnings;
    std::optional<CommandAssistPlan> plan =
        request_ollama_command_plan(base_url_, model_, prompt, allowlisted_commands, &warnings);
    if (!plan.has_value()) {
        return std::nullopt;
    }
    if (!warnings.empty()) {
        plan->safety_notes.insert(plan->safety_notes.end(), warnings.begin(), warnings.end());
    }
    return plan;
}

std::optional<std::string> OllamaProvider::resolve_freeform(std::string_view) const {
    return std::nullopt;
}

UnsupportedProvider::UnsupportedProvider(std::string selected_id)
    : selected_id_(std::move(selected_id)) {}

std::string_view UnsupportedProvider::id() const {
    return selected_id_;
}

bool UnsupportedProvider::configured() const {
    return true;
}

bool UnsupportedProvider::available() const {
    return false;
}

ProviderProbeReport UnsupportedProvider::probe() const {
    return make_unsupported_probe(selected_id_);
}

std::optional<AssistAnnotation> UnsupportedProvider::assist_annotation(const AssistRequest&) const {
    return std::nullopt;
}

std::optional<ToolAssistPlan> UnsupportedProvider::propose_tool_action(std::string_view,
                                                                       const std::vector<std::string>&) const {
    return std::nullopt;
}

std::optional<BuildAssistPlan> UnsupportedProvider::propose_build_recipe(const BuildAssistRequest&) const {
    return std::nullopt;
}

std::optional<CommandAssistPlan> UnsupportedProvider::propose_command_route(
    std::string_view,
    const std::vector<std::string>&) const {
    return std::nullopt;
}

std::optional<std::string> UnsupportedProvider::resolve_freeform(std::string_view) const {
    return std::nullopt;
}

std::unique_ptr<ReasoningProvider> make_reasoning_provider_from_env() {
    const char* selected = std::getenv("OMNIX_REASONING_PROVIDER");
    if (selected == nullptr || *selected == '\0') {
        return std::make_unique<NullProvider>();
    }

    const std::string provider_id = lowercase(selected);
    if (provider_id == "null") {
        return std::make_unique<NullProvider>();
    }
    if (provider_id == "ollama") {
        return std::make_unique<OllamaProvider>(
            env_or_default("OMNIX_OLLAMA_BASE_URL", "http://127.0.0.1:11434"),
            env_or_default("OMNIX_OLLAMA_MODEL", ""));
    }
    return std::make_unique<UnsupportedProvider>(provider_id);
}

}  // namespace tze
