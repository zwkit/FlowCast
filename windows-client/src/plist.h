// FlowCast Windows client - minimal Apple XML plist reader/writer.
//
// Every RTSP body in RAOP is a plist, so this is on the hot path of the
// handshake. Only the types AirPlay actually uses are modelled.
#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace fc {

enum class PlistType { Null, Bool, Int, Real, String, Data, Array, Dict };

struct PlistValue {
    PlistType type = PlistType::Null;
    bool b = false;
    long long i = 0;
    double r = 0.0;
    std::string s; // String: UTF-8 text.  Data: raw bytes.
    std::vector<PlistValue> arr;
    std::vector<std::pair<std::string, PlistValue>> dict;

    PlistValue() = default;

    static PlistValue boolean(bool v) {
        PlistValue p;
        p.type = PlistType::Bool;
        p.b = v;
        return p;
    }
    static PlistValue integer(long long v) {
        PlistValue p;
        p.type = PlistType::Int;
        p.i = v;
        return p;
    }
    static PlistValue real(double v) {
        PlistValue p;
        p.type = PlistType::Real;
        p.r = v;
        return p;
    }
    static PlistValue string(const std::string& v) {
        PlistValue p;
        p.type = PlistType::String;
        p.s = v;
        return p;
    }
    static PlistValue data(const std::string& rawBytes) {
        PlistValue p;
        p.type = PlistType::Data;
        p.s = rawBytes;
        return p;
    }
    static PlistValue array(std::vector<PlistValue> v) {
        PlistValue p;
        p.type = PlistType::Array;
        p.arr = std::move(v);
        return p;
    }
    static PlistValue dictOf(std::vector<std::pair<std::string, PlistValue>> v) {
        PlistValue p;
        p.type = PlistType::Dict;
        p.dict = std::move(v);
        return p;
    }

    const PlistValue* find(const char* key) const {
        if (type != PlistType::Dict) return nullptr;
        for (const auto& kv : dict) {
            if (kv.first == key) return &kv.second;
        }
        return nullptr;
    }
    void set(const std::string& key, PlistValue v) {
        for (auto& kv : dict) {
            if (kv.first == key) {
                kv.second = std::move(v);
                return;
            }
        }
        dict.emplace_back(key, std::move(v));
    }
    bool asBool(bool def = false) const {
        return type == PlistType::Bool ? b : def;
    }
    long long asInt(long long def = 0) const {
        return type == PlistType::Int ? i : def;
    }
    double asReal(double def = 0.0) const {
        return type == PlistType::Real ? r : def;
    }
    const std::string& asString() const { return s; }
};

// Parses a full plist document. Returns false with a log line on malformed input.
bool plist_parse(const char* xml, size_t len, PlistValue& out);
bool plist_parse(const std::string& xml, PlistValue& out);

// Serializes a value as an XML plist. Booleans become <true/>/<false/>.
std::string plist_dump(const PlistValue& v);

// Serializes a value as a binary plist. This is what RAOP actually uses.
std::string plist_dump_bin(const PlistValue& v);

} // namespace fc
