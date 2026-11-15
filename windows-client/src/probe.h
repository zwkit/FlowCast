// FlowCast Windows client - AirPlay receiver capability probe.
#pragma once

namespace fc {

// Asks an AirPlay receiver what it actually implements, one request per fresh
// connection, and prints each answer. Written after a MAXHUB panel answered
// OPTIONS and then ignored POST /fp-setup until the socket timed out: "no
// answer" is not an HTTP status, so the only way to learn which generation of
// the protocol a device speaks is to send every candidate request and look.
void run_probe(const char* host, unsigned short port);

} // namespace fc
