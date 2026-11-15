// FlowCast Windows client - plist implementation (XML subset, no external XML lib).
#include "plist.h"
#include "bplist.h"
#include "log.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace fc {
namespace {

struct Parser {
    const char* p;
    const char* end;
    int depth = 0;

    void skipWs() {
        while (p < end) {
            if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') { p++; continue; }
            if (p + 3 < end && p[0] == '<' && p[1] == '!' && p[2] == '-' && p[3] == '-') {
                p += 4;
                while (p + 2 < end && !(p[0] == '-' && p[1] == '-' && p[2] == '>')) p++;
                if (p + 2 < end) p += 3;
                continue;
            }
            break;
        }
    }

    bool lit(const char* s) {
        size_t n = strlen(s);
        if ((size_t)(end - p) < n) return false;
        if (memcmp(p, s, n) != 0) return false;
        p += n;
        return true;
    }

    bool expect(const char* s) {
        skipWs();
        if (!lit(s)) {
            LOGE("plist: expected '%s' near '%.24s'", s, p);
            return false;
        }
        return true;
    }

    // Skips a declaration / doctype that starts with '<' but is not an element
    // or the closing tag of the current element.
    bool skipPreamble() {
        skipWs();
        while (p < end && *p == '<') {
            if (lit("<?")) {
                while (p < end && !lit("?>")) p++;
                continue;
            }
            if (p + 1 < end && p[1] == '!') {
                // <!DOCTYPE ...> or <!ENTITY ...>
                while (p < end && *p != '>') p++;
                if (p < end) p++;
                continue;
            }
            break;
        }
        return true;
    }

    std::string parseText() {
        // Raw text between tags; entity-unescaped.
        std::string out;
        while (p < end && *p != '<') {
            if (*p == '&') {
                if (lit("&amp;")) { out += '&'; continue; }
                if (lit("&lt;")) { out += '<'; continue; }
                if (lit("&gt;")) { out += '>'; continue; }
                if (lit("&quot;")) { out += '"'; continue; }
                if (lit("&apos;")) { out += '\''; continue; }
            }
            out += *p++;
        }
        return out;
    }

    bool parseValue(PlistValue& out, int maxDepth) {
        if (depth > maxDepth) {
            LOGE("plist: nesting too deep");
            return false;
        }
        skipWs();
        if (p >= end || *p != '<') {
            LOGE("plist: expected element");
            return false;
        }
        if (lit("<true/>")) { out = PlistValue::boolean(true); return true; }
        if (lit("<false/>")) { out = PlistValue::boolean(false); return true; }
        if (lit("<dict>")) {
            depth++;
            out = PlistValue::array({}); // placeholder, replaced below
            out.type = PlistType::Dict;
            for (;;) {
                skipWs();
                if (lit("</dict>")) break;
                if (!expect("<key>")) return false;
                std::string key = parseText();
                if (!expect("</key>")) return false;
                PlistValue v;
                if (!parseValue(v, maxDepth)) return false;
                out.dict.emplace_back(std::move(key), std::move(v));
            }
            depth--;
            return true;
        }
        if (lit("<array/>")) { out = PlistValue::array({}); return true; }
        if (lit("<dict/>")) { out = PlistValue::dictOf({}); return true; }
        if (lit("<array>")) {
            depth++;
            out = PlistValue::array({});
            for (;;) {
                skipWs();
                if (lit("</array>")) break;
                PlistValue v;
                if (!parseValue(v, maxDepth)) return false;
                out.arr.push_back(std::move(v));
            }
            depth--;
            return true;
        }
        if (lit("<data>")) {
            std::string b64 = parseText();
            // strip whitespace
            std::string compact;
            compact.reserve(b64.size());
            for (char c : b64) {
                if (c != ' ' && c != '\t' && c != '\r' && c != '\n') compact += c;
            }
            if (!expect("</data>")) return false;
            static const char* kAlpha =
                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            int val = 0, bits = 0;
            std::string raw;
            for (char c : compact) {
                if (c == '=') break;
                const char* at = strchr(kAlpha, c);
                if (!at || c == '\0') {
                    LOGE("plist: bad base64 in <data>");
                    return false;
                }
                val = (val << 6) | (int)(at - kAlpha);
                bits += 6;
                if (bits >= 8) {
                    bits -= 8;
                    raw += (char)((val >> bits) & 0xff);
                }
            }
            out = PlistValue::data(raw);
            return true;
        }
        // <string>, <integer>, <real>: same shape, different tag.
        const char* tags[] = {"<string>", "<integer>", "<real>"};
        const char* closes[] = {"</string>", "</integer>", "</real>"};
        for (int t = 0; t < 3; t++) {
            if (lit(tags[t])) {
                std::string text = parseText();
                if (!expect(closes[t])) return false;
                if (t == 0) {
                    out = PlistValue::string(text);
                } else if (t == 1) {
                    out = PlistValue::integer(strtoll(text.c_str(), nullptr, 10));
                } else {
                    out = PlistValue::real(strtod(text.c_str(), nullptr));
                }
                return true;
            }
        }
        LOGE("plist: unrecognized element near '%.24s'", p);
        return false;
    }
};

void escapeTo(const std::string& in, std::string& out) {
    for (char c : in) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        default: out += c;
        }
    }
}

void dumpTo(const PlistValue& v, std::string& out, int indent) {
    auto pad = [&](int n) { for (int i = 0; i < n; i++) out += "  "; };
    char tmp[64];
    switch (v.type) {
    case PlistType::Null: out += "<null/>"; break;
    case PlistType::Bool: out += v.b ? "<true/>" : "<false/>"; break;
    case PlistType::Int:
        snprintf(tmp, sizeof(tmp), "<integer>%lld</integer>", v.i);
        out += tmp;
        break;
    case PlistType::Real:
        snprintf(tmp, sizeof(tmp), "<real>%.17g</real>", v.r);
        out += tmp;
        break;
    case PlistType::String:
        out += "<string>";
        escapeTo(v.s, out);
        out += "</string>";
        break;
    case PlistType::Data: {
        static const char* kAlpha =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        out += "<data>";
        size_t i = 0;
        while (i + 2 < v.s.size()) {
            uint32_t trip = ((uint8_t)v.s[i] << 16) | ((uint8_t)v.s[i + 1] << 8) | (uint8_t)v.s[i + 2];
            out += kAlpha[(trip >> 18) & 63];
            out += kAlpha[(trip >> 12) & 63];
            out += kAlpha[(trip >> 6) & 63];
            out += kAlpha[trip & 63];
            i += 3;
        }
        size_t rem = v.s.size() - i;
        if (rem == 1) {
            uint32_t trip = (uint8_t)v.s[i] << 16;
            out += kAlpha[(trip >> 18) & 63];
            out += kAlpha[(trip >> 12) & 63];
            out += "==";
        } else if (rem == 2) {
            uint32_t trip = ((uint8_t)v.s[i] << 16) | ((uint8_t)v.s[i + 1] << 8);
            out += kAlpha[(trip >> 18) & 63];
            out += kAlpha[(trip >> 12) & 63];
            out += kAlpha[(trip >> 6) & 63];
            out += "=";
        }
        out += "</data>";
        break;
    }
    case PlistType::Array:
        if (v.arr.empty()) { out += "<array/>"; break; }
        out += "<array>\n";
        for (const auto& e : v.arr) {
            pad(indent + 1);
            dumpTo(e, out, indent + 1);
            out += '\n';
        }
        pad(indent);
        out += "</array>";
        break;
    case PlistType::Dict:
        if (v.dict.empty()) { out += "<dict/>"; break; }
        out += "<dict>\n";
        for (const auto& kv : v.dict) {
            pad(indent + 1);
            out += "<key>";
            escapeTo(kv.first, out);
            out += "</key>\n";
            pad(indent + 1);
            dumpTo(kv.second, out, indent + 1);
            out += '\n';
        }
        pad(indent);
        out += "</dict>";
        break;
    }
}

} // namespace

bool plist_parse(const char* xml, size_t len, PlistValue& out) {
    // UxPlay always answers with plist_to_bin(), so a binary document is the
    // normal case, not an oddity.
    if (len >= 8 && memcmp(xml, "bplist00", 8) == 0) {
        return bplist_parse(xml, len, out);
    }
    Parser ps{xml, xml + len, 0};
    if (!ps.skipPreamble()) return false;
    ps.skipWs();
    if (!ps.lit("<?xml")) {
        // some servers omit the declaration; that is fine
    } else {
        while (ps.p < ps.end && !ps.lit("?>")) ps.p++;
    }
    if (!ps.skipPreamble()) return false;
    if (!ps.expect("<plist")) return false;
    // swallow attributes on <plist ...>
    while (ps.p < ps.end && *ps.p != '>') ps.p++;
    if (ps.p < ps.end) ps.p++;
    if (!ps.parseValue(out, 64)) return false;
    ps.skipWs();
    if (!ps.expect("</plist>")) return false;
    return true;
}

bool plist_parse(const std::string& xml, PlistValue& out) {
    return plist_parse(xml.data(), xml.size(), out);
}

std::string plist_dump(const PlistValue& v) {
    std::string out;
    out += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    out += "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
           "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n";
    out += "<plist version=\"1.0\">\n";
    dumpTo(v, out, 0);
    out += "\n</plist>\n";
    return out;
}

std::string plist_dump_bin(const PlistValue& v) {
    return bplist_dump(v);
}

} // namespace fc
