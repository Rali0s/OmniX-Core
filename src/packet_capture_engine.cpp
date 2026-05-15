#include "tze/packet_capture_engine.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#if defined(__APPLE__) || defined(__linux__) || defined(__unix__)
#include <unistd.h>
#endif

#ifdef OMNIX_HAS_LIBPCAP
#include <pcap/pcap.h>
#endif

namespace tze {
namespace {

constexpr int kLinkEthernet = 1;
constexpr int kLinkNull = 0;
constexpr int kLinkLoop = 108;
constexpr int kLinkRaw = 12;

std::string default_loopback_interface() {
#if defined(__APPLE__)
    return "lo0";
#else
    return "lo";
#endif
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

std::string ipv4_string(const unsigned char* bytes) {
    std::ostringstream out;
    out << static_cast<int>(bytes[0]) << "."
        << static_cast<int>(bytes[1]) << "."
        << static_cast<int>(bytes[2]) << "."
        << static_cast<int>(bytes[3]);
    return out.str();
}

std::uint16_t read_be16(const unsigned char* bytes) {
    return static_cast<std::uint16_t>((static_cast<unsigned int>(bytes[0]) << 8) |
                                      static_cast<unsigned int>(bytes[1]));
}

std::uint32_t read_u32(const unsigned char* bytes, bool swapped) {
    if (swapped) {
        return (static_cast<std::uint32_t>(bytes[3]) << 24) |
               (static_cast<std::uint32_t>(bytes[2]) << 16) |
               (static_cast<std::uint32_t>(bytes[1]) << 8) |
               static_cast<std::uint32_t>(bytes[0]);
    }
    return (static_cast<std::uint32_t>(bytes[0]) << 24) |
           (static_cast<std::uint32_t>(bytes[1]) << 16) |
           (static_cast<std::uint32_t>(bytes[2]) << 8) |
           static_cast<std::uint32_t>(bytes[3]);
}

std::string flags_string(unsigned char flags) {
    std::string out;
    if ((flags & 0x02) != 0) {
        out += "S";
    }
    if ((flags & 0x10) != 0) {
        out += "A";
    }
    if ((flags & 0x01) != 0) {
        out += "F";
    }
    if ((flags & 0x04) != 0) {
        out += "R";
    }
    if ((flags & 0x08) != 0) {
        out += "P";
    }
    if ((flags & 0x20) != 0) {
        out += "U";
    }
    return out.empty() ? "-" : out;
}

std::string hex_preview(const unsigned char* bytes, std::size_t length, std::size_t cap) {
    const std::size_t limit = std::min(length, cap);
    std::ostringstream out;
    for (std::size_t i = 0; i < limit; ++i) {
        if (i != 0) {
            out << ' ';
        }
        out << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(bytes[i]);
    }
    if (length > limit) {
        out << " ...";
    }
    return out.str();
}

std::string ascii_preview(const unsigned char* bytes, std::size_t length, std::size_t cap) {
    const std::size_t limit = std::min(length, cap);
    std::string out;
    out.reserve(limit + 4);
    for (std::size_t i = 0; i < limit; ++i) {
        const unsigned char c = bytes[i];
        out.push_back(std::isprint(c) ? static_cast<char>(c) : '.');
    }
    if (length > limit) {
        out += "...";
    }
    return out;
}

std::string escape_json(std::string_view value) {
    std::ostringstream out;
    for (unsigned char c : value) {
        switch (c) {
            case '\\':
                out << "\\\\";
                break;
            case '"':
                out << "\\\"";
                break;
            case '\n':
                out << "\\n";
                break;
            case '\r':
                out << "\\r";
                break;
            case '\t':
                out << "\\t";
                break;
            default:
                if (c < 0x20) {
                    out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(c)
                        << std::dec << std::setfill(' ');
                } else {
                    out << static_cast<char>(c);
                }
                break;
        }
    }
    return out.str();
}

bool valid_utf8_sequence(const unsigned char* bytes, std::size_t length, std::size_t index, std::size_t* width) {
    const unsigned char c = bytes[index];
    if (c < 0x80) {
        *width = 1;
        return true;
    }
    if ((c & 0xe0) == 0xc0 && index + 1 < length && (bytes[index + 1] & 0xc0) == 0x80 && c >= 0xc2) {
        *width = 2;
        return true;
    }
    if ((c & 0xf0) == 0xe0 && index + 2 < length &&
        (bytes[index + 1] & 0xc0) == 0x80 && (bytes[index + 2] & 0xc0) == 0x80) {
        *width = 3;
        return true;
    }
    if ((c & 0xf8) == 0xf0 && index + 3 < length &&
        (bytes[index + 1] & 0xc0) == 0x80 && (bytes[index + 2] & 0xc0) == 0x80 &&
        (bytes[index + 3] & 0xc0) == 0x80 && c <= 0xf4) {
        *width = 4;
        return true;
    }
    return false;
}

std::pair<std::string, std::string> utf8_text_preview(const unsigned char* bytes, std::size_t length, std::size_t cap) {
    if (length == 0) {
        return {"", "empty"};
    }
    const std::size_t limit = std::min(length, cap);
    std::string out;
    out.reserve(limit + 16);
    std::size_t readable = 0;
    std::size_t inspected = 0;
    bool invalid = false;
    for (std::size_t i = 0; i < limit;) {
        std::size_t width = 1;
        const unsigned char c = bytes[i];
        if (c == '\r') {
            out += "\\r";
            ++readable;
            ++inspected;
            ++i;
            continue;
        }
        if (c == '\n') {
            out += "\\n";
            ++readable;
            ++inspected;
            ++i;
            continue;
        }
        if (c == '\t') {
            out += "\\t";
            ++readable;
            ++inspected;
            ++i;
            continue;
        }
        if (c < 0x80) {
            out.push_back(std::isprint(c) ? static_cast<char>(c) : '.');
            readable += std::isprint(c) ? 1 : 0;
            ++inspected;
            ++i;
            continue;
        }
        if (valid_utf8_sequence(bytes, limit, i, &width)) {
            out.append(reinterpret_cast<const char*>(bytes + i), width);
            readable += width;
            inspected += width;
            i += width;
            continue;
        }
        invalid = true;
        out.push_back('.');
        ++inspected;
        ++i;
    }
    if (length > limit) {
        out += "...";
    }
    const double ratio = inspected == 0 ? 0.0 : static_cast<double>(readable) / static_cast<double>(inspected);
    if (ratio < 0.70 || invalid) {
        return {out, length > limit ? "binary_or_mixed_truncated" : "binary_or_mixed"};
    }
    return {out, length > limit ? "text_utf8_truncated" : "text_utf8"};
}

bool starts_with_any_http_method(std::string_view payload) {
    static constexpr std::array<std::string_view, 9> kPrefixes = {
        "GET ", "POST ", "PUT ", "PATCH ", "DELETE ", "HEAD ", "OPTIONS ", "TRACE ", "HTTP/1."
    };
    return std::any_of(kPrefixes.begin(), kPrefixes.end(), [&](std::string_view prefix) {
        return payload.substr(0, prefix.size()) == prefix;
    });
}

std::vector<std::string> decode_plaintext(const unsigned char* bytes, std::size_t length) {
    std::vector<std::string> decoded;
    if (length >= 3 && (bytes[0] == 0x14 || bytes[0] == 0x15 || bytes[0] == 0x16 || bytes[0] == 0x17) &&
        bytes[1] == 0x03 && bytes[2] <= 0x04) {
        decoded.push_back("tls_or_encrypted");
        return decoded;
    }

    std::string text;
    text.reserve(std::min<std::size_t>(length, 512));
    bool mostly_text = true;
    const std::size_t inspect = std::min<std::size_t>(length, 512);
    for (std::size_t i = 0; i < inspect; ++i) {
        const unsigned char c = bytes[i];
        if (c == '\r' || c == '\n' || c == '\t' || std::isprint(c)) {
            text.push_back(static_cast<char>(c));
        } else {
            mostly_text = false;
            break;
        }
    }
    if (!mostly_text || !starts_with_any_http_method(text)) {
        return decoded;
    }

    std::istringstream lines(text);
    for (std::string line; std::getline(lines, line) && decoded.size() < 8;) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        line = trim(line);
        if (line.empty()) {
            break;
        }
        decoded.push_back(line);
    }
    return decoded;
}

std::optional<std::size_t> ip_offset_for(int link_type, const unsigned char* bytes, std::size_t length) {
    if (link_type == kLinkRaw) {
        return 0;
    }
    if (link_type == kLinkNull || link_type == kLinkLoop) {
        return length >= 4 ? std::optional<std::size_t>{4} : std::nullopt;
    }
    if (link_type == kLinkEthernet) {
        if (length < 14) {
            return std::nullopt;
        }
        const std::uint16_t ethertype = read_be16(bytes + 12);
        return ethertype == 0x0800 ? std::optional<std::size_t>{14} : std::nullopt;
    }
    return std::nullopt;
}

std::string build_filter(const PacketCaptureRequest& request) {
    std::ostringstream filter;
    filter << "tcp";
    if (request.port > 0) {
        filter << " and port " << request.port;
    }
    if (!request.host_filter.empty()) {
        filter << " and host " << request.host_filter;
    }
    return filter.str();
}

void add_flow_summaries(PacketCaptureReport& report) {
    std::map<std::string, std::pair<std::size_t, std::size_t>> flows;
    for (const PacketRecord& packet : report.packets) {
        std::ostringstream key;
        key << packet.src_ip << ":" << packet.src_port << " -> "
            << packet.dst_ip << ":" << packet.dst_port;
        auto& entry = flows[key.str()];
        entry.first += 1;
        entry.second += packet.payload_length;
    }
    for (const auto& [flow, counts] : flows) {
        std::ostringstream line;
        line << flow << " packets=" << counts.first << " payload_bytes=" << counts.second;
        report.flow_summary.push_back(line.str());
    }
}

std::string analysis_code_for(const PacketRecord& record) {
    if (record.classification == "tcp_control") {
        return "NET.TCP.CONTROL";
    }
    if (record.classification == "plaintext_http") {
        return "NET.TCP.HTTP_PLAINTEXT";
    }
    if (record.classification == "tls_or_encrypted") {
        return "NET.TCP.TLS_OPAQUE";
    }
    if (record.classification == "text_utf8") {
        return "NET.TCP.TEXT_UTF8";
    }
    if (record.classification == "parse_error") {
        return "NET.TCP.PARSE_ERROR";
    }
    return "NET.TCP.OPAQUE_PAYLOAD";
}

bool bpf_readable() {
#if defined(__APPLE__)
    std::error_code ec;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator("/dev", ec)) {
        if (ec) {
            break;
        }
        const std::string name = entry.path().filename().string();
        if (name.rfind("bpf", 0) == 0 && access(entry.path().c_str(), R_OK) == 0) {
            return true;
        }
    }
    return false;
#else
    return geteuid() == 0;
#endif
}

std::vector<std::string> known_interfaces() {
    std::vector<std::string> interfaces;
#ifdef OMNIX_HAS_LIBPCAP
    char errbuf[PCAP_ERRBUF_SIZE]{};
    pcap_if_t* alldevs = nullptr;
    if (pcap_findalldevs(&alldevs, errbuf) == 0 && alldevs != nullptr) {
        for (pcap_if_t* cursor = alldevs; cursor != nullptr; cursor = cursor->next) {
            if (cursor->name != nullptr) {
                interfaces.emplace_back(cursor->name);
            }
        }
        pcap_freealldevs(alldevs);
    }
#endif
    if (interfaces.empty()) {
        interfaces.push_back(default_loopback_interface());
    }
    return interfaces;
}

PacketCaptureReport unavailable_report(std::string mode) {
    PacketCaptureReport report;
    report.mode = std::move(mode);
    report.status = "capture_unavailable";
    report.summary = "Native libpcap support was not available when OmniX was built.";
    report.warnings.push_back("Install libpcap development headers, reconfigure CMake, and rebuild OmniX.");
    return report;
}

#ifdef OMNIX_HAS_LIBPCAP
struct LiveCaptureState {
    PacketCaptureReport* report = nullptr;
    std::size_t payload_cap = 96;
    int link_type = kLinkEthernet;
};

void capture_callback(unsigned char* user, const pcap_pkthdr* header, const unsigned char* bytes) {
    auto* state = reinterpret_cast<LiveCaptureState*>(user);
    if (state == nullptr || state->report == nullptr || header == nullptr || bytes == nullptr) {
        return;
    }
    std::ostringstream timestamp;
    timestamp << header->ts.tv_sec << "." << std::setw(6) << std::setfill('0') << header->ts.tv_usec;
    std::optional<PacketRecord> record =
        PacketCaptureEngine::parse_packet_bytes(bytes, header->caplen, state->link_type, state->payload_cap, timestamp.str());
    if (record.has_value()) {
        record->captured_length = header->caplen;
        record->original_length = header->len;
        state->report->packets.push_back(std::move(*record));
    }
}
#endif

}  // namespace

std::optional<PacketRecord> PacketCaptureEngine::parse_packet_bytes(const unsigned char* bytes,
                                                                    std::size_t length,
                                                                    int link_type,
                                                                    std::size_t payload_cap,
                                                                    std::string timestamp) {
    const std::optional<std::size_t> ip_offset = ip_offset_for(link_type, bytes, length);
    if (!ip_offset.has_value() || *ip_offset + 20 > length) {
        return std::nullopt;
    }
    const unsigned char* ip = bytes + *ip_offset;
    if ((ip[0] >> 4) != 4) {
        return std::nullopt;
    }
    const std::size_t ip_header_len = static_cast<std::size_t>(ip[0] & 0x0f) * 4;
    if (ip_header_len < 20 || *ip_offset + ip_header_len + 20 > length || ip[9] != 6) {
        return std::nullopt;
    }

    const std::size_t total_len = read_be16(ip + 2);
    const std::size_t tcp_offset = *ip_offset + ip_header_len;
    const unsigned char* tcp = bytes + tcp_offset;
    const std::size_t tcp_header_len = static_cast<std::size_t>(tcp[12] >> 4) * 4;
    if (tcp_header_len < 20 || tcp_offset + tcp_header_len > length ||
        total_len < ip_header_len + tcp_header_len) {
        return std::nullopt;
    }

    const std::size_t payload_offset = tcp_offset + tcp_header_len;
    const std::size_t ip_payload_len = total_len - ip_header_len - tcp_header_len;
    const std::size_t captured_payload_len = payload_offset < length
        ? std::min(ip_payload_len, length - payload_offset)
        : 0;
    const unsigned char* payload = bytes + payload_offset;

    PacketRecord record;
    record.timestamp = std::move(timestamp);
    record.src_ip = ipv4_string(ip + 12);
    record.dst_ip = ipv4_string(ip + 16);
    record.src_port = read_be16(tcp);
    record.dst_port = read_be16(tcp + 2);
    record.tcp_flags = flags_string(tcp[13]);
    record.captured_length = length;
    record.original_length = length;
    record.payload_length = ip_payload_len;
    if (captured_payload_len > 0) {
        record.hex_preview = hex_preview(payload, captured_payload_len, payload_cap);
        record.ascii_preview = ascii_preview(payload, captured_payload_len, payload_cap);
        const auto [text_preview, text_status] = utf8_text_preview(payload, captured_payload_len, payload_cap);
        record.payload_text_utf8 = text_preview;
        record.payload_text_status = text_status;
        record.plaintext_decode = decode_plaintext(payload, captured_payload_len);
        if (!record.plaintext_decode.empty() && record.plaintext_decode.front() == "tls_or_encrypted") {
            record.classification = "tls_or_encrypted";
        } else if (!record.plaintext_decode.empty()) {
            record.classification = "plaintext_http";
        } else if (record.payload_text_status == "text_utf8" || record.payload_text_status == "text_utf8_truncated") {
            record.classification = "text_utf8";
        }
    }
    if (record.classification.empty()) {
        record.classification = captured_payload_len == 0 ? "tcp_control" : "opaque_payload";
    }
    record.analysis_code = analysis_code_for(record);
    return record;
}

PacketCaptureReport PacketCaptureEngine::doctor(const PacketCaptureRequest& request) const {
    PacketCaptureReport report;
    report.mode = "doctor";
    report.interface_name = request.interface_name.empty() ? default_loopback_interface() : request.interface_name;
    report.interfaces = known_interfaces();
#ifdef OMNIX_HAS_LIBPCAP
    report.status = bpf_readable() ? "capture_ready" : "capture_attention_needed";
    report.summary = bpf_readable()
        ? "libpcap is available and the current process appears able to read a capture device."
        : "libpcap is available, but live capture may require manual packet-capture permissions.";
#else
    report.status = "capture_unavailable";
    report.summary = "Native libpcap support was not available when OmniX was built.";
#endif

#if defined(__APPLE__)
    report.privilege_diagnostics.push_back(bpf_readable()
        ? "macOS BPF device readable by this process."
        : "macOS BPF devices are not readable by this process; run live capture from an appropriately privileged shell or adjust BPF permissions manually.");
    report.privilege_diagnostics.push_back("Default loopback interface: lo0.");
#elif defined(__linux__)
    report.privilege_diagnostics.push_back(geteuid() == 0
        ? "Running as root; live capture should be permitted if the interface exists."
        : "Not running as root; live capture usually needs CAP_NET_RAW/CAP_NET_ADMIN or manual tcpdump/libpcap permissions.");
    report.privilege_diagnostics.push_back("Default loopback interface: lo.");
#else
    report.privilege_diagnostics.push_back("Packet-capture permission diagnostics are not specialized for this platform.");
#endif
    report.warnings.push_back("OmniX never invokes sudo or changes capture-device permissions automatically.");
    return report;
}

PacketCaptureReport PacketCaptureEngine::capture(const PacketCaptureRequest& request) const {
#ifndef OMNIX_HAS_LIBPCAP
    return unavailable_report("live");
#else
    PacketCaptureReport report;
    report.mode = "live";
    report.interface_name = request.interface_name.empty() ? default_loopback_interface() : request.interface_name;
    report.export_path = request.export_path;
    report.filter = build_filter(request);
    const std::size_t max_packets = request.packet_count == 0 ? 10 : request.packet_count;
    const std::size_t max_seconds = request.seconds == 0 ? 5 : request.seconds;
    const std::size_t payload_cap = request.payload_bytes == 0 ? 96 : request.payload_bytes;

    char errbuf[PCAP_ERRBUF_SIZE]{};
    pcap_t* handle = pcap_open_live(report.interface_name.c_str(), 65535, 0, 250, errbuf);
    if (handle == nullptr) {
        report.status = "capture_blocked";
        report.summary = std::string("Unable to open live capture on `") + report.interface_name + "`.";
        report.privilege_diagnostics.push_back(errbuf);
        report.privilege_diagnostics.push_back("Run `omnix tview doctor` for manual permission guidance.");
        return report;
    }

    bpf_program program{};
    if (pcap_compile(handle, &program, report.filter.c_str(), 1, PCAP_NETMASK_UNKNOWN) != 0) {
        report.status = "capture_filter_failed";
        report.summary = "Unable to compile the requested packet filter.";
        report.warnings.push_back(pcap_geterr(handle));
        pcap_close(handle);
        return report;
    }
    if (pcap_setfilter(handle, &program) != 0) {
        report.status = "capture_filter_failed";
        report.summary = "Unable to install the requested packet filter.";
        report.warnings.push_back(pcap_geterr(handle));
        pcap_freecode(&program);
        pcap_close(handle);
        return report;
    }
    pcap_freecode(&program);

    LiveCaptureState state{&report, payload_cap, pcap_datalink(handle)};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(max_seconds);
    while (report.packets.size() < max_packets && std::chrono::steady_clock::now() < deadline) {
        const int remaining = static_cast<int>(max_packets - report.packets.size());
        const int dispatched = pcap_dispatch(handle, remaining, capture_callback, reinterpret_cast<unsigned char*>(&state));
        if (dispatched < 0) {
            report.status = "capture_failed";
            report.summary = "libpcap returned an error during live capture.";
            report.warnings.push_back(pcap_geterr(handle));
            pcap_close(handle);
            return report;
        }
    }
    pcap_close(handle);

    report.packet_count = report.packets.size();
    add_flow_summaries(report);
    report.status = report.packet_count == 0 ? "capture_empty" : "capture_complete";
    report.summary = report.packet_count == 0
        ? "No matching TCP packets were observed before the capture window ended."
        : "Captured matching TCP packets with bounded payload previews.";
    return report;
#endif
}

PacketCaptureReport PacketCaptureEngine::read_pcap(const PacketCaptureRequest& request) const {
    PacketCaptureReport report;
    report.mode = "pcap";
    report.pcap_path = request.pcap_path;
    report.export_path = request.export_path;
    report.filter = build_filter(request);
    if (request.pcap_path.empty()) {
        report.status = "capture_invalid_request";
        report.summary = "A pcap file path is required.";
        return report;
    }
    std::ifstream input(request.pcap_path, std::ios::binary);
    if (!input) {
        report.status = "capture_file_missing";
        report.summary = "Unable to open the requested pcap file.";
        return report;
    }

    std::array<unsigned char, 24> global{};
    input.read(reinterpret_cast<char*>(global.data()), static_cast<std::streamsize>(global.size()));
    if (input.gcount() != static_cast<std::streamsize>(global.size())) {
        report.status = "capture_parse_failed";
        report.summary = "The pcap file is too small to contain a global header.";
        return report;
    }

    const std::uint32_t magic = read_u32(global.data(), false);
    bool swapped = false;
    if (magic == 0xa1b2c3d4 || magic == 0xa1b23c4d) {
        swapped = false;
    } else if (magic == 0xd4c3b2a1 || magic == 0x4d3cb2a1) {
        swapped = true;
    } else {
        report.status = "capture_parse_failed";
        report.summary = "The pcap file does not have a recognized magic header.";
        return report;
    }
    const int link_type = static_cast<int>(read_u32(global.data() + 20, swapped));
    const std::size_t max_packets = request.packet_count == 0 ? static_cast<std::size_t>(1000) : request.packet_count;
    const std::size_t payload_cap = request.payload_bytes == 0 ? 96 : request.payload_bytes;

    while (input && report.packets.size() < max_packets) {
        std::array<unsigned char, 16> header{};
        input.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
        if (input.gcount() == 0) {
            break;
        }
        if (input.gcount() != static_cast<std::streamsize>(header.size())) {
            report.warnings.push_back("Encountered a truncated packet header at end of file.");
            break;
        }
        const std::uint32_t ts_sec = read_u32(header.data(), swapped);
        const std::uint32_t ts_subsec = read_u32(header.data() + 4, swapped);
        const std::uint32_t incl_len = read_u32(header.data() + 8, swapped);
        const std::uint32_t orig_len = read_u32(header.data() + 12, swapped);
        std::vector<unsigned char> bytes(incl_len);
        input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (input.gcount() != static_cast<std::streamsize>(bytes.size())) {
            report.warnings.push_back("Encountered a truncated packet body at end of file.");
            break;
        }
        std::ostringstream timestamp;
        timestamp << ts_sec << "." << std::setw(6) << std::setfill('0') << ts_subsec;
        std::optional<PacketRecord> record =
            parse_packet_bytes(bytes.data(), bytes.size(), link_type, payload_cap, timestamp.str());
        if (!record.has_value()) {
            continue;
        }
        record->captured_length = incl_len;
        record->original_length = orig_len;
        if (request.port > 0 && record->src_port != request.port && record->dst_port != request.port) {
            continue;
        }
        report.packets.push_back(std::move(*record));
    }

    report.packet_count = report.packets.size();
    add_flow_summaries(report);
    report.status = report.packet_count == 0 ? "capture_empty" : "capture_complete";
    report.summary = report.packet_count == 0
        ? "No matching TCP packets were found in the pcap file."
        : "Read matching TCP packets from the pcap file with bounded payload previews.";
    return report;
}

bool PacketCaptureEngine::export_jsonl(const PacketCaptureReport& report,
                                       const std::string& path,
                                       std::string* error) const {
    if (path.empty()) {
        if (error != nullptr) {
            *error = "No export path was provided.";
        }
        return false;
    }
    std::ofstream out(path);
    if (!out) {
        if (error != nullptr) {
            *error = "Unable to open TView JSONL export path `" + path + "`.";
        }
        return false;
    }
    for (const PacketRecord& packet : report.packets) {
        out << "{\"event_type\":\"omnix.tview.packet.v1\""
            << ",\"timestamp\":\"" << escape_json(packet.timestamp) << "\""
            << ",\"mode\":\"" << escape_json(report.mode) << "\""
            << ",\"interface\":\"" << escape_json(report.interface_name) << "\""
            << ",\"pcap_path\":\"" << escape_json(report.pcap_path) << "\""
            << ",\"filter\":\"" << escape_json(report.filter) << "\""
            << ",\"src_ip\":\"" << escape_json(packet.src_ip) << "\""
            << ",\"src_port\":" << packet.src_port
            << ",\"dst_ip\":\"" << escape_json(packet.dst_ip) << "\""
            << ",\"dst_port\":" << packet.dst_port
            << ",\"tcp_flags\":\"" << escape_json(packet.tcp_flags) << "\""
            << ",\"captured_length\":" << packet.captured_length
            << ",\"original_length\":" << packet.original_length
            << ",\"payload_length\":" << packet.payload_length
            << ",\"classification\":\"" << escape_json(packet.classification) << "\""
            << ",\"analysis_code\":\"" << escape_json(packet.analysis_code) << "\""
            << ",\"payload_text_status\":\"" << escape_json(packet.payload_text_status) << "\""
            << ",\"payload_text_utf8\":\"" << escape_json(packet.payload_text_utf8) << "\""
            << ",\"ascii_preview\":\"" << escape_json(packet.ascii_preview) << "\""
            << ",\"hex_preview\":\"" << escape_json(packet.hex_preview) << "\""
            << ",\"decode_lines\":[";
        for (std::size_t index = 0; index < packet.plaintext_decode.size(); ++index) {
            if (index != 0) {
                out << ",";
            }
            out << "\"" << escape_json(packet.plaintext_decode[index]) << "\"";
        }
        out << "]}\n";
    }
    return true;
}

}  // namespace tze
