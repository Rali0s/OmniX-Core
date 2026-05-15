#pragma once

#include <cstddef>
#include <optional>
#include <string>

#include "tze/types.hpp"

namespace tze {

class PacketCaptureEngine {
public:
    PacketCaptureReport doctor(const PacketCaptureRequest& request = {}) const;
    PacketCaptureReport capture(const PacketCaptureRequest& request) const;
    PacketCaptureReport read_pcap(const PacketCaptureRequest& request) const;
    bool export_jsonl(const PacketCaptureReport& report, const std::string& path, std::string* error = nullptr) const;

    static std::optional<PacketRecord> parse_packet_bytes(const unsigned char* bytes,
                                                          std::size_t length,
                                                          int link_type,
                                                          std::size_t payload_cap,
                                                          std::string timestamp = {});
};

}  // namespace tze
