#pragma once

#include <string_view>

#include "xpp/types.hpp"

namespace xpp {

SymbolIndex build_symbol_index(const MappingUnit& unit);
const SymbolMapping* find_mapping(const SymbolIndex& index, std::string_view query);
std::string normalize_symbol(std::string_view raw_symbol);

}  // namespace xpp
