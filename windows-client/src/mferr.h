// FlowCast Windows client - HRESULT names for Media Foundation failures.
//
// Encoder setup fails in a handful of very specific ways, and "0xc00d6d60"
// tells a reader nothing. This maps the codes the MF pipeline can actually
// hand back during SetInputType / SetOutputType / ProcessInput / ProcessOutput
// to their symbol names.
#pragma once
#include <windows.h>

namespace fc {

// Returns a static string such as "MF_E_INVALIDMEDIATYPE", or nullptr when the
// code is not one we recognise.
const char* mf_error_name(HRESULT hr);

// Formats "0x%08lx (%s)" or just "0x%08lx" for unknown codes. The returned
// pointer is a static ring of a few buffers, valid until the next call.
const char* mf_error_str(HRESULT hr);

} // namespace fc
