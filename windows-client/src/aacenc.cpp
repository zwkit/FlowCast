// FlowCast Windows client - Media Foundation AAC encoder implementation.
#include "aacenc.h"
#include "mferr.h"
#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfapi.h>
#include <mftransform.h>
#include <mfidl.h>
#include <codecapi.h>
#include <mferror.h>
#include <wrl/client.h>

#include <cstring>
#include <vector>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfuuid.lib")

using Microsoft::WRL::ComPtr;

namespace fc {

struct AacEncoder::Impl {
    ComPtr<IMFTransform> mft;
    ComPtr<IMFMediaType> inType;
    ComPtr<IMFMediaType> outType;
    MFT_OUTPUT_STREAM_INFO outInfo{};
    std::vector<uint8_t> asc;
    bool started = false;
    // Samples waiting to reach a whole 1024-sample AAC frame, in the byte
    // width the encoder negotiated (int16 or float32).
    std::vector<uint8_t> pending;
    LONGLONG pts100 = 0;
    // Whatever input type the encoder actually accepted. The capture path always
    // produces float32, so a 16-bit input has to be converted on the way in.
    UINT32 inputBits = 32;
    // Non-null when the encoder is an asynchronous MFT. Those only hand back
    // samples after signalling METransformHaveOutput; calling ProcessOutput on
    // its own returns MF_E_TRANSFORM_NEED_MORE_INPUT forever.
    ComPtr<IMFMediaEventGenerator> asyncGen;
};

AacEncoder::~AacEncoder() {
    stop();
}

bool AacEncoder::start(int sampleRate, int channels, int bitrate) {
    stop();
    Impl* im = new Impl();
    m_impl = im;

    if (FAILED(MFStartup(MF_VERSION))) {
        LOGE("aac: MFStartup failed");
        return false;
    }

    MFT_REGISTER_TYPE_INFO regIn = {MFMediaType_Audio, MFAudioFormat_PCM};
    MFT_REGISTER_TYPE_INFO regOut = {MFMediaType_Audio, MFAudioFormat_AAC};
    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    // Ask for synchronous MFTs first. An asynchronous one answers every
    // ProcessOutput with MF_E_TRANSFORM_NEED_MORE_INPUT until its
    // METransformHaveOutput event is drained, which this synchronous pipeline
    // never does -- audio would be silently dead.
    HRESULT hr = MFTEnumEx(MFT_CATEGORY_AUDIO_ENCODER, MFT_ENUM_FLAG_SYNCMFT, &regIn, &regOut,
                           &activates, &count);
    if (FAILED(hr) || count == 0) {
        if (activates) CoTaskMemFree(activates);
        activates = nullptr;
        count = 0;
        hr = MFTEnumEx(MFT_CATEGORY_AUDIO_ENCODER,
                       MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT, &regIn, &regOut,
                       &activates, &count);
        if (SUCCEEDED(hr) && count > 0) {
            LOGW("aac: no synchronous AAC encoder; falling back to an async one");
        }
    }
    LOGD("aac: MFTEnumEx -> %u encoder(s)", count);
    if (FAILED(hr) || count == 0) {
        LOGE("aac: no AAC encoder MFT available (%s, %u found)", mf_error_str(hr), count);
        if (activates) CoTaskMemFree(activates);
        return false;
    }
    // An asynchronous MFT answers every ProcessOutput with
    // MF_E_TRANSFORM_NEED_MORE_INPUT until its METransformHaveOutput event is
    // drained, which this synchronous pipeline never does -- audio would be
    // silently dead. Prefer a synchronous encoder explicitly.
    ComPtr<IMFTransform> asyncFallback;
    bool ok = false;
    for (UINT32 i = 0; i < count; i++) {
        ComPtr<IMFTransform> cand;
        if (FAILED(activates[i]->ActivateObject(IID_PPV_ARGS(&cand)))) continue;
        wchar_t* name = nullptr;
        UINT32 nameLen = 0;
        if (SUCCEEDED(activates[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &name,
                                                       &nameLen))) {
            char narrow[256] = {0};
            WideCharToMultiByte(CP_UTF8, 0, name, -1, narrow, sizeof(narrow) - 1, nullptr, nullptr);
            LOGD("aac: candidate %u/%u: %s", i + 1, count, narrow);
            CoTaskMemFree(name);
        }
        UINT32 isAsync = 0;
        ComPtr<IMFAttributes> attrs;
        bool async = false;
        if (SUCCEEDED(cand.As(&attrs))) {
            const HRESULT ha = attrs->GetUINT32(MF_TRANSFORM_ASYNC, &isAsync);
            LOGD("aac: candidate %u async attr -> 0x%08lx value %u", i + 1, (unsigned long)ha,
                 isAsync);
            if (SUCCEEDED(ha)) async = (isAsync != 0);
        }
        if (async) {
            if (!asyncFallback) asyncFallback = cand;
            continue;
        }
        im->mft = cand;
        ok = true;
        break;
    }
    if (!ok && asyncFallback) {
        im->mft = asyncFallback;
        ok = true;
        LOGW("aac: only an asynchronous AAC encoder is available; audio may stall");
    }
    for (UINT32 i = 0; i < count; i++) activates[i]->Release();
    CoTaskMemFree(activates);
    if (!ok) {
        LOGE("aac: could not activate the encoder MFT");
        return false;
    }

    // --- output type first, then input --------------------------------------
    // Same rule as the video encoder: an input type set before any output type
    // is rejected outright (MF_E_INVALIDMEDIATYPE for the AAC MFT), even when
    // the encoder advertises support for exactly that input.
    ComPtr<IMFMediaType> outType;
    if (FAILED(MFCreateMediaType(&outType))) return false;
    outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    outType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    outType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    outType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, (UINT32)sampleRate);
    outType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, (UINT32)channels);
    outType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, (UINT32)(bitrate / 8));
    hr = im->mft->SetOutputType(0, outType.Get(), 0);
    if (FAILED(hr)) {
        LOGE("aac: encoder refused AAC output (%s)", mf_error_str(hr));
        return false;
    }
    im->outType = outType;

    // The encoder reports the real AudioSpecificConfig through MF_MT_USER_DATA.
    // For AAC the blob is the 12-byte AAC WAVEFORMATEX extension followed by the
    // 2-byte AudioSpecificConfig, e.g. 00 00 29 00 ... 12 10 for LC/44.1k/stereo.
    // The Microsoft encoder does not prefix the ASC to its first sample, so this
    // is the only trustworthy source.
    {
        UINT32 blobSize = 0;
        if (SUCCEEDED(outType->GetBlobSize(MF_MT_USER_DATA, &blobSize)) && blobSize > 0) {
            std::vector<uint8_t> blob(blobSize);
            if (SUCCEEDED(outType->GetBlob(MF_MT_USER_DATA, blob.data(), blobSize, nullptr))) {
                char hex[3 * 24 + 1] = {0};
                const size_t n = blob.size() < 16 ? blob.size() : 16;
                for (size_t i = 0; i < n; i++) snprintf(hex + i * 3, 4, "%02x ", blob[i]);
                LOGD("aac: MF_MT_USER_DATA (%u bytes): %s", blobSize, hex);
                if (blob.size() >= 14) {
                    im->asc.assign(blob.end() - 2, blob.end());
                    LOGD("aac: ASC %02x %02x", im->asc[0], im->asc[1]);
                }
            }
        }
    }

    // Input: 32-bit float PCM (what our resampler produces), falling back to
    // 16-bit integer PCM on the rare encoder that only takes that.
    ComPtr<IMFMediaType> inType;
    if (FAILED(MFCreateMediaType(&inType))) return false;
    inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    // PCM input types are only complete with block alignment and the derived
    // byte rate; without them the AAC MFT accepts the type, takes every input
    // sample and then never reports an output.
    inType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, (UINT32)channels * 4);
    inType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, (UINT32)sampleRate * (UINT32)channels * 4);
    inType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    inType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
    inType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 32);
    inType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, (UINT32)sampleRate);
    inType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, (UINT32)channels);
    hr = im->mft->SetInputType(0, inType.Get(), 0);
    if (FAILED(hr)) {
        inType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        inType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        inType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, (UINT32)channels * 2);
        inType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
                          (UINT32)sampleRate * (UINT32)channels * 2);
        hr = im->mft->SetInputType(0, inType.Get(), 0);
        if (FAILED(hr)) {
            LOGE("aac: encoder refused PCM input (%s)", mf_error_str(hr));
            return false;
        }
        LOGW("aac: encoder took 16-bit PCM instead of float");
    }
    im->inType = inType;
    im->inType->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &im->inputBits);

    if (FAILED(im->mft->GetOutputStreamInfo(0, &im->outInfo))) return false;

    // Only a genuinely asynchronous MFT gates its output behind
    // METransformHaveOutput events. Synchronous encoders also expose
    // IMFMediaEventGenerator, so the MF_TRANSFORM_ASYNC attribute -- not the
    // interface -- is what decides whether a wait is needed.
    UINT32 isAsyncAttr = 0;
    ComPtr<IMFAttributes> mftAttrs;
    const bool isAsyncMft = SUCCEEDED(im->mft.As(&mftAttrs)) && mftAttrs &&
                            SUCCEEDED(mftAttrs->GetUINT32(MF_TRANSFORM_ASYNC, &isAsyncAttr)) &&
                            isAsyncAttr != 0;
    if (isAsyncMft) {
        ComPtr<IMFMediaEventGenerator> gen;
        if (SUCCEEDED(im->mft.As(&gen)) && gen) {
            im->asyncGen = gen;
            LOGW("aac: encoder is asynchronous; draining its event queue inline");
        }
        // An async MFT requires this opt-in before it will unlock or produce.
        mftAttrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
    }

    // Without BEGIN_STREAMING the AAC MFT happily accepts every input sample
    // and then reports MF_E_TRANSFORM_NEED_MORE_INPUT forever, so the whole
    // audio stream silently produces nothing.
    im->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    im->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    im->started = true;
    im->pts100 = 0;
    LOGI("aac: encoder ready %d Hz, %d ch, %d bps", sampleRate, channels, bitrate);
    return true;
}

void AacEncoder::stop() {
    if (!m_impl) return;
    if (m_impl->mft) {
        m_impl->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        m_impl->mft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    }
    delete m_impl;
    m_impl = nullptr;
    MFShutdown();
}

int AacEncoder::encode(const float* pcm, size_t frames, std::vector<uint8_t>& out) {
    if (!m_impl || !m_impl->started || !pcm || frames == 0) return 0;
    Impl* im = m_impl;
    UINT32 channels = 0, rate = 0;
    im->inType->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
    im->inType->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
    const size_t samples = frames * (size_t)channels;

    // The input type decides the byte width: float32 goes straight through, a
    // 16-bit input has to be narrowed. Hardcoding either one silently feeds the
    // MFT garbage on the machines that take the other.
    const bool as16 = im->inputBits <= 16;
    const size_t bytesPerSample = as16 ? 2 : 4;

    // The AAC MFT only emits an access unit once it holds a whole 1024-sample
    // frame, and it does not accept partial frames at all: feeding it the
    // capture buffer's 440-frame chunks produced nothing for the entire
    // session. Accumulate to exact frame boundaries here instead.
    static const size_t kFrameSamples = 1024;
    const size_t frameBytes = kFrameSamples * (size_t)channels * bytesPerSample;

    const size_t base = im->pending.size();
    im->pending.resize(base + samples * bytesPerSample);
    uint8_t* dst = im->pending.data() + base;
    if (as16) {
        int16_t* d = reinterpret_cast<int16_t*>(dst);
        for (size_t i = 0; i < samples; i++) {
            float v = pcm[i];
            if (v > 1.0f) v = 1.0f;
            else if (v < -1.0f) v = -1.0f;
            d[i] = (int16_t)(v * 32767.0f);
        }
    } else {
        memcpy(dst, pcm, samples * 4);
    }

    int produced = 0;
    size_t consumed = 0;
    while (im->pending.size() - consumed >= frameBytes) {
        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(MFCreateMemoryBuffer((DWORD)frameBytes, &buffer))) break;
        BYTE* in = nullptr;
        if (FAILED(buffer->Lock(&in, nullptr, nullptr))) break;
        memcpy(in, im->pending.data() + consumed, frameBytes);
        buffer->Unlock();
        buffer->SetCurrentLength((DWORD)frameBytes);

        ComPtr<IMFSample> sample;
        if (FAILED(MFCreateSample(&sample))) break;
        sample->AddBuffer(buffer.Get());
        sample->SetSampleTime(im->pts100);
        sample->SetSampleDuration((LONGLONG)kFrameSamples * 10000000LL / rate);
        im->pts100 += (LONGLONG)kFrameSamples * 10000000LL / rate;

        HRESULT hrIn = im->mft->ProcessInput(0, sample.Get(), 0);
        if (FAILED(hrIn)) {
            LOGW("aac: ProcessInput failed (%s)", mf_error_str(hrIn));
            break;
        }
        consumed += frameBytes;

        // An async MFT never yields on the first ask: it posts
        // METransformHaveOutput when a sample is ready, and ProcessOutput
        // keeps reporting NEED_MORE_INPUT until then.
        if (im->asyncGen) {
            for (int tries = 0; tries < 40; tries++) {
                ComPtr<IMFMediaEvent> ev;
                const HRESULT he = im->asyncGen->GetEvent(MF_EVENT_FLAG_NO_WAIT, &ev);
                if (he == MF_E_NO_EVENTS_AVAILABLE) {
                    Sleep(1);
                    continue;
                }
                if (FAILED(he)) break;
                MediaEventType type = MEUnknown;
                ev->GetType(&type);
                if (type == METransformHaveOutput || type == METransformNeedInput) break;
            }
        }

        for (;;) {
        MFT_OUTPUT_DATA_BUFFER odb{};
        odb.dwStreamID = 0;
        ComPtr<IMFSample> outSample;
        if ((im->outInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) == 0) {
            if (FAILED(MFCreateSample(&outSample))) break;
            ComPtr<IMFMediaBuffer> ob;
            if (FAILED(MFCreateMemoryBuffer(65536, &ob))) break;
            outSample->AddBuffer(ob.Get());
            odb.pSample = outSample.Get();
        }
        DWORD status = 0;
        HRESULT hr = im->mft->ProcessOutput(0, 1, &odb, &status);
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) break;
        if (FAILED(hr)) {
            LOGW("aac: ProcessOutput failed (%s)", mf_error_str(hr));
            break;
        }
        if (odb.pEvents) odb.pEvents->Release();
        IMFSample* s = odb.pSample;
        if (!s) continue;
        ComPtr<IMFMediaBuffer> ob;
        if (SUCCEEDED(s->GetBufferByIndex(0, &ob))) {
            BYTE* p = nullptr;
            DWORD maxLen = 0, curLen = 0;
            if (SUCCEEDED(ob->Lock(&p, &maxLen, &curLen)) && curLen > 0) {
                const uint8_t* b = reinterpret_cast<const uint8_t*>(p);
                // Every input frame yields exactly one raw AAC access unit: no
                // length prefix, no AudioSpecificConfig header. RAOP carries the
                // config implicitly through ct/spf, so the AU goes out verbatim.
                out.insert(out.end(), b, b + curLen);
                produced++;
                ob->Unlock();
            }
        }
        if (odb.pSample && (im->outInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
            odb.pSample->Release();
        }
        }
    }
    if (consumed > 0) {
        im->pending.erase(im->pending.begin(), im->pending.begin() + (long)consumed);
    }
    return produced;
}

bool AacEncoder::asc(std::vector<uint8_t>& out) const {
    if (!m_impl || m_impl->asc.empty()) return false;
    out = m_impl->asc;
    return true;
}

} // namespace fc
