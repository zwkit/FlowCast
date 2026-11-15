#include "fairplay.h"
#include "crypto.h"
#include "playfair.h"

#include <cstring>

// Forward-only pieces of the vendored playfair port (omg_hax.c / playfair.c).
extern "C" {
void generate_session_key(unsigned char* oldSap, unsigned char* messageIn, unsigned char* sessionKey);
void generate_key_schedule(unsigned char* key_material, uint32_t key_schedule[11][4]);
void cycle(unsigned char* block, uint32_t key_schedule[11][4]);
void z_xor(unsigned char* in, unsigned char* out, int blocks);
void x_xor(unsigned char* in, unsigned char* out, int blocks);
extern unsigned char default_sap[];
}

void fairplay_m1(uint8_t out[16]) {
    // Bytes 5-6 (0x01 0x01) are the crypto version pair, byte 11 (0x04) the
    // reply mode, byte 12 (0x02) the request type. A zeroed tail here is not
    // "close enough": a receiver that validates the request ignores it.
    static const uint8_t kM1[16] = {0x46, 0x50, 0x4c, 0x59, 0x03, 0x01, 0x01, 0x00,
                                    0x00, 0x00, 0x00, 0x04, 0x02, 0x00, 0x00, 0x00};
    memcpy(out, kM1, sizeof(kM1));
}

void fairplay_build_keymsg(uint8_t keymsg[164]) {
    memset(keymsg, 0, 164);
    // Same 16-byte header as the greeting, byte for byte, minus byte 12: that
    // one is the message table (`mode` in playfair's decryptMessage) and has to
    // be 0..3, while in the greeting it means something else. An iPhone's key
    // message starts with the identical prefix, so anything that compares the
    // two - which is the cheapest way for a panel to tell a phone from a
    // third-party sender - sees no difference here.
    static const uint8_t kHeader[16] = {0x46, 0x50, 0x4c, 0x59, 0x03, 0x01, 0x01, 0x00,
                                        0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00};
    memcpy(keymsg, kHeader, sizeof(kHeader));
    keymsg[12] = 0;  // message mode for decryptMessage (0..3)
    fc::random_bytes(keymsg + 16, 128);  // the "encrypted message" (ours)
    fc::random_bytes(keymsg + 144, 20);  // echoed back by the receiver
}

void fairplay_make_ekey(const uint8_t keymsg[164], const uint8_t aes_key[16], uint8_t ekey[72]) {
    uint8_t sap_key[16];
    generate_session_key(default_sap, const_cast<unsigned char*>(keymsg), sap_key);

    uint32_t ks[11][4];
    generate_key_schedule(sap_key, ks);

    // chunk2 (ekey[56:72]) is free: run the forward cipher on it.
    uint8_t chunk2[16];
    fc::random_bytes(chunk2, 16);
    uint8_t block_in[16];
    z_xor(chunk2, block_in, 1);
    cycle(block_in, ks);

    // Receiver ends with: aes_key = P ^ x_key ^ z_key  where P = blockIn ^ chunk1.
    // (z_xor/x_xor are XOR-with-constant, hence their own inverse.)
    uint8_t p[16];
    z_xor(const_cast<unsigned char*>(aes_key), p, 1);
    x_xor(p, p, 1);

    uint8_t chunk1[16];
    for (int i = 0; i < 16; i++) chunk1[i] = p[i] ^ block_in[i];

    fc::random_bytes(ekey, 16);          // ekey[0:16]  unused by the receiver
    memcpy(ekey + 16, chunk1, 16);       // ekey[16:32]
    fc::random_bytes(ekey + 32, 24);     // ekey[32:56] unused
    memcpy(ekey + 56, chunk2, 16);       // ekey[56:72]
}
