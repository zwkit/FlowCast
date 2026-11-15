// FlowCast Windows client - mDNS/DNS-SD browser implementation.
#include "mdns.h"
#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include <cstring>
#include <map>
#include <string>
#include <vector>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")

namespace fc {
namespace {

constexpr uint16_t kMdnsPort = 5353;
constexpr const char* kMdnsGroup = "224.0.0.251";

// DNS record types we care about.
constexpr uint16_t kA = 1;
constexpr uint16_t kPtr = 12;
constexpr uint16_t kTxt = 16;
constexpr uint16_t kAaaa = 28;
constexpr uint16_t kSrv = 33;

struct Reader {
    const uint8_t* base;
    size_t len;
    size_t pos = 0;

    bool u8(uint8_t& v) {
        if (pos + 1 > len) return false;
        v = base[pos++];
        return true;
    }
    bool u16(uint16_t& v) {
        if (pos + 2 > len) return false;
        v = (uint16_t)((base[pos] << 8) | base[pos + 1]);
        pos += 2;
        return true;
    }
    bool u32(uint32_t& v) {
        if (pos + 4 > len) return false;
        v = ((uint32_t)base[pos] << 24) | ((uint32_t)base[pos + 1] << 16) |
            ((uint32_t)base[pos + 2] << 8) | (uint32_t)base[pos + 3];
        pos += 4;
        return true;
    }
    bool skip(size_t n) {
        if (pos + n > len) return false;
        pos += n;
        return true;
    }
};

// Reads a possibly-compressed DNS name. Returns the lowercase dotted form and
// advances `pos` past the name in the *current* position only.
bool readName(Reader& r, std::string& out) {
    out.clear();
    size_t jumps = 0;
    size_t nextPos = 0; // where to resume after following a pointer
    while (true) {
        if (r.pos >= r.len) return false;
        uint8_t l = r.base[r.pos];
        if ((l & 0xc0) == 0xc0) {
            if (r.pos + 1 >= r.len) return false;
            size_t ptr = (size_t)(((l & 0x3f) << 8) | r.base[r.pos + 1]);
            if (nextPos == 0) nextPos = r.pos + 2;
            if (++jumps > 64) return false; // compression loop guard
            r.pos = ptr;
            continue;
        }
        if ((l & 0xc0) != 0) return false; // invalid label type
        r.pos++;
        if (l == 0) {
            if (nextPos != 0) r.pos = nextPos;
            while (!out.empty() && out.back() == '.') out.pop_back();
            return true;
        }
        if (!out.empty()) out += '.';
        if (r.pos + l > r.len) return false;
        for (size_t i = 0; i < l; i++) {
            char c = (char)r.base[r.pos + i];
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            out += c;
        }
        r.pos += l;
    }
}

void encodeName(const std::string& name, std::vector<uint8_t>& out) {
    size_t start = 0;
    while (start < name.size()) {
        size_t dot = name.find('.', start);
        if (dot == std::string::npos) dot = name.size();
        size_t l = dot - start;
        if (l == 0 || l > 63) { start = dot + 1; continue; }
        out.push_back((uint8_t)l);
        for (size_t i = start; i < dot; i++) out.push_back((uint8_t)name[i]);
        start = dot + 1;
    }
    out.push_back(0);
}

struct Pending {
    MdnsServiceInfo info;
    bool hasSrv = false;
};

// Every usable IPv4 address on this machine.
//
// A multicast send with no IP_MULTICAST_IF follows the routing table, which on a
// machine with a VPN or virtual adapter can pick an interface the receiver is
// not on -- the query then leaves through the wrong NIC and nothing answers.
// Sending once per interface removes that guesswork.
std::vector<in_addr> localIpv4Addresses() {
    std::vector<in_addr> out;
    ULONG size = 16 * 1024;
    std::vector<uint8_t> storage(size);
    auto* addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG rc = GetAdaptersAddresses(AF_INET, flags, nullptr, addrs, &size);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        storage.resize(size);
        addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());
        rc = GetAdaptersAddresses(AF_INET, flags, nullptr, addrs, &size);
    }
    if (rc != NO_ERROR) {
        LOGW("mdns: GetAdaptersAddresses failed (%lu), falling back to the default route", rc);
        return out;
    }
    for (auto* a = addrs; a; a = a->Next) {
        // Skip down links and the loopback pseudo-adapter; anything else with a
        // unicast IPv4 address is worth a query.
        if (a->OperStatus != IfOperStatusUp) continue;
        if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        if (a->Flags & IP_ADAPTER_NO_MULTICAST) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            if (!u->Address.lpSockaddr) continue;
            if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
            const auto* sin = reinterpret_cast<const sockaddr_in*>(u->Address.lpSockaddr);
            if (sin->sin_addr.s_addr == htonl(INADDR_LOOPBACK)) continue;
            out.push_back(sin->sin_addr);
        }
    }
    return out;
}

} // namespace

struct MdnsBrowser::Impl {
    SOCKET sock = INVALID_SOCKET;
    bool wsaUp = false;
    std::string serviceType; // "_airplay._tcp"
    MdnsFoundFn cb = nullptr;
    void* user = nullptr;
    std::map<std::string, Pending> pending; // instance name -> what we know
    std::map<std::string, std::string> hosts; // hostname -> IPv4
    int found = 0;
};

// True when `name` ends with "<serviceType>.local".
static bool endsWithService(const std::string& name, const std::string& serviceType) {
    std::string suffix = serviceType + ".local";
    if (name.size() < suffix.size()) return false;
    return name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
}

MdnsBrowser::~MdnsBrowser() {
    stop();
}

void MdnsBrowser::stop() {
    if (!m_impl) return;
    if (m_impl->sock != INVALID_SOCKET) closesocket(m_impl->sock);
    if (m_impl->wsaUp) WSACleanup();
    delete m_impl;
    m_impl = nullptr;
}

bool MdnsBrowser::start(const char* serviceType, MdnsFoundFn cb, void* user, int timeoutMs) {
    stop();
    Impl* im = new Impl();
    m_impl = im;
    im->serviceType = serviceType ? serviceType : "_airplay._tcp";
    im->cb = cb;
    im->user = user;

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        LOGE("mdns: WSAStartup failed");
        return false;
    }
    im->wsaUp = true;

    im->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (im->sock == INVALID_SOCKET) {
        LOGE("mdns: socket() failed (%d)", WSAGetLastError());
        return false;
    }
    BOOL reuse = TRUE;
    setsockopt(im->sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
    // Non-blocking: the drain loop below reads until the socket is empty, and on
    // a blocking socket that last recvfrom() waits for the *next* datagram,
    // which stalls the whole browse until the timeout window has passed.
    u_long nonBlocking = 1;
    ioctlsocket(im->sock, FIONBIO, &nonBlocking);

    // Bind to an ephemeral port: 5353 may already be taken by Bonjour or the
    // OS responder, and unicast answers come back to whatever port we use.
    sockaddr_in any{};
    any.sin_family = AF_INET;
    any.sin_addr.s_addr = INADDR_ANY;
    any.sin_port = 0;
    if (bind(im->sock, (sockaddr*)&any, sizeof(any)) != 0) {
        LOGE("mdns: bind() failed (%d)", WSAGetLastError());
        return false;
    }

    ip_mreq mreq{};
    mreq.imr_multiaddr.s_addr = inet_addr(kMdnsGroup);
    mreq.imr_interface.s_addr = INADDR_ANY;
    if (setsockopt(im->sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char*)&mreq, sizeof(mreq)) != 0) {
        // Multicast may be unavailable on some VPN/virtual adapters; unicast
        // answers still work, so this is not fatal.
        LOGW("mdns: could not join %s (%d), relying on unicast answers", kMdnsGroup,
             WSAGetLastError());
    }
    DWORD ttl = 255;
    setsockopt(im->sock, IPPROTO_IP, IP_MULTICAST_TTL, (const char*)&ttl, sizeof(ttl));

    // --- PTR query for "<serviceType>.local" ---------------------------------
    std::string qname = im->serviceType + ".local";
    std::vector<uint8_t> q;
    q.push_back(0); q.push_back(0);                       // id
    q.push_back(0); q.push_back(0);                       // flags: standard query
    q.push_back(0); q.push_back(1);                       // qdcount
    q.push_back(0); q.push_back(0);                       // ancount
    q.push_back(0); q.push_back(0);                       // nscount
    q.push_back(0); q.push_back(0);                       // arcount
    encodeName(qname, q);
    q.push_back(0); q.push_back((uint8_t)kPtr);           // type
    q.push_back(0); q.push_back(1);                       // class IN

    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(kMdnsPort);
    inet_pton(AF_INET, kMdnsGroup, &dst.sin_addr);

    const std::vector<in_addr> locals = localIpv4Addresses();
    auto sendQuery = [&](bool firstTime) {
        int ok = 0;
        const size_t n = locals.empty() ? 1 : locals.size();
        for (size_t i = 0; i < n; i++) {
            const in_addr ia = locals.empty() ? in_addr{} : locals[i];
            setsockopt(im->sock, IPPROTO_IP, IP_MULTICAST_IF, (const char*)&ia, sizeof(ia));
            if (firstTime && ia.s_addr != 0) {
                // Also listen on the group for this interface, in case a
                // responder answers to 224.0.0.251:5353 instead of our port.
                ip_mreq per{};
                per.imr_multiaddr = dst.sin_addr;
                per.imr_interface = ia;
                setsockopt(im->sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char*)&per,
                           sizeof(per));
            }
            if (sendto(im->sock, (const char*)q.data(), (int)q.size(), 0, (sockaddr*)&dst,
                       sizeof(dst)) >= 0) {
                ok++;
            }
        }
        return ok;
    };
    if (sendQuery(true) == 0) {
        LOGE("mdns: sendto failed (%d)", WSAGetLastError());
        return false;
    }
    LOGD("mdns: browsing %s on %zu interface(s) for %d ms", qname.c_str(),
         locals.empty() ? (size_t)1 : locals.size(), timeoutMs);

    // --- receive loop --------------------------------------------------------
    // mDNS runs over UDP, so re-ask a couple of times: one lost datagram would
    // otherwise mean "no receiver found" on an otherwise healthy network.
    std::vector<uint8_t> buf(4096);
    int waited = 0;
    int resendAt = 800;
    const int pollMs = 100;
    while (waited < timeoutMs) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(im->sock, &rfds);
        timeval tv{};
        tv.tv_sec = pollMs / 1000;
        tv.tv_usec = (pollMs % 1000) * 1000;
        int r = select(0, &rfds, nullptr, nullptr, &tv);
        waited += pollMs;
        if (waited >= resendAt) {
            resendAt += 800;
            sendQuery(false);
        }
        if (r <= 0) continue;

        for (;;) {
            sockaddr_in from{};
            int fromLen = sizeof(from);
            int n = recvfrom(im->sock, (char*)buf.data(), (int)buf.size(), 0, (sockaddr*)&from,
                             &fromLen);
            if (n == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) break;
            if (n <= 0) break;
            Reader rd{buf.data(), (size_t)n, 0};
            uint16_t id = 0, flags = 0, qd = 0, an = 0, ns = 0, ar = 0;
            if (!rd.u16(id) || !rd.u16(flags) || !rd.u16(qd) || !rd.u16(an) || !rd.u16(ns) ||
                !rd.u16(ar))
                continue;
            if (flags & 0x8000) { /* response */ } else { continue; } // ignore queries
            bool truncated = (flags & 0x0200) != 0;
            (void)truncated;

            // skip questions
            bool ok = true;
            for (int i = 0; i < qd && ok; i++) {
                std::string qn;
                uint16_t qt = 0, qc = 0;
                ok = readName(rd, qn) && rd.u16(qt) && rd.u16(qc);
            }
            if (!ok) continue;

            auto handleRr = [&](std::string& owner, uint16_t type, uint16_t cls, uint32_t ttl,
                                uint16_t rdlen) -> bool {
                size_t rdStart = rd.pos;
                if (type == kPtr) {
                    std::string target;
                    if (!readName(rd, target)) return false;
                    // The PTR owner is the service type; the RDATA is the
                    // instance name we will later match SRV/TXT records on.
                    if (endsWithService(target, im->serviceType)) {
                        Pending& p = im->pending[target];
                        p.info.instance = target;
                        p.info.service = im->serviceType;
                        im->found++;
                        LOGD("mdns: PTR %s", target.c_str());
                    }
                } else if (type == kSrv) {
                    uint16_t prio = 0, weight = 0, port = 0;
                    std::string target;
                    if (!rd.u16(prio) || !rd.u16(weight) || !rd.u16(port)) return false;
                    if (!readName(rd, target)) return false;
                    // find or create the pending entry this SRV belongs to
                    for (auto& kv : im->pending) {
                        if (kv.first == owner) {
                            kv.second.info.host = target;
                            kv.second.info.port = port;
                            kv.second.hasSrv = true;
                            LOGD("mdns: SRV %s -> %s:%u", owner.c_str(), target.c_str(),
                                 (unsigned)port);
                        }
                    }
                } else if (type == kTxt) {
                    size_t end = rdStart + rdlen;
                    std::vector<std::pair<std::string, std::string>> items;
                    while (rd.pos < end) {
                        uint8_t l = 0;
                        if (!rd.u8(l)) break;
                        if (rd.pos + l > end) break;
                        std::string item((const char*)rd.base + rd.pos, l);
                        rd.pos += l;
                        size_t eq = item.find('=');
                        if (eq == std::string::npos) items.emplace_back(item, "");
                        else items.emplace_back(item.substr(0, eq), item.substr(eq + 1));
                    }
                    rd.pos = end;
                    for (auto& kv : im->pending) {
                        if (kv.first == owner) {
                            kv.second.info.txt = items;
                            LOGD("mdns: TXT %s (%zu keys)", owner.c_str(), items.size());
                        }
                    }
                } else if (type == kA) {
                    if (rdlen != 4) { rd.pos = rdStart + rdlen; return true; }
                    char ip[64] = {0};
                    snprintf(ip, sizeof(ip), "%u.%u.%u.%u", rd.base[rd.pos], rd.base[rd.pos + 1],
                             rd.base[rd.pos + 2], rd.base[rd.pos + 3]);
                    rd.pos = rdStart + rdlen;
                    // The A record's owner is the SRV *target* hostname, not the
                    // service instance, so keep a hostname -> address table.
                    im->hosts[owner] = ip;
                    LOGD("mdns: A %s -> %s", owner.c_str(), ip);
                } else {
                    rd.pos = rdStart + rdlen;
                }
                return true;
            };

            for (int sec = 0; sec < 3 && ok; sec++) {
                uint16_t count = sec == 0 ? an : (sec == 1 ? ns : ar);
                for (int i = 0; i < count && ok; i++) {
                    std::string owner;
                    uint16_t type = 0, cls = 0;
                    uint32_t ttl = 0;
                    uint16_t rdlen = 0;
                    ok = readName(rd, owner) && rd.u16(type) && rd.u16(cls) && rd.u32(ttl) &&
                         rd.u16(rdlen);
                    if (!ok) break;
                    ok = handleRr(owner, type, cls, ttl, rdlen);
                }
            }
        }
    }

    // --- report anything complete enough to connect to ----------------------
    int reported = 0;
    for (auto& kv : im->pending) {
        Pending& p = kv.second;
        if (!p.hasSrv || p.info.port == 0 || p.info.host.empty()) continue;
        // Fill the address from the hostname table when the A record showed up.
        if (p.info.address.empty()) {
            auto it = im->hosts.find(p.info.host);
            if (it != im->hosts.end()) p.info.address = it->second;
        }
        im->cb(p.info, im->user);
        reported++;
    }
    if (reported == 0) {
        LOGW("mdns: no %s service answered within %d ms", im->serviceType.c_str(), timeoutMs);
    } else {
        LOGI("mdns: %d %s instance(s) found", reported, im->serviceType.c_str());
    }
    return reported > 0;
}

} // namespace fc
