// FlowCast Windows client - binary plist ("bplist00") reader.
//
// UxPlay answers every RAOP request with plist_to_bin(), so the handshake
// responses are binary plists, not XML. The format is Apple's documented
// "binary property list" layout:
//
//   header   "bplist00"
//   body     object table, referenced by offset
//   trailer  32 bytes, big-endian:
//              [0..5]  unused
//              [6]     sort version
//              [7]     size of each offset-table entry (1..8)
//              [8..15] number of objects
//              [16..23] offset of the root object
//              [24..31] offset of the offset table
//
// Object markers:
//   0x00 null, 0x08 false, 0x09 true, 0x0F fill
//   0x1n int of 2^n bytes      0x2n real of 2^n bytes   0x3n date
//   0x4n data of n bytes       0x5n ASCII string of n chars
//   0x6n UTF-16 string         0x8n UID of 2^n bytes
//   0xAn array of n items      0xDn dict of n key/value pairs
// When the low nibble is 0xF the real count follows as an int object.
#pragma once
#include <string>

namespace fc {

struct PlistValue;

// Parses a binary plist document into `out`. Returns false with a log line on
// malformed input.
bool bplist_parse(const char* data, size_t len, PlistValue& out);

// Serializes a value as a binary plist ("bplist00"). RAOP exchanges binary
// plists in both directions: the receiver answers with plist_to_bin() and its
// request bodies are parsed with plist_from_bin(), which rejects XML outright.
std::string bplist_dump(const PlistValue& v);

} // namespace fc
