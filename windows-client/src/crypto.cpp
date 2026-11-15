#include "crypto.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <cstring>
#include <cstdlib>

#pragma comment(lib, "bcrypt.lib")

namespace fc {

void random_bytes(uint8_t* p, size_t n) {
    NTSTATUS st = BCryptGenRandom(nullptr, p, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (st < 0) { // BCRYPT_RNG_ALGORITHM failure should never happen; fail loud
        memset(p, 0, n);
        abort();
    }
}

void sha256(const uint8_t* data, size_t len, uint8_t out[32]) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) abort();
    if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) < 0) abort();
    BCryptHashData(hash, const_cast<PUCHAR>(data), (ULONG)len, 0);
    BCryptFinishHash(hash, out, 32, 0);
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
}

// The receiver's sha_init() is EVP_sha512(), and its video key/IV buffer is
// sized 64 bytes to match. Using SHA-256 here produces a keystream the
// receiver cannot invert: every frame comes back marked undecryptable.
void sha512(const uint8_t* data, size_t len, uint8_t out[64]) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA512_ALGORITHM, nullptr, 0) < 0) abort();
    if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) < 0) abort();
    BCryptHashData(hash, const_cast<PUCHAR>(data), (ULONG)len, 0);
    BCryptFinishHash(hash, out, 64, 0);
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
}

Aes128::~Aes128() {
    release();
}

void Aes128::release() {
    if (m_key) {
        BCryptDestroyKey(reinterpret_cast<BCRYPT_KEY_HANDLE>(m_key));
        m_key = nullptr;
    }
    if (m_obj) {
        HeapFree(GetProcessHeap(), 0, m_obj);
        m_obj = nullptr;
        m_objLen = 0;
    }
}

bool Aes128::init(const uint8_t key[16]) {
    release();
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0) < 0) return false;
    // ECB mode: we do the chaining ourselves so CTR/CBC state stays explicit.
    // NOTE: the chaining-mode property is a null-terminated wide string; the
    // byte count must cover the terminator, not just the pointer.
    PCWSTR mode = BCRYPT_CHAIN_MODE_ECB;
    ULONG modeLen = (ULONG)((wcslen(mode) + 1) * sizeof(wchar_t));
    NTSTATUS st = BCryptSetProperty(alg, BCRYPT_CHAINING_MODE,
                                    reinterpret_cast<PUCHAR>(const_cast<PWSTR>(mode)), modeLen, 0);
    if (st < 0) { BCryptCloseAlgorithmProvider(alg, 0); return false; }

    // CNG refuses pbKeyObject == NULL (STATUS_INVALID_PARAMETER), so ask it
    // how much scratch a symmetric key needs and supply that buffer.
    DWORD objLen = 0, got = 0;
    st = BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objLen),
                           sizeof(objLen), &got, 0);
    if (st < 0 || got != sizeof(objLen)) { BCryptCloseAlgorithmProvider(alg, 0); return false; }
    m_obj = HeapAlloc(GetProcessHeap(), 0, objLen);
    m_objLen = objLen;
    if (!m_obj) { BCryptCloseAlgorithmProvider(alg, 0); return false; }

    // Plain key material (not a KEY_DATA_BLOB): the default CNG provider on
    // Windows 10 rejects key-import blobs with STATUS_INVALID_PARAMETER, but
    // accepts the raw 16 bytes directly. Verified on 10.0.19045.
    BCRYPT_KEY_HANDLE hk = nullptr;
    st = BCryptGenerateSymmetricKey(alg, &hk, reinterpret_cast<PUCHAR>(m_obj), m_objLen,
                                    const_cast<PUCHAR>(key), 16, 0);
    BCryptCloseAlgorithmProvider(alg, 0);
    if (st < 0) { release(); return false; }
    m_key = hk;
    return true;
}

void Aes128::encrypt_block(const uint8_t in[16], uint8_t out[16]) {
    if (!m_key) { memset(out, 0, 16); return; }
    ULONG done = 0;
    BCryptEncrypt(reinterpret_cast<BCRYPT_KEY_HANDLE>(m_key), const_cast<PUCHAR>(in), 16,
                  nullptr, nullptr, 0, out, 16, &done, 0);
}

bool AesCtr::init(const uint8_t key[16], const uint8_t iv[16]) {
    if (!m_aes.init(key)) return false;
    memcpy(m_counter, iv, 16);
    m_ks_pos = 16;
    return true;
}

void AesCtr::process(const uint8_t* in, uint8_t* out, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (m_ks_pos == 16) {
            m_aes.encrypt_block(m_counter, m_ks);
            // increment the 128-bit big-endian counter
            for (int b = 15; b >= 0; b--) {
                if (++m_counter[b] != 0) break;
            }
            m_ks_pos = 0;
        }
        out[i] = in[i] ^ m_ks[m_ks_pos++];
    }
}

bool AesCbc::init(const uint8_t key[16], const uint8_t iv[16]) {
    if (!m_aes.init(key)) return false;
    memcpy(m_iv, iv, 16);
    return true;
}

void AesCbc::encrypt_reset(const uint8_t* in, uint8_t* out, size_t blocks) {
    uint8_t iv[16];
    memcpy(iv, m_iv, 16);
    for (size_t b = 0; b < blocks; b++) {
        uint8_t blk[16];
        for (int i = 0; i < 16; i++) blk[i] = in[b * 16 + i] ^ iv[i];
        uint8_t enc[16];
        m_aes.encrypt_block(blk, enc);
        memcpy(out + b * 16, enc, 16);
        memcpy(iv, enc, 16);
    }
}

} // namespace fc
