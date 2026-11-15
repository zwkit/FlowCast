// Known-answer test for the binary plist reader, plus a round-trip suite for
// the writer.
//
// The vectors in test/vectors/bplist_vectors.h were produced by Python's
// plistlib in FMT_BINARY mode, which is the same format UxPlay's plist_to_bin()
// emits, so the reader checks below are an independent reference rather than a
// self-consistency check. Regenerate with tools/gen_bplist_vectors.py.
//
// The writer is checked two ways: a byte-exact known answer, and a
// dump -> parse -> structural-compare round trip. The round trip is the net
// that catches the classic writer bug - a dict of n entries carrying 2n
// references but advertising n as its reference count - which our own reader
// would otherwise silently accept.
#include "src/bplist.h"
#include "src/log.h"
#include "src/plist.h"
#include "vectors/bplist_vectors.h"

#include <cstdio>
#include <cstring>

static int fails = 0;
static void check(const char* what, bool ok) {
    printf("%-46s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) fails++;
}

// Structural equality, independent of the order keys were emitted in.
static bool sameValue(const fc::PlistValue& a, const fc::PlistValue& b) {
    if (a.type != b.type) return false;
    switch (a.type) {
    case fc::PlistType::Null: return true;
    case fc::PlistType::Bool: return a.b == b.b;
    case fc::PlistType::Int: return a.i == b.i;
    case fc::PlistType::Real: return a.r == b.r;
    case fc::PlistType::String:
    case fc::PlistType::Data: return a.s == b.s;
    case fc::PlistType::Array: {
        if (a.arr.size() != b.arr.size()) return false;
        for (size_t i = 0; i < a.arr.size(); i++) {
            if (!sameValue(a.arr[i], b.arr[i])) return false;
        }
        return true;
    }
    case fc::PlistType::Dict: {
        if (a.dict.size() != b.dict.size()) return false;
        for (const auto& kv : a.dict) {
            const fc::PlistValue* other = b.find(kv.first.c_str());
            if (!other || !sameValue(kv.second, *other)) return false;
        }
        return true;
    }
    }
    return false;
}

static void dumpHex(const char* label, const std::string& s) {
    printf("       %s (%zu bytes):\n", label, s.size());
    for (size_t i = 0; i < s.size(); i++) {
        printf("%02x", (unsigned char)s[i]);
        if (i % 16 == 15) printf("\n");
    }
    if (s.size() % 16) printf("\n");
}

static void roundTrip(const char* what, const fc::PlistValue& v) {
    const std::string bin = fc::plist_dump_bin(v);
    fc::PlistValue back;
    const bool ok = !bin.empty() && fc::plist_parse(bin, back);
    check(what, ok && sameValue(v, back));
    if (!ok) dumpHex("dumped", bin);
    else if (!sameValue(v, back)) dumpHex("dumped", bin);
}

int main() {
    fc::log_init(fc::LogLevel::Error, nullptr, nullptr);

    // --- SETUP #1 -----------------------------------------------------------
    {
        fc::PlistValue v;
        const bool ok = fc::bplist_parse((const char*)kSetup1, sizeof(kSetup1), v);
        check("bplist SETUP #1 parses", ok && v.type == fc::PlistType::Dict);
        if (ok) {
            const fc::PlistValue* tp = v.find("timingPort");
            const fc::PlistValue* ep = v.find("eventPort");
            const fc::PlistValue* id = v.find("deviceID");
            check("  timingPort", tp && tp->asInt() == 63680);
            check("  eventPort", ep && ep->asInt() == 0);
            check("  deviceID string", id && id->asString() == "AA:BB:CC:DD:EE:FF");
        }
    }

    // --- SETUP #2: array of stream dicts ------------------------------------
    {
        fc::PlistValue v;
        const bool ok = fc::bplist_parse((const char*)kStreams, sizeof(kStreams), v);
        check("bplist SETUP #2 parses", ok);
        if (ok) {
            const fc::PlistValue* st = v.find("streams");
            check("  streams array of 2", st && st->arr.size() == 2);
            if (st && st->arr.size() == 2) {
                const fc::PlistValue& s0 = st->arr[0];
                const fc::PlistValue& s1 = st->arr[1];
                check("  mirror stream type 110",
                      s0.find("type") && s0.find("type")->asInt() == 110);
                check("  mirror dataPort 7100",
                      s0.find("dataPort") && s0.find("dataPort")->asInt() == 7100);
                check("  audio stream type 96", s1.find("type") && s1.find("type")->asInt() == 96);
                check("  audio dataPort 6100",
                      s1.find("dataPort") && s1.find("dataPort")->asInt() == 6100);
                check("  audio controlPort 6200",
                      s1.find("controlPort") && s1.find("controlPort")->asInt() == 6200);
            }
        }
    }

    // --- every scalar type --------------------------------------------------
    {
        fc::PlistValue v;
        const bool ok = fc::bplist_parse((const char*)kMixed, sizeof(kMixed), v);
        check("bplist mixed types parse", ok && v.type == fc::PlistType::Dict);
        if (ok) {
            const fc::PlistValue* t = v.find("b");
            const fc::PlistValue* f = v.find("f");
            const fc::PlistValue* r = v.find("r");
            const fc::PlistValue* big = v.find("big");
            const fc::PlistValue* neg = v.find("neg");
            const fc::PlistValue* d = v.find("d");
            const fc::PlistValue* n = v.find("nested");
            check("  true", t && t->asBool());
            check("  false", f && !f->asBool());
            check("  real (float64)", r && r->asReal() > 0.166666 && r->asReal() < 0.166667);
            check("  24-bit uint", big && big->asInt() == 0x3fffffc);
            check("  negative int", neg && neg->asInt() == -5);
            check("  data", d && d->s == std::string("\x00\x01\xfe\xff", 4));
            check("  nested dict", n && n->type == fc::PlistType::Dict);
            if (n) {
                const fc::PlistValue* a = n->find("a");
                check("  nested array of 3", a && a->arr.size() == 3);
                if (a && a->arr.size() == 3) {
                    check("    array[0] int", a->arr[0].asInt() == 1);
                    check("    array[1] string", a->arr[1].asString() == "two");
                    check("    array[2] real", a->arr[2].asReal() > 3.49 && a->arr[2].asReal() < 3.51);
                }
            }
        }
    }

    // --- empty and deeply nested documents ----------------------------------
    {
        fc::PlistValue v;
        const bool ok = fc::bplist_parse((const char*)kEmpty, sizeof(kEmpty), v);
        check("bplist empty dict", ok && v.type == fc::PlistType::Dict && v.dict.empty());
    }
    {
        fc::PlistValue v;
        const bool ok = fc::bplist_parse((const char*)kDeep, sizeof(kDeep), v);
        check("bplist deep nesting", ok);
        if (ok) {
            const fc::PlistValue* a = v.find("a");
            const fc::PlistValue* b = a ? a->find("b") : nullptr;
            const fc::PlistValue* c = b ? b->find("c") : nullptr;
            const fc::PlistValue* d = c ? c->find("d") : nullptr;
            const fc::PlistValue* e = d ? d->find("e") : nullptr;
            check("  five levels down", e && e->arr.size() == 3 && e->arr[2].asInt() == 3);
        }
    }

    // --- plist_parse dispatches on the magic --------------------------------
    {
        fc::PlistValue v;
        const bool ok = fc::plist_parse((const char*)kSetup1, sizeof(kSetup1), v);
        check("plist_parse auto-detects binary", ok && v.find("timingPort") != nullptr);
    }

    // --- non-ASCII strings --------------------------------------------------
    {
        fc::PlistValue v;
        const bool ok = fc::bplist_parse((const char*)kUnicode, sizeof(kUnicode), v);
        check("bplist unicode vector parses", ok && v.type == fc::PlistType::Dict);
        if (ok) {
            // "café 中文 🎉" - accented, CJK and an astral code point, the last
            // of which costs two UTF-16 units.
            const fc::PlistValue* n = v.find("name");
            check("  UTF-16BE -> UTF-8", n && n->asString() == "caf\xc3\xa9 \xe4\xb8\xad\xe6\x96\x87 \xf0\x9f\x8e\x89");
            check("  ascii stays ascii", v.find("ascii") && v.find("ascii")->asString() == "plain");
            check("  15-char string", v.find("u15") && v.find("u15")->asString() == "0123456789abcde");
        }
    }

    // --- malformed input is rejected, not crashed on ------------------------
    {
        fc::PlistValue v;
        check("bplist rejects short input", !fc::bplist_parse("bplist00", 8, v));
    }
    {
        // Magic present, but the trailer points the offset table past the end.
        // The trailer starts at len-32 = 16: [16+6]=offsetIntSize,
        // [16+7]=objectRefSize, [16+8..16+15]=numObjects, [16+24..]=tableOffset.
        unsigned char bad[48];
        memset(bad, 0, sizeof(bad));
        memcpy(bad, "bplist00", 8);
        bad[22] = 0x01; // offsetIntSize = 1
        bad[23] = 0x01; // objectRefSize = 1
        bad[31] = 0x01; // numObjects = 1
        bad[40] = 0x64; // offsetTableOffset = 100, past a 48-byte buffer
        fc::PlistValue v;
        check("bplist rejects bogus trailer",
              !fc::bplist_parse((const char*)bad, sizeof(bad), v));
    }
    {
        // A well-formed document cut short: the trailer no longer lines up.
        unsigned char trunc[32];
        memcpy(trunc, kSetup1, sizeof(trunc));
        fc::PlistValue v;
        check("bplist rejects truncated doc",
              !fc::bplist_parse((const char*)trunc, sizeof(trunc), v));
    }

    // --- writer: byte-exact known answer ------------------------------------
    // Hand-derived and confirmed byte-identical to Python's plistlib for the
    // same document, so this pins the emitter's layout rather than trusting it
    // to agree with itself.
    {
        // {"a": 1}
        //   obj0 = dict of 1 entry -> "d1" + key ref 1 + value ref 2
        //   obj1 = ASCII string of 1 -> "51 61"
        //   obj2 = int of 1 byte     -> "10 01"
        static const unsigned char kExpected[] = {
            0x62,0x70,0x6c,0x69,0x73,0x74,0x30,0x30,             // "bplist00"
            0xd1, 0x01, 0x02,                                     // dict, 1 entry
            0x51, 0x61,                                           // "a"
            0x10, 0x01,                                           // int 1
            0x08, 0x0b, 0x0d,                                     // offset table
            0x00,0x00,0x00,0x00,0x00, 0x00, 0x01, 0x01,           // trailer head
            0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x03,              // numObjects = 3
            0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,              // root index 0
            0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0f,              // table at 15
        };
        fc::PlistValue v = fc::PlistValue::dictOf({{"a", fc::PlistValue::integer(1)}});
        const std::string bin = fc::plist_dump_bin(v);
        const bool ok = bin.size() == sizeof(kExpected) &&
                        memcmp(bin.data(), kExpected, sizeof(kExpected)) == 0;
        check("bplist writer byte-exact {'a':1}", ok);
        if (!ok) dumpHex("dumped", bin);
    }

    // --- writer: round trip of everything we actually send -------------------
    {
        // The real SETUP #1 body: 7 entries mixing data, int and string.
        fc::PlistValue setup1 = fc::PlistValue::dictOf({
            {"ekey",           fc::PlistValue::data(std::string(72, '\x5a'))},
            {"eiv",            fc::PlistValue::data(std::string(16, '\xa7'))},
            {"timingPort",     fc::PlistValue::integer(12345)},
            {"timingProtocol", fc::PlistValue::string("NTP")},
            {"deviceID",       fc::PlistValue::string("00:11:22:33:44:55")},
            {"model",          fc::PlistValue::string("WindowsPC")},
            {"name",           fc::PlistValue::string("Windows PC")},
        });
        roundTrip("writer round-trip SETUP #1 body", setup1);

        // The real SETUP #2 body: a dict holding an array of two stream dicts.
        // This is the shape that exercises a dict inside an array inside a dict.
        fc::PlistValue setup2 = fc::PlistValue::dictOf({
            {"streams", fc::PlistValue::array({
                fc::PlistValue::dictOf({
                    {"streamConnectionID", fc::PlistValue::integer(1)},
                    {"type",               fc::PlistValue::integer(110)},
                }),
                fc::PlistValue::dictOf({
                    {"controlPort", fc::PlistValue::integer(6101)},
                    {"ct",          fc::PlistValue::integer(4)},
                    {"isMedia",     fc::PlistValue::boolean(false)},
                    {"spf",         fc::PlistValue::integer(1024)},
                    {"audioFormat", fc::PlistValue::integer(0)},
                    {"type",        fc::PlistValue::integer(96)},
                    {"usingScreen", fc::PlistValue::boolean(true)},
                }),
            })},
        });
        roundTrip("writer round-trip SETUP #2 body", setup2);

        roundTrip("writer round-trip empty dict", fc::PlistValue::dictOf({}));
        roundTrip("writer round-trip empty array", fc::PlistValue::array({}));

        // Every scalar type, including the ones RAOP does not use but the
        // format defines, so a future caller cannot silently corrupt them.
        roundTrip("writer round-trip all scalars", fc::PlistValue::dictOf({
            {"null",  fc::PlistValue()},
            {"true",  fc::PlistValue::boolean(true)},
            {"false", fc::PlistValue::boolean(false)},
            {"i8",    fc::PlistValue::integer(200)},
            {"i16",   fc::PlistValue::integer(40000)},
            {"i32",   fc::PlistValue::integer(3000000000LL)},
            {"i64",   fc::PlistValue::integer(-9000000000LL)},
            {"neg",   fc::PlistValue::integer(-5)},
            {"real",  fc::PlistValue::real(0.16666666666666666)},
            {"str",   fc::PlistValue::string("hello")},
            {"data",  fc::PlistValue::data(std::string("\x00\x01\xfe\xff", 4))},
        }));

        // A 15-char string and a 20-byte data both cross the "count in the low
        // nibble" boundary and must switch to the 0x?F + int encoding.
        roundTrip("writer round-trip extended counts", fc::PlistValue::dictOf({
            {"s15", fc::PlistValue::string("0123456789abcde")},
            {"s40", fc::PlistValue::string(std::string(40, 'x'))},
            {"d20", fc::PlistValue::data(std::string(20, '\x11'))},
            {"d300", fc::PlistValue::data(std::string(300, '\x22'))},
        }));

        // Non-ASCII widens to UTF-16BE, which the reader must fold back. The
        // emoji is an astral code point, so it occupies a surrogate pair.
        roundTrip("writer round-trip non-ASCII string",
                  fc::PlistValue::dictOf({{"u", fc::PlistValue::string("caf\xc3\xa9")}}));
        roundTrip("writer round-trip CJK + emoji string",
                  fc::PlistValue::dictOf({{"u", fc::PlistValue::string("\xe4\xb8\xad\xe6\x96\x87 \xf0\x9f\x8e\x89")}}));
        // A non-ASCII *key* takes the same path as a non-ASCII value.
        roundTrip("writer round-trip non-ASCII key",
                  fc::PlistValue::dictOf({{"\xc3\xa9", fc::PlistValue::integer(7)}}));

        // Nesting deeper than the round trips above.
        fc::PlistValue deep = fc::PlistValue::dictOf({
            {"a", fc::PlistValue::dictOf({
                {"b", fc::PlistValue::dictOf({
                    {"c", fc::PlistValue::array({
                        fc::PlistValue::integer(1),
                        fc::PlistValue::string("two"),
                        fc::PlistValue::real(3.5),
                    })},
                })},
            })},
        });
        roundTrip("writer round-trip deep nesting", deep);

        // More than 255 objects forces a 2-byte object-reference width, which
        // is where a writer that hardcodes refSize = 1 falls apart.
        {
            std::vector<fc::PlistValue> many;
            for (int i = 0; i < 300; i++) {
                many.push_back(fc::PlistValue::dictOf({{"i", fc::PlistValue::integer(i)}}));
            }
            roundTrip("writer round-trip >255 objects (refSize 2)",
                      fc::PlistValue::dictOf({{"many", fc::PlistValue::array(std::move(many))}}));
        }

        // A >64KB document forces a 2-byte offset-table entry width.
        {
            roundTrip("writer round-trip >64KB doc (offsetSize 2)",
                      fc::PlistValue::dictOf({{"blob", fc::PlistValue::data(std::string(70000, '\x33'))}}));
        }
    }

    // --- writer: the Python vectors survive a re-emit ------------------------
    {
        fc::PlistValue v;
        const bool ok = fc::bplist_parse((const char*)kSetup1, sizeof(kSetup1), v);
        check("writer re-emits SETUP #1 vector", ok);
        if (ok) roundTrip("  re-emitted SETUP #1", v);
    }
    {
        fc::PlistValue v;
        const bool ok = fc::bplist_parse((const char*)kStreams, sizeof(kStreams), v);
        check("writer re-emits SETUP #2 vector", ok);
        if (ok) roundTrip("  re-emitted SETUP #2", v);
    }
    {
        fc::PlistValue v;
        const bool ok = fc::bplist_parse((const char*)kMixed, sizeof(kMixed), v);
        check("writer re-emits mixed-types vector", ok);
        if (ok) roundTrip("  re-emitted mixed types", v);
    }
    {
        fc::PlistValue v;
        const bool ok = fc::bplist_parse((const char*)kEmpty, sizeof(kEmpty), v);
        check("writer re-emits empty-dict vector", ok);
        if (ok) roundTrip("  re-emitted empty dict", v);
    }
    {
        fc::PlistValue v;
        const bool ok = fc::bplist_parse((const char*)kUnicode, sizeof(kUnicode), v);
        check("writer re-emits unicode vector", ok);
        if (ok) roundTrip("  re-emitted unicode", v);
    }

    // --- 128-bit integer (marker 0x14) -----------------------------------
    // Real case, not hypothetical: every AirPlay receiver that answers
    // `GET /info` encodes `features` as a 16-byte int, and rejecting it made
    // the whole info plist unreadable - which is exactly the document we need
    // to decide whether a device still supports the old FairPlay handshake.
    // Value here is Sony TV's, 0x04005A7FFEF7.
    {
        std::string doc = "bplist00";
        std::string obj("\x14", 1);
        const unsigned char kVal[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 0, 0x5a, 0x7f, 0xfe, 0xf7};
        obj.append((const char*)kVal, sizeof(kVal));
        const size_t objOffset = doc.size();
        doc += obj;
        const size_t offsetTable = doc.size();
        doc += (char)objOffset;
        doc.append(6, '\0');
        doc += (char)1; // offset int size
        doc += (char)1; // object ref size
        for (int shift = 56; shift >= 0; shift -= 8) doc += (char)((1ull >> shift) & 0xff);
        for (int shift = 56; shift >= 0; shift -= 8) doc += (char)((0ull >> shift) & 0xff);
        for (int shift = 56; shift >= 0; shift -= 8) doc += (char)(((unsigned long long)offsetTable >> shift) & 0xff);

        fc::PlistValue v;
        const bool ok = fc::bplist_parse(doc.data(), doc.size(), v);
        check("16-byte int parses", ok);
        check("  keeps low 64 bits", ok && v.type == fc::PlistType::Int &&
                                          (unsigned long long)v.i == 0x04005A7FFEF7ull);
    }

    if (fails == 0) printf("\nRESULT: ALL OK\n");
    else printf("\nRESULT: %d FAILURE(S)\n", fails);
    return fails == 0 ? 0 : 1;
}