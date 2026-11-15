// Round-trip test for the plist reader/writer, using the exact shapes RAOP
// sends and receives.
#include "src/plist.h"
#include "src/log.h"

#include <cstdio>
#include <cstring>

static int fails = 0;
static void check(const char* what, bool ok) {
    printf("%-46s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) fails++;
}

int main() {
    fc::log_init(fc::LogLevel::Error, nullptr, nullptr);

    // --- every supported type round-trips --------------------------------
    {
        fc::PlistValue v = fc::PlistValue::dictOf({
            {"aString", fc::PlistValue::string("hello <world> & \"friends\"")},
            {"anInt", fc::PlistValue::integer(-1234567890123LL)},
            {"aReal", fc::PlistValue::real(0.125)},
            {"trueKey", fc::PlistValue::boolean(true)},
            {"falseKey", fc::PlistValue::boolean(false)},
            {"blob", fc::PlistValue::data(std::string("\x00\x01\x02\xfe\xff", 5))},
            {"list", fc::PlistValue::array({fc::PlistValue::integer(1),
                                            fc::PlistValue::string("two"),
                                            fc::PlistValue::array({})})},
            {"empty", fc::PlistValue::dictOf({})},
        });
        std::string xml = fc::plist_dump(v);
        fc::PlistValue back;
        bool ok = fc::plist_parse(xml, back);
        check("plist round-trip parse", ok);
        if (ok) {
            const fc::PlistValue* s = back.find("aString");
            const fc::PlistValue* i = back.find("anInt");
            const fc::PlistValue* r = back.find("aReal");
            const fc::PlistValue* t = back.find("trueKey");
            const fc::PlistValue* f = back.find("falseKey");
            const fc::PlistValue* d = back.find("blob");
            const fc::PlistValue* l = back.find("list");
            check("  string with entities", s && s->asString() == "hello <world> & \"friends\"");
            check("  integer", i && i->asInt() == -1234567890123LL);
            check("  real", r && r->asReal() == 0.125);
            check("  true", t && t->asBool());
            check("  false", f && !f->asBool());
            check("  data (binary)", d && d->s == std::string("\x00\x01\x02\xfe\xff", 5));
            check("  nested array", l && l->arr.size() == 3 && l->arr[1].asString() == "two" &&
                                        l->arr[2].type == fc::PlistType::Array && l->arr[2].arr.empty());
            check("  empty dict stays empty", back.find("empty") &&
                                                 back.find("empty")->type == fc::PlistType::Dict);
        }
    }

    // --- a real RAOP SETUP response body -----------------------------------
    {
        static const char* body =
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
            "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
            "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">"
            "<plist version=\"1.0\"><dict>"
            "<key>eventPort</key><integer>53692</integer>"
            "<key>timingPort</key><integer>60963</integer>"
            "<key>streams</key><array>"
            "<dict><key>type</key><integer>110</integer>"
            "<key>dataPort</key><integer>53050</integer>"
            "<key>audioBufferSize</key><integer>1024</integer></dict>"
            "</array></dict></plist>";
        fc::PlistValue v;
        bool ok = fc::plist_parse(body, strlen(body), v);
        check("plist parse RAOP SETUP body", ok);
        if (ok) {
            const fc::PlistValue* ev = v.find("eventPort");
            const fc::PlistValue* tp = v.find("timingPort");
            const fc::PlistValue* st = v.find("streams");
            check("  eventPort", ev && ev->asInt() == 53692);
            check("  timingPort", tp && tp->asInt() == 60963);
            check("  streams[0].dataPort", st && st->arr.size() == 1 &&
                                               st->arr[0].find("dataPort") &&
                                               st->arr[0].find("dataPort")->asInt() == 53050);
        }
    }

    // --- malformed input must fail loudly, not silently --------------------
    {
        fc::PlistValue v;
        check("plist rejects truncated doc",
              !fc::plist_parse("<plist version=\"1.0\"><dict><key>x</key>", v));
        check("plist rejects wrong root", !fc::plist_parse("<html><body/></html>", v));
    }

    // --- comments and the DOCTYPE are tolerated ---------------------------
    {
        const char* body = "<?xml version=\"1.0\"?>"
                           "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"x\">"
                           "<plist version=\"1.0\">"
                           "<!-- a comment --><dict>"
                            "<key>k</key><string>v</string>"
                            "</dict></plist>";
        fc::PlistValue v;
        bool ok = fc::plist_parse(body, strlen(body), v);
        check("plist tolerates comments", ok && v.find("k") && v.find("k")->asString() == "v");
    }

    printf(fails ? "\nRESULT: %d FAILURE(S)\n" : "\nRESULT: ALL OK\n", fails);
    return fails ? 1 : 0;
}
