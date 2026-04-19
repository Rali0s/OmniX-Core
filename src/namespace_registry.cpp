#include "tze/namespace_registry.hpp"

namespace tze {
namespace {

std::string prefix(std::string_view label, std::string_view value) {
    return std::string(label) + ":" + std::string(value);
}

}  // namespace

std::string NamespaceRegistry::root_symbol() {
    return "x-root";
}

std::string NamespaceRegistry::base_namespace() {
    return "xBase";
}

std::string NamespaceRegistry::resolve_namespace(std::string_view name) {
    return prefix("namespace", name);
}

std::string NamespaceRegistry::temporary_namespace(std::string_view name) {
    return prefix("namespace-temp", name);
}

std::string NamespaceRegistry::persistent_namespace(std::string_view name) {
    return prefix("namespace-perm", name);
}

std::string NamespaceRegistry::core_namespace(std::string_view name) {
    return prefix("namespace-core", name);
}

std::string NamespaceRegistry::map_value(std::string_view name, std::string_view value) {
    return std::string(name) + "=" + std::string(value);
}

std::string NamespaceRegistry::store_value(std::string_view name) {
    return prefix("store", name);
}

}  // namespace tze
