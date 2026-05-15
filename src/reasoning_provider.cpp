#include "tze/reasoning_provider.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/wait.h>
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

std::string env_or_default_any(std::initializer_list<const char*> keys, std::string_view fallback = {}) {
    for (const char* key : keys) {
        const char* value = std::getenv(key);
        if (value != nullptr && *value != '\0') {
            return value;
        }
    }
    return std::string(fallback);
}

std::string shell_quote(std::string_view value) {
    std::string escaped = "'";
    for (char c : value) {
        if (c == '\'') {
            escaped += "'\\''";
        } else {
            escaped.push_back(c);
        }
    }
    escaped.push_back('\'');
    return escaped;
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

int hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return 10 + (c - 'a');
    }
    if (c >= 'A' && c <= 'F') {
        return 10 + (c - 'A');
    }
    return -1;
}

bool parse_json_unicode_escape(std::string_view value, std::size_t offset, unsigned int* codepoint) {
    if (offset + 4 > value.size()) {
        return false;
    }
    unsigned int parsed = 0;
    for (std::size_t index = 0; index < 4; ++index) {
        const int digit = hex_value(value[offset + index]);
        if (digit < 0) {
            return false;
        }
        parsed = (parsed << 4U) | static_cast<unsigned int>(digit);
    }
    if (codepoint != nullptr) {
        *codepoint = parsed;
    }
    return true;
}

void append_utf8_codepoint(std::string* output, unsigned int codepoint) {
    if (output == nullptr) {
        return;
    }
    if (codepoint <= 0x7F) {
        output->push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7FF) {
        output->push_back(static_cast<char>(0xC0 | ((codepoint >> 6U) & 0x1F)));
        output->push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else if (codepoint <= 0xFFFF) {
        output->push_back(static_cast<char>(0xE0 | ((codepoint >> 12U) & 0x0F)));
        output->push_back(static_cast<char>(0x80 | ((codepoint >> 6U) & 0x3F)));
        output->push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else if (codepoint <= 0x10FFFF) {
        output->push_back(static_cast<char>(0xF0 | ((codepoint >> 18U) & 0x07)));
        output->push_back(static_cast<char>(0x80 | ((codepoint >> 12U) & 0x3F)));
        output->push_back(static_cast<char>(0x80 | ((codepoint >> 6U) & 0x3F)));
        output->push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
}

std::string unescape_json(std::string_view value) {
    std::string unescaped;
    unescaped.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        const char c = value[index];
        if (c != '\\' || index + 1 >= value.size()) {
            unescaped.push_back(c);
            continue;
        }

        const char escaped = value[++index];
        switch (escaped) {
            case '"':
                unescaped.push_back('"');
                break;
            case '\\':
                unescaped.push_back('\\');
                break;
            case '/':
                unescaped.push_back('/');
                break;
            case 'b':
                unescaped.push_back('\b');
                break;
            case 'f':
                unescaped.push_back('\f');
                break;
            case 'n':
                unescaped.push_back('\n');
                break;
            case 'r':
                unescaped.push_back('\r');
                break;
            case 't':
                unescaped.push_back('\t');
                break;
            case 'u': {
                unsigned int codepoint = 0;
                if (!parse_json_unicode_escape(value, index + 1, &codepoint)) {
                    unescaped.push_back('u');
                    break;
                }
                index += 4;
                if (codepoint >= 0xD800 && codepoint <= 0xDBFF &&
                    index + 2 < value.size() && value[index + 1] == '\\' && value[index + 2] == 'u') {
                    unsigned int low = 0;
                    if (parse_json_unicode_escape(value, index + 3, &low) && low >= 0xDC00 && low <= 0xDFFF) {
                        codepoint = 0x10000 + (((codepoint - 0xD800) << 10U) | (low - 0xDC00));
                        index += 6;
                    }
                }
                append_utf8_codepoint(&unescaped, codepoint);
                break;
            }
            default:
                unescaped.push_back(escaped);
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

std::string extract_json_object(std::string_view text, std::string_view key) {
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
    if (cursor >= text.size() || text[cursor] != '{') {
        return {};
    }

    const std::size_t start = cursor;
    int depth = 0;
    bool in_string = false;
    bool escaping = false;
    while (cursor < text.size()) {
        const char c = text[cursor];
        if (in_string) {
            if (escaping) {
                escaping = false;
            } else if (c == '\\') {
                escaping = true;
            } else if (c == '"') {
                in_string = false;
            }
            ++cursor;
            continue;
        }
        if (c == '"') {
            in_string = true;
        } else if (c == '{') {
            ++depth;
        } else if (c == '}') {
            --depth;
            if (depth == 0) {
                return std::string(text.substr(start, cursor - start + 1));
            }
        }
        ++cursor;
    }
    return {};
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

std::string normalize_base_url(std::string url, std::string_view default_value) {
    url = trim(url);
    if (url.empty()) {
        return std::string(default_value);
    }
    while (!url.empty() && url.back() == '/') {
        url.pop_back();
    }
    return url;
}

bool is_http_or_https_url(std::string_view url) {
    return url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0;
}

std::string openai_models_url(std::string base_url) {
    base_url = normalize_base_url(std::move(base_url), "https://api.openai.com/v1");
    if (ends_with(base_url, "/models")) {
        return base_url;
    }
    if (ends_with(base_url, "/v1")) {
        return base_url + "/models";
    }
    return base_url + "/v1/models";
}

std::string openai_responses_url(std::string base_url) {
    base_url = normalize_base_url(std::move(base_url), "https://api.openai.com/v1");
    if (ends_with(base_url, "/responses")) {
        return base_url;
    }
    if (ends_with(base_url, "/v1")) {
        return base_url + "/responses";
    }
    return base_url + "/v1/responses";
}

HttpResponse curl_json_request(std::string_view method,
                               std::string_view url,
                               std::string_view bearer_token,
                               std::string_view json_body = {},
                               std::string_view organization = {},
                               std::string_view project = {}) {
    HttpResponse response;
    if (url.empty()) {
        response.error = "request URL is empty";
        return response;
    }
    if (bearer_token.empty()) {
        response.error = "missing bearer token";
        return response;
    }

    std::ostringstream command;
    command << "curl -sS -L --max-time 25"
            << " -X " << shell_quote(method)
            << " -H " << shell_quote("Authorization: Bearer " + std::string(bearer_token))
            << " -H " << shell_quote("Accept: application/json");
    if (!organization.empty()) {
        command << " -H " << shell_quote("OpenAI-Organization: " + std::string(organization));
    }
    if (!project.empty()) {
        command << " -H " << shell_quote("OpenAI-Project: " + std::string(project));
    }
    if (!json_body.empty()) {
        command << " -H " << shell_quote("Content-Type: application/json")
                << " --data " << shell_quote(json_body);
    }
    command << " " << shell_quote(url)
            << " -w '\\n__OMNIX_STATUS__:%{http_code}' 2>&1";

    FILE* pipe = popen(command.str().c_str(), "r");
    if (pipe == nullptr) {
        response.error = "unable to launch curl";
        return response;
    }

    std::string raw;
    char buffer[4096];
    while (fgets(buffer, static_cast<int>(sizeof(buffer)), pipe) != nullptr) {
        raw += buffer;
    }
    const int close_status = pclose(pipe);

    const std::string marker = "\n__OMNIX_STATUS__:";
    const std::size_t marker_pos = raw.rfind(marker);
    if (marker_pos != std::string::npos) {
        const std::string status_text = trim(raw.substr(marker_pos + marker.size()));
        raw = raw.substr(0, marker_pos);
        try {
            response.status_code = std::stoi(status_text);
        } catch (const std::exception&) {
            response.status_code = 0;
        }
    }

    response.body = trim(raw);
    response.ok = response.status_code >= 200 && response.status_code < 300;
    if (!response.ok) {
        const std::string message = extract_json_string(response.body, "message");
        response.error = !message.empty()
            ? message
            : (response.status_code > 0 ? "HTTP " + std::to_string(response.status_code) : trim(raw));
    }

    if (close_status != 0 && response.status_code == 0 && response.error.empty()) {
        response.error = trim(raw);
    }
    if (response.body.empty() && response.error.empty()) {
        response.error = "no response body received";
    }
    return response;
}

std::string extract_openai_output_text(std::string_view body) {
    std::size_t cursor = 0;
    while (true) {
        const std::size_t type_pos = body.find("\"output_text\"", cursor);
        if (type_pos == std::string::npos) {
            return extract_json_string(body, "output_text");
        }
        if (const std::string text = extract_json_string(body.substr(type_pos), "text"); !text.empty()) {
            return text;
        }
        cursor = type_pos + std::string_view{"\"output_text\""}.size();
    }
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

std::string model_show_path(std::string base_path) {
    base_path = normalize_path(std::move(base_path));
    if (base_path == "/" || ends_with(base_path, "/v1")) {
        return "/api/show";
    }
    if (ends_with(base_path, "/api/show")) {
        return base_path;
    }
    if (ends_with(base_path, "/api")) {
        return base_path + "/show";
    }
    return "/api/show";
}

std::string read_text_file_if_exists(const char* path) {
    if (path == nullptr || *path == '\0') {
        return {};
    }
    std::ifstream input(path);
    if (!input) {
        return {};
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return trim(buffer.str());
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
        "},\"additionalProperties\":false,\"required\":[\"summary\",\"highlights\",\"operator_takeaway\"]}";
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
        "},\"additionalProperties\":false,\"required\":[\"tool_name\",\"arguments\",\"rationale\",\"safety_notes\"]}";
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
    plan.metadata.provider = std::string(provider_id);
    plan.metadata.model = std::string(model);
    plan.metadata.status = plan.status;
    plan.metadata.rationale = plan.rationale;
    plan.metadata.warnings = plan.safety_notes;
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
        "},\"additionalProperties\":false,\"required\":[\"selected_recipe_id\",\"fallback_recipe_id\",\"rationale\",\"confidence\",\"safety_notes\"]}";
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
    plan.metadata.provider = std::string(provider_id);
    plan.metadata.model = std::string(model);
    plan.metadata.status = plan.status;
    plan.metadata.confidence = plan.confidence;
    plan.metadata.rationale = plan.rationale;
    plan.metadata.warnings = plan.safety_notes;
    return plan;
}

BuildRecipe parse_recipe_object(std::string_view object_text) {
    BuildRecipe recipe;
    recipe.id = trim(extract_json_string(object_text, "id"));
    recipe.acquisition_method = trim(extract_json_string(object_text, "acquisition_method"));
    if (recipe.acquisition_method.empty()) {
        recipe.acquisition_method = "local";
    }
    recipe.build_system = trim(extract_json_string(object_text, "build_system"));
    recipe.supported_platforms = extract_json_string_array(object_text, "supported_platforms");
    recipe.default_target = trim(extract_json_string(object_text, "default_target"));
    recipe.install_target = trim(extract_json_string(object_text, "install_target"));
    recipe.artifact_patterns = extract_json_string_array(object_text, "artifact_patterns");
    recipe.install_output_patterns = extract_json_string_array(object_text, "install_output_patterns");
    recipe.fallback_stage_patterns = extract_json_string_array(object_text, "fallback_stage_patterns");
    recipe.dependency_hints = extract_json_string_array(object_text, "dependency_hints");
    recipe.configure_arguments = extract_json_string_array(object_text, "configure_arguments");
    recipe.supports_install = extract_json_bool(object_text, "supports_install", true);
    recipe.copy_artifacts_on_install = extract_json_bool(object_text, "copy_artifacts_on_install", false);
    return recipe;
}

std::string render_recipe_authoring_plan_schema() {
    return
        "{\"type\":\"object\",\"properties\":{"
        "\"recipe\":{\"type\":\"object\",\"properties\":{"
        "\"id\":{\"type\":\"string\"},"
        "\"acquisition_method\":{\"type\":\"string\"},"
        "\"build_system\":{\"type\":\"string\"},"
        "\"supported_platforms\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},"
        "\"default_target\":{\"type\":\"string\"},"
        "\"install_target\":{\"type\":\"string\"},"
        "\"artifact_patterns\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},"
        "\"install_output_patterns\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},"
        "\"fallback_stage_patterns\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},"
        "\"dependency_hints\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},"
        "\"configure_arguments\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},"
        "\"supports_install\":{\"type\":\"boolean\"},"
        "\"copy_artifacts_on_install\":{\"type\":\"boolean\"}"
        "},\"additionalProperties\":false,\"required\":[\"id\",\"acquisition_method\",\"build_system\",\"supported_platforms\",\"default_target\",\"install_target\",\"artifact_patterns\",\"install_output_patterns\",\"fallback_stage_patterns\",\"dependency_hints\",\"configure_arguments\",\"supports_install\",\"copy_artifacts_on_install\"]},"
        "\"rationale\":{\"type\":\"string\"},"
        "\"evidence_references\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},"
        "\"confidence\":{\"type\":\"number\"},"
        "\"warnings\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}"
        "},\"additionalProperties\":false,\"required\":[\"recipe\",\"rationale\",\"evidence_references\",\"confidence\",\"warnings\"]}";
}

std::string build_recipe_authoring_system_prompt() {
    return "You are an Ollama-backed build recipe author for OmniX. "
           "Author exactly one local build recipe for the inspected source tree. "
           "Return strict JSON only. Do not write prose outside the schema, do not emit shell commands, "
           "and do not assume remote acquisition in this phase.";
}

std::string build_recipe_authoring_prompt(const RecipeAuthoringRequest& request) {
    std::ostringstream prompt;
    prompt << "Task: " << request.task_id << "\n";
    prompt << "Scope: local-path only\n";
    prompt << "Source path: " << request.source_path << "\n";
    prompt << "Resolved source path: " << request.resolved_source_path << "\n";
    prompt << "Canonical project name: " << request.canonical_project_name << "\n";
    prompt << "Detected build system: " << request.detected_build_system << "\n";
    if (!request.requested_target.empty()) {
        prompt << "Requested target: " << request.requested_target << "\n";
    }
    if (!request.build_type.empty()) {
        prompt << "Build type: " << request.build_type << "\n";
    }
    if (!request.install_prefix.empty()) {
        prompt << "Install prefix: " << request.install_prefix << "\n";
    }
    if (!request.detected_files.empty()) {
        prompt << "Detected files:\n";
        for (const std::string& file : request.detected_files) {
            prompt << "- " << file << "\n";
        }
    }
    if (!request.ranked_evidence.empty()) {
        prompt << "Ranked evidence:\n";
        for (const std::string& entry : request.ranked_evidence) {
            prompt << "- " << entry << "\n";
        }
    }
    if (!request.recommended_modules.empty()) {
        prompt << "Recommended modules:\n";
        for (const std::string& module : request.recommended_modules) {
            prompt << "- " << module << "\n";
        }
    }
    if (!request.missing_modules.empty()) {
        prompt << "Missing modules:\n";
        for (const std::string& module : request.missing_modules) {
            prompt << "- " << module << "\n";
        }
    }
    if (!request.validation_feedback.empty()) {
        prompt << "Validation feedback from the failed attempt:\n";
        for (const std::string& line : request.validation_feedback) {
            prompt << "- " << line << "\n";
        }
    }
    prompt << "\nInstructions:\n"
           << "- Author one recipe under the nested `recipe` object.\n"
           << "- Keep `acquisition_method` as `local`.\n"
           << "- Build system must match a supported local executor: cmake, configure, or make.\n"
           << "- `artifact_patterns` must name build outputs OmniX can verify.\n"
           << "- `install_output_patterns` should be included when install outputs are expected.\n"
           << "- `evidence_references` must cite only the supplied evidence lines.\n"
           << "- `confidence` must be between 0.0 and 1.0.\n"
           << "- Do not assume remote fetch, package-manager mutation, or direct file editing.\n";
    return prompt.str();
}

std::optional<RecipeAuthoringPlan> parse_recipe_authoring_plan(std::string_view payload,
                                                               std::string_view provider_id,
                                                               std::string_view model) {
    const std::string normalized = strip_code_fences(trim(payload));
    if (normalized.empty() || normalized.front() != '{') {
        return std::nullopt;
    }

    const std::string recipe_text = extract_json_object(normalized, "recipe");
    if (recipe_text.empty()) {
        return std::nullopt;
    }

    RecipeAuthoringPlan plan;
    plan.task_id = "recipe_author";
    plan.provider_id = std::string(provider_id);
    plan.model = std::string(model);
    plan.status = "planned";
    plan.recipe = parse_recipe_object(recipe_text);
    plan.rationale = trim(extract_json_string(normalized, "rationale"));
    plan.evidence_references = extract_json_string_array(normalized, "evidence_references");
    plan.confidence = extract_json_double(normalized, "confidence", -1.0);
    plan.warnings = extract_json_string_array(normalized, "warnings");
    if (plan.recipe.id.empty() || plan.recipe.build_system.empty() || plan.rationale.empty()) {
        return std::nullopt;
    }
    plan.metadata.provider = std::string(provider_id);
    plan.metadata.model = std::string(model);
    plan.metadata.status = plan.status;
    plan.metadata.confidence = plan.confidence;
    plan.metadata.rationale = plan.rationale;
    plan.metadata.warnings = plan.warnings;
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
        "},\"additionalProperties\":false,\"required\":[\"canonical_command\",\"command_family\",\"rationale\",\"confidence\",\"requires_confirmation\",\"safety_notes\"]}";
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
    plan.metadata.provider = std::string(provider_id);
    plan.metadata.model = std::string(model);
    plan.metadata.status = plan.status;
    plan.metadata.confidence = plan.confidence;
    plan.metadata.rationale = plan.rationale;
    plan.metadata.warnings = plan.safety_notes;
    return plan;
}

std::string render_next_step_plan_schema() {
    return
        "{\"type\":\"object\",\"properties\":{"
        "\"suggested_next_step\":{\"type\":\"string\"},"
        "\"safer_alternative\":{\"type\":\"string\"},"
        "\"rationale\":{\"type\":\"string\"},"
        "\"confidence\":{\"type\":\"number\"},"
        "\"warnings\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}"
        "},\"additionalProperties\":false,\"required\":[\"suggested_next_step\",\"safer_alternative\",\"rationale\",\"confidence\",\"warnings\"]}";
}

std::string build_next_step_system_prompt() {
    return "You are a guarded next-step advisor for OmniX. Suggest one next safe operator step based on the deterministic guidance. "
           "Do not invent arbitrary shell commands or unsafe actions. Return strict JSON matching the provided schema.";
}

std::string build_next_step_prompt(std::string_view prompt_text, std::string_view deterministic_guidance) {
    std::ostringstream prompt;
    prompt << "Operator follow-up request:\n" << prompt_text << "\n\n";
    prompt << "Deterministic guidance:\n" << deterministic_guidance << "\n\n";
    prompt << "Instructions:\n"
           << "- Suggest one next safe step.\n"
           << "- Prefer an OmniX command or a bounded guarded action.\n"
           << "- Use safer_alternative when the strongest next step may still be too aggressive.\n"
           << "- Confidence must be between 0.0 and 1.0.\n";
    return prompt.str();
}

std::optional<NextStepAssistPlan> parse_next_step_assist_plan(std::string_view payload,
                                                              std::string_view provider_id,
                                                              std::string_view model) {
    const std::string normalized = strip_code_fences(trim(payload));
    if (normalized.empty() || normalized.front() != '{') {
        return std::nullopt;
    }
    NextStepAssistPlan plan;
    plan.task_id = "next_step";
    plan.provider_id = std::string(provider_id);
    plan.model = std::string(model);
    plan.status = "planned";
    plan.suggested_next_step = trim(extract_json_string(normalized, "suggested_next_step"));
    plan.safer_alternative = trim(extract_json_string(normalized, "safer_alternative"));
    plan.rationale = trim(extract_json_string(normalized, "rationale"));
    plan.confidence = extract_json_double(normalized, "confidence", -1.0);
    plan.warnings = extract_json_string_array(normalized, "warnings");
    if (plan.suggested_next_step.empty() || plan.rationale.empty()) {
        return std::nullopt;
    }
    plan.metadata.provider = std::string(provider_id);
    plan.metadata.model = std::string(model);
    plan.metadata.status = plan.status;
    plan.metadata.confidence = plan.confidence;
    plan.metadata.rationale = plan.rationale;
    plan.metadata.warnings = plan.warnings;
    return plan;
}

std::string render_case_summary_plan_schema() {
    return
        "{\"type\":\"object\",\"properties\":{"
        "\"summary_title\":{\"type\":\"string\"},"
        "\"executive_summary\":{\"type\":\"string\"},"
        "\"highlights\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},"
        "\"rationale\":{\"type\":\"string\"},"
        "\"confidence\":{\"type\":\"number\"},"
        "\"warnings\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}"
        "},\"additionalProperties\":false,\"required\":[\"summary_title\",\"executive_summary\",\"highlights\",\"rationale\",\"confidence\",\"warnings\"]}";
}

std::string build_case_summary_system_prompt() {
    return "You are a guarded case-summary assistant for OmniX. Summarize deterministic case or incident output without changing facts. "
           "Return strict JSON matching the provided schema.";
}

std::string build_case_summary_prompt(std::string_view target_label, std::string_view deterministic_summary) {
    std::ostringstream prompt;
    prompt << "Target:\n" << target_label << "\n\n";
    prompt << "Deterministic summary:\n" << deterministic_summary << "\n\n";
    prompt << "Instructions:\n"
           << "- Preserve deterministic facts.\n"
           << "- Keep the executive summary concise and operational.\n"
           << "- Highlights should be short bullet-like strings.\n"
           << "- Confidence must be between 0.0 and 1.0.\n";
    return prompt.str();
}

std::string render_freeform_answer_schema() {
    return
        "{\"type\":\"object\",\"properties\":{"
        "\"answer\":{\"type\":\"string\"},"
        "\"rationale\":{\"type\":\"string\"},"
        "\"confidence\":{\"type\":\"number\"},"
        "\"suggested_commands\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},"
        "\"safety_warnings\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},"
        "\"used_context\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}"
        "},\"additionalProperties\":false,\"required\":[\"answer\",\"rationale\",\"confidence\",\"suggested_commands\",\"safety_warnings\",\"used_context\"]}";
}

std::string build_freeform_system_prompt() {
    return "You are OpenAI assisting OmniX after deterministic local routing, definitions, memory, and guarded tool planning missed. "
           "Answer directly, but respect OmniX guardrails: do not claim local actions were executed, do not invent packet/Nmap/log evidence, "
           "do not recommend destructive actions as automatic, and prefer explicit OmniX diagnostic commands for security questions. Return strict JSON only.";
}

std::vector<std::pair<std::string, std::string>> freeform_command_dictionary() {
    return {
        {"ask <prompt> --assist", "Ask OmniX to answer through local memory and guarded assist when local routing misses."},
        {"define <term>", "Resolve a concept from operator teaching, memory artifacts, dictionary sources, and local retrieval."},
        {"provider probe", "Check whether the selected reasoning provider is configured and ready."},
        {"defend diag cpu", "Collect read-only CPU diagnostic evidence and propose manual next steps."},
        {"defend diag memory", "Collect read-only memory diagnostic evidence and propose manual next steps."},
        {"defend diag port <port>", "Inspect local evidence for a port and recommend manual investigation steps."},
        {"defend diag pid <pid>", "Inspect a process ID and propose manual action without killing it."},
        {"tview port <port>", "Capture bounded TCP packet summaries for a concrete local port; live capture may require elevated permissions."},
        {"tview pcap <file> --port <port>", "Review a pcap file deterministically without live capture privileges."},
        {"tool nmap -- <args>", "Run allowlisted native Nmap invocations; guarded Nmap scanning is local/loopback-first."},
        {"memory history", "Inspect compact run history and retained artifacts."},
        {"tze replay latest", "Replay the latest TZE run with provenance and stages."},
        {"tze chain latest", "Render the latest TZE chain."},
    };
}

std::string build_freeform_prompt(std::string_view prompt_text) {
    std::ostringstream prompt;
    prompt << "Operator prompt:\n" << prompt_text << "\n\n";
    prompt << "Local resolution status:\n"
           << "- Deterministic intent routing did not produce an executable or definition answer.\n"
           << "- Local memory, Storage.Permanent history, operator glossary, dictionary/retrieval, command routing, and bounded tool planning have priority over this fallback.\n"
           << "- This fallback may explain, calculate, or propose commands, but it must not claim that commands were executed.\n\n";
    prompt << "OmniX command dictionary:\n";
    for (const auto& [command, description] : freeform_command_dictionary()) {
        prompt << "- " << command << ": " << description << "\n";
    }
    prompt << "\nGuardrails:\n"
           << "- If a concrete command is appropriate, put it in suggested_commands instead of pretending it ran.\n"
           << "- For broad security prompts such as 'am I secure', propose a diagnostic chain first: defend diag cpu, defend diag memory, defend diag port <port>, guarded loopback Nmap, then tview for concrete ports.\n"
           << "- If a port is UNKNOWN, ask for discovery or suggest local discovery commands before tview.\n"
           << "- Never say 'you are secure' or 'you are not secure' without deterministic evidence; say what evidence is missing.\n"
           << "- Keep the answer concise and professional.\n";
    return prompt.str();
}

std::optional<FreeformAssistAnswer> parse_freeform_assist_answer(std::string_view payload,
                                                                 std::string_view provider_id,
                                                                 std::string_view model) {
    const std::string normalized = strip_code_fences(trim(payload));
    if (normalized.empty() || normalized.front() != '{') {
        return std::nullopt;
    }

    FreeformAssistAnswer answer;
    answer.task_id = "freeform_answer";
    answer.provider_id = std::string(provider_id);
    answer.model = std::string(model);
    answer.status = "assist_freeform_used";
    answer.answer = trim(extract_json_string(normalized, "answer"));
    answer.rationale = trim(extract_json_string(normalized, "rationale"));
    answer.confidence = extract_json_double(normalized, "confidence", -1.0);
    answer.suggested_commands = extract_json_string_array(normalized, "suggested_commands");
    answer.safety_warnings = extract_json_string_array(normalized, "safety_warnings");
    answer.used_context = extract_json_string_array(normalized, "used_context");
    if (answer.answer.empty() || answer.rationale.empty() || answer.confidence < 0.0 || answer.confidence > 1.0) {
        return std::nullopt;
    }
    answer.validated = true;
    answer.metadata.provider = std::string(provider_id);
    answer.metadata.model = std::string(model);
    answer.metadata.status = answer.status;
    answer.metadata.confidence = answer.confidence;
    answer.metadata.rationale = answer.rationale;
    answer.metadata.warnings = answer.safety_warnings;
    return answer;
}

std::optional<CaseSummaryAssistPlan> parse_case_summary_assist_plan(std::string_view payload,
                                                                    std::string_view provider_id,
                                                                    std::string_view model) {
    const std::string normalized = strip_code_fences(trim(payload));
    if (normalized.empty() || normalized.front() != '{') {
        return std::nullopt;
    }
    CaseSummaryAssistPlan plan;
    plan.task_id = "case_summary";
    plan.provider_id = std::string(provider_id);
    plan.model = std::string(model);
    plan.status = "planned";
    plan.summary_title = trim(extract_json_string(normalized, "summary_title"));
    plan.executive_summary = trim(extract_json_string(normalized, "executive_summary"));
    plan.highlights = extract_json_string_array(normalized, "highlights");
    plan.rationale = trim(extract_json_string(normalized, "rationale"));
    plan.confidence = extract_json_double(normalized, "confidence", -1.0);
    plan.warnings = extract_json_string_array(normalized, "warnings");
    if (plan.executive_summary.empty() || plan.rationale.empty()) {
        return std::nullopt;
    }
    plan.metadata.provider = std::string(provider_id);
    plan.metadata.model = std::string(model);
    plan.metadata.status = plan.status;
    plan.metadata.confidence = plan.confidence;
    plan.metadata.rationale = plan.rationale;
    plan.metadata.warnings = plan.warnings;
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
        "` is not supported yet. OmniX currently supports `null`, `ollama`, and `openai`.";
    report.configured = true;
    report.available = false;
    report.checks = {
        "selected provider: " + std::string(selected_id),
    };
    report.warnings = {
        "Use `OMNIX_REASONING_PROVIDER=ollama`, `OMNIX_REASONING_PROVIDER=openai`, or unset the provider selection.",
    };
    return report;
}

ProviderProbeReport make_openai_probe(std::string base_url,
                                      std::string api_key,
                                      std::string model,
                                      std::string organization,
                                      std::string project) {
    ProviderProbeReport report;
    report.provider_id = "openai";
    report.configured = true;
    report.available = false;
    report.base_url = normalize_base_url(std::move(base_url), "https://api.openai.com/v1");
    report.model = std::move(model);
    report.checks.push_back("selected provider: openai");
    report.checks.push_back("base URL: " + report.base_url);

    if (api_key.empty()) {
        report.status = "config_incomplete";
        report.summary = "OpenAI is selected, but `OMNIX_OPENAI_API_KEY` or `OPENAI_API_KEY` is not set.";
        report.warnings.push_back("Set `OPENAI_API_KEY` or `OMNIX_OPENAI_API_KEY`, then rerun `omnix provider probe`.");
        return report;
    }
    report.checks.push_back("api key: configured");

    if (report.model.empty()) {
        report.status = "config_incomplete";
        report.summary = "OpenAI is selected, but `OMNIX_OPENAI_MODEL` or `OPENAI_MODEL` is not set.";
        report.warnings.push_back("Set `OMNIX_OPENAI_MODEL` or `OPENAI_MODEL`, then rerun `omnix provider probe`.");
        return report;
    }
    report.checks.push_back("requested model: " + report.model);

    if (!organization.empty()) {
        report.checks.push_back("organization: configured");
    }
    if (!project.empty()) {
        report.checks.push_back("project: configured");
    }
    if (!is_http_or_https_url(report.base_url)) {
        report.status = "invalid_config";
        report.summary = "OpenAI base URL `" + report.base_url + "` is invalid: only http:// and https:// URLs are supported.";
        report.warnings.push_back("Use a URL such as `https://api.openai.com/v1`.");
        return report;
    }

    if (const char* fixture_path = std::getenv("OMNIX_OPENAI_MODEL_LIST_FILE");
        fixture_path != nullptr && *fixture_path != '\0') {
        std::ifstream input(fixture_path);
        if (!input) {
            report.status = "endpoint_error";
            report.summary = "OpenAI model list fixture could not be read.";
            report.warnings.push_back("Verify `OMNIX_OPENAI_MODEL_LIST_FILE` points to a readable JSON file.");
            return report;
        }
        std::ostringstream buffer;
        buffer << input.rdbuf();
        const std::vector<std::string> model_names = extract_model_names(buffer.str());
        const bool model_found =
            std::find(model_names.begin(), model_names.end(), report.model) != model_names.end() ||
            buffer.str().find(report.model) != std::string::npos;
        if (!model_found) {
            report.status = "model_missing";
            report.summary = "OpenAI fixture data loaded, but model `" + report.model + "` is not present.";
            report.warnings.push_back("Update the fixture or choose a model present in the fixture.");
            return report;
        }
        report.status = "ready";
        report.available = true;
        report.summary = "OpenAI is configured and model `" + report.model +
            "` is available. Guarded assist is ready when you use `--assist` or enable `/assist on` in the shell.";
        report.checks.push_back("fixture model listing loaded");
        return report;
    }

    const HttpResponse response =
        curl_json_request("GET", openai_models_url(report.base_url), api_key, {}, organization, project);
    if (response.status_code > 0) {
        report.checks.push_back("endpoint responded with HTTP " + std::to_string(response.status_code));
    }
    if (!response.ok) {
        report.status = response.status_code > 0 ? "endpoint_error" : "endpoint_unreachable";
        report.summary = response.status_code > 0
            ? "Reached OpenAI at `" + report.base_url + "`, but the model listing request failed with HTTP " +
                std::to_string(response.status_code) + "."
            : "Could not reach OpenAI at `" + report.base_url + "`.";
        if (!response.error.empty()) {
            report.warnings.push_back(response.error);
        }
        report.warnings.push_back("Verify the API key, network access, and optional OpenAI project/organization settings, then rerun `omnix provider probe`.");
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

    const bool model_found =
        std::find(model_names.begin(), model_names.end(), report.model) != model_names.end() ||
        response.body.find(report.model) != std::string::npos;
    if (!model_found) {
        report.status = "model_missing";
        report.summary = "OpenAI is reachable, but model `" + report.model + "` is not currently available.";
        report.warnings.push_back("Choose a model that is available to this API key, then rerun `omnix provider probe`.");
        return report;
    }

    report.status = "ready";
    report.available = true;
    report.summary = "OpenAI is configured and model `" + report.model +
        "` is available. Guarded assist is ready when you use `--assist` or enable `/assist on` in the shell.";
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

    if (report.model == "fixture" &&
        (std::getenv("OMNIX_OLLAMA_TOOL_PLAN_FILE") != nullptr ||
         std::getenv("OMNIX_OLLAMA_BUILD_PLAN_FILE") != nullptr ||
         std::getenv("OMNIX_OLLAMA_RECIPE_PLAN_FILE") != nullptr ||
         std::getenv("OMNIX_OLLAMA_RECIPE_PLAN_REPAIR_FILE") != nullptr ||
         std::getenv("OMNIX_OLLAMA_COMMAND_PLAN_FILE") != nullptr ||
         std::getenv("OMNIX_OLLAMA_NEXT_STEP_PLAN_FILE") != nullptr ||
         std::getenv("OMNIX_OLLAMA_CASE_SUMMARY_PLAN_FILE") != nullptr)) {
        report.status = "ready";
        report.available = true;
        report.summary = "Ollama fixture mode is configured and guarded assist is ready for fixture-backed validation.";
        report.checks.push_back("fixture-backed assist mode");
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
    HttpResponse response;
    if (const char* fixture_path = std::getenv("OMNIX_OLLAMA_MODEL_LIST_FILE")) {
        response.body = read_text_file_if_exists(fixture_path);
        response.status_code = response.body.empty() ? 0 : 200;
        response.ok = !response.body.empty();
        if (!response.ok) {
            response.error = "Unable to read OMNIX_OLLAMA_MODEL_LIST_FILE fixture.";
        }
    } else {
        response = http_get_json(parsed, request_path);
    }
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
        if (report.model == "deepnimsec-omni:latest") {
            report.warnings.push_back("Rebuild the custom DeepNimSec profile with `./scripts/omnix_deepnimsec.sh --refresh-model`, then rerun `omnix provider probe`.");
        } else {
            report.warnings.push_back("Pull or load the requested model in Ollama, then rerun `omnix provider probe`.");
        }
        return report;
    }

    const std::string show_request_path = model_show_path(parsed.path);
    report.checks.push_back("model show path: " + show_request_path);
    HttpResponse show_response;
    if (const char* show_fixture_path = std::getenv("OMNIX_OLLAMA_MODEL_SHOW_FILE")) {
        show_response.body = read_text_file_if_exists(show_fixture_path);
        show_response.status_code = show_response.body.empty() ? 0 : 200;
        show_response.ok = !show_response.body.empty() &&
            show_response.body.find("\"error\"") == std::string::npos &&
            show_response.body.find("not found") == std::string::npos;
        if (!show_response.ok && show_response.body.empty()) {
            show_response.error = "Unable to read OMNIX_OLLAMA_MODEL_SHOW_FILE fixture.";
        }
    } else {
        std::ostringstream body;
        body << "{\"name\":\"" << escape_json(report.model) << "\"}";
        show_response = http_post_json(parsed, show_request_path, body.str());
    }
    if (!show_response.ok) {
        report.status = "model_unusable";
        report.summary = "Ollama listed model `" + report.model + "`, but the model could not be resolved for use.";
        if (!show_response.error.empty()) {
            report.warnings.push_back(show_response.error);
        }
        if (report.model == "deepnimsec-omni:latest") {
            report.warnings.push_back("Rebuild the custom DeepNimSec profile with `./scripts/omnix_deepnimsec.sh --refresh-model`, then rerun `omnix provider probe`.");
        } else {
            report.warnings.push_back("Rebuild or reload the requested Ollama model, then rerun `omnix provider probe`.");
        }
        return report;
    }

    report.status = "ready";
    report.available = true;
    report.summary = "Ollama is configured and model `" + report.model +
        "` is available. Guarded assist is ready when you use `--assist` or enable `/assist on` in the shell.";
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

std::optional<RecipeAuthoringPlan> request_ollama_recipe_authoring_plan(std::string_view base_url,
                                                                        std::string_view model,
                                                                        const RecipeAuthoringRequest& request,
                                                                        std::vector<std::string>* warnings) {
    const char* fixture_name = request.task_id == "recipe_author_repair"
        ? "OMNIX_OLLAMA_RECIPE_PLAN_REPAIR_FILE"
        : "OMNIX_OLLAMA_RECIPE_PLAN_FILE";
    if (const char* fixture_path = std::getenv(fixture_name);
        fixture_path != nullptr && *fixture_path != '\0') {
        std::ifstream input(fixture_path);
        if (!input) {
            if (warnings != nullptr) {
                warnings->push_back(std::string("Unable to read ") + fixture_name + " fixture.");
            }
            return std::nullopt;
        }
        std::ostringstream buffer;
        buffer << input.rdbuf();
        return parse_recipe_authoring_plan(buffer.str(), "ollama", model);
    }

    const ParsedUrl parsed = parse_http_url(base_url);
    if (!parsed.valid) {
        if (warnings != nullptr) {
            warnings->push_back("Invalid Ollama base URL for recipe authoring: " + parsed.error);
        }
        return std::nullopt;
    }

    std::ostringstream body;
    body << "{\"model\":\"" << escape_json(model)
         << "\",\"system\":\"" << escape_json(build_recipe_authoring_system_prompt())
         << "\",\"prompt\":\"" << escape_json(build_recipe_authoring_prompt(request))
         << "\",\"format\":" << render_recipe_authoring_plan_schema()
         << ",\"stream\":false}";
    const HttpResponse response = http_post_json(parsed, generate_path(parsed.path), body.str());
    if (!response.ok) {
        if (warnings != nullptr) {
            warnings->push_back(response.error.empty() ? "Ollama recipe-authoring request failed." : response.error);
        }
        return std::nullopt;
    }

    const std::string generated = extract_json_string(response.body, "response");
    if (generated.empty()) {
        if (warnings != nullptr) {
            warnings->push_back("Ollama recipe-authoring response did not include a `response` field.");
        }
        return std::nullopt;
    }
    return parse_recipe_authoring_plan(generated, "ollama", model);
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

std::optional<NextStepAssistPlan> request_ollama_next_step_plan(std::string_view base_url,
                                                                std::string_view model,
                                                                std::string_view prompt_text,
                                                                std::string_view deterministic_guidance,
                                                                std::vector<std::string>* warnings) {
    if (const char* fixture_path = std::getenv("OMNIX_OLLAMA_NEXT_STEP_PLAN_FILE");
        fixture_path != nullptr && *fixture_path != '\0') {
        std::ifstream input(fixture_path);
        if (!input) {
            if (warnings != nullptr) {
                warnings->push_back("Unable to read OMNIX_OLLAMA_NEXT_STEP_PLAN_FILE fixture.");
            }
            return std::nullopt;
        }
        std::ostringstream buffer;
        buffer << input.rdbuf();
        return parse_next_step_assist_plan(buffer.str(), "ollama", model);
    }
    const ParsedUrl parsed = parse_http_url(base_url);
    if (!parsed.valid) {
        if (warnings != nullptr) {
            warnings->push_back("Invalid Ollama base URL for next-step planning: " + parsed.error);
        }
        return std::nullopt;
    }
    std::ostringstream body;
    body << "{\"model\":\"" << escape_json(model)
         << "\",\"system\":\"" << escape_json(build_next_step_system_prompt())
         << "\",\"prompt\":\"" << escape_json(build_next_step_prompt(prompt_text, deterministic_guidance))
         << "\",\"format\":" << render_next_step_plan_schema()
         << ",\"stream\":false}";
    const HttpResponse response = http_post_json(parsed, generate_path(parsed.path), body.str());
    if (!response.ok) {
        if (warnings != nullptr) {
            warnings->push_back(response.error.empty() ? "Ollama next-step request failed." : response.error);
        }
        return std::nullopt;
    }
    const std::string generated = extract_json_string(response.body, "response");
    if (generated.empty()) {
        if (warnings != nullptr) {
            warnings->push_back("Ollama next-step response did not include a `response` field.");
        }
        return std::nullopt;
    }
    return parse_next_step_assist_plan(generated, "ollama", model);
}

std::optional<CaseSummaryAssistPlan> request_ollama_case_summary_plan(std::string_view base_url,
                                                                      std::string_view model,
                                                                      std::string_view target_label,
                                                                      std::string_view deterministic_summary,
                                                                      std::vector<std::string>* warnings) {
    if (const char* fixture_path = std::getenv("OMNIX_OLLAMA_CASE_SUMMARY_PLAN_FILE");
        fixture_path != nullptr && *fixture_path != '\0') {
        std::ifstream input(fixture_path);
        if (!input) {
            if (warnings != nullptr) {
                warnings->push_back("Unable to read OMNIX_OLLAMA_CASE_SUMMARY_PLAN_FILE fixture.");
            }
            return std::nullopt;
        }
        std::ostringstream buffer;
        buffer << input.rdbuf();
        return parse_case_summary_assist_plan(buffer.str(), "ollama", model);
    }
    const ParsedUrl parsed = parse_http_url(base_url);
    if (!parsed.valid) {
        if (warnings != nullptr) {
            warnings->push_back("Invalid Ollama base URL for case-summary planning: " + parsed.error);
        }
        return std::nullopt;
    }
    std::ostringstream body;
    body << "{\"model\":\"" << escape_json(model)
         << "\",\"system\":\"" << escape_json(build_case_summary_system_prompt())
         << "\",\"prompt\":\"" << escape_json(build_case_summary_prompt(target_label, deterministic_summary))
         << "\",\"format\":" << render_case_summary_plan_schema()
         << ",\"stream\":false}";
    const HttpResponse response = http_post_json(parsed, generate_path(parsed.path), body.str());
    if (!response.ok) {
        if (warnings != nullptr) {
            warnings->push_back(response.error.empty() ? "Ollama case-summary request failed." : response.error);
        }
        return std::nullopt;
    }
    const std::string generated = extract_json_string(response.body, "response");
    if (generated.empty()) {
        if (warnings != nullptr) {
            warnings->push_back("Ollama case-summary response did not include a `response` field.");
        }
        return std::nullopt;
    }
    return parse_case_summary_assist_plan(generated, "ollama", model);
}

std::optional<std::string> request_openai_structured_output(std::string_view base_url,
                                                            std::string_view api_key,
                                                            std::string_view model,
                                                            std::string_view instructions,
                                                            std::string_view input_text,
                                                            std::string_view schema_name,
                                                            std::string_view schema_json,
                                                            std::vector<std::string>* warnings,
                                                            std::string_view organization = {},
                                                            std::string_view project = {}) {
    std::ostringstream body;
    body << "{"
         << "\"model\":\"" << escape_json(model) << "\","
         << "\"instructions\":\"" << escape_json(instructions) << "\","
         << "\"input\":\"" << escape_json(input_text) << "\","
         << "\"store\":false,"
         << "\"text\":{\"format\":{"
         << "\"type\":\"json_schema\","
         << "\"name\":\"" << escape_json(schema_name) << "\","
         << "\"strict\":true,"
         << "\"schema\":" << schema_json
         << "}}}";

    const HttpResponse response = curl_json_request("POST",
                                                    openai_responses_url(std::string(base_url)),
                                                    api_key,
                                                    body.str(),
                                                    organization,
                                                    project);
    if (!response.ok) {
        if (warnings != nullptr) {
            warnings->push_back(response.error.empty() ? "OpenAI request failed." : response.error);
        }
        return std::nullopt;
    }

    const std::string generated = extract_openai_output_text(response.body);
    if (generated.empty()) {
        if (warnings != nullptr) {
            warnings->push_back("OpenAI response did not include an `output_text` item.");
        }
        return std::nullopt;
    }
    return generated;
}

std::optional<AssistAnnotation> request_openai_annotation(std::string_view base_url,
                                                          std::string_view api_key,
                                                          std::string_view model,
                                                          const AssistRequest& request,
                                                          std::vector<std::string>* warnings,
                                                          std::string_view organization = {},
                                                          std::string_view project = {}) {
    if (const char* fixture_path = std::getenv("OMNIX_OPENAI_ASSIST_FILE");
        fixture_path != nullptr && *fixture_path != '\0') {
        std::ifstream input(fixture_path);
        if (!input) {
            if (warnings != nullptr) {
                warnings->push_back("Unable to read OMNIX_OPENAI_ASSIST_FILE fixture.");
            }
            return std::nullopt;
        }
        std::ostringstream buffer;
        buffer << input.rdbuf();
        return parse_assist_annotation(buffer.str(), "openai", model, request.task_id);
    }
    const std::optional<std::string> generated =
        request_openai_structured_output(base_url,
                                         api_key,
                                         model,
                                         build_assist_system_prompt(),
                                         build_assist_prompt(request),
                                         "omnix_assist_annotation",
                                         render_assist_schema(),
                                         warnings,
                                         organization,
                                         project);
    if (!generated.has_value()) {
        return std::nullopt;
    }
    return parse_assist_annotation(*generated, "openai", model, request.task_id);
}

std::optional<ToolAssistPlan> request_openai_tool_plan(std::string_view base_url,
                                                       std::string_view api_key,
                                                       std::string_view model,
                                                       std::string_view prompt_text,
                                                       const std::vector<std::string>& allowlisted_tools,
                                                       std::vector<std::string>* warnings,
                                                       std::string_view organization = {},
                                                       std::string_view project = {}) {
    if (const char* fixture_path = std::getenv("OMNIX_OPENAI_TOOL_PLAN_FILE");
        fixture_path != nullptr && *fixture_path != '\0') {
        std::ifstream input(fixture_path);
        if (!input) {
            if (warnings != nullptr) {
                warnings->push_back("Unable to read OMNIX_OPENAI_TOOL_PLAN_FILE fixture.");
            }
            return std::nullopt;
        }
        std::ostringstream buffer;
        buffer << input.rdbuf();
        return parse_tool_assist_plan(buffer.str(), "openai", model);
    }
    const std::optional<std::string> generated =
        request_openai_structured_output(base_url,
                                         api_key,
                                         model,
                                         build_tool_plan_system_prompt(),
                                         build_tool_plan_prompt(prompt_text, allowlisted_tools),
                                         "omnix_tool_plan",
                                         render_tool_plan_schema(),
                                         warnings,
                                         organization,
                                         project);
    if (!generated.has_value()) {
        return std::nullopt;
    }
    return parse_tool_assist_plan(*generated, "openai", model);
}

std::optional<BuildAssistPlan> request_openai_build_plan(std::string_view base_url,
                                                         std::string_view api_key,
                                                         std::string_view model,
                                                         const BuildAssistRequest& request,
                                                         std::vector<std::string>* warnings,
                                                         std::string_view organization = {},
                                                         std::string_view project = {}) {
    if (const char* fixture_path = std::getenv("OMNIX_OPENAI_BUILD_PLAN_FILE");
        fixture_path != nullptr && *fixture_path != '\0') {
        std::ifstream input(fixture_path);
        if (!input) {
            if (warnings != nullptr) {
                warnings->push_back("Unable to read OMNIX_OPENAI_BUILD_PLAN_FILE fixture.");
            }
            return std::nullopt;
        }
        std::ostringstream buffer;
        buffer << input.rdbuf();
        return parse_build_assist_plan(buffer.str(), "openai", model);
    }
    const std::optional<std::string> generated =
        request_openai_structured_output(base_url,
                                         api_key,
                                         model,
                                         build_build_plan_system_prompt(),
                                         build_build_plan_prompt(request),
                                         "omnix_build_plan",
                                         render_build_plan_schema(),
                                         warnings,
                                         organization,
                                         project);
    if (!generated.has_value()) {
        return std::nullopt;
    }
    return parse_build_assist_plan(*generated, "openai", model);
}

std::optional<CommandAssistPlan> request_openai_command_plan(std::string_view base_url,
                                                             std::string_view api_key,
                                                             std::string_view model,
                                                             std::string_view prompt_text,
                                                             const std::vector<std::string>& allowlisted_commands,
                                                             std::vector<std::string>* warnings,
                                                             std::string_view organization = {},
                                                             std::string_view project = {}) {
    if (const char* fixture_path = std::getenv("OMNIX_OPENAI_COMMAND_PLAN_FILE");
        fixture_path != nullptr && *fixture_path != '\0') {
        std::ifstream input(fixture_path);
        if (!input) {
            if (warnings != nullptr) {
                warnings->push_back("Unable to read OMNIX_OPENAI_COMMAND_PLAN_FILE fixture.");
            }
            return std::nullopt;
        }
        std::ostringstream buffer;
        buffer << input.rdbuf();
        return parse_command_assist_plan(buffer.str(), "openai", model);
    }
    const std::optional<std::string> generated =
        request_openai_structured_output(base_url,
                                         api_key,
                                         model,
                                         build_command_plan_system_prompt(),
                                         build_command_plan_prompt(prompt_text, allowlisted_commands),
                                         "omnix_command_plan",
                                         render_command_plan_schema(),
                                         warnings,
                                         organization,
                                         project);
    if (!generated.has_value()) {
        return std::nullopt;
    }
    return parse_command_assist_plan(*generated, "openai", model);
}

std::optional<NextStepAssistPlan> request_openai_next_step_plan(std::string_view base_url,
                                                                std::string_view api_key,
                                                                std::string_view model,
                                                                std::string_view prompt_text,
                                                                std::string_view deterministic_guidance,
                                                                std::vector<std::string>* warnings,
                                                                std::string_view organization = {},
                                                                std::string_view project = {}) {
    if (const char* fixture_path = std::getenv("OMNIX_OPENAI_NEXT_STEP_PLAN_FILE");
        fixture_path != nullptr && *fixture_path != '\0') {
        std::ifstream input(fixture_path);
        if (!input) {
            if (warnings != nullptr) {
                warnings->push_back("Unable to read OMNIX_OPENAI_NEXT_STEP_PLAN_FILE fixture.");
            }
            return std::nullopt;
        }
        std::ostringstream buffer;
        buffer << input.rdbuf();
        return parse_next_step_assist_plan(buffer.str(), "openai", model);
    }
    const std::optional<std::string> generated =
        request_openai_structured_output(base_url,
                                         api_key,
                                         model,
                                         build_next_step_system_prompt(),
                                         build_next_step_prompt(prompt_text, deterministic_guidance),
                                         "omnix_next_step_plan",
                                         render_next_step_plan_schema(),
                                         warnings,
                                         organization,
                                         project);
    if (!generated.has_value()) {
        return std::nullopt;
    }
    return parse_next_step_assist_plan(*generated, "openai", model);
}

std::optional<CaseSummaryAssistPlan> request_openai_case_summary_plan(std::string_view base_url,
                                                                      std::string_view api_key,
                                                                      std::string_view model,
                                                                      std::string_view target_label,
                                                                      std::string_view deterministic_summary,
                                                                      std::vector<std::string>* warnings,
                                                                      std::string_view organization = {},
                                                                      std::string_view project = {}) {
    if (const char* fixture_path = std::getenv("OMNIX_OPENAI_CASE_SUMMARY_PLAN_FILE");
        fixture_path != nullptr && *fixture_path != '\0') {
        std::ifstream input(fixture_path);
        if (!input) {
            if (warnings != nullptr) {
                warnings->push_back("Unable to read OMNIX_OPENAI_CASE_SUMMARY_PLAN_FILE fixture.");
            }
            return std::nullopt;
        }
        std::ostringstream buffer;
        buffer << input.rdbuf();
        return parse_case_summary_assist_plan(buffer.str(), "openai", model);
    }
    const std::optional<std::string> generated =
        request_openai_structured_output(base_url,
                                         api_key,
                                         model,
                                         build_case_summary_system_prompt(),
                                         build_case_summary_prompt(target_label, deterministic_summary),
                                         "omnix_case_summary_plan",
                                         render_case_summary_plan_schema(),
                                         warnings,
                                         organization,
                                         project);
    if (!generated.has_value()) {
        return std::nullopt;
    }
    return parse_case_summary_assist_plan(*generated, "openai", model);
}

std::optional<FreeformAssistAnswer> request_openai_freeform_answer(std::string_view base_url,
                                                                   std::string_view api_key,
                                                                   std::string_view model,
                                                                   std::string_view prompt_text,
                                                                   std::vector<std::string>* warnings,
                                                                   std::string_view organization = {},
                                                                   std::string_view project = {}) {
    if (const char* fixture_path = std::getenv("OMNIX_OPENAI_FREEFORM_FILE");
        fixture_path != nullptr && *fixture_path != '\0') {
        std::ifstream input(fixture_path);
        if (!input) {
            if (warnings != nullptr) {
                warnings->push_back("Unable to read OMNIX_OPENAI_FREEFORM_FILE fixture.");
            }
            return std::nullopt;
        }
        std::ostringstream buffer;
        buffer << input.rdbuf();
        return parse_freeform_assist_answer(buffer.str(), "openai", model);
    }
    const std::optional<std::string> generated =
        request_openai_structured_output(base_url,
                                         api_key,
                                         model,
                                         build_freeform_system_prompt(),
                                         build_freeform_prompt(prompt_text),
                                         "omnix_freeform_answer",
                                         render_freeform_answer_schema(),
                                         warnings,
                                         organization,
                                         project);
    if (!generated.has_value()) {
        return std::nullopt;
    }
    return parse_freeform_assist_answer(*generated, "openai", model);
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

std::optional<RecipeAuthoringPlan> NullProvider::propose_authored_recipe(const RecipeAuthoringRequest&) const {
    return std::nullopt;
}

std::optional<CommandAssistPlan> NullProvider::propose_command_route(std::string_view,
                                                                     const std::vector<std::string>&) const {
    return std::nullopt;
}

std::optional<NextStepAssistPlan> NullProvider::propose_next_step(std::string_view,
                                                                  std::string_view) const {
    return std::nullopt;
}

std::optional<CaseSummaryAssistPlan> NullProvider::propose_case_summary(std::string_view,
                                                                        std::string_view) const {
    return std::nullopt;
}

std::optional<FreeformAssistAnswer> NullProvider::resolve_freeform(std::string_view) const {
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
    return probe().status == "ready";
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

std::optional<RecipeAuthoringPlan> OllamaProvider::propose_authored_recipe(const RecipeAuthoringRequest& request) const {
    const char* fixture_path = std::getenv("OMNIX_OLLAMA_RECIPE_PLAN_FILE");
    if (fixture_path == nullptr || *fixture_path == '\0') {
        const ProviderProbeReport probe_report = probe();
        if (probe_report.status != "ready") {
            return std::nullopt;
        }
    }
    std::vector<std::string> warnings;
    std::optional<RecipeAuthoringPlan> plan =
        request_ollama_recipe_authoring_plan(base_url_, model_, request, &warnings);
    if (!plan.has_value()) {
        return std::nullopt;
    }
    if (!warnings.empty()) {
        plan->warnings.insert(plan->warnings.end(), warnings.begin(), warnings.end());
        plan->metadata.warnings = plan->warnings;
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

std::optional<NextStepAssistPlan> OllamaProvider::propose_next_step(std::string_view prompt,
                                                                    std::string_view deterministic_guidance) const {
    const char* fixture_path = std::getenv("OMNIX_OLLAMA_NEXT_STEP_PLAN_FILE");
    if (fixture_path == nullptr || *fixture_path == '\0') {
        const ProviderProbeReport probe_report = probe();
        if (probe_report.status != "ready") {
            return std::nullopt;
        }
    }
    std::vector<std::string> warnings;
    std::optional<NextStepAssistPlan> plan =
        request_ollama_next_step_plan(base_url_, model_, prompt, deterministic_guidance, &warnings);
    if (!plan.has_value()) {
        return std::nullopt;
    }
    if (!warnings.empty()) {
        plan->warnings.insert(plan->warnings.end(), warnings.begin(), warnings.end());
        plan->metadata.warnings = plan->warnings;
    }
    return plan;
}

std::optional<CaseSummaryAssistPlan> OllamaProvider::propose_case_summary(std::string_view target_label,
                                                                          std::string_view deterministic_summary) const {
    const char* fixture_path = std::getenv("OMNIX_OLLAMA_CASE_SUMMARY_PLAN_FILE");
    if (fixture_path == nullptr || *fixture_path == '\0') {
        const ProviderProbeReport probe_report = probe();
        if (probe_report.status != "ready") {
            return std::nullopt;
        }
    }
    std::vector<std::string> warnings;
    std::optional<CaseSummaryAssistPlan> plan =
        request_ollama_case_summary_plan(base_url_, model_, target_label, deterministic_summary, &warnings);
    if (!plan.has_value()) {
        return std::nullopt;
    }
    if (!warnings.empty()) {
        plan->warnings.insert(plan->warnings.end(), warnings.begin(), warnings.end());
        plan->metadata.warnings = plan->warnings;
    }
    return plan;
}

std::optional<FreeformAssistAnswer> OllamaProvider::resolve_freeform(std::string_view) const {
    return std::nullopt;
}

OpenAIProvider::OpenAIProvider(std::string base_url,
                               std::string api_key,
                               std::string model,
                               std::string organization,
                               std::string project)
    : base_url_(std::move(base_url)),
      api_key_(std::move(api_key)),
      model_(std::move(model)),
      organization_(std::move(organization)),
      project_(std::move(project)) {}

std::string_view OpenAIProvider::id() const {
    return "openai";
}

bool OpenAIProvider::configured() const {
    return true;
}

bool OpenAIProvider::available() const {
    return probe().status == "ready";
}

ProviderProbeReport OpenAIProvider::probe() const {
    return make_openai_probe(base_url_, api_key_, model_, organization_, project_);
}

std::optional<AssistAnnotation> OpenAIProvider::assist_annotation(const AssistRequest& request) const {
    const ProviderProbeReport probe_report = probe();
    if (probe_report.status != "ready") {
        return std::nullopt;
    }
    std::vector<std::string> warnings;
    std::optional<AssistAnnotation> annotation =
        request_openai_annotation(base_url_, api_key_, model_, request, &warnings, organization_, project_);
    if (!annotation.has_value()) {
        return std::nullopt;
    }
    annotation->warnings = warnings;
    return annotation;
}

std::optional<ToolAssistPlan> OpenAIProvider::propose_tool_action(std::string_view prompt,
                                                                  const std::vector<std::string>& allowlisted_tools) const {
    const char* fixture_path = std::getenv("OMNIX_OPENAI_TOOL_PLAN_FILE");
    if (fixture_path == nullptr || *fixture_path == '\0') {
        const ProviderProbeReport probe_report = probe();
        if (probe_report.status != "ready") {
            return std::nullopt;
        }
    }
    std::vector<std::string> warnings;
    std::optional<ToolAssistPlan> plan =
        request_openai_tool_plan(base_url_, api_key_, model_, prompt, allowlisted_tools, &warnings, organization_, project_);
    if (!plan.has_value()) {
        return std::nullopt;
    }
    if (!warnings.empty()) {
        plan->safety_notes.insert(plan->safety_notes.end(), warnings.begin(), warnings.end());
    }
    return plan;
}

std::optional<BuildAssistPlan> OpenAIProvider::propose_build_recipe(const BuildAssistRequest& request) const {
    const char* fixture_path = std::getenv("OMNIX_OPENAI_BUILD_PLAN_FILE");
    if (fixture_path == nullptr || *fixture_path == '\0') {
        const ProviderProbeReport probe_report = probe();
        if (probe_report.status != "ready") {
            return std::nullopt;
        }
    }
    std::vector<std::string> warnings;
    std::optional<BuildAssistPlan> plan =
        request_openai_build_plan(base_url_, api_key_, model_, request, &warnings, organization_, project_);
    if (!plan.has_value()) {
        return std::nullopt;
    }
    if (!warnings.empty()) {
        plan->safety_notes.insert(plan->safety_notes.end(), warnings.begin(), warnings.end());
    }
    return plan;
}

std::optional<RecipeAuthoringPlan> OpenAIProvider::propose_authored_recipe(const RecipeAuthoringRequest&) const {
    return std::nullopt;
}

std::optional<CommandAssistPlan> OpenAIProvider::propose_command_route(
    std::string_view prompt,
    const std::vector<std::string>& allowlisted_commands) const {
    const char* fixture_path = std::getenv("OMNIX_OPENAI_COMMAND_PLAN_FILE");
    if (fixture_path == nullptr || *fixture_path == '\0') {
        const ProviderProbeReport probe_report = probe();
        if (probe_report.status != "ready") {
            return std::nullopt;
        }
    }
    std::vector<std::string> warnings;
    std::optional<CommandAssistPlan> plan =
        request_openai_command_plan(base_url_, api_key_, model_, prompt, allowlisted_commands, &warnings, organization_, project_);
    if (!plan.has_value()) {
        return std::nullopt;
    }
    if (!warnings.empty()) {
        plan->safety_notes.insert(plan->safety_notes.end(), warnings.begin(), warnings.end());
    }
    return plan;
}

std::optional<NextStepAssistPlan> OpenAIProvider::propose_next_step(std::string_view prompt,
                                                                    std::string_view deterministic_guidance) const {
    const char* fixture_path = std::getenv("OMNIX_OPENAI_NEXT_STEP_PLAN_FILE");
    if (fixture_path == nullptr || *fixture_path == '\0') {
        const ProviderProbeReport probe_report = probe();
        if (probe_report.status != "ready") {
            return std::nullopt;
        }
    }
    std::vector<std::string> warnings;
    std::optional<NextStepAssistPlan> plan =
        request_openai_next_step_plan(base_url_, api_key_, model_, prompt, deterministic_guidance, &warnings, organization_, project_);
    if (!plan.has_value()) {
        return std::nullopt;
    }
    if (!warnings.empty()) {
        plan->warnings.insert(plan->warnings.end(), warnings.begin(), warnings.end());
        plan->metadata.warnings = plan->warnings;
    }
    return plan;
}

std::optional<CaseSummaryAssistPlan> OpenAIProvider::propose_case_summary(std::string_view target_label,
                                                                          std::string_view deterministic_summary) const {
    const char* fixture_path = std::getenv("OMNIX_OPENAI_CASE_SUMMARY_PLAN_FILE");
    if (fixture_path == nullptr || *fixture_path == '\0') {
        const ProviderProbeReport probe_report = probe();
        if (probe_report.status != "ready") {
            return std::nullopt;
        }
    }
    std::vector<std::string> warnings;
    std::optional<CaseSummaryAssistPlan> plan =
        request_openai_case_summary_plan(base_url_, api_key_, model_, target_label, deterministic_summary, &warnings, organization_, project_);
    if (!plan.has_value()) {
        return std::nullopt;
    }
    if (!warnings.empty()) {
        plan->warnings.insert(plan->warnings.end(), warnings.begin(), warnings.end());
        plan->metadata.warnings = plan->warnings;
    }
    return plan;
}

std::optional<FreeformAssistAnswer> OpenAIProvider::resolve_freeform(std::string_view prompt) const {
    const char* fixture_path = std::getenv("OMNIX_OPENAI_FREEFORM_FILE");
    if (fixture_path == nullptr || *fixture_path == '\0') {
        const ProviderProbeReport probe_report = probe();
        if (probe_report.status != "ready") {
            return std::nullopt;
        }
    }
    std::vector<std::string> warnings;
    std::optional<FreeformAssistAnswer> answer =
        request_openai_freeform_answer(base_url_, api_key_, model_, prompt, &warnings, organization_, project_);
    if (!answer.has_value()) {
        return std::nullopt;
    }
    if (!warnings.empty()) {
        answer->safety_warnings.insert(answer->safety_warnings.end(), warnings.begin(), warnings.end());
        answer->metadata.warnings = answer->safety_warnings;
    }
    return answer;
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

std::optional<RecipeAuthoringPlan> UnsupportedProvider::propose_authored_recipe(const RecipeAuthoringRequest&) const {
    return std::nullopt;
}

std::optional<CommandAssistPlan> UnsupportedProvider::propose_command_route(
    std::string_view,
    const std::vector<std::string>&) const {
    return std::nullopt;
}

std::optional<NextStepAssistPlan> UnsupportedProvider::propose_next_step(std::string_view,
                                                                         std::string_view) const {
    return std::nullopt;
}

std::optional<CaseSummaryAssistPlan> UnsupportedProvider::propose_case_summary(std::string_view,
                                                                               std::string_view) const {
    return std::nullopt;
}

std::optional<FreeformAssistAnswer> UnsupportedProvider::resolve_freeform(std::string_view) const {
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
    if (provider_id == "openai") {
        return std::make_unique<OpenAIProvider>(
            env_or_default_any({"OMNIX_OPENAI_BASE_URL", "OPENAI_BASE_URL"}, "https://api.openai.com/v1"),
            env_or_default_any({"OMNIX_OPENAI_API_KEY", "OPENAI_API_KEY"}),
            env_or_default_any({"OMNIX_OPENAI_MODEL", "OPENAI_MODEL"}),
            env_or_default_any({"OMNIX_OPENAI_ORGANIZATION", "OPENAI_ORGANIZATION"}),
            env_or_default_any({"OMNIX_OPENAI_PROJECT", "OPENAI_PROJECT"}));
    }
    return std::make_unique<UnsupportedProvider>(provider_id);
}

}  // namespace tze
