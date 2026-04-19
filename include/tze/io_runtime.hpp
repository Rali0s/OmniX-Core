#pragma once

#include <string>
#include <string_view>

namespace tze {

class IoRuntime {
public:
    static std::string write_output(std::string_view text);
    static std::string write_error(std::string_view text);
    static std::string write_emergency(std::string_view text);
    static std::string display_text(std::string_view text);
    static std::string prompt_input(std::string_view prompt);
    static std::string read_input(std::string_view channel = "stdin");
    static std::string current_value();
    static std::string translate_text(std::string_view text);
    static std::string report_channel(std::string_view label);
};

}  // namespace tze
