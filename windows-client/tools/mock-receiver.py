"""Minimal AirPlay receiver that behaves like a MAXHUB panel.

It answers OPTIONS and every SETUP/RECORD, but silently drops /fp-setup -- no
error, no close, just nothing until the sender's socket times out. That is the
one behaviour we cannot reproduce with real hardware here, and the whole
"continue without FairPlay" fallback depends on it.

--strict-fairplay reproduces a MAXHUB panel exactly: /fp-setup #1 is answered
from the canned reply table, /fp-setup #2 is never answered, and SETUP #1 waits
for a key message that never arrives - so it stays silent too. That is the case
the client's fallback ladder exists for, and the only way to test it without
borrowing the panel.

Run:  python tools/mock-receiver.py [port]
Then: flowcast.exe --host 127.0.0.1 --port 7099 --test-pattern --duration 5
"""

import plistlib
import socket
import sys
import threading


def reply(sock, cseq, body, headers=None, status=b"200 OK"):
    h = [b"RTSP/1.0 " + status]
    if cseq:
        h.append(b"CSeq: " + cseq)
    if headers:
        for k, v in headers.items():
            h.append(("%s: %s" % (k, v)).encode())
    h.append(b"Content-Length: %d" % len(body))
    sock.sendall(b"\r\n".join(h) + b"\r\n\r\n" + body)


# UxPlay's canned answer to /fp-setup #1, mode 0 - byte for byte what a MAXHUB
# sends back. Copied rather than computed: the whole point is that a receiver
# which answers here may still be a stub that never runs the key exchange.
FP1_REPLY = bytes.fromhex(
    "46504c59030102000000008202000f9f3f9e0a2521dbdf312ab2bfb29e8d232b"
    "6376a8c818701d22ae93d82737feaf9db4fdf41c2dba9d1f49caaabf6591ac1f"
    "7bc6f7e0663d21afe01565953eab81f418ceed095adb7c3d0e254909a79831d4"
    "9c3982973434facb42c63a1cd911a6fe941a8a6d4a743b46c3a7649e44c78955"
    "e49d815500954 9c4e2f7a3f6d5ba".replace(" ", ""))


def serve(conn, drop_fairplay, fail_setup1=False, apple_only=False, strict=False):
    buf = b""
    keymsg_seen = False
    log = []
    while True:
        try:
            chunk = conn.recv(65536)
        except OSError:
            break
        if not chunk:
            break
        buf += chunk
        while True:
            head, sep, rest = buf.partition(b"\r\n\r\n")
            if not sep:
                break
            lines = head.split(b"\r\n")
            method, url = lines[0].split()[0], lines[0].split()[1]
            cseq = b""
            clen = 0
            ua = b""
            for line in lines[1:]:
                if line.lower().startswith(b"cseq:"):
                    cseq = line.split(b":", 1)[1].strip()
                elif line.lower().startswith(b"content-length:"):
                    clen = int(line.split(b":", 1)[1])
                elif line.lower().startswith(b"user-agent:"):
                    ua = line.split(b":", 1)[1].strip()
            if len(rest) < clen:
                break
            # Keep the body: the single-SETUP shape is recognised by the
            # `streams` key inside it, so "is this receiver Apple-shaped" has to
            # be answered from the request itself.
            req_body = rest[:clen]
            buf = rest[clen:]
            name = method.decode() + " " + url.decode()
            log.append(name)
            print("  <-", name, flush=True)

            # --apple-only: behave exactly like a panel that will only talk to
            # an Apple sender. It answers OPTIONS either way (so the device
            # looks alive), then goes quiet on /fp-setup and refuses the mirror
            # setup for anyone whose User-Agent is not Apple's.
            apple = ua.startswith(b"AirPlay")
            silent = apple_only and not apple
            if method == b"OPTIONS":
                reply(conn, cseq, b"", {"Public": "ANNOUNCE, SETUP, RECORD, OPTIONS, POST, GET"})
            elif url == b"/fp-setup" and strict and clen == 16:
                reply(conn, cseq, FP1_REPLY, {"Content-Type": "application/octet-stream"})
            elif url == b"/fp-setup" and strict:
                print("     (#2 dropped: the key message is never answered)", flush=True)
            elif url == b"/fp-setup" and (drop_fairplay or silent):
                print("     (dropped, never answering)", flush=True)
            elif method == b"SETUP" and url.endswith(b"/1") and strict and not keymsg_seen:
                # Same silence as the panel: it is waiting for a pairing that
                # never completed, and it says nothing about it.
                print("     (SETUP #1 dropped: no completed FairPlay session)", flush=True)
            elif method == b"SETUP" and url.endswith(b"/1") and (fail_setup1 or silent):
                # What a panel that requires pairing looks like from here: it
                # answered OPTIONS, it ignored /fp-setup, and it refuses the
                # mirror setup itself. The client must report "SETUP #1 403",
                # not a generic "refused".
                print("     (SETUP #1 refused with 403)", flush=True)
                reply(conn, cseq, b"", status=b"403 Forbidden")
            elif method == b"SETUP" and url.endswith(b"/1"):
                # Two shapes arrive here. The split one carries only keys, so
                # the answer is just a timing port and the streams come in #2.
                # The Apple-shaped one carries `streams` as well, and a receiver
                # that understands it answers with the ports in this same
                # response - which is what makes the single-shape fallback
                # testable without hardware.
                res = {"timingPort": 44444}
                if b"streams" in req_body:
                    res["streams"] = [
                        {"type": 110, "dataPort": 5555, "controlPort": 5556},
                        {"type": 96, "dataPort": 5557, "controlPort": 5558},
                    ]
                reply(conn, cseq, plistlib.dumps(res, fmt=plistlib.FMT_BINARY),
                      {"Content-Type": "application/x-apple-binary-plist"})
            elif method == b"SETUP" and url.endswith(b"/2"):
                body = plistlib.dumps({"streams": [
                    {"type": 110, "dataPort": 5555, "controlPort": 5556},
                    {"type": 96, "dataPort": 5557, "controlPort": 5558},
                ]}, fmt=plistlib.FMT_BINARY)
                reply(conn, cseq, body, {"Content-Type": "application/x-apple-binary-plist"})
            else:
                reply(conn, cseq, b"")


class Drain(threading.Thread):
    """Bind one UDP port and count packets, so "did video actually flow" is
    answerable without a decoder. A port nobody listens on is worse than
    useless here: Windows answers the sender with ICMP port-unreachable and the
    send loop dies with 10053, which looks like a client bug and is not one."""

    def __init__(self, port, label):
        super().__init__(daemon=True)
        self.port, self.label, self.count = port, label, 0
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("0.0.0.0", port))

    def run(self):
        while True:
            try:
                self.sock.recvfrom(65536)
            except OSError:
                return
            self.count += 1


class MirrorDrain(threading.Thread):
    """Accepts the mirror data connection. The video channel is TCP, so a UDP
    listener on that port is invisible to it: the client's connect() gets
    ECONNREFUSED (10061) and gives up before a single frame, which reads as a
    client bug and is not one."""

    def __init__(self, port):
        super().__init__(daemon=True)
        self.port = port
        self.count = 0
        self.sock = socket.socket()
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("0.0.0.0", port))
        self.sock.listen(2)

    def run(self):
        while True:
            try:
                conn, _ = self.sock.accept()
            except OSError:
                return
            threading.Thread(target=self._drain, args=(conn,), daemon=True).start()

    def _drain(self, conn):
        while True:
            try:
                data = conn.recv(65536)
            except OSError:
                return
            if not data:
                return
            self.count += len(data)


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 7099
    drop = "--keep-fairplay" not in sys.argv
    fail_setup1 = "--fail-setup1" in sys.argv
    apple_only = "--apple-only" in sys.argv
    strict = "--strict-fairplay" in sys.argv
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", port))
    srv.listen(4)
    # Not `for port, label in ...`: that shadows the RTSP port and made this
    # banner announce the last drain port, so a client refusing the real port
    # looked like the mock was listening somewhere it was not.
    for drain_port, label in ((5557, "audio"), (5556, "ctrl"), (5558, "audio-ctrl")):
        Drain(drain_port, label).start()
    MirrorDrain(5555).start()
    print("mock receiver on :%d, /fp-setup %s" %
          (port, "DROPPED" if drop else "answered with a fake 200"), flush=True)
    if fail_setup1: print("  SETUP #1 will answer 403", flush=True)
    if apple_only: print("  only Apple User-Agents get a working handshake", flush=True)
    if strict: print("  /fp-setup #1 answered, #2 and SETUP #1 stay silent (MAXHUB)", flush=True)
    while True:
        conn, _ = srv.accept()
        threading.Thread(target=serve, args=(conn, drop, fail_setup1, apple_only, strict),
                         daemon=True).start()


if __name__ == "__main__":
    main()
