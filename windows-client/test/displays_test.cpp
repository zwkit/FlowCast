// FlowCast Windows client - display enumeration self-test.
//
// The "which screen" setting is a device name saved in an INI and resolved at
// capture time, so the rules worth pinning down are: the list is ordered and
// indexed, the primary screen is findable, and a stale name never turns into
// "capture nothing".
#include "src/capture.h"
#include "src/log.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    std::printf("%-58s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) g_failures++;
}

} // namespace

int main() {
    fc::log_init(fc::LogLevel::Warn, nullptr, nullptr);

    const std::vector<fc::DisplayInfo> all = fc::list_displays();
    std::printf("displays found: %d\n", (int)all.size());
    for (const fc::DisplayInfo& d : all) {
        std::printf("  %d  %dx%d  %-14ls%s\n", d.index, d.width, d.height, d.device.c_str(),
                    d.primary ? "  (primary)" : "");
    }

    check(!all.empty(), "at least one screen is attached");
    check(!all.empty() && all.front().primary, "the primary screen is listed first");

    bool indices = true;
    bool names = true;
    bool sizes = true;
    int primaries = 0;
    for (size_t i = 0; i < all.size(); i++) {
        if (all[i].index != (int)i + 1) indices = false;
        if (all[i].device.empty() || all[i].device.rfind(L"\\\\.\\", 0) != 0) names = false;
        if (all[i].width <= 0 || all[i].height <= 0) sizes = false;
        if (all[i].primary) primaries++;
    }
    check(indices, "indices are 1..n in list order");
    check(names, "every entry carries a \\\\.\\DISPLAYn device name");
    check(sizes, "every entry has a positive size");
    check(primaries == 1, "exactly one screen is marked primary");

    // An empty choice means "primary", which is what a fresh install has.
    const fc::DisplayInfo byDefault = fc::find_display(L"");
    check(byDefault.primary, "an empty device name resolves to the primary screen");

    if (!all.empty()) {
        const fc::DisplayInfo exact = fc::find_display(all.front().device);
        check(exact.device == all.front().device, "a known device name resolves to itself");
    }

    // The important one: a screen that was unplugged must not make capture fail,
    // it must fall back to the primary one.
    const fc::DisplayInfo stale = fc::find_display(L"\\\\.\\DISPLAY99");
    check(stale.primary, "an unknown device name falls back to the primary screen");

    // Case sensitivity: Windows device names are upper case, but an INI edited
    // by hand may not be.
    const fc::DisplayInfo lower = fc::find_display(L"\\\\.\\display99");
    check(lower.primary, "the fallback also applies to a lower-case name");

    if (!all.empty()) {
        std::wstring swapped = all.front().device;
        for (wchar_t& c : swapped) {
            if (c >= L'A' && c <= L'Z') c = (wchar_t)(c - L'A' + L'a');
        }
        check(fc::find_display(swapped).device == all.front().device,
              "device names are matched case-insensitively");
    }

    if (g_failures) {
        std::printf("\n%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("\nall display checks passed\n");
    return 0;
}
