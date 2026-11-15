// FlowCast Windows client - binary plist reader implementation.
#include "bplist.h"
#include "log.h"
#include "plist.h"

#include <cstring>
#include <vector>

namespace fc {
namespace {

// Reads a big-endian unsigned integer of `n` bytes.
uint64_t readBE(const uint8_t* p, size_t n) {
    uint64_t v = 0;
    for (size_t i = 0; i < n; i++) v = (v << 8) | p[i];
    return v;
}

struct Reader {
    const uint8_t* buf;
    size_t len;
    std::vector<size_t> offsets; // object index -> byte offset
    size_t offsetSize = 1;       // bytes per offset-table entry
    size_t refSize = 1;          // bytes per object reference inside collections

    bool fail(const char* why) const {
        LOGE("bplist: %s", why);
        return false;
    }

    // Reads the count that follows a 0x?F marker.
    bool readExtendedCount(size_t& pos, uint64_t& count) {
        if (pos >= len) return false;
        const uint8_t m = buf[pos++];
        if ((m & 0xf0) != 0x10) return false; // must be an int marker
        const size_t n = (size_t)1 << (m & 0x0f);
        if (n < 1 || n > 8 || pos + n > len) return false;
        count = readBE(buf + pos, n);
        pos += n;
        return true;
    }

    // Parses the object at byte offset `off` into `out`. `depth` guards against
    // a corrupt document building an infinitely deep structure.
    bool parseAt(size_t off, PlistValue& out, int depth) {
        if (depth > 64) return fail("nesting too deep");
        if (off >= len) return fail("object offset past end");
        size_t pos = off;
        const uint8_t marker = buf[pos++];

        // collections: 0xAn = array of n items, 0xDn = dict of n key/value pairs
        if ((marker & 0xf0) == 0xa0 || (marker & 0xf0) == 0xd0) {
            const bool isDict = (marker & 0xf0) == 0xd0;
            uint64_t count = marker & 0x0f;
            if (count == 0x0f && !readExtendedCount(pos, count)) return fail("bad collection length");
            if (count > 4096) return fail("unreasonable collection length");

            // A dict of n entries carries 2n references: all n keys first, then
            // all n values. Reading only n of them silently corrupts the parse.
            const uint64_t refCount = isDict ? count * 2 : count;
            std::vector<size_t> refs;
            refs.reserve((size_t)refCount);
            for (uint64_t i = 0; i < refCount; i++) {
                if (pos + refSize > len) return fail("truncated reference list");
                refs.push_back((size_t)readBE(buf + pos, refSize));
                pos += refSize;
            }

            if (isDict) {
                out.type = PlistType::Dict;
                out.dict.clear();
                for (uint64_t i = 0; i < count; i++) {
                    PlistValue key, val;
                    if (!objectAt(refs[i], key, depth + 1)) return false;
                    if (key.type != PlistType::String) return fail("non-string dict key");
                    if (!objectAt(refs[count + i], val, depth + 1)) return false;
                    out.dict.emplace_back(key.s, std::move(val));
                }
            } else {
                out.type = PlistType::Array;
                out.arr.clear();
                for (uint64_t i = 0; i < count; i++) {
                    PlistValue v;
                    if (!objectAt(refs[i], v, depth + 1)) return false;
                    out.arr.push_back(std::move(v));
                }
            }
            return true;
        }

        switch (marker) {
        case 0x00:
            out.type = PlistType::Null;
            return true;
        case 0x08:
            out.type = PlistType::Bool;
            out.b = false;
            return true;
        case 0x09:
            out.type = PlistType::Bool;
            out.b = true;
            return true;
        default:
            break;
        }

        const uint8_t hi = marker & 0xf0;
        const uint8_t lo = marker & 0x0f;
        switch (hi) {
        case 0x10: { // int of 2^lo bytes (0x10=1, 0x11=2, 0x12=4, 0x13=8, 0x14=16)
            const size_t n = (size_t)1 << lo;
            if (pos + n > len) return fail("truncated int");
            // 0x14 (16-byte) is real: Apple encodes /info `features` as a
            // 128-bit int, so keying the parser to 8 bytes rejected every
            // AirPlay 2 receiver's info plist outright. The value is
            // big-endian, so the low 64 bits - the only part any frame we
            // read uses - are the LAST eight bytes.
            const size_t take = n < 8 ? n : 8;
            out.type = PlistType::Int;
            out.i = (long long)readBE(buf + pos + (n - take), take);
            return true;
        }
        case 0x20: { // real of 2^lo bytes (0x22 = float32, 0x23 = float64)
            const size_t n = (size_t)1 << lo;
            if (n != 4 && n != 8) return fail("unsupported real width");
            if (pos + n > len) return fail("truncated real");
            out.type = PlistType::Real;
            if (n == 4) {
                uint32_t bits = (uint32_t)readBE(buf + pos, 4);
                float f;
                memcpy(&f, &bits, 4);
                out.r = (double)f;
            } else {
                uint64_t bits = readBE(buf + pos, 8);
                double d;
                memcpy(&d, &bits, 8);
                out.r = d;
            }
            return true;
        }
        case 0x30: // date (seconds since 2001-01-01 as a float64)
            if (lo != 3) return fail("unsupported date width");
            if (pos + 8 > len) return fail("truncated date");
            out.type = PlistType::Real;
            {
                uint64_t bits = readBE(buf + pos, 8);
                double d;
                memcpy(&d, &bits, 8);
                out.r = d;
            }
            return true;
        case 0x40: { // data
            size_t n = lo;
            if (n == 0x0f) {
                uint64_t c = 0;
                if (!readExtendedCount(pos, c)) return fail("bad data length");
                n = (size_t)c;
            }
            if (pos + n > len) return fail("truncated data");
            out.type = PlistType::Data;
            out.s.assign((const char*)buf + pos, n);
            return true;
        }
        case 0x50: { // ASCII string of lo chars
            size_t n = lo;
            if (n == 0x0f) {
                uint64_t c = 0;
                if (!readExtendedCount(pos, c)) return fail("bad string length");
                n = (size_t)c;
            }
            if (pos + n > len) return fail("truncated string");
            out.type = PlistType::String;
            out.s.assign((const char*)buf + pos, n);
            return true;
        }
        case 0x60: { // UTF-16 string of lo units -> UTF-8
            size_t units = lo;
            if (units == 0x0f) {
                uint64_t c = 0;
                if (!readExtendedCount(pos, c)) return fail("bad string length");
                units = (size_t)c;
            }
            if (pos + units * 2 > len) return fail("truncated utf16 string");
            out.type = PlistType::String;
            out.s.clear();
            out.s.reserve(units);
            for (size_t i = 0; i < units; i++) {
                uint32_t u = (uint32_t)readBE(buf + pos + i * 2, 2);
                // A high surrogate must be immediately followed by its low
                // half. Anything else is malformed and is folded to U+FFFD
                // rather than leaked into the output as a raw half.
                if (u >= 0xd800 && u <= 0xdbff) {
                    if (i + 1 < units) {
                        const uint32_t lo2 = (uint32_t)readBE(buf + pos + (i + 1) * 2, 2);
                        if (lo2 >= 0xdc00 && lo2 <= 0xdfff) {
                            u = 0x10000 + ((u - 0xd800) << 10) + (lo2 - 0xdc00);
                            i++;
                        } else {
                            u = 0xfffd;
                        }
                    } else {
                        u = 0xfffd;
                    }
                } else if (u >= 0xdc00 && u <= 0xdfff) {
                    u = 0xfffd;
                }
                if (u < 0x80) {
                    out.s.push_back((char)u);
                } else if (u < 0x800) {
                    out.s.push_back((char)(0xc0 | (u >> 6)));
                    out.s.push_back((char)(0x80 | (u & 0x3f)));
                } else if (u < 0x10000) {
                    out.s.push_back((char)(0xe0 | (u >> 12)));
                    out.s.push_back((char)(0x80 | ((u >> 6) & 0x3f)));
                    out.s.push_back((char)(0x80 | (u & 0x3f)));
                } else {
                    out.s.push_back((char)(0xf0 | (u >> 18)));
                    out.s.push_back((char)(0x80 | ((u >> 12) & 0x3f)));
                    out.s.push_back((char)(0x80 | ((u >> 6) & 0x3f)));
                    out.s.push_back((char)(0x80 | (u & 0x3f)));
                }
            }
            return true;
        }
        case 0x80: // uid of 2^lo bytes
            return fail("uid objects are not used by RAOP");
        default:
            return fail("unknown object marker");
        }
    }

    bool objectAt(size_t index, PlistValue& out, int depth) {
        if (index >= offsets.size()) return fail("object index out of range");
        return parseAt(offsets[index], out, depth);
    }
};

} // namespace

bool bplist_parse(const char* data, size_t len, PlistValue& out) {
    if (len < 40 || memcmp(data, "bplist00", 8) != 0) {
        LOGE("bplist: missing magic");
        return false;
    }
    const uint8_t* buf = reinterpret_cast<const uint8_t*>(data);

    // The 32-byte trailer sits at the very end of the buffer. Byte 6 is the
    // size of each offset-table entry; byte 7 is the size of each object
    // reference inside a collection. They are independent fields and swapping
    // them breaks every document with more than 255 objects.
    const uint8_t* tr = buf + len - 32;
    const size_t offsetSize = tr[6];
    const size_t refSize = tr[7];
    const uint64_t numObjects = readBE(tr + 8, 8);
    // NOTE: bytes [16..23] hold the *index* of the root object in the offset
    // table, not a byte offset into the buffer. Reading it as an offset lands
    // on the "bplist00" magic and every parse silently produces garbage.
    const uint64_t rootIndex = readBE(tr + 16, 8);
    const uint64_t tableOffset = readBE(tr + 24, 8);
    if (offsetSize < 1 || offsetSize > 8) {
        LOGE("bplist: bad offset size %zu", offsetSize);
        return false;
    }
    if (refSize < 1 || refSize > 8) {
        LOGE("bplist: bad object reference size %zu", refSize);
        return false;
    }
    if (numObjects == 0 || numObjects > 100000) {
        LOGE("bplist: unreasonable object count %llu", (unsigned long long)numObjects);
        return false;
    }
    if (rootIndex >= numObjects) {
        LOGE("bplist: root object index %llu out of range", (unsigned long long)rootIndex);
        return false;
    }
    if (tableOffset + numObjects * offsetSize > len) {
        LOGE("bplist: offset table does not fit");
        return false;
    }

    Reader r;
    r.buf = buf;
    r.len = len;
    r.offsetSize = offsetSize;
    r.refSize = refSize;
    r.offsets.reserve((size_t)numObjects);
    for (uint64_t i = 0; i < numObjects; i++) {
        r.offsets.push_back((size_t)readBE(buf + tableOffset + i * offsetSize, offsetSize));
    }

    if (!r.parseAt(r.offsets[rootIndex], out, 0)) return false;
    return true;
}

// ---------------------------------------------------------------------------
// writer
// ---------------------------------------------------------------------------

namespace {

// One node of the flattened object graph. Containers keep their child indices
// in the exact order the format wants them emitted: for a dict that is all
// keys first, then all values.
struct Obj {
    bool isKeyString = false;
    bool isDict = false;
    const PlistValue* val = nullptr;
    std::string str;               // for isKeyString
    std::vector<size_t> refs;      // for containers, in emitted order
    size_t size = 0;               // encoded byte length
    size_t offset = 0;
};

size_t addValue(const PlistValue& v, std::vector<Obj>& objs) {
    const size_t self = objs.size();
    Obj o;
    objs.push_back(o);
    if (v.type == PlistType::Array) {
        for (const auto& c : v.arr) {
            const size_t idx = addValue(c, objs);
            objs[self].refs.push_back(idx);
        }
    } else if (v.type == PlistType::Dict) {
        objs[self].isDict = true;
        std::vector<size_t> keys, vals;
        for (const auto& kv : v.dict) {
            Obj k;
            k.isKeyString = true;
            k.str = kv.first;
            keys.push_back(objs.size());
            objs.push_back(k);
        }
        for (const auto& kv : v.dict) vals.push_back(addValue(kv.second, objs));
        objs[self].refs = keys;
        objs[self].refs.insert(objs[self].refs.end(), vals.begin(), vals.end());
    } else {
        objs[self].val = &v;
    }
    return self;
}

size_t intWidth(long long v) {
    if (v < 0) return 8;
    if (v < (1LL << 8)) return 1;
    if (v < (1LL << 16)) return 2;
    if (v < (1LL << 32)) return 4;
    return 8;
}

// Byte length of a count-carrying token (0x?n, or 0x?F followed by an int).
size_t countSize(size_t count) {
    if (count < 15) return 1;
    return 1 + 1 + intWidth((long long)count);
}

void putMarkerCount(std::string& out, uint8_t token, size_t count) {
    if (count < 15) {
        out.push_back((char)(token | (uint8_t)count));
        return;
    }
    out.push_back((char)(token | 0x0f));
    const long long c = (long long)count;
    const size_t w = intWidth(c);
    out.push_back((char)(0x10 | (uint8_t)(w == 1 ? 0 : w == 2 ? 1 : w == 4 ? 2 : 3)));
    for (size_t i = 0; i < w; i++) out.push_back((char)((uint64_t)c >> (8 * (w - 1 - i))));
}

void putBE(std::string& out, uint64_t v, size_t w) {
    for (size_t i = 0; i < w; i++) out.push_back((char)((v >> (8 * (w - 1 - i))) & 0xff));
}

bool allAscii(const std::string& s) {
    for (unsigned char c : s) {
        if (c >= 0x80) return false;
    }
    return true;
}

// Decodes one UTF-8 code point from s[i], advancing i past it. Malformed input
// yields U+FFFD and advances a single byte, so a garbage device name degrades
// instead of spinning.
uint32_t nextUtf8(const std::string& s, size_t& i) {
    const unsigned char c = (unsigned char)s[i];
    uint32_t cp;
    size_t extra;
    if (c < 0x80)                  { cp = c;            extra = 0; }
    else if ((c & 0xe0) == 0xc0)   { cp = c & 0x1f;     extra = 1; }
    else if ((c & 0xf0) == 0xe0)   { cp = c & 0x0f;     extra = 2; }
    else if ((c & 0xf8) == 0xf0)   { cp = c & 0x07;     extra = 3; }
    else                           { i++; return 0xfffd; }
    if (i + extra >= s.size())     { i++; return 0xfffd; }
    for (size_t k = 1; k <= extra; k++) {
        const unsigned char cc = (unsigned char)s[i + k];
        if ((cc & 0xc0) != 0x80)   { i++; return 0xfffd; }
        cp = (cp << 6) | (cc & 0x3f);
    }
    i += 1 + extra;
    return cp;
}

// A UTF-16 string marker counts 16-bit *units*, not bytes and not source
// characters, so astral code points take two units.
size_t utf16UnitCount(const std::string& s) {
    size_t units = 0;
    size_t i = 0;
    while (i < s.size()) units += (nextUtf8(s, i) >= 0x10000) ? 2 : 1;
    return units;
}

void appendUtf16BE(std::string& out, const std::string& s) {
    size_t i = 0;
    while (i < s.size()) {
        const uint32_t cp = nextUtf8(s, i);
        if (cp >= 0x10000) {
            const uint32_t v = cp - 0x10000;
            const uint16_t hi = (uint16_t)(0xd800 + (v >> 10));
            const uint16_t lo = (uint16_t)(0xdc00 + (v & 0x3ff));
            out.push_back((char)(hi >> 8)); out.push_back((char)(hi & 0xff));
            out.push_back((char)(lo >> 8)); out.push_back((char)(lo & 0xff));
        } else {
            out.push_back((char)(cp >> 8)); out.push_back((char)(cp & 0xff));
        }
    }
}

} // namespace

std::string bplist_dump(const PlistValue& root) {
    std::vector<Obj> objs;
    addValue(root, objs);
    const size_t numObjects = objs.size();
    if (numObjects == 0) return std::string();

    const size_t refSize = numObjects < 256 ? 1 : (numObjects < 65536 ? 2 : 4);

    // Pass 1: encoded size of every object, given the reference width.
    for (auto& o : objs) {
        if (o.isKeyString || (o.val && o.val->type == PlistType::String)) {
            const std::string& s = o.isKeyString ? o.str : o.val->s;
            // Pure ASCII is emitted as-is; anything else is widened to
            // UTF-16BE, which costs one 16-bit unit per BMP code point and two
            // per astral one. The count in the marker is that unit count.
            if (allAscii(s)) {
                o.size = countSize(s.size()) + s.size();
            } else {
                const size_t units = utf16UnitCount(s);
                o.size = countSize(units) + units * 2;
            }
        } else if (o.val && o.val->type == PlistType::Data) {
            o.size = countSize(o.val->s.size()) + o.val->s.size();
        } else if (o.val && o.val->type == PlistType::Int) {
            o.size = 1 + intWidth(o.val->i);
        } else if (o.val && o.val->type == PlistType::Real) {
            o.size = 1 + 8;
        } else if (o.val && (o.val->type == PlistType::Bool || o.val->type == PlistType::Null)) {
            o.size = 1;
        } else {
            // A dict of n entries carries 2n references (all keys, then all
            // values), but the marker's low nibble counts *entries*, not
            // references. Using refs.size() here makes a 7-entry dict claim 14
            // entries and the receiver walks off the end of the ref list.
            const size_t entries = o.isDict ? o.refs.size() / 2 : o.refs.size();
            o.size = countSize(entries) + o.refs.size() * refSize;
        }
    }

    size_t tableOffset = 8;
    for (const auto& o : objs) tableOffset += o.size;
    const size_t offsetSize = tableOffset < 256 ? 1 : (tableOffset < 65536 ? 2 : 4);

    // Pass 2: assign offsets.
    size_t cur = 8;
    for (auto& o : objs) {
        o.offset = cur;
        cur += o.size;
    }

    std::string out;
    out.append("bplist00", 8);
    for (size_t i = 0; i < numObjects; i++) {
        const Obj& o = objs[i];
        if (o.isKeyString || (o.val && o.val->type == PlistType::String)) {
            const std::string& s = o.isKeyString ? o.str : o.val->s;
            if (allAscii(s)) {
                putMarkerCount(out, 0x50, s.size());
                out.append(s);
            } else {
                putMarkerCount(out, 0x60, utf16UnitCount(s));
                appendUtf16BE(out, s);
            }
        } else if (o.val && o.val->type == PlistType::Data) {
            putMarkerCount(out, 0x40, o.val->s.size());
            out.append(o.val->s);
        } else if (o.val && o.val->type == PlistType::Int) {
            const long long v = o.val->i;
            const size_t w = intWidth(v);
            out.push_back((char)(0x10 | (uint8_t)(w == 1 ? 0 : w == 2 ? 1 : w == 4 ? 2 : 3)));
            putBE(out, (uint64_t)v, w);
        } else if (o.val && o.val->type == PlistType::Real) {
            out.push_back((char)0x23);
            uint64_t bits;
            memcpy(&bits, &o.val->r, 8);
            putBE(out, bits, 8);
        } else if (o.val && o.val->type == PlistType::Bool) {
            out.push_back((char)(o.val->b ? 0x09 : 0x08));
        } else if (o.val && o.val->type == PlistType::Null) {
            out.push_back((char)0x00);
        } else {
            // An empty dict and an empty array share the "no children" shape,
            // so the container kind has to be carried explicitly.
            const uint8_t token = o.isDict ? 0xd0 : 0xa0;
            const size_t entries = o.isDict ? o.refs.size() / 2 : o.refs.size();
            putMarkerCount(out, token, entries);
            // A collection reference is an *index into the object table*, not a
            // byte offset. Writing offsets here makes every document with more
            // than one object unparseable, and our own reader is the first to
            // notice.
            for (const size_t r : o.refs) putBE(out, r, refSize);
        }
    }

    for (size_t i = 0; i < numObjects; i++) putBE(out, objs[i].offset, offsetSize);

    // trailer: 5 unused, sortVersion, offsetIntSize, objectRefSize, numObjects,
    // rootObjectIndex, offsetTableOffset - all big-endian.
    std::string tr;
    tr.append(5, '\0');
    tr.push_back((char)0x00); // sort version
    tr.push_back((char)offsetSize);
    tr.push_back((char)refSize);
    putBE(tr, numObjects, 8);
    putBE(tr, 0, 8); // the root is always object 0
    putBE(tr, tableOffset, 8);
    out.append(tr);
    return out;
}

} // namespace fc
