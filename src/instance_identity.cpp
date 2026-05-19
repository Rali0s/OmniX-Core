#include "tze/instance_identity.hpp"

#include "tze/memory_store.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#if defined(__APPLE__) || defined(__linux__) || defined(__unix__)
#include <sys/utsname.h>
#include <unistd.h>
#endif

namespace tze {
namespace {

std::string read_file_trimmed(const std::filesystem::path& path) {
    std::ifstream input(path);
    std::string value;
    if (input) {
        std::getline(input, value);
    }
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r' || value.back() == ' ' || value.back() == '\t')) {
        value.pop_back();
    }
    return value;
}

std::string write_generated_salt(const std::filesystem::path& path) {
    std::filesystem::create_directories(path.parent_path());
    std::random_device random;
    const auto tick = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    std::ostringstream seed;
    seed << std::hex << tick;
    for (int index = 0; index < 8; ++index) {
        seed << '-' << std::setw(8) << std::setfill('0') << random();
    }
    std::ofstream output(path, std::ios::trunc);
    output << seed.str() << "\n";
    return seed.str();
}

std::string platform_name() {
#if defined(__APPLE__)
    return "macos";
#elif defined(__linux__)
    return "linux";
#elif defined(_WIN32)
    return "windows";
#else
    return "unknown";
#endif
}

std::string uname_value(bool machine) {
#if defined(__APPLE__) || defined(__linux__) || defined(__unix__)
    utsname info{};
    if (uname(&info) == 0) {
        return machine ? info.machine : info.sysname;
    }
#endif
    return {};
}

std::string host_name() {
#if defined(__APPLE__) || defined(__linux__) || defined(__unix__)
    std::array<char, 256> buffer{};
    if (gethostname(buffer.data(), buffer.size() - 1) == 0) {
        return buffer.data();
    }
#endif
    return "unknown-host";
}

std::string architecture_name() {
#if defined(__aarch64__) || defined(_M_ARM64)
    const std::string compiled = "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
    const std::string compiled = "x86_64";
#elif defined(__i386__) || defined(_M_IX86)
    const std::string compiled = "x86";
#else
    const std::string compiled = "unknown";
#endif
    const std::string runtime = uname_value(true);
    return runtime.empty() ? compiled : runtime;
}

std::uint32_t rotr(std::uint32_t value, std::uint32_t bits) {
    return (value >> bits) | (value << (32 - bits));
}

std::string sha256_hex(const std::string& input) {
    static constexpr std::array<std::uint32_t, 64> k = {
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
        0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
        0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
        0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
        0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
        0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
        0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
        0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
    };
    std::array<std::uint32_t, 8> h = {
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U,
    };

    std::vector<std::uint8_t> data(input.begin(), input.end());
    const std::uint64_t bit_len = static_cast<std::uint64_t>(data.size()) * 8U;
    data.push_back(0x80U);
    while ((data.size() % 64U) != 56U) {
        data.push_back(0U);
    }
    for (int shift = 56; shift >= 0; shift -= 8) {
        data.push_back(static_cast<std::uint8_t>((bit_len >> shift) & 0xffU));
    }

    for (std::size_t offset = 0; offset < data.size(); offset += 64) {
        std::array<std::uint32_t, 64> w{};
        for (std::size_t i = 0; i < 16; ++i) {
            const std::size_t j = offset + i * 4;
            w[i] = (static_cast<std::uint32_t>(data[j]) << 24U) |
                (static_cast<std::uint32_t>(data[j + 1]) << 16U) |
                (static_cast<std::uint32_t>(data[j + 2]) << 8U) |
                static_cast<std::uint32_t>(data[j + 3]);
        }
        for (std::size_t i = 16; i < 64; ++i) {
            const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3U);
            const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10U);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hv = h[7];
        for (std::size_t i = 0; i < 64; ++i) {
            const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const std::uint32_t ch = (e & f) ^ ((~e) & g);
            const std::uint32_t temp1 = hv + s1 + ch + k[i] + w[i];
            const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temp2 = s0 + maj;
            hv = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hv;
    }

    std::ostringstream out;
    for (std::uint32_t value : h) {
        out << std::hex << std::setw(8) << std::setfill('0') << value;
    }
    return out.str();
}

std::string short_hash(const std::string& input) {
    return sha256_hex(input).substr(0, 16);
}

}  // namespace

InstanceIdentityReport resolve_instance_identity(const std::filesystem::path& memory_root) {
    MemoryStore store;
    const MemoryPaths paths = store.resolve_paths(memory_root);
    const std::filesystem::path identity_path = paths.root / "instance_identity.json";
    const std::filesystem::path salt_path = paths.root / "instance_salt";

    std::string salt = read_file_trimmed(salt_path);
    bool generated = false;
    if (salt.empty()) {
        salt = write_generated_salt(salt_path);
        generated = true;
    }

    const std::string arch = architecture_name();
    const std::string platform = platform_name();
    const std::string host = host_name();
    const std::string kernel = uname_value(false);
    const std::string version = OMNIX_VERSION;
    const std::string seed =
        "omnix-instance-v1|arch=" + arch +
        "|platform=" + platform +
        "|kernel=" + kernel +
        "|host=" + host +
        "|version=" + version +
        "|salt=" + salt;
    const std::string fingerprint = sha256_hex(seed);

    InstanceIdentityReport report;
    report.status = "instance_identity_ready";
    report.instance_id = "omnixid-v1-" + fingerprint.substr(0, 32);
    report.fingerprint = "sha256:" + fingerprint;
    report.architecture = arch;
    report.platform = platform;
    report.host_hint_hash = "sha256:" + short_hash(host);
    report.salt_path = salt_path.string();
    report.mode = "local_software_identity";
    report.warning = "Local SGX-inspired software identity only; this is not Intel SGX, TPM, Secure Enclave, or remote hardware attestation.";
    report.generated_new_salt = generated;
    report.components = {
        "cpu_architecture",
        "platform",
        "kernel_family",
        "host_hint_hash",
        "omnix_version",
        "local_instance_salt",
    };

    std::ofstream output(identity_path, std::ios::trunc);
    output << "{\n"
           << "  \"event_type\": \"omnix.instance_identity.v1\",\n"
           << "  \"status\": \"" << report.status << "\",\n"
           << "  \"instance_id\": \"" << report.instance_id << "\",\n"
           << "  \"fingerprint\": \"" << report.fingerprint << "\",\n"
           << "  \"architecture\": \"" << report.architecture << "\",\n"
           << "  \"platform\": \"" << report.platform << "\",\n"
           << "  \"host_hint_hash\": \"" << report.host_hint_hash << "\",\n"
           << "  \"mode\": \"" << report.mode << "\",\n"
           << "  \"warning\": \"" << report.warning << "\"\n"
           << "}\n";
    return report;
}

}  // namespace tze
