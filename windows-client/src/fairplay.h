// FlowCast Windows client -- FairPlay (AirPlay) ekey generation.
//
// The receiver (UxPlay/FlowCast) derives the session's AES audio key with
// playfair_decrypt(keymsg, ekey) -- see third_party/playfair (GPL-3.0, ported
// from UxPlay). Instead of inverting cycle() we exploit the fact that chunk1
// (ekey bytes 16..31) is free: pick a random chunk2, run cycle() FORWARD, and
// solve chunk1 = P ^ cycle(z_xor(chunk2)) so the receiver's decryption lands
// exactly on our chosen AES key.
#pragma once
#include <cstdint>

// Fills a 164-byte fp-setup message ("keymsg"). The receiver only checks
// byte 4 == 0x03; byte 12 selects an internal table (0..3); bytes 144..163
// are echoed back. Everything else is ours to choose.
void fairplay_build_keymsg(uint8_t keymsg[164]);

// Fills the 16-byte fp-setup #1 message: the canonical Apple M1, byte for byte.
// Kept here, next to the rest of FairPlay, because two callers need it (the
// client and the capability probe) and the exact bytes matter: UxPlay accepts
// "FPLY" + zeros, but stricter receivers ignore that request outright.
void fairplay_m1(uint8_t out[16]);

// Builds the 72-byte ekey for the SETUP request so that
// playfair_decrypt(keymsg, ekey) == aes_key on the receiver.
void fairplay_make_ekey(const uint8_t keymsg[164], const uint8_t aes_key[16], uint8_t ekey[72]);
