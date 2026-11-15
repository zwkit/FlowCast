// FlowCast Windows client - mDNS/DNS-SD service browser (Winsock, no Bonjour).
//
// Windows has no system-wide DNS-SD API that is guaranteed present, and the
// client must stay a single dependency-free binary, so this is a small
// DNS message parser driving one multicast UDP socket.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace fc {

struct MdnsServiceInfo {
    std::string instance; // "<name>._airplay._tcp.local"
    std::string service;  // "_airplay._tcp"
    std::string host;     // SRV target, e.g. "airplay-a1b2c3.local"
    std::string address;  // first A record seen (IPv4 dotted quad)
    uint16_t port = 0;
    std::vector<std::pair<std::string, std::string>> txt;

    const std::string* txtFind(const char* key) const {
        for (const auto& kv : txt) {
            if (kv.first == key) return &kv.second;
        }
        return nullptr;
    }
};

using MdnsFoundFn = void (*)(const MdnsServiceInfo& info, void* user);

class MdnsBrowser {
public:
    MdnsBrowser() = default;
    ~MdnsBrowser();

    // Browses `serviceType` (e.g. "_airplay._tcp") for up to `timeoutMs`.
    // Returns false if the socket could not be set up; discovered services are
    // delivered through `cb` on the calling thread.
    bool start(const char* serviceType, MdnsFoundFn cb, void* user, int timeoutMs);
    void stop();

private:
    struct Impl;
    Impl* m_impl = nullptr;
};

} // namespace fc
