// Isolates the AAC encoder from the network: feeds a known tone and checks
// that access units actually come back out of the MFT.
//
// The failure mode this guards against is silent: ProcessInput succeeds,
// ProcessOutput reports MF_E_TRANSFORM_NEED_MORE_INPUT forever, and the
// session runs with no audio at all.
#include "src/aacenc.h"
#include "src/log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

static int fails = 0;
static void check(const char* what, bool ok) {
    printf("%-46s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) fails++;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("aacenc_test: start\n");
    fflush(stdout);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    printf("aacenc_test: com ok\n");
    fflush(stdout);
    fc::log_init(fc::LogLevel::Debug, nullptr, nullptr);
    printf("aacenc_test: log ok\n");
    fflush(stdout);

    fc::AacEncoder enc;
    const bool started = enc.start(44100, 2, 128000);
    printf("aacenc_test: started=%d\n", (int)started);
    fflush(stdout);
    if (!started) {
        printf("encoder failed to start\n");
        return 1;
    }

    // 1 s of a 440 Hz tone, delivered in the same chunk size the capture
    // path produces, so this exercises the accumulation logic too.
    std::vector<float> chunk;
    const size_t chunkFrames = 440;
    chunk.resize(chunkFrames * 2);
    size_t produced = 0;
    size_t totalAuBytes = 0;
    int callsWithOutput = 0;

    for (int i = 0; i < 100; i++) {
        for (size_t f = 0; f < chunkFrames; f++) {
            const double t = (double)((size_t)i * chunkFrames + f) / 44100.0;
            const float v = (float)(0.5 * std::sin(2.0 * 3.14159265358979 * 440.0 * t));
            chunk[f * 2] = v;
            chunk[f * 2 + 1] = v;
        }
        std::vector<uint8_t> aac;
        const int n = enc.encode(chunk.data(), chunkFrames, aac);
        if (n > 0) {
            callsWithOutput++;
            produced += (size_t)n;
            totalAuBytes += aac.size();
        }
    }

    printf("100 encode calls -> %zu AU, %zu bytes, %d calls produced\n", produced, totalAuBytes,
           callsWithOutput);
    check("aac: encoder produces access units", produced > 0);
    // 1 s at 44.1 kHz is ~43 AAC frames; allow a wide margin for MFT latency.
    check("aac: at least 20 AU per second", produced >= 20);
    {
        std::vector<uint8_t> asc;
        const bool haveAsc = enc.asc(asc);
        // AAC-LC, 44.1 kHz, stereo == 0x12 0x10. The receiver rebuilds this from
        // ct/spf, so a mismatch here means the stream would be decoded wrong.
        check("aac: ASC is LC/44.1k/stereo (12 10)",
              haveAsc && asc.size() == 2 && asc[0] == 0x12 && asc[1] == 0x10);
    }
    const int rates[] = {44100, 48000};
    const int chs[] = {2, 1};
    const int bitrates[] = {128000, 192000, 96000};
    for (int r = 0; r < 2; r++) {
        for (int c = 0; c < 2; c++) {
            for (int b = 0; b < 3; b++) {
                fc::AacEncoder e;
                if (!e.start(rates[r], chs[c], bitrates[b])) {
                    printf("  %d/%d/%d: start failed\n", rates[r], chs[c], bitrates[b]);
                    continue;
                }
                size_t au = 0;
                for (int i = 0; i < 60; i++) {
                    std::vector<float> chunk(1024 * (size_t)chs[c]);
                    for (size_t k = 0; k < chunk.size(); k++) chunk[k] = 0.1f;
                    std::vector<uint8_t> aac;
                    const int n = e.encode(chunk.data(), 1024, aac);
                    if (n > 0) au += (size_t)n;
                }
                printf("  %d Hz / %d ch / %d bps -> %zu AU\n", rates[r], chs[c], bitrates[b], au);
            }
        }
    }
    printf("matrix done\n");
    return 0;
}
