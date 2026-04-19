#include "tze/io_runtime.hpp"

namespace tze {
namespace {

std::string prefix(std::string_view channel, std::string_view value) {
    return std::string(channel) + ":" + std::string(value);
}

}  // namespace

std::string IoRuntime::write_output(std::string_view text) {
    return prefix("stdout", text);
}

std::string IoRuntime::write_error(std::string_view text) {
    return prefix("stderr", text);
}

std::string IoRuntime::write_emergency(std::string_view text) {
    return prefix("emergency", text);
}

std::string IoRuntime::display_text(std::string_view text) {
    return std::string(text);
}

std::string IoRuntime::prompt_input(std::string_view prompt) {
    return prefix("prompt", prompt);
}

std::string IoRuntime::read_input(std::string_view channel) {
    return prefix("input", channel);
}

std::string IoRuntime::current_value() {
    return "current-value";
}

std::string IoRuntime::translate_text(std::string_view text) {
    return prefix("translate", text);
}

std::string IoRuntime::report_channel(std::string_view label) {
    return prefix("report", label);
}

}  // namespace tze
