// FlowCast Windows client - growable byte buffer with explicit endian helpers.
//
// The AirPlay mirror protocol mixes byte orders inside a single packet
// (little-endian 128-byte header fields, big-endian NAL length prefixes,
// big-endian RTP fields), so every accessor names its byte order.
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>

namespace fc {

class Buf {
public:
    Buf() = default;
    explicit Buf(size_t reserve) { m_data.reserve(reserve); }

    uint8_t* data() { return m_data.data(); }
    const uint8_t* data() const { return m_data.data(); }
    size_t size() const { return m_data.size(); }
    bool empty() const { return m_data.empty(); }
    void clear() { m_data.clear(); }

    void append(const void* p, size_t n) {
        if (!n) return;
        const uint8_t* b = static_cast<const uint8_t*>(p);
        m_data.insert(m_data.end(), b, b + n);
    }
    void appendByte(uint8_t b) { m_data.push_back(b); }
    void appendU16BE(uint16_t v) {
        m_data.push_back((uint8_t)(v >> 8));
        m_data.push_back((uint8_t)(v & 0xff));
    }

    // Zero-fills to `n` bytes without growing past it.
    void appendZeros(size_t n) { m_data.resize(m_data.size() + n, 0); }

    // Grows to exactly `n` bytes; existing bytes are preserved.
    void resize(size_t n) { m_data.resize(n, 0); }

    std::vector<uint8_t>& vec() { return m_data; }

    // --- little-endian writers (mirror header / NTP timestamp fields) --------
    //
    // Every offset writer grows the buffer to cover the field before storing.
    // `std::vector::operator[]` does NOT grow, so a caller that wrote past the
    // current size used to scribble on unallocated heap: the bytes vanished on
    // the next reallocation and the packet went out malformed. That silently
    // corrupted the SPS/PPS packet badly enough to crash the receiver.
    void putU16LE(size_t off, uint16_t v) {
        ensure(off + 2);
        m_data[off] = (uint8_t)(v & 0xff);
        m_data[off + 1] = (uint8_t)(v >> 8);
    }
    void putU32LE(size_t off, uint32_t v) {
        ensure(off + 4);
        for (int i = 0; i < 4; i++) m_data[off + i] = (uint8_t)(v >> (8 * i));
    }
    void putU64LE(size_t off, uint64_t v) {
        ensure(off + 8);
        for (int i = 0; i < 8; i++) m_data[off + i] = (uint8_t)(v >> (8 * i));
    }
    void putF32LE(size_t off, float f) {
        uint32_t v;
        memcpy(&v, &f, 4);
        putU32LE(off, v);
    }

    // --- big-endian writers (NAL lengths, RTP headers) -----------------------
    static void putU16BE(uint8_t* p, size_t off, uint16_t v) {
        p[off] = (uint8_t)(v >> 8);
        p[off + 1] = (uint8_t)(v & 0xff);
    }
    static void putU32BE(uint8_t* p, size_t off, uint32_t v) {
        for (int i = 0; i < 4; i++) p[off + i] = (uint8_t)(v >> (24 - 8 * i));
    }
    static void putU64BE(uint8_t* p, size_t off, uint64_t v) {
        for (int i = 0; i < 8; i++) p[off + i] = (uint8_t)(v >> (56 - 8 * i));
    }

    void putU16BE(size_t off, uint16_t v) {
        ensure(off + 2);
        m_data[off] = (uint8_t)(v >> 8);
        m_data[off + 1] = (uint8_t)(v & 0xff);
    }
    void putU32BE(size_t off, uint32_t v) {
        ensure(off + 4);
        for (int i = 0; i < 4; i++) m_data[off + i] = (uint8_t)(v >> (24 - 8 * i));
    }
    void putU64BE(size_t off, uint64_t v) {
        ensure(off + 8);
        for (int i = 0; i < 8; i++) m_data[off + i] = (uint8_t)(v >> (56 - 8 * i));
    }

    // --- little-endian readers ----------------------------------------------
    static uint16_t getU16LE(const uint8_t* p) {
        return (uint16_t)(p[0] | (p[1] << 8));
    }
    static uint32_t getU32LE(const uint8_t* p) {
        return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
               ((uint32_t)p[3] << 24);
    }
    static uint64_t getU64LE(const uint8_t* p) {
        uint64_t v = 0;
        for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
        return v;
    }
    static float getF32LE(const uint8_t* p) {
        uint32_t v = getU32LE(p);
        float f;
        memcpy(&f, &v, 4);
        return f;
    }

    // --- big-endian readers --------------------------------------------------
    static uint16_t getU16BE(const uint8_t* p) {
        return (uint16_t)((p[0] << 8) | p[1]);
    }
    static uint32_t getU32BE(const uint8_t* p) {
        return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) |
               (uint32_t)p[3];
    }
    static uint64_t getU64BE(const uint8_t* p) {
        uint64_t v = 0;
        for (int i = 0; i < 8; i++) v = (v << 8) | p[i];
        return v;
    }

private:
    // Grows to at least `n` bytes, zero-filling any gap.
    void ensure(size_t n) {
        if (m_data.size() < n) m_data.resize(n, 0);
    }
    std::vector<uint8_t> m_data;
};

} // namespace fc
