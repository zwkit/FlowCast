// Round-trip self-test: fairplay_make_ekey must produce an ekey that the
// receiver's own playfair_decrypt() maps back to our AES key.
// Also checks CNG primitives against published vectors.
#include "src/crypto.h"
#include "src/fairplay.h"
#include "third_party/playfair/playfair.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>

static int fails = 0;

static void check(const char* what, bool ok) {
    printf("%-46s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) fails++;
}

int main() {
    // --- CNG: SHA-256 ("abc") ------------------------------------------------
    {
        uint8_t out[32];
        fc::sha256((const uint8_t*)"abc", 3, out);
        static const uint8_t expect[32] = {0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
                                           0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
                                           0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
                                           0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad};
        check("sha256(\"abc\")", memcmp(out, expect, 32) == 0);
    }

    // --- CNG: AES-128 ECB (FIPS-197) -----------------------------------------
    {
        uint8_t key[16], pt[16], ct[16];
        for (int i = 0; i < 16; i++) key[i] = (uint8_t)i;
        for (int i = 0; i < 16; i++) pt[i] = (uint8_t)(i * 0x11);
        fc::Aes128 aes;
        bool init = aes.init(key);
        aes.encrypt_block(pt, ct);
        static const uint8_t expect[16] = {0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30,
                                           0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5, 0x5a};
        check("aes128-ecb fips197", init && memcmp(ct, expect, 16) == 0);
        // CTR round-trip through the same keystream
        fc::AesCtr ctr;
        ctr.init(key, expect);
        uint8_t buf[40];
        for (int i = 0; i < 40; i++) buf[i] = (uint8_t)(i * 7 + 1);
        uint8_t enc[40], dec[40];
        ctr.process(buf, enc, 40);
        fc::AesCtr ctr2;
        ctr2.init(key, expect);
        ctr2.process(enc, dec, 40);
        check("aes128-ctr continuity round-trip", memcmp(buf, dec, 40) == 0 && memcmp(enc, buf, 40) != 0);
    }

    // --- mirror video key/IV derivation (known answer, independent SHA-512) --
    // The digest MUST be SHA-512: UxPlay's sha_init() is EVP_sha512() and its
    // aeskey_video/aesiv_video buffers are 64 bytes wide to match. SHA-256 here
    // silently produced a keystream the receiver could not invert.
    {
        // sha512("AirPlayStreamKey4294967296" + aeskey_audio)[0:16]
        // (verified independently with Python hashlib; 4294967296 = 2^32,
        //  aeskey_audio = 00 01 ... 0f)
        uint8_t aeskey[16];
        for (int i = 0; i < 16; i++) aeskey[i] = (uint8_t)i;
        uint8_t in[64];
        memcpy(in, "AirPlayStreamKey4294967296", 26);
        memcpy(in + 26, aeskey, 16);
        uint8_t d[64];
        fc::sha512(in, 42, d);
        static const uint8_t expect[16] = {0x2e, 0x99, 0x7f, 0x4a, 0x43, 0x7c, 0x7a, 0x9c,
                                           0x13, 0x38, 0x6d, 0x37, 0x3b, 0x33, 0x63, 0xef};
        check("mirror video key derivation (sha512)", memcmp(d, expect, 16) == 0);

        memcpy(in, "AirPlayStreamIV4294967296", 25);
        memcpy(in + 25, aeskey, 16);
        fc::sha512(in, 41, d);
        static const uint8_t expectIv[16] = {0x56, 0x8d, 0x22, 0xb1, 0x0b, 0xe5, 0xdb, 0x21,
                                             0xff, 0x1c, 0xe7, 0x86, 0x06, 0xc1, 0x90, 0x9e};
        check("mirror video iv derivation (sha512)", memcmp(d, expectIv, 16) == 0);
    }

    // --- mirror video stream: receiver-semantics round-trip ------------------
    // The receiver keeps one AES-CTR context alive across packets and only
    // re-aligns to a block edge between them (aes_ctr_start_fresh_block).
    // Our AesCtr must emit a byte stream that survives that exactly, with
    // deliberately non-block-aligned packet lengths like real mirror data.
    {
        char tag[48];
        uint8_t d[64], vkey[16], iv[16];
        snprintf(tag, sizeof(tag), "AirPlayStreamKey%llu", 4242ULL);
        fc::sha512((const uint8_t*)tag, strlen(tag), d);
        memcpy(vkey, d, 16);
        snprintf(tag, sizeof(tag), "AirPlayStreamIV%llu", 4242ULL);
        fc::sha512((const uint8_t*)tag, strlen(tag), d);
        memcpy(iv, d, 16);

        fc::AesCtr tx;                       // sender: one continuous CTR
        tx.init(vkey, iv);
        fc::Aes128 rx;                       // receiver: ECB + own counter
        rx.init(vkey);
        uint8_t counter[16];
        memcpy(counter, iv, 16);
        uint8_t ks[16];
        size_t ksPos = 16;

        static const size_t lens[] = {1, 15, 16, 17, 100, 3, 4096, 7, 31, 32, 33, 512, 1};
        uint8_t plain[8192], cipher[8192], back[8192];
        for (size_t i = 0; i < 8192; i++) plain[i] = (uint8_t)(i * 31 + (i >> 8));
        size_t off = 0;
        for (size_t p = 0; p < sizeof(lens) / sizeof(lens[0]); p++) {
            size_t n = lens[p];
            if (off + n > sizeof(plain)) break;
            tx.process(plain + off, cipher + off, n);
            for (size_t i = 0; i < n; i++) {
                if (ksPos == 16) {
                    rx.encrypt_block(counter, ks);
                    for (int b = 15; b >= 0; b--) { if (++counter[b] != 0) break; }
                    ksPos = 0;
                }
                back[off + i] = cipher[off + i] ^ ks[ksPos++];
            }
            off += n;
        }
        check("mirror video CTR stream continuity", off > 0 && memcmp(plain, back, off) == 0);
    }

    // --- FairPlay ekey round-trip (the receiver's own decrypt) ----------------
    {
        const int iters = 200;
        int bad = 0;
        for (int i = 0; i < iters; i++) {
            uint8_t keymsg[164];
            fairplay_build_keymsg(keymsg);
            uint8_t aes_key[16];
            fc::random_bytes(aes_key, 16);
            uint8_t ekey[72];
            fairplay_make_ekey(keymsg, aes_key, ekey);
            uint8_t out[16];
            playfair_decrypt((unsigned char*)keymsg, (unsigned char*)ekey, out);
            if (memcmp(out, aes_key, 16) != 0) bad++;
        }
        char label[64];
        snprintf(label, sizeof(label), "fairplay ekey round-trip (%d iters)", iters);
        check(label, bad == 0);
    }

    printf(fails ? "\nRESULT: %d FAILURE(S)\n" : "\nRESULT: ALL OK\n", fails);
    return fails ? 1 : 0;
}
