// FlowCast Windows client - AirPlay receiver capability probe.
//
// Context: a MAXHUB panel answers OPTIONS with a full method list and
// "Server: AirTunes/320.20.1", then silently drops POST /fp-setup - no 4xx, no
// close, just a read timeout. That rules our request format out (our own
// receiver and a vivo phone both answer it) and points at a receiver that only
// implements the newer pairing flow. This probe confirms which one, because
// guessing costs a rebuild per attempt and this costs one run.
//
// Two things the first version got wrong and this one fixes:
//  * it sent XML/binary *plists* to /pair-setup, but AirPlay 2 pairing speaks
//    TLV8. A receiver that expected TLV8 answered with a constant 32-byte blob,
//    which looked like "pairing is supported" when it was really "I cannot read
//    your body". Bodies are TLV8 now.
//  * it dumped /info as hex and threw away the one field that decides
//    everything: the `features` bitmask, whose bit 27 is SupportsLegacyPairing.
#include "probe.h"

#include "crypto.h"
#include "fairplay.h"
#include "log.h"
#include "plist.h"
#include "rtsp.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace fc {
namespace {

// Long enough for a slow embedded receiver, short enough that a device which
// ignores the endpoint does not make the whole probe take a minute.
constexpr int kProbeTimeoutMs = 3000;

// ---------------------------------------------------------------------------
// TLV8 - the actual body format of AirPlay 2 pairing. Values longer than 255
// bytes are split across several TLVs of the same type, so decoding has to
// concatenate neighbours with an equal type or an SRP public key arrives as two
// unrelated fragments.
// ---------------------------------------------------------------------------

enum TlvType {
    kTlvMethod = 0x00,
    kTlvIdentifier = 0x01,
    kTlvSalt = 0x02,
    kTlvPublicKey = 0x03,
    kTlvProof = 0x04,
    kTlvEncryptedData = 0x05,
    kTlvState = 0x06,
    kTlvError = 0x07,
    kTlvRetryDelay = 0x08,
    kTlvCertificate = 0x09,
    kTlvSignature = 0x0a,
    kTlvPermissions = 0x0b,
    kTlvFragmentData = 0x0c,
    kTlvFragmentLast = 0x0d,
    kTlvSeparator = 0xff,
};

const char* tlvName(unsigned char t) {
    switch (t) {
    case kTlvMethod: return "Method";
    case kTlvIdentifier: return "Identifier";
    case kTlvSalt: return "Salt";
    case kTlvPublicKey: return "PublicKey";
    case kTlvProof: return "Proof";
    case kTlvEncryptedData: return "EncryptedData";
    case kTlvState: return "State";
    case kTlvError: return "Error";
    case kTlvRetryDelay: return "RetryDelay";
    case kTlvCertificate: return "Certificate";
    case kTlvSignature: return "Signature";
    case kTlvPermissions: return "Permissions";
    case kTlvFragmentData: return "FragmentData";
    case kTlvFragmentLast: return "FragmentLast";
    case kTlvSeparator: return "Separator";
    default: return "?";
    }
}

struct TlvOut {
    std::string bytes;
    void add(unsigned char type, const std::string& v) {
        size_t off = 0;
        do {
            const size_t n = v.size() - off < 255 ? v.size() - off : 255;
            bytes += (char)type;
            bytes += (char)n;
            bytes += v.substr(off, n);
            off += n;
        } while (off < v.size());
    }
};

std::string hexOf(const std::string& s, size_t limit);  // defined below

void dumpTlv(const std::string& body) {
    size_t i = 0;
    while (i + 1 < body.size()) {
        const unsigned char t = (unsigned char)body[i];
        const unsigned char n = (unsigned char)body[i + 1];
        if (i + 2 + n > body.size()) {
            // Not TLV8 at all. Print the bytes instead of a verdict: a fixed
            // 32-byte blob is either a binary error token or 32 characters of
            // hex text, and those two look nothing alike once dumped. Whatever
            // it is, it is not an SRP answer (Salt 16 + PublicKey 384 = 404+).
            LOGI("        not TLV8 (offset %zu claims %u bytes) - raw body:", i, n);
            for (size_t off = 0; off < body.size(); off += 32) {
                const size_t take = body.size() - off < 32 ? body.size() - off : 32;
                std::string line = hexOf(body.substr(off, take), 32);
                // Printable runs are called out because a 32-character answer
                // is almost always text (an MD5 hex digest, a token) rather
                // than a crypto blob.
                bool text = true;
                for (size_t k = 0; k < take; ++k) {
                    const unsigned char c = (unsigned char)body[off + k];
                    if (c < 0x20 || c > 0x7e) { text = false; break; }
                }
                if (text) line += "   \"" + body.substr(off, take) + "\"";
                LOGI("          %s", line.c_str());
            }
            return;
        }
        const std::string v = body.substr(i + 2, n);
        // State and Method are one-byte enums; everything else is opaque.
        if ((t == kTlvState || t == kTlvMethod || t == kTlvError) && n == 1) {
            LOGI("        TLV %-16s = %u", tlvName(t), (unsigned char)v[0]);
        } else {
            std::string hex;
            char buf[4];
            const size_t show = n < 16 ? n : 16;
            for (size_t k = 0; k < show; ++k) {
                std::snprintf(buf, sizeof(buf), "%02x", (unsigned char)v[k]);
                hex += buf;
            }
            LOGI("        TLV %-16s = %zu byte(s)  %s%s", tlvName(t), (size_t)n, hex.c_str(),
                 n > 16 ? "..." : "");
        }
        i += 2 + n;
    }
}

// ---------------------------------------------------------------------------
// Output helpers
// ---------------------------------------------------------------------------

// Hex, not prose: the interesting answers here are binary (an SRP salt, a TLV8
// blob, a FairPlay reply). Pretty-printing would throw away the very bytes that
// identify the protocol.
void dumpBody(const std::string& body, size_t limit) {
    const size_t n = body.size() < limit ? body.size() : limit;
    for (size_t i = 0; i < n; i += 16) {
        std::string hex;
        std::string ascii;
        char buf[8];
        for (size_t j = 0; j < 16; ++j) {
            if (i + j < n) {
                const unsigned char c = (unsigned char)body[i + j];
                std::snprintf(buf, sizeof(buf), "%02x ", c);
                hex += buf;
                ascii += (c >= 32 && c < 127) ? (char)c : '.';
            } else {
                hex += "   ";
                ascii += ' ';
            }
        }
        LOGI("        %04zx  %s |%s|", i, hex.c_str(), ascii.c_str());
    }
    if (body.size() > limit) LOGI("        ... %zu more byte(s)", body.size() - limit);
}

std::string hexOf(const std::string& s, size_t limit) {
    std::string out;
    char buf[4];
    const size_t n = s.size() < limit ? s.size() : limit;
    for (size_t i = 0; i < n; ++i) {
        std::snprintf(buf, sizeof(buf), "%02x", (unsigned char)s[i]);
        out += buf;
    }
    return out;
}

std::string oneLine(const PlistValue& v, bool* isBinary) {
    switch (v.type) {
    case PlistType::Null: return "null";
    case PlistType::Bool: return v.b ? "true" : "false";
    case PlistType::Int: return std::to_string(v.i);
    case PlistType::Real: {
        char b[32];
        std::snprintf(b, sizeof(b), "%g", v.r);
        return b;
    }
    case PlistType::String: return v.s;
    case PlistType::Data:
        // A 16- or 32-byte blob is a key, a MAC or an ID - worth showing whole;
        // anything bigger is a certificate and only its size matters.
        if (v.s.size() <= 32) return hexOf(v.s, 32) + " (data, " + std::to_string(v.s.size()) + "B)";
        *isBinary = true;
        return "<data " + std::to_string(v.s.size()) + " bytes>";
    case PlistType::Array: return "[" + std::to_string(v.arr.size()) + " item(s)]";
    case PlistType::Dict: return "{" + std::to_string(v.dict.size()) + " key(s)}";
    }
    return "?";
}

// Bit 27 of `features` is SupportsLegacyPairing - i.e. "I still answer
// /fp-setup". That single bit decides whether the current AirPlay 1 client can
// ever talk to this device. The rest of the table is partial on purpose: a
// wrong name is worse than no name.
void explainFeatures(unsigned long long f) {
    struct Bit {
        int bit;
        const char* name;
    };
    static const Bit kBits[] = {
        {0, "Video"}, {1, "Photo"}, {2, "VideoFairPlay"}, {3, "VideoVolumeControl"},
        {4, "VideoHTTPLiveStreams"}, {5, "Slideshow"}, {9, "Screen"}, {27, "LegacyPairing"},
    };
    std::string on;
    for (const Bit& b : kBits) {
        if (f & (1ull << b.bit)) {
            if (!on.empty()) on += ", ";
            on += b.name;
        }
    }
    LOGI("        features           0x%016llx", f);
    LOGI("        features set       %s", on.empty() ? "(none of the known bits)" : on.c_str());
    LOGI("        SupportsLegacyPairing (bit 27, answers /fp-setup) = %s",
         (f & (1ull << 27)) ? "YES" : "NO");
}

void explainStatus(long long sf) {
    if (sf == 0) {
        LOGI("        statusFlags        0  (ready)");
        return;
    }
    std::string why;
    if (sf & 0x1) why += "Problem ";
    if (sf & 0x2) why += "NotReady ";
    if (sf & 0x10) why += "PinRequired ";
    LOGI("        statusFlags        0x%llx  (%s)", sf, why.empty() ? "other" : why.c_str());
}

// Walks the /info dict. Key order is whatever the receiver sent, so the three
// decisive fields are called out first instead of being buried.
void dumpInfoDict(const PlistValue& d) {
    unsigned long long features = 0;
    bool haveFeatures = false;
    long long statusFlags = 0;
    bool haveStatus = false;

    for (const auto& kv : d.dict) {
        if (kv.first == "features" && kv.second.type == PlistType::Int) {
            features = (unsigned long long)kv.second.i;
            haveFeatures = true;
            continue;
        }
        if (kv.first == "statusFlags" && kv.second.type == PlistType::Int) {
            statusFlags = kv.second.i;
            haveStatus = true;
            continue;
        }
    }
    if (haveFeatures) explainFeatures(features);
    if (haveStatus) explainStatus(statusFlags);

    LOGI("        --- /info contents ---");
    for (const auto& kv : d.dict) {
        if (kv.first == "features" || kv.first == "statusFlags") continue;
        bool isBinary = false;
        const std::string v = oneLine(kv.second, &isBinary);
        LOGI("        %-20s %s", kv.first.c_str(), v.c_str());
        // Nested dicts are rare here but audioFormats/et live there.
        if (kv.second.type == PlistType::Dict) {
            for (const auto& sub : kv.second.dict) {
                bool b2 = false;
                LOGI("          %-18s %s", sub.first.c_str(), oneLine(sub.second, &b2).c_str());
            }
        }
    }
}

// Returns the /info body when the receiver sent a plist we could read.
std::string dumpInfoBody(const std::string& body) {
    if (body.size() >= 8 && std::memcmp(body.data(), "bplist00", 8) == 0) {
        PlistValue v;
        if (plist_parse(body.c_str(), body.size(), v) && v.type == PlistType::Dict) {
            LOGI("        (decoded binary plist)");
            dumpInfoDict(v);
            return "dict";
        }
        LOGI("        (binary plist did not parse)");
    } else if (!body.empty() && body[0] == '<') {
        LOGI("        (xml plist, %zu bytes)", body.size());
        PlistValue v;
        if (plist_parse(body.c_str(), body.size(), v) && v.type == PlistType::Dict) {
            dumpInfoDict(v);
            return "dict";
        }
        LOGI("        (xml plist did not parse)");
    }
    dumpBody(body, 160);
    return "";
}

// One probe, one fresh connection. A receiver that drops a request usually
// leaves that socket unusable, and reusing it would make every later probe
// timeout too - blaming endpoints that were never asked.
bool probeOne(const char* host, unsigned short port, const char* label, const char* method,
              const char* url, const char* ctype, const std::string& body,
              std::string* bodyOut = nullptr, const char* bodyKind = nullptr,
              const char* userAgent = nullptr, const char* extraHeaders = nullptr) {
    RtspClient c;
    if (!c.connect(host, port)) {
        LOGI("PROBE %-22s -> could not connect", label);
        return false;
    }
    RtspResponse r;
    if (!c.request(method, url, ctype, body.data(), body.size(), r, kProbeTimeoutMs, userAgent,
                   extraHeaders)) {
        // The reason matters: "dropped" means the endpoint exists and hung up,
        // "no reply" means it never answered. Those lead to different fixes and
        // used to both print as the same line.
        LOGI("PROBE %-22s -> NO ANSWER in %d ms (%s)", label, kProbeTimeoutMs,
             net_error_text(r.err));
        c.close();
        return false;
    }
    LOGI("PROBE %-22s -> %d %s, %zu body byte(s)", label, r.status, r.reason.c_str(),
         r.body.size());
    for (const auto& kv : r.headers) {
        // Echoed request bookkeeping, not capability information.
        if (kv.first == "cseq" || kv.first == "content-length") continue;
        LOGI("        %-26s %s", (kv.first + ":").c_str(), kv.second.c_str());
    }
    if (bodyOut) *bodyOut = r.body;
    if (r.body.empty()) {
        c.close();
        return true;
    }
    if (bodyKind && std::strcmp(bodyKind, "tlv") == 0) {
        dumpTlv(r.body);
    } else if (bodyKind && std::strcmp(bodyKind, "plist") == 0) {
        // Decoded by the caller, which prints a readable key list instead.
    } else {
        dumpBody(r.body, 192);
    }
    c.close();
    return true;
}

// Looks for the fingerprint of a real SRP pair-setup answer: a Salt plus a
// PublicKey far too big to be anything but the 384-byte SRP group element.
// The plist-bodied probe got a constant 32-byte blob back, which parses as
// nothing - so size is the honest discriminator here, not "did it answer".
bool looksLikeSrpAnswer(const std::string& body) {
    bool salt = false;
    bool bigKey = false;
    size_t i = 0;
    while (i + 1 < body.size()) {
        const unsigned char t = (unsigned char)body[i];
        const unsigned char n = (unsigned char)body[i + 1];
        if (i + 2 + n > body.size()) break;
        if (t == kTlvSalt && n >= 8) salt = true;
        if (t == kTlvPublicKey && n >= 64) bigKey = true;
        i += 2 + n;
    }
    return salt && bigKey;
}

// Deterministic filler, so a re-run compares against the same bytes. A real
// Curve25519 key would be better, but a probe only needs the endpoint to prove
// it answers in TLV8 - the signature check happens one step later.
std::string fakePubKey() {
    std::string k(32, '\0');
    unsigned int x = 0x1234567u;
    for (size_t i = 0; i < k.size(); ++i) {
        x = x * 1103515245u + 12345u;
        k[i] = (char)((x >> 16) & 0xff);
    }
    return k;
}

// One connection, OPTIONS then SETUP #1 - exactly what the client sends when it
// skips FairPlay. Run before any /fp-setup: a receiver that hung up once may
// have left the advertised port in a bad state, and the whole point is to see
// the clean answer.
//
// This is the decisive question for a panel that answers /fp-setup #1 and then
// drops #2 (MAXHUB). If SETUP is accepted here, the exchange was never needed
// and the fix is to stop sending it. If it is refused, the refusal has a status
// code worth reading - which is more than the client's "dropped" can offer.
bool probeSetupWithoutPairing(const char* host, unsigned short port, const char* label,
                              const char* userAgent, const char* extraHeaders, bool xmlBody = false,
                              bool appleShape = false) {
    RtspClient c;
    if (!c.connect(host, port)) {
        LOGI("PROBE %-22s -> could not connect", label);
        return false;
    }
    RtspResponse r;
    if (!c.request("OPTIONS", "rtsp://localhost/", nullptr, "", r, kProbeTimeoutMs, userAgent,
                   extraHeaders) ||
        r.status != 200) {
        LOGI("PROBE %-22s -> OPTIONS gave %d (%s), skipped", label, r.status,
             net_error_text(r.err));
        c.close();
        return false;
    }

    // Random keys: with no FairPlay session the receiver has nothing to compare
    // them against, so 72 arbitrary bytes are indistinguishable from a derived
    // ekey.
    uint8_t keymsg[164];
    fairplay_build_keymsg(keymsg);
    uint8_t aes[16], iv[16], ekey[72];
    fc::random_bytes(aes, sizeof(aes));
    fc::random_bytes(iv, sizeof(iv));
    fairplay_make_ekey(keymsg, aes, ekey);

    PlistValue body = PlistValue::dictOf({
        {"ekey", PlistValue::data(std::string((const char*)ekey, sizeof(ekey)))},
        {"eiv", PlistValue::data(std::string((const char*)iv, sizeof(iv)))},
        // A real port is not needed: nothing dials back within one probe, and a
        // fake one keeps the probe from having to open a UDP socket.
        {"timingPort", PlistValue::integer(59999)},
        {"timingProtocol", PlistValue::string("NTP")},
        {"deviceID", PlistValue::string("00:11:22:33:44:55")},
        {"model", PlistValue::string("WindowsPC")},
        {"name", PlistValue::string("FlowCast probe")},
        {"isRemoteControlOnly", PlistValue::boolean(false)},
    });
    if (appleShape) {
        // The shape iOS actually sends: one SETUP carrying the stream list, the
        // encryption story inside the stream, and isScreenMirroring. A receiver
        // written against iOS has somewhere to look here; the split form above
        // leaves it with no stream to set up, which is a plausible reason to
        // hang up without a status code.
        std::vector<PlistValue> streams{PlistValue::dictOf({
            {"type", PlistValue::integer(110)},
            {"streamConnectionID", PlistValue::integer(
                                       (long long)0x1122334455667788ull)},
            {"dataPort", PlistValue::integer(0)},
            {"ctlPort", PlistValue::integer(0)},
            {"eventPort", PlistValue::integer(0)},
            {"headerCompression", PlistValue::boolean(true)},
            {"encryptionType", PlistValue::string("FairPlay")},
        })};
        body.set("isScreenMirroring", PlistValue::boolean(true));
        body.set("streams", PlistValue::array(std::move(streams)));
    }
    // Apple always sends the binary form, but this device answers /info with an
    // XML plist, so it is worth one request to see whether the body parser is
    // XML-only. Costs nothing: same connection shape, different serialisation.
    const std::string wire = xmlBody ? plist_dump(body) : plist_dump_bin(body);
    // Addressed by the receiver's own address, like every genuine sender. A
    // receiver that keys sessions on the request URL treats "localhost" as an
    // address it never handed out and closes instead of answering - which used
    // to be read here as "SETUP is refused".
    char setupUrl[256];
    snprintf(setupUrl, sizeof(setupUrl), "rtsp://%s:%u/1", host, (unsigned)port);
    const bool answered =
        c.request("SETUP", setupUrl,
                  xmlBody ? "text/xml" : "application/x-apple-binary-plist", wire, r,
                  kProbeTimeoutMs, userAgent, extraHeaders);
    if (!answered) {
        LOGI("PROBE %-22s -> NO ANSWER (%s)", label, net_error_text(r.err));
    } else {
        LOGI("PROBE %-22s -> %d %s, %zu body byte(s)", label, r.status, r.reason.c_str(),
             r.body.size());
        if (!r.body.empty()) dumpBody(r.body, 128);
    }
    // Hand the session back. A receiver that only allows one keeps the slot busy
    // after an abandoned SETUP and then refuses the next real attempt - measured
    // on a Sony BRAVIA, whose AirPlay port stopped accepting connections after a
    // probe that got this far. Best effort: we do not read the answer.
    if (answered && r.status == 200) {
        RtspResponse td;
        c.request("TEARDOWN", setupUrl, nullptr, "", td, 1500, userAgent, extraHeaders);
    }
    c.close();
    return answered && r.status == 200;
}

} // namespace

void run_probe(const char* host, unsigned short port) {
    LOGI("probing AirPlay receiver at %s:%u", host, port);
    LOGI("each line below is a fresh connection, %d ms timeout per request", kProbeTimeoutMs);
    // Worth stating, because it is the one thing that makes the first lines
    // conclusive: if the panel is left in a bad state by a failed key exchange,
    // a reset seen here cannot be blamed on one.
    LOGI("the SETUP lines run first, before any FairPlay message is sent");

    // Baseline: this already works on every receiver we have tried, so it
    // proves the socket, the request formatting and the timeout all behave.
    probeOne(host, port, "OPTIONS", "OPTIONS", "rtsp://localhost/", nullptr, "");

    // The controls run first, on a pristine port: does this receiver accept a
    // mirror session with no pairing at all? Asked twice, because "refused as a
    // stranger" and "refused as anyone" are different answers - and the second
    // one is the one that decides whether pairing is mandatory.
    const bool setupNoPairing = probeSetupWithoutPairing(host, port, "SETUP without pairing",
                                                         nullptr, nullptr);
    const bool setupAsApple =
        setupNoPairing ? false
                       : probeSetupWithoutPairing(
                             host, port, "SETUP as Apple, no pairing", "AirPlay/670.99.20",
                             "X-Apple-ProtocolVersion: 1\r\n"
                             "X-Apple-Device-ID: 0x0\r\n"
                             "X-Apple-Session-ID: 6C1B4E4A-5D2F-4E4B-9B5F-6D8A0C1E2F30\r\n");
    // Same body, serialised as XML. Both the ordering and the keys match the
    // two lines above, so a different answer here isolates the body format.
    const bool setupXml =
        (setupNoPairing || setupAsApple)
            ? false
            : probeSetupWithoutPairing(host, port, "SETUP as XML plist", nullptr, nullptr, true);
    // And the one shape that has never been tried on this device: a single
    // SETUP that carries the stream list, the way iOS describes a session. The
    // three above all split keys from streams, so this is the only line that
    // answers "does it want the Apple layout?" rather than "does it like us?".
    const bool setupAppleShape =
        (setupNoPairing || setupAsApple || setupXml)
            ? false
            : probeSetupWithoutPairing(host, port, "SETUP Apple-shaped", nullptr, nullptr, false,
                                       true);

    // /info is what AirPlay 2 asks before any pairing, and it carries the
    // features bitmask plus any status flags (busy, PIN required).
    std::string info;
    const bool gotInfo =
        probeOne(host, port, "GET /info", "GET", "/info", nullptr, "", &info, "plist");
    if (gotInfo && !info.empty()) dumpInfoBody(info);

    // AirPlay 2 pair-setup, in TLV8 this time: State = 1, Method = 0. That is
    // the transient (no-PIN) handshake TVs use. A working SRP endpoint answers
    // State = 2 with a 16-byte Salt and a 384-byte PublicKey, which is
    // impossible to confuse with the constant 32-byte blob a plist body got.
    TlvOut m1;
    m1.add(kTlvState, std::string(1, (char)1));
    m1.add(kTlvMethod, std::string(1, (char)0));
    std::string srpBody;
    probeOne(host, port, "pair-setup TLV8 m0", "POST", "/pair-setup", "application/octet-stream",
             m1.bytes, &srpBody, "tlv");
    const bool srpOk = looksLikeSrpAnswer(srpBody);

    // Method = 1 is the PIN variant. Some panels only run this one, and the
    // error code (or a RetryDelay TLV) says so.
    TlvOut m1pin;
    m1pin.add(kTlvState, std::string(1, (char)1));
    m1pin.add(kTlvMethod, std::string(1, (char)1));
    std::string srpPinBody;
    probeOne(host, port, "pair-setup TLV8 m1", "POST", "/pair-setup", "application/octet-stream",
             m1pin.bytes, &srpPinBody, "tlv");

    // pair-verify M1: State + a 32-byte Curve25519 key. We do not have a real
    // one, but a live endpoint still answers State = 2 with EncryptedData
    // before it can possibly check the signature.
    TlvOut v1;
    v1.add(kTlvState, std::string(1, (char)1));
    v1.add(kTlvPublicKey, fakePubKey());
    std::string verifyBody;
    probeOne(host, port, "pair-verify TLV8", "POST", "/pair-verify", "application/octet-stream",
             v1.bytes, &verifyBody, "tlv");

    // Our current path, as a control. The canonical Apple M1, the same bytes the
    // client sends: the older "FPLY" + zeros version got no answer here, which
    // made the first probe report the endpoint as missing when it was really
    // rejecting a malformed request.
    uint8_t m1bytes[16];
    fairplay_m1(m1bytes);
    std::string fp((const char*)m1bytes, sizeof(m1bytes));
    std::string fpReply;
    const bool fpOk =
        probeOne(host, port, "POST /fp-setup #1", "POST", "/fp-setup", "application/octet-stream",
                 fp, &fpReply);

    // Round two, the 164-byte key message. A receiver that answers #1 and hangs
    // up on #2 has a FairPlay stub, not an implementation - and since it closed
    // the socket on the way out, every later request on that connection fails
    // with a socket error instead of a status code.
    bool fp2Ok = false;
    if (fpOk) {
        uint8_t keymsg[164];
        fairplay_build_keymsg(keymsg);
        std::string keyReply;
        fp2Ok = probeOne(host, port, "POST /fp-setup #2", "POST", "/fp-setup",
                         "application/octet-stream", std::string((const char*)keymsg, sizeof(keymsg)),
                         &keyReply);
    }

    // Same request, but announcing ourselves the way an Apple sender does. If
    // this one answers while the plain one does not, the receiver is gating on
    // the caller rather than missing the endpoint - and that would be a header
    // change, not a protocol rewrite.
    static const char* kAppleHeaders =
        "X-Apple-ProtocolVersion: 1\r\n"
        "X-Apple-Device-ID: 0x0\r\n"
        "X-Apple-Session-ID: 6C1B4E4A-5D2F-4E4B-9B5F-6D8A0C1E2F30\r\n";
    const bool fpApple =
        probeOne(host, port, "fp-setup as iOS", "POST", "/fp-setup", "application/octet-stream", fp,
                 nullptr, nullptr, "AirPlay/670.99.20", kAppleHeaders);

    // The receiver advertises a non-standard port. Standard AirPlay is 7000, and
    // a device that runs two stacks - its own on the advertised port, AirPlay on
    // 7000 - is common enough to be worth two requests. Skipped when the
    // advertised port already is 7000.
    bool port7000 = false;
    if (port != 7000) {
        port7000 = probeOne(host, 7000, "OPTIONS on :7000", "OPTIONS", "rtsp://localhost/", nullptr,
                            "");
    }

    // One paragraph that states the conclusion, so the answer does not depend
    // on someone reading five hex dumps. Ordered by what the reader should do
    // next, not by how interesting the finding was.
    LOGI("---- verdict ----");
    if (fp2Ok) {
        LOGI("AirPlay 1 works end to end: /fp-setup answers both rounds.");
        LOGI("If mirroring still fails, the refusal is later in the handshake - send -v log.");
    } else if (setupAppleShape) {
        LOGI("SETUP is accepted only in the APPLE shape: one request carrying the stream list,");
        LOGI("with the encryption story inside the stream. The split form this client speaks");
        LOGI("leaves the receiver with nothing to set up, so it hangs up without a status.");
        LOGI("Fix is on our side: send the single SETUP (--single-setup, already the second");
        LOGI("variant of the fallback ladder).");
    } else if (setupXml && !setupNoPairing && !setupAsApple) {
        LOGI("SETUP is accepted only as an XML plist: this receiver does not parse the binary");
        LOGI("form Apple sends. That is a body-format fix, not a protocol one.");
    } else if (setupNoPairing || setupAsApple) {
        if (setupAsApple && !setupNoPairing) {
            LOGI("SETUP is accepted with no pairing - but only when we announce as an Apple sender.");
            LOGI("So the receiver gates on the caller, not on FairPlay: keep the Apple identity");
            LOGI("and skip /fp-setup.");
        } else {
            LOGI("SETUP is accepted with NO pairing at all, so the FairPlay exchange is what");
            LOGI("breaks this device: whatever it answers, it leaves the connection unusable and");
            LOGI("the SETUP that follows fails on a dead socket. Fix is on our side - do not send");
            LOGI("/fp-setup to a receiver that does not need it.");
        }
    } else if (fpOk) {
        LOGI("FairPlay stub: /fp-setup #1 is answered, #2 is not, and SETUP is reset.");
        LOGI("The SETUP above was sent before any key message, on a fresh connection, and it was");
        LOGI("still reset - so the reset is not the exchange order and not the plist body.");
        LOGI("What is left is a real FairPlay check: it wants a key message only Apple can");
        LOGI("produce. Confirm with a power-cycled panel; if it still resets, stop here.");
        if (fpApple) LOGI("(Announcing as an iPhone changes nothing, so it is not a header gate.)");
    } else if (port7000) {
        LOGI("The advertised port refuses /fp-setup but port 7000 answers: retry with --port 7000.");
    } else if (srpOk) {
        LOGI("This device is AirPlay 2 only: it does SRP pair-setup but never answers /fp-setup.");
        LOGI("Talking to it needs SRP-6a + Curve25519 pair-verify + an encrypted channel.");
        LOGI("That is a new subsystem, not a patch.");
    } else {
        LOGI("Neither pairing path works: /fp-setup is dropped and /pair-setup returns no");
        LOGI("readable SRP answer. Its AirPlay service is closed to non-Apple senders.");
    }
    LOGI("probe finished - send this whole output back, it decides the fix");
}

} // namespace fc
