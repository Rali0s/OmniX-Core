#include "tze/omni_bridge.hpp"

#include <string>
#include <vector>

namespace tze {
namespace {

std::string prefix(std::string_view label, std::string_view value) {
    return std::string(label) + ":" + std::string(value);
}

}  // namespace

std::string OmniBridge::active_ping_placeholder(std::string_view target) {
    return prefix("active-ping", target);
}

std::string OmniBridge::index_context(std::string_view label) {
    return prefix("index", label);
}

std::string OmniBridge::map_context(std::string_view label) {
    return prefix("map", label);
}

std::string OmniBridge::seek_records(std::string_view label) {
    return prefix("seek", label);
}

std::string OmniBridge::contextualize_cache(std::string_view label) {
    return prefix("context-cache", label);
}

std::string OmniBridge::host_link(std::string_view label) {
    return prefix("host", label);
}

std::string OmniBridge::load_native_os_bound(std::string_view label) {
    return prefix("load-native-os", label);
}

std::string OmniBridge::match_context(std::string_view label) {
    return prefix("match", label);
}

std::string OmniBridge::permit_mode(std::string_view label) {
    return prefix("permit", label);
}

std::string OmniBridge::read_context(std::string_view label) {
    return prefix("read", label);
}

std::string OmniBridge::request_operation(std::string_view label) {
    return prefix("request", label);
}

std::string OmniBridge::detect_patterns(std::string_view label) {
    return prefix("detect", label);
}

std::string OmniBridge::map_records(std::string_view label) {
    return prefix("map-records", label);
}

std::string OmniBridge::ccd_test_placeholder(std::string_view label) {
    return prefix("ccd-test", label);
}

std::string OmniBridge::omnibase() {
    return "xXOmniBase";
}

std::string OmniBridge::x2_link(std::string_view label) {
    return prefix("x2", label);
}

std::vector<std::string> OmniBridge::running_process_list() {
    return {"omnix", "cmake", "c++"};
}

std::string OmniBridge::pid_temp_location() {
    return "pid-temp-location";
}

std::string OmniBridge::binary_decompressor() {
    return "binary-decompressor";
}

std::string OmniBridge::compression_algo_classes() {
    return "native-compression-algorithms";
}

std::string OmniBridge::access_search(std::string_view label) {
    return prefix("search", label);
}

std::string OmniBridge::access_compare(std::string_view lhs, std::string_view rhs) {
    return std::string(lhs) + "<=>" + std::string(rhs);
}

}  // namespace tze
