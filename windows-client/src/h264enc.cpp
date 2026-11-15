// FlowCast Windows client - Media Foundation H.264 encoder implementation.
#include "h264enc.h"
#include "log.h"
#include "bytebuf.h"
#include "mferr.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfapi.h>
#include <mftransform.h>
#include <mfidl.h>
#include <codecapi.h>
#include <mferror.h>
#include <strmif.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfuuid.lib")

using Microsoft::WRL::ComPtr;

namespace fc {
namespace {

// Converts an MF attribute store into a single IMFMediaType we can feed to the
// encoder's input.
void setLong(IMFAttributes* a, REFGUID key, UINT32 v) {
    a->SetUINT32(key, v);
}

// The Microsoft H.264 encoder MFT -- the software fallback that ships with
// Windows itself. Declared as CLSID_CMSH264EncoderMFT in wmcodecdsp.h; spelled
// out here so the build does not need that header, and so the fallback can be
// created even when enumeration hands back nothing usable.
const GUID kMsH264EncoderMft = {0x6ca50344,
                                0x1a1a,
                                0x460d,
                                {0x9b, 0x1b, 0xb3, 0x5b, 0xc1, 0xae, 0x5a, 0x5b}};

// An asynchronous MFT rejects SetInputType with MF_E_TRANSFORM_ASYNC_LOCKED
// until it is unlocked. A no-op on a synchronous one, so call it unconditionally.
void unlockAsync(IMFTransform* mft) {
    ComPtr<IMFAttributes> attrs;
    if (SUCCEEDED(mft->QueryInterface(IID_PPV_ARGS(&attrs)))) {
        attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
    }
}

// Friendly name for the log, so a failure report says which encoder was tried.
std::string mftName(IMFActivate* act) {
    wchar_t buf[128] = {};
    UINT32 len = 0;
    if (FAILED(act->GetString(MFT_FRIENDLY_NAME_Attribute, buf, 128, &len))) return "?";
    std::string out;
    for (const wchar_t* p = buf; *p; ++p) out.push_back(*p < 128 ? (char)*p : '?');
    return out;
}

// Compact GUID for logs -- enough to tell media subtypes apart.
std::string guidStr(const GUID& g) {
    char b[64];
    snprintf(b, sizeof(b), "%08lx-%04x-%04x", (unsigned long)g.Data1, g.Data2, g.Data3);
    return b;
}

// Strips Annex-B start codes, returning one NAL (header byte first).
const uint8_t* nextNal(const uint8_t* p, const uint8_t* end, size_t& nalLen) {
    while (p + 3 < end) {
        if (p[0] == 0 && p[1] == 0 && p[2] == 1) {
            const uint8_t* start = p + 3;
            const uint8_t* q = start;
            while (q + 2 < end && !(q[0] == 0 && q[1] == 0 && q[2] == 1)) q++;
            nalLen = (size_t)(q - start);
            return start;
        }
        p++;
    }
    nalLen = 0;
    return nullptr;
}

} // namespace

struct H264Encoder::Impl {
    ComPtr<IMFTransform> mft;
    ComPtr<IMFMediaType> inType;
    ComPtr<IMFMediaType> outType;
    MFT_OUTPUT_STREAM_INFO outInfo{};
    std::vector<uint8_t> sps;
    std::vector<uint8_t> pps;
    std::vector<uint8_t> vps;
    bool hevc = false;
    // MFT runs on the async event model (METransformNeedInput/HaveOutput).
    bool async = false;
    bool keyframeRequested = false;
    LONGLONG frameDuration = 0;
    bool started = false;
    bool hw = false;

    // Where a bring-up attempt gave up and with what. Kept per attempt so the
    // caller can report "hardware encoder, stage=set-output, MF_E_INVALIDMEDIATYPE"
    // instead of a bare "could not start".
    const char* failStage = "none";
    HRESULT failHr = S_OK;

    // Negotiates types and knobs on the MFT already in `mft`. Returns false
    // with failStage/failHr set; safe to call again with another MFT.
    bool setup(size_t width, size_t height, int fps, int bitrate, bool lowLatency, int maxQp,
               bool hevc);

    // Reusable sample + buffer pairs. Creating a 12 MB input buffer and an 18 MB
    // output buffer per frame is ~900 MB/s of heap churn at 4K30 for no reason;
    // recycling three of each stays clear of the MFT still holding one.
    struct Slot {
        ComPtr<IMFSample> sample;
        ComPtr<IMFMediaBuffer> buffer;
        size_t capacity = 0;
    };
    std::vector<Slot> inSlots;
    std::vector<Slot> outSlots;
    size_t inNext = 0;
    size_t outNext = 0;
    // Scratch target for the CPU BGRA conversion.
    std::vector<uint8_t> scratchNv12;

    Slot* take(std::vector<Slot>& pool, size_t& next, size_t need) {
        if (pool.size() < 3) pool.resize(3);
        Slot& s = pool[next % pool.size()];
        next++;
        if (!s.sample || s.capacity < need) {
            ComPtr<IMFMediaBuffer> b;
            if (FAILED(MFCreateMemoryBuffer((DWORD)need, &b))) return nullptr;
            ComPtr<IMFSample> smp;
            if (FAILED(MFCreateSample(&smp))) return nullptr;
            if (FAILED(smp->AddBuffer(b.Get()))) return nullptr;
            s.buffer = b;
            s.sample = smp;
            s.capacity = need;
        }
        return &s;
    }

    // --- async MFT plumbing -------------------------------------------------
    // AMD's encoder ignores MF_TRANSFORM_ASYNC_UNLOCK on the activation object
    // (readback stays 0) and then refuses ProcessInput with MF_E_NOTACCEPTING
    // unless it has already announced METransformNeedInput. The only way in is
    // to poll its event queue.
    ComPtr<IMFMediaEventGenerator> evgen;

    // Bit flags used by waitEvent().
    enum { kNeedInput = 1, kHaveOutput = 2 };
    bool needInput = false;
    // Counter, not a flag: when two outputs are ready the MFT queues two events,
    // and collapsing them into one bool silently dropped every other frame.
    int haveOutput = 0;

    // Async drop accounting, reported once from stop(). `submit` vs `frames`
    // is the number the HEVC path lives or dies by: every accepted frame
    // produces exactly one NAL, so frames it never accepted are frames lost.
    int stFrames = 0, stNoNeed = 0, stSubmit = 0, stNotAccepting = 0, stNals = 0;

    // Drains everything queued, folding it into the flags above. Bounded so a
    // misbehaving driver that never empties its queue cannot hang the frame.
    void pumpEvents() {
        if (!evgen) return;
        for (int guard = 0; guard < 64; guard++) {
            ComPtr<IMFMediaEvent> ev;
            const HRESULT hr = evgen->GetEvent(MF_EVENT_FLAG_NO_WAIT, &ev);
            if (FAILED(hr)) return;
            MediaEventType t = MEUnknown;
            if (FAILED(ev->GetType(&t))) continue;
            if (t == METransformNeedInput) needInput = true;
            else if (t == METransformHaveOutput) haveOutput++;
        }
    }

    // Pulls finished frames out of the MFT and appends their NALs to `out`.
    // waitMs == 0 collects only what is already done; a positive value blocks up
    // to that long for the first pending frame.
    int drainOutputs(std::vector<uint8_t>& out, size_t outCapacity, DWORD waitMs);

    // Blocks until one of `want` is pending or `timeoutMs` elapses.
    bool waitEvent(int want, DWORD timeoutMs) {
        const DWORD start = GetTickCount();
        for (;;) {
            pumpEvents();
            if ((want & kNeedInput) && needInput) return true;
            if ((want & kHaveOutput) && haveOutput > 0) return true;
            if (GetTickCount() - start >= timeoutMs) return false;
            // Sleep(0) only yields to same-or-higher priority threads. The MFT
            // does its encoding on its own worker, so a tight Sleep(0) loop here
            // starves it and the event only lands after the timeout expires.
            Sleep(1);
        }
    }
};

int H264Encoder::Impl::drainOutputs(std::vector<uint8_t>& out, size_t outCapacity, DWORD waitMs) {
    int nals = 0;
    const bool mftProvides = (outInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;
    const char* codec = hevc ? "h265" : "h264";
    for (;;) {
        if (async) {
            // ProcessOutput without a pending METransformHaveOutput is undefined
            // on async MFTs, so the event gates every call.
            if (haveOutput <= 0) {
                if (waitMs == 0) {
                    pumpEvents();
                    if (haveOutput <= 0) break;
                } else if (!waitEvent(kHaveOutput, waitMs)) {
                    break;
                }
            }
            haveOutput--;
        }
        MFT_OUTPUT_DATA_BUFFER odb{};
        odb.dwStreamID = 0;
        Slot* os = nullptr;
        if (!mftProvides) {
            os = take(outSlots, outNext, outCapacity);
            if (!os) break;
            os->buffer->SetCurrentLength(0);
            odb.pSample = os->sample.Get();
        }
        DWORD status = 0;
        HRESULT hrr = mft->ProcessOutput(0, 1, &odb, &status);
        if (hrr == MF_E_TRANSFORM_NEED_MORE_INPUT) break;
        if (FAILED(hrr)) {
            LOGW("%s: ProcessOutput failed (%s)", codec, mf_error_str(hrr));
            if (odb.pEvents) odb.pEvents->Release();
            break;
        }
        if (odb.pEvents) odb.pEvents->Release();
        IMFSample* smp = odb.pSample;
        if (!smp) continue;
        ComPtr<IMFMediaBuffer> ob;
        if (FAILED(smp->GetBufferByIndex(0, &ob))) continue;
        BYTE* p = nullptr;
        DWORD maxLen = 0, curLen = 0;
        if (FAILED(ob->Lock(&p, &maxLen, &curLen))) continue;
        const uint8_t* begin = reinterpret_cast<const uint8_t*>(p);
        const uint8_t* end = begin + curLen;
        const uint8_t* q = begin;
        size_t nalLen = 0;
        while ((q = nextNal(q, end, nalLen)) != nullptr) {
            if (nalLen == 0) break;
            // H.264 keeps the type in 5 bits of the first byte; HEVC uses 6 bits
            // of a two-byte header, so the same byte means something else.
            const uint8_t type =
                hevc ? (nalLen >= 2 ? (uint8_t)((q[0] >> 1) & 0x3f) : (uint8_t)0xff)
                     : (uint8_t)(q[0] & 0x1f);
            bool emit = false;
            if (hevc) {
                // 0..31 VCL, 32 VPS, 33 SPS, 34 PPS. Everything above that is
                // SEI/AUD/filler, which the receiver's HEVC branch does not parse
                // and the decoder does not need.
                if (type == 32) vps.assign(q, q + nalLen);
                else if (type == 33) sps.assign(q, q + nalLen);
                else if (type == 34) pps.assign(q, q + nalLen);
                emit = type <= 34;
            } else {
                if (type == 7) sps.assign(q, q + nalLen);
                else if (type == 8) pps.assign(q, q + nalLen);
                emit = (type == 1 || type == 5 || type == 6 || type == 7 || type == 8);
            }
            if (emit) {
                // 4-byte big-endian NAL length prefix, then the raw NAL.
                out.push_back((uint8_t)(nalLen >> 24));
                out.push_back((uint8_t)(nalLen >> 16));
                out.push_back((uint8_t)(nalLen >> 8));
                out.push_back((uint8_t)(nalLen));
                out.insert(out.end(), q, q + nalLen);
                nals++;
            }
            q += nalLen;
        }
        ob->Unlock();
        // With MFT_OUTPUT_STREAM_PROVIDES_SAMPLES the MFT handed us a new
        // reference; the pooled path keeps its own and must not release it.
        if (mftProvides && odb.pSample) odb.pSample->Release();
    }
    return nals;
}

H264Encoder::~H264Encoder() {
    stop();
}

// Bring-up of one MFT: types, low-latency knobs, output stream info. Split out
// of start() so a candidate that *activates* but then refuses the media types
// can be abandoned and the next one tried. Ending the session on the first
// refusal is what made one build work on one machine and fail on another --
// enumeration order is not the same everywhere.
bool H264Encoder::Impl::setup(size_t width, size_t height, int fps, int bitrate,
                              bool lowLatency, int maxQp, bool hevc) {
    Impl::hevc = hevc;
    failStage = "none";
    failHr = S_OK;
    inType.Reset();
    outType.Reset();
    // Deliberately NOT calling MF_TRANSFORM_ASYNC_UNLOCK here. AMD's encoders
    // report the property as set and then still refuse SetInputType with
    // MF_E_TRANSFORM_ASYNC_LOCKED -- i.e. they ignore the unlock and stay in
    // async mode. Probed the other way round below.
    UINT32 asyncFlag = 0;
    {
        ComPtr<IMFAttributes> ma;
        if (SUCCEEDED(mft->GetAttributes(&ma))) ma->GetUINT32(MF_TRANSFORM_ASYNC, &asyncFlag);
    }
    async = asyncFlag != 0;
    needInput = false;
    haveOutput = 0;
    evgen.Reset();
    if (async) {
        const HRESULT ehr = mft.As(&evgen);
        if (FAILED(ehr)) {
            LOGE("%s: async MFT has no event generator (%s)", hevc ? "h265" : "h264",
                 mf_error_str(ehr));
            failStage = "get-event-generator";
            failHr = ehr;
            return false;
        }
    }
    {
        // Try setting the unlock directly on the MFT too and read it back: if the
        // attribute is write-through this tells us whether the driver honours it
        // at all, or whether it is silently filtered.
        UINT32 unlNow = 0;
        HRESULT shr = S_FALSE;
        ComPtr<IMFAttributes> ma;
        if (SUCCEEDED(mft->GetAttributes(&ma))) {
            // Unconditional, including for the synchronous MFTs: the software
            // encoder reports async=0 and still needs this flag, otherwise it
            // accepts every frame and emits almost none.
            shr = ma->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
            ma->GetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, &unlNow);
        }
        LOGI("%s: candidate async=%u setOnMft=%s readback=%u", hevc ? "h265" : "h264", asyncFlag,
             mf_error_str(shr), unlNow);
    }
    // The two encoder families want *opposite* bring-up orders, and getting it
    // wrong fails silently in both directions (negotiation succeeds, then the MFT
    // emits nothing):
    //   sync  -> BEGIN_STREAMING before the types, no D3D manager, no start msg
    //   async -> D3D manager before the types, BEGIN_STREAMING + START_OF_STREAM
    //            after them
    if (async) {
        // Async hardware encoders negotiate happily without this and then produce
        // zero output; the synchronous ones stop producing output *because* of it.
        mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)nullptr);
    } else {
        mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    }
    // BEGIN_STREAMING deliberately moved below the type negotiation. An async
    // MFT that receives it before its media types are set enters the streaming
    // state and then rejects SetInputType with MF_E_TRANSFORM_ASYNC_LOCKED --
    // which is exactly the wall this path kept hitting.

    // --- output type first, then input --------------------------------------
    // Encoder MFTs refuse an input type until an output type exists:
    // SetInputType() before SetOutputType() fails with
    // MF_E_TRANSFORM_TYPE_NOT_SET (0xc00d6d60) even when the input type is one
    // the encoder advertises support for. Order is not interchangeable here.
    ComPtr<IMFMediaType> ot;
    HRESULT hr = MFCreateMediaType(&ot);
    if (FAILED(hr)) {
        failStage = "create-output-type";
        failHr = hr;
        return false;
    }
    ot->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    // HEVC_ES, not HEVC: the latter describes hvcC-wrapped samples (what an MP4
    // stores) and every encoder here rejects it for output. ES is the raw
    // length-prefixed stream this path actually needs.
    ot->SetGUID(MF_MT_SUBTYPE, hevc ? MFVideoFormat_HEVC_ES : MFVideoFormat_H264);
    MFSetAttributeSize(ot.Get(), MF_MT_FRAME_SIZE, (UINT32)width, (UINT32)height);
    MFSetAttributeRatio(ot.Get(), MF_MT_FRAME_RATE, (UINT32)fps, 1);
    ot->SetUINT32(MF_MT_AVG_BITRATE, (UINT32)bitrate);
    ot->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);

    // Profile ladder, best first. High buys the 8x8 transform and CABAC, which
    // is what stops desktop text from shimmering; Baseline (the old default)
    // exists for ancient decoders and wastes a lot of bits on screen content.
    // Every H.264 decoder that can do 1080p handles High, but keep the fallbacks
    // because a few third-party encoder MFTs advertise only a subset.
    struct ProfileStep {
        UINT32 profile;
        const char* name;
    };
    // HEVC has no Baseline/High split worth negotiating here: Main already
    // carries the whole toolset this path needs, and every H.265 decoder that
    // exists handles it. H.264 keeps the ladder because older encoders do not.
    static const ProfileStep kH264Profiles[] = {
        {eAVEncH264VProfile_High, "high"},
        {eAVEncH264VProfile_Main, "main"},
        {eAVEncH264VProfile_Base, "base"},
    };
    static const ProfileStep kHevcProfiles[] = {
        {eAVEncH265VProfile_Main_420_8, "main"},
        {eAVEncH265VProfile_Main_420_8, "main"},
        {eAVEncH265VProfile_Main_420_8, "main"},
    };
    const ProfileStep* kProfiles = hevc ? kHevcProfiles : kH264Profiles;
    const char* chosen = "encoder default";
    hr = E_FAIL;
    // HEVC encoders disagree about which subtype they accept as *output*:
    // HEVC_ES is the elementary stream every one of them produces, but a few
    // drivers only accept plain HEVC. Try ES first, then the other, each with
    // the profile ladder.
    const GUID* subCandidates[] = {hevc ? &MFVideoFormat_HEVC_ES : &MFVideoFormat_H264,
                                   &MFVideoFormat_HEVC};
    const int subCount = hevc ? 2 : 1;
    for (int si = 0; si < subCount && FAILED(hr); si++) {
        ot->SetGUID(MF_MT_SUBTYPE, *subCandidates[si]);
        for (int pi = 0; pi < 3; pi++) {
            const ProfileStep& step = kProfiles[pi];
            ot->SetUINT32(MF_MT_MPEG2_PROFILE, step.profile);
            hr = mft->SetOutputType(0, ot.Get(), 0);
            if (SUCCEEDED(hr)) {
                chosen = step.name;
                break;
            }
        }
        if (FAILED(hr)) {
            // Last resort for this subtype: no profile at all, let it pick.
            ot->DeleteItem(MF_MT_MPEG2_PROFILE);
            hr = mft->SetOutputType(0, ot.Get(), 0);
        }
    }
    if (FAILED(hr)) {
        failStage = "set-output-type";
        failHr = hr;
        return false;
    }
    outType = ot;

    // --- input type: NV12 at the target size --------------------------------
    ComPtr<IMFMediaType> it;
    hr = MFCreateMediaType(&it);
    if (FAILED(hr)) {
        failStage = "create-input-type";
        failHr = hr;
        return false;
    }
    it->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    it->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(it.Get(), MF_MT_FRAME_SIZE, (UINT32)width, (UINT32)height);
    MFSetAttributeRatio(it.Get(), MF_MT_FRAME_RATE, (UINT32)fps, 1);
    MFSetAttributeRatio(it.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    it->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    it->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);

    hr = mft->SetInputType(0, it.Get(), 0);
    if (FAILED(hr)) {
        // A couple of encoders object to the "all samples independent" hint on
        // the input; retry without it before giving up.
        it->DeleteItem(MF_MT_ALL_SAMPLES_INDEPENDENT);
        hr = mft->SetInputType(0, it.Get(), 0);
        if (FAILED(hr)) {
            // The driver refuses without saying why, so ask it directly what it
            // would accept. If this lists texture subtypes instead of NV12 the
            // MFT only takes D3D11 surfaces and a CPU-side path can never work.
            const char* tg = hevc ? "h265" : "h264";
            for (DWORD i = 0; i < 12; i++) {
                ComPtr<IMFMediaType> t;
                HRESULT gh = mft->GetInputAvailableType(0, i, &t);
                if (FAILED(gh)) {
                    if (i == 0) LOGI("%s: input types probe -> %s", tg, mf_error_str(gh));
                    break;
                }
                GUID sub{};
                t->GetGUID(MF_MT_SUBTYPE, &sub);
                UINT32 tw = 0, th2 = 0;
                MFGetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, &tw, &th2);
                LOGI("%s: accepts input[%lu] subtype=%s %ux%u", tg, (unsigned long)i,
                     guidStr(sub).c_str(), tw, th2);
            }
            for (DWORD i = 0; i < 12; i++) {
                ComPtr<IMFMediaType> t;
                if (FAILED(mft->GetOutputAvailableType(0, i, &t))) break;
                GUID sub{};
                t->GetGUID(MF_MT_SUBTYPE, &sub);
                UINT32 tw = 0, th2 = 0;
                MFGetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, &tw, &th2);
                LOGI("%s: accepts output[%lu] subtype=%s %ux%u", tg, (unsigned long)i,
                     guidStr(sub).c_str(), tw, th2);
            }
            LOGI("%s: intended input was NV12 %zux%zu", tg, width, height);
            failStage = "set-input-type";
            failHr = hr;
            return false;
        }
    }
    inType = it;

    // --- low-latency knobs (all best-effort) --------------------------------
    // Both of the two below are reported in the bring-up log: a soft image is
    // usually a knob the MFT accepted the call for and then ignored, and there
    // is no other way to tell that apart from "the driver is just bad".
    const char* rcName = "unset";
    int qpApplied = 0;
    // What the MFT reports back; 0xffffffff = it will not say.
    UINT32 rbRc = 0xffffffffu, rbMean = 0xffffffffu, rbMax = 0xffffffffu, rbQp = 0xffffffffu;
    ComPtr<ICodecAPI> codecApi;
    if (SUCCEEDED(mft.As(&codecApi))) {
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_BOOL;
        v.boolVal = lowLatency ? VARIANT_TRUE : VARIANT_FALSE;
        codecApi->SetValue(&CODECAPI_AVLowLatencyMode, &v);

        // No B-frames: they reorder the output, which would both add latency and
        // break the one-frame-in / N-NALs-out assumption of the send loop.
        VariantInit(&v);
        v.vt = VT_UI4;
        v.ulVal = 0;
        codecApi->SetValue(&CODECAPI_AVEncMPVDefaultBPictureCount, &v);

        // GOP of 3 s instead of 1 s. An IDR at 1080p costs 3-8x a normal frame
        // (and 80-130 ms at 4K), so keying every second was the main source of
        // frame drops; three seconds keeps the spike rare. A receiver that joins
        // mid-stream gets its first IDR from the keyframeRequested flag below,
        // and a reconnect restarts the encoder, so nothing waits on the GOP.
        // The SDK spells the GUID without "Default"; some newer SDKs call it
        // CODECAPI_AVEncMPVDefaultGOPSize, which is the same property.
        VariantInit(&v);
        v.vt = VT_UI4;
        v.ulVal = (UINT32)fps * 3;
        codecApi->SetValue(&CODECAPI_AVEncMPVGOPSize, &v);

        // MF_MT_AVG_BITRATE is only a media-type hint to some MFTs; the rate
        // control property is what the Microsoft encoder actually reads.
        VariantInit(&v);
        v.vt = VT_UI4;
        v.ulVal = (UINT32)bitrate;
        codecApi->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &v);

        // Peak allowance. Peak-constrained VBR sizes keyframes against this,
        // and with it unset "peak" collapses to the mean -- an IDR costs 3-8x a
        // normal frame, so it had to be squeezed into one average frame's budget
        // and visibly smeared the whole screen for a moment on every GOP. Twice
        // the mean is the usual ratio and still well inside what 5 GHz / wired
        // carries. Ignored (harmlessly) by constant-quality modes.
        VariantInit(&v);
        v.vt = VT_UI4;
        v.ulVal = (UINT32)bitrate * 2;
        codecApi->SetValue(&CODECAPI_AVEncCommonMaxBitRate, &v);

        // Rate control mode. Screen content is the awkward case for a rate
        // controller: a mostly idle desktop leaves the bit budget unspent while
        // text edges get quantised away, and plain VBR has no reason to care.
        // Low-delay VBR spends the idle bits where the detail is and still has
        // no lookahead or reordering, so it costs nothing in latency. CBR is
        // the fallback for encoders that only implement that. 4 and 0 are the
        // enum values, spelled out because eAVEncCommonRateControlMode_LowDelayVBR
        // is missing from older SDK headers.
        static const struct {
            UINT32 mode;
            const char* name;
        } kRcModes[] = {{4u, "low-delay-vbr"}, {1u, "peak-vbr"}, {0u, "cbr"}};
        for (const auto& rc : kRcModes) {
            VariantInit(&v);
            v.vt = VT_UI4;
            v.ulVal = rc.mode;
            if (SUCCEEDED(codecApi->SetValue(&CODECAPI_AVEncCommonRateControlMode, &v))) {
                rcName = rc.name;
                break;
            }
        }

        // Quantiser ceiling, the knob that decides whether small text survives.
        // With QP free to climb, a static desktop turns to mush no matter how
        // generous the bitrate is, because the controller is spending its
        // budget on nothing.
        if (maxQp > 0) {
            VariantInit(&v);
            v.vt = VT_UI4;
            v.ulVal = (UINT32)maxQp;
            if (SUCCEEDED(codecApi->SetValue(&CODECAPI_AVEncVideoMaxQP, &v))) qpApplied = maxQp;
        }

        // Read back what the MFT actually kept. SetValue returning S_OK only
        // means the GUID was recognised, not that the value is in effect --
        // "the knob is a no-op" and "the encoder is just weak" produce identical
        // output otherwise. 0xffffffff marks a property the MFT will not report.
        auto readback = [&](const GUID& g) -> UINT32 {
            VARIANT gv;
            VariantInit(&gv);
            UINT32 got = 0xffffffffu;
            if (SUCCEEDED(codecApi->GetValue(&g, &gv)) && gv.vt == VT_UI4) got = gv.ulVal;
            VariantClear(&gv);
            return got;
        };
        rbRc = readback(CODECAPI_AVEncCommonRateControlMode);
        rbMean = readback(CODECAPI_AVEncCommonMeanBitRate);
        rbMax = readback(CODECAPI_AVEncCommonMaxBitRate);
        rbQp = readback(CODECAPI_AVEncVideoMaxQP);

        VariantClear(&v);
    }

    hr = mft->GetOutputStreamInfo(0, &outInfo);
    if (FAILED(hr)) {
        failStage = "output-stream-info";
        failHr = hr;
        return false;
    }
    // Streaming starts only now that both types are accepted. Async MFTs that
    // got this message earlier refused SetInputType outright.
    if (async) {
        // Only now, with both types accepted, may an async MFT start streaming.
        mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        // And START_OF_STREAM is what makes it emit its first
        // METransformNeedInput; without it the encoder idles forever.
        mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    }

    frameDuration = 10000000LL / fps;
    started = true;
    keyframeRequested = true;
    LOGI("%s: negotiated %zux%zu @%dfps %dkbps gop=%d profile=%s rc=%s maxqp=%d",
         hevc ? "h265" : "h264", width, height, fps, bitrate / 1000, fps * 3, chosen, rcName,
         qpApplied);
    LOGI("%s: encoder holds rc=%d mean=%dk max=%dk qp=%d (0xffffffff = not reported)",
         hevc ? "h265" : "h264", (int)rbRc, (int)(rbMean / 1000), (int)(rbMax / 1000), (int)rbQp);
    return true;
}

bool H264Encoder::start(size_t width, size_t height, int fps, int bitrate, bool lowLatency,
                        int maxQp, bool hevc) {
    stop();
    Impl* im = new Impl();
    m_impl = im;
    m_width = width;
    m_height = height;
    m_hevc = hevc;
    m_detail.clear();
    const char* tag = hevc ? "h265" : "h264";

    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        LOGE("%s: MFStartup failed (0x%08lx)", tag, (unsigned long)hr);
        m_detail = std::string("MFStartup/") + mf_error_str(hr);
        return false;
    }

    MFT_REGISTER_TYPE_INFO regIn = {MFMediaType_Video, MFVideoFormat_NV12};
    // Enumerate by the *registered* subtype. For HEVC that is MFVideoFormat_HEVC
    // even though the same MFT only accepts HEVC_ES as an output type -- the two
    // are not interchangeable, and asking MFTEnumEx for HEVC_ES finds nothing at
    // all on this class of driver.
    MFT_REGISTER_TYPE_INFO regOut = {MFMediaType_Video, hevc ? MFVideoFormat_HEVC : MFVideoFormat_H264};

    // Several passes, best first, and every candidate has to survive the whole
    // negotiation -- not just ActivateObject(). A vendor encoder can activate
    // happily and then refuse the output type for a resolution or bitrate range
    // it does not handle; sorted so the preferred MFTs come first.
    struct Pass {
        DWORD flags;
        const char* name;
        bool hw;
    };
    static const Pass kPasses[] = {
        {MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER, "hardware",
         true},
        {MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER, "software", false},
        // Async encoders need an event loop this send path does not have, so
        // they are only worth a try once everything synchronous has refused.
        {MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
         "hardware-async", true},
    };

    std::string fails; // "name:stage/hresult", one entry per candidate that refused
    for (const Pass& pass : kPasses) {
        IMFActivate** activates = nullptr;
        UINT32 count = 0;
        hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, pass.flags, &regIn, &regOut, &activates,
                       &count);
        if (FAILED(hr) || count == 0) {
            if (activates) CoTaskMemFree(activates);
            continue;
        }
        LOGI("%s: %u %s encoder(s)", tag, count, pass.name);
        for (UINT32 i = 0; i < count; i++) {
            const std::string name = mftName(activates[i]);
            ComPtr<IMFTransform> mft;
            // Async vendors (AMD among them) must be unlocked *before* the object
            // exists. Setting MF_TRANSFORM_ASYNC_UNLOCK on the MFT afterwards is
            // too late: SetInputType still answers MF_E_TRANSFORM_ASYNC_LOCKED,
            // which is why these encoders were being skipped entirely before.
            // Set the async-unlock on the activation object (the only place MF
            // honours it) and then read it back from the MFT to see whether the
            // driver actually carried it across.
            // Decide before activating: creating an async MFT twice (once to
            // probe, once for real) leaves the second instance unable to produce
            // output. IMFActivate itself carries the async flag.
            {
                UINT32 isAsync = 0;
                activates[i]->GetUINT32(MF_TRANSFORM_ASYNC, &isAsync);
                const bool wantAsync = (pass.flags & MFT_ENUM_FLAG_ASYNCMFT) != 0;
                if (isAsync && !wantAsync) {
                    fails += name + ":async-deferred ";
                    continue;
                }
            }
            HRESULT ahr = activates[i]->ActivateObject(IID_PPV_ARGS(&mft));
            if (FAILED(ahr)) {
                fails += name + ":activate/" + mf_error_str(ahr) + " ";
                continue;
            }
            im->mft = mft;
            if (im->setup(width, height, fps, bitrate, lowLatency, maxQp, hevc)) {
                im->hw = pass.hw;
                m_backend = std::string(pass.name) + ": " + name;
                for (UINT32 j = 0; j < count; j++) activates[j]->Release();
                CoTaskMemFree(activates);
                LOGI("%s: encoder ready via %s", tag, m_backend.c_str());
                return true;
            }
            LOGW("%s: %s refused (%s, %s) - trying the next one", tag, name.c_str(),
                 im->failStage, mf_error_str(im->failHr));
            fails += name + ":" + im->failStage + "/" + mf_error_str(im->failHr) + " ";
            im->mft.Reset();
        }
        for (UINT32 j = 0; j < count; j++) activates[j]->Release();
        CoTaskMemFree(activates);
    }

    // Nothing that enumerated negotiated. The in-box Microsoft software encoder
    // is the guaranteed last resort: creating it by CLSID skips enumeration, so
    // it still works when the MFT cache on that machine is damaged, filtered by
    // a security product, or only lists encoders that want an async event loop.
    {
        ComPtr<IMFTransform> mft;
        HRESULT chr =
            CoCreateInstance(kMsH264EncoderMft, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&mft));
        if (SUCCEEDED(chr)) {
            im->mft = mft;
            if (im->setup(width, height, fps, bitrate, lowLatency, maxQp, hevc)) {
                im->hw = false;
                m_backend = "built-in Microsoft software encoder";
                LOGI("%s: encoder ready via %s", tag, m_backend.c_str());
                return true;
            }
            fails += std::string("ms-software:") + im->failStage + "/" + mf_error_str(im->failHr);
            im->mft.Reset();
        } else {
            fails += std::string("ms-software:create/") + mf_error_str(chr);
        }
    }

    m_detail = fails.empty() ? "no H.264 encoder MFT found" : fails;
    LOGE("%s: no usable encoder MFT (%s)", tag, m_detail.c_str());
    return false;
}

void H264Encoder::stop() {
    if (!m_impl) return;
    if (m_impl->stFrames > 0) {
        // One line per session. `dropped` is the async encoder refusing input
        // for long enough that we gave up on that frame -- a nonzero value here
        // means frame rate is below the target, not that the stream is broken.
        LOGI("%s: %d frames, %d submitted, %d dropped, %d NALs", m_impl->hevc ? "h265" : "h264",
             m_impl->stFrames, m_impl->stSubmit, m_impl->stNoNeed, m_impl->stNals);
    }
    if (m_impl->mft) {
        m_impl->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        m_impl->mft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    }
    delete m_impl;
    m_impl = nullptr;
    MFShutdown();
}

int H264Encoder::encodeNv12(const uint8_t* nv12, size_t width, size_t height,
                           uint64_t timestampNs, std::vector<uint8_t>& out) {
    if (!m_impl || !m_impl->started || !nv12) return 0;
    Impl* im = m_impl;
    if (width != m_width || height != m_height) {
        LOGW("h264: %zux%zu frame does not match the configured %zux%zu", width, height, m_width,
             m_height);
        return 0;
    }
    const size_t sampleSize = width * height * 3 / 2;

    Impl::Slot* in = im->take(im->inSlots, im->inNext, sampleSize);
    if (!in) return 0;
    BYTE* dst = nullptr;
    if (FAILED(in->buffer->Lock(&dst, nullptr, nullptr))) return 0;
    memcpy(dst, nv12, sampleSize);
    in->buffer->Unlock();
    in->buffer->SetCurrentLength((DWORD)sampleSize);

    IMFSample* sample = in->sample.Get();
    // Frame timestamps in the mirror stream are boot-relative nanoseconds; the
    // MFT only needs monotonic values, so reuse them directly (converted to
    // 100ns units).
    sample->SetSampleTime((LONGLONG)(timestampNs / 100));
    sample->SetSampleDuration(im->frameDuration);

    if (im->keyframeRequested) {
        ComPtr<IMFAttributes> attr;
        if (SUCCEEDED(sample->QueryInterface(IID_PPV_ARGS(&attr)))) {
            attr->SetUINT32(MFSampleExtension_CleanPoint, TRUE);
        }
        im->keyframeRequested = false;
    }

    const char* codec = im->hevc ? "h265" : "h264";
    const size_t outCapacity = sampleSize + (sampleSize / 2) + 65536;
    HRESULT hr = S_OK;
    int nals = 0;

    if (im->async) {
        // Collect the PREVIOUS frame before submitting this one. An async encoder
        // only asks for new input once its pending output has been taken, so
        // submitting first means waiting for an event that cannot arrive yet --
        // that was the "did not ask for input" stall. Ordering it this way costs
        // one frame of pipeline depth (~33 ms) and keeps the full frame rate.
        im->stFrames++;
        nals += im->drainOutputs(out, outCapacity, 0);
        if (!im->needInput) {
            // An async encoder will not announce NeedInput while it is still
            // holding a finished frame nobody collected, so this wait has to
            // service outputs as well. A bare event loop deadlocks until the
            // timeout and loses the frame -- that was ~11% of all frames.
            // The timeout itself has to be generous too: the AMD HEVC encoder
            // goes up to ~150 ms between asks (43% accepted at 30 ms, 89% at
            // 150 ms, no further gain at 400 ms).
            const DWORD t0 = GetTickCount();
            while (GetTickCount() - t0 < 150) {
                im->pumpEvents();
                if (im->haveOutput > 0) {
                    nals += im->drainOutputs(out, outCapacity, 0);
                    continue;
                }
                if (im->needInput) break;
                Sleep(1);
            }
        }
        if (im->needInput) {
            hr = im->mft->ProcessInput(0, sample, 0);
            if (hr == MF_E_NOTACCEPTING) {
                im->stNotAccepting++;
                if (im->waitEvent(Impl::kNeedInput, 150)) hr = im->mft->ProcessInput(0, sample, 0);
            }
            im->needInput = false;
            if (SUCCEEDED(hr)) im->stSubmit++;
            // Short wait: if this frame is already encoded, it goes out now
            // instead of a frame later.
            if (SUCCEEDED(hr)) nals += im->drainOutputs(out, outCapacity, 6);
        } else {
            im->stNoNeed++;
        }
        im->stNals += nals;
    } else {
        hr = im->mft->ProcessInput(0, sample, 0);
        if (SUCCEEDED(hr)) nals += im->drainOutputs(out, outCapacity, 0);
    }

    if (FAILED(hr)) {
        if (hr == MF_E_NOTACCEPTING) {
            LOGW("%s: encoder not accepting input", codec);
        } else {
            LOGW("%s: ProcessInput failed (%s)", codec, mf_error_str(hr));
        }
        // Whatever was already collected still belongs to a real frame, so it is
        // returned rather than thrown away.
        return nals;
    }
    return nals;
}

int H264Encoder::encodeFrame(const uint8_t* bgra, size_t width, size_t height, size_t stride,
                             uint64_t timestampNs, std::vector<uint8_t>& out) {
    if (!m_impl || !m_impl->started || !bgra) return 0;
    Impl* im = m_impl;
    if (width != m_width || height != m_height) {
        LOGW("h264: %zux%zu frame does not match the configured %zux%zu", width, height, m_width,
             m_height);
        return 0;
    }

    // Fallback path: the capture side could not convert on the GPU (GDI mode,
    // or a driver without video processing), so do it here.
    const size_t yPlane = width * height;
    const size_t nv12Size = yPlane + yPlane / 2;
    if (im->scratchNv12.size() < nv12Size) im->scratchNv12.assign(nv12Size, 0);
    uint8_t* dst = im->scratchNv12.data();

    // Y
    for (size_t y = 0; y < height; y++) {
        const uint8_t* src = bgra + y * stride;
        uint8_t* dy = dst + y * width;
        for (size_t x = 0; x < width; x++) {
            const uint8_t b = src[x * 4 + 0];
            const uint8_t g = src[x * 4 + 1];
            const uint8_t r = src[x * 4 + 2];
            // BT.601 full-range luma; matches what the mirror has always sent,
            // so a machine stuck on this path looks identical to before.
            int yv = (r * 77 + g * 150 + b * 29) >> 8;
            dy[x] = (uint8_t)(yv < 0 ? 0 : (yv > 255 ? 255 : yv));
        }
    }
    // UV (interleaved, half resolution)
    uint8_t* duv = dst + yPlane;
    for (size_t y = 0; y < height / 2; y++) {
        const uint8_t* s0 = bgra + (y * 2) * stride;
        const uint8_t* s1 = s0 + stride;
        uint8_t* d = duv + y * width;
        for (size_t x = 0; x < width / 2; x++) {
            int r = 0, g = 0, b = 0;
            const uint8_t* p[4] = {s0 + x * 8, s0 + x * 8 + 4, s1 + x * 8, s1 + x * 8 + 4};
            for (int i = 0; i < 4; i++) {
                b += p[i][0];
                g += p[i][1];
                r += p[i][2];
            }
            b >>= 2;
            g >>= 2;
            r >>= 2;
            int u = ((b * 127 - r * 43 - g * 84) >> 8) + 128;
            int v = ((r * 127 - g * 107 - b * 20) >> 8) + 128;
            d[x * 2 + 0] = (uint8_t)(u < 0 ? 0 : (u > 255 ? 255 : u));
            d[x * 2 + 1] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
        }
    }
    return encodeNv12(dst, width, height, timestampNs, out);
}

bool H264Encoder::parameterSets(std::vector<uint8_t>& sps, std::vector<uint8_t>& pps) const {
    if (!m_impl || m_impl->sps.empty() || m_impl->pps.empty()) return false;
    sps = m_impl->sps;
    pps = m_impl->pps;
    return true;
}

bool H264Encoder::hevcParameterSets(std::vector<uint8_t>& vps, std::vector<uint8_t>& sps,
                                    std::vector<uint8_t>& pps) const {
    if (!m_impl || m_impl->vps.empty() || m_impl->sps.empty() || m_impl->pps.empty()) return false;
    vps = m_impl->vps;
    sps = m_impl->sps;
    pps = m_impl->pps;
    return true;
}

// Diagnostics: which encoder MFTs this machine actually has, per codec. Kept in
// the encoder translation unit because it needs the MF headers, and next to the
// real start() path so the flags match what that path enumerates with. A machine
// can advertise an HEVC decoder but no HEVC encoder, and that difference decides
// whether switching codecs is even an option here.
void list_video_encoders() {
    if (FAILED(MFStartup(MF_VERSION))) {
        std::printf("mf: MFStartup failed\n");
        return;
    }
    struct Kind {
        const GUID* subtype;
        const char* name;
    };
    static const Kind kKinds[] = {
        {&MFVideoFormat_H264, "H.264"},
        {&MFVideoFormat_HEVC_ES, "H.265"},
    };
    static const struct {
        DWORD flags;
        const char* name;
    } kPasses[] = {
        {MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER, "hw"},
        {MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER, "sw"},
        {MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER, "hw-async"},
    };
    for (const Kind& k : kKinds) {
        MFT_REGISTER_TYPE_INFO regIn = {MFMediaType_Video, MFVideoFormat_NV12};
        MFT_REGISTER_TYPE_INFO regOut = {MFMediaType_Video, *k.subtype};
        std::printf("%s encoders:\n", k.name);
        int total = 0;
        for (const auto& pass : kPasses) {
            IMFActivate** activates = nullptr;
            UINT32 count = 0;
            if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, pass.flags, &regIn, &regOut, &activates,
                                 &count)) ||
                count == 0) {
                if (activates) CoTaskMemFree(activates);
                continue;
            }
            for (UINT32 i = 0; i < count; i++) {
                std::printf("  [%s] %s\n", pass.name, mftName(activates[i]).c_str());
                activates[i]->Release();
                total++;
            }
            CoTaskMemFree(activates);
        }
        if (total == 0) std::printf("  (none)\n");
        // The software H.264 transform is not enumerated under every Windows
        // build; it can still be created by CLSID and is the last-resort
        // fallback in start(). Report that separately so "(none)" above is not
        // read as "no H.264 at all".
        if (k.subtype == &MFVideoFormat_H264) std::printf("  (built-in fallback: CLSID_CMSH264EncoderMFT)\n");
    }
    MFShutdown();
}

} // namespace fc
