// FlowCast Windows client - HRESULT names for Media Foundation failures.
#include "mferr.h"

#include <cstdio>

namespace fc {
namespace {

struct Entry {
    HRESULT hr;
    const char* name;
};

// Only the codes the encoder paths can realistically produce. An unrecognised
// code is reported as a bare hex value rather than guessed at.
const Entry kTable[] = {
    {static_cast<HRESULT>(0xc00d36b4), "MF_E_INVALIDMEDIATYPE"},
    {static_cast<HRESULT>(0xc00d36b5), "MF_E_NOTACCEPTING"},
    {static_cast<HRESULT>(0xc00d36b6), "MF_E_NOT_INITIALIZED"},
    {static_cast<HRESULT>(0xc00d36b9), "MF_E_NO_MORE_TYPES"},
    {static_cast<HRESULT>(0xc00d36bd), "MF_E_INVALIDTYPE"},
    {static_cast<HRESULT>(0xc00d36c4), "MF_E_UNSUPPORTED_BYTESTREAM_TYPE"},
    {static_cast<HRESULT>(0xc00d5212), "MF_E_INVALID_STREAM_STATE"},
    {static_cast<HRESULT>(0xc00d6d60), "MF_E_TRANSFORM_TYPE_NOT_SET"},
    {static_cast<HRESULT>(0xc00d6d61), "MF_E_TRANSFORM_STREAM_CHANGE"},
    {static_cast<HRESULT>(0xc00d6d72), "MF_E_TRANSFORM_NEED_MORE_INPUT"},
    {static_cast<HRESULT>(0xc00d6d73), "MF_E_SINK_NO_SAMPLES_PROCESSED"},
    {static_cast<HRESULT>(0xc00d4a44), "MF_E_HW_MFT_FAILED_START_STREAMING"},
    {static_cast<HRESULT>(0xc00d36b0), "MF_E_PLATFORM_NOT_INITIALIZED"},
    {static_cast<HRESULT>(0xc00d36b1), "MF_E_BUFFERTOOSMALL"},
    {static_cast<HRESULT>(0xc00d36b2), "MF_E_INVALIDREQUEST"},
};

} // namespace

const char* mf_error_name(HRESULT hr) {
    for (const Entry& e : kTable) {
        if (e.hr == hr) return e.name;
    }
    return nullptr;
}

const char* mf_error_str(HRESULT hr) {
    static char bufs[4][48];
    static int next = 0;
    char* b = bufs[next];
    next = (next + 1) % 4;
    const char* name = mf_error_name(hr);
    if (name) {
        std::snprintf(b, 48, "0x%08lx (%s)", (unsigned long)hr, name);
    } else {
        std::snprintf(b, 48, "0x%08lx", (unsigned long)hr);
    }
    return b;
}

} // namespace fc
