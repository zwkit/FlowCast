// FlowCast Windows client -- crypto helpers (Windows CNG, no external deps).
#pragma once
#include <cstdint>
#include <cstddef>

namespace fc {

// Cryptographically strong random bytes (BCRYPT_USE_SYSTEM_PREFERRED_RNG).
void random_bytes(uint8_t* p, size_t n);

// One-shot SHA-256.
void sha256(const uint8_t* data, size_t len, uint8_t out[32]);

// One-shot SHA-512. This is what the receiver uses to derive the mirror video
// key and IV (UxPlay's sha_init() is EVP_sha512()), so it must be used there.
void sha512(const uint8_t* data, size_t len, uint8_t out[64]);

// AES-128 single-block ECB primitive; CTR/CBC are layered on top so the
// keystream/IV state stays fully under our control.
class Aes128 {
public:
    Aes128() = default;
    ~Aes128();
    Aes128(const Aes128&) = delete;
    Aes128& operator=(const Aes128&) = delete;
    bool init(const uint8_t key[16]);
    void encrypt_block(const uint8_t in[16], uint8_t out[16]);

private:
    void release();
    void* m_key = nullptr; // BCRYPT_KEY_HANDLE
    void* m_obj = nullptr; // CNG key object scratch buffer
    uint32_t m_objLen = 0;
};

// Continuous AES-128-CTR keystream for the mirror video stream. The counter
// never resets between packets -- exactly what the receiver expects.
class AesCtr {
public:
    bool init(const uint8_t key[16], const uint8_t iv[16]);
    void process(const uint8_t* in, uint8_t* out, size_t len); // in-place ok

private:
    Aes128 m_aes;
    uint8_t m_counter[16] = {0};
    uint8_t m_ks[16] = {0};
    size_t m_ks_pos = 16; // forces a fresh keystream block on first use
};

// AES-128-CBC with a per-packet IV reset, for RAOP audio.
class AesCbc {
public:
    bool init(const uint8_t key[16], const uint8_t iv[16]);
    // encrypts `blocks` 16-byte units; restarts from the original IV each call
    void encrypt_reset(const uint8_t* in, uint8_t* out, size_t blocks);

private:
    Aes128 m_aes;
    uint8_t m_iv[16] = {0};
};

} // namespace fc
