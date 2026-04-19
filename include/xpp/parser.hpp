#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "xpp/types.hpp"

namespace xpp {

MappingUnit parse_pseudo(std::string_view source, std::string source_name, PseudoDialect dialect);
MappingUnit parse_xpp(std::string_view source);
MappingUnit parse_xpp(std::string_view source, std::string source_name);
std::string read_text_file(const std::filesystem::path& path);

}  // namespace xpp
