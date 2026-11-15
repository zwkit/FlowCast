// FlowCast Windows client - WASAPI loopback capture implementation.
#include "audiocap.h"
#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys.h>

#include <cmath>
#include <chrono>
#include <cstring>

#pragma comment(lib, "mmdevapi.lib")
#pragma comment(lib, "ole32.lib")

namespace fc {

struct AudioCapture::Impl {
    IAudioClient* client = nullptr;
    IAudioCaptureClient* capture = nullptr;
    WAVEFORMATEX* mixFmt = nullptr;
    UINT32 bufferFrames = 0;
    bool started = false;
    bool comInit = false;
    // WASAPI loopback delivers nothing at all while the desktop is silent: no
    // packets, not even silence packets. The receiver's audio playout clock is
    // the master clock for A/V sync, so a silent desktop would leave the whole
    // session without a clock (and audio would only start on the first sound).
    // A real AirPlay sender streams continuously, so pad with silence here.
    std::chrono::steady_clock::time_point due{};
    bool dueInit = false;
};

AudioCapture::~AudioCapture() {
    stop();
}

bool AudioCapture::start(int sampleRate, int channels) {
    stop();
    Impl* im = new Impl();
    m_impl = im;
    m_rate = sampleRate;
    m_channels = channels;

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (hr == RPC_E_CHANGED_MODE) {
        hr = S_OK; // already initialised on this thread, that is fine
    }
    if (FAILED(hr)) {
        LOGE("audio: CoInitializeEx failed (0x%08lx)", (unsigned long)hr);
        return false;
    }
    im->comInit = true;

    IMMDeviceEnumerator* enumr = nullptr;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator), (void**)&enumr);
    if (FAILED(hr)) {
        LOGE("audio: cannot create device enumerator (0x%08lx)", (unsigned long)hr);
        return false;
    }
    IMMDevice* device = nullptr;
    hr = enumr->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    enumr->Release();
    if (FAILED(hr)) {
        LOGE("audio: no default render endpoint (0x%08lx)", (unsigned long)hr);
        return false;
    }

    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&im->client);
    device->Release();
    if (FAILED(hr)) {
        LOGE("audio: IAudioClient activate failed (0x%08lx)", (unsigned long)hr);
        return false;
    }

    hr = im->client->GetMixFormat(&im->mixFmt);
    if (FAILED(hr)) {
        LOGE("audio: GetMixFormat failed (0x%08lx)", (unsigned long)hr);
        return false;
    }
    LOGI("audio: mix format %u Hz, %u ch, %u bit", im->mixFmt->nSamplesPerSec,
         im->mixFmt->nChannels, im->mixFmt->wBitsPerSample);

    // AUDCLNT_STREAMFLAGS_LOOPBACK is what makes this a system-audio tap.
    // A 20 ms buffer keeps latency low without starving the engine.
    const REFERENCE_TIME bufDur = 20 * 10000; // 100ns units
    hr = im->client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                    AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                bufDur, 0, im->mixFmt, nullptr);
    if (FAILED(hr)) {
        // Some older stacks dislike the conversion flags; retry without them.
        hr = im->client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                                    bufDur, 0, im->mixFmt, nullptr);
        if (FAILED(hr)) {
            LOGE("audio: IAudioClient::Initialize failed (0x%08lx)", (unsigned long)hr);
            return false;
        }
    }

    hr = im->client->GetService(__uuidof(IAudioCaptureClient), (void**)&im->capture);
    if (FAILED(hr)) {
        LOGE("audio: IAudioCaptureClient service failed (0x%08lx)", (unsigned long)hr);
        return false;
    }
    hr = im->client->GetBufferSize(&im->bufferFrames);
    if (FAILED(hr)) return false;

    hr = im->client->Start();
    if (FAILED(hr)) {
        LOGE("audio: Start failed (0x%08lx)", (unsigned long)hr);
        return false;
    }
    im->started = true;
    LOGI("audio: loopback capture running (%u frame buffer)", im->bufferFrames);
    return true;
}

void AudioCapture::stop() {
    if (!m_impl) return;
    Impl* im = m_impl;
    if (im->started && im->client) im->client->Stop();
    if (im->capture) im->capture->Release();
    if (im->client) im->client->Release();
    if (im->mixFmt) CoTaskMemFree(im->mixFmt);
    delete im;
    m_impl = nullptr;
}

bool AudioCapture::readPacket(std::vector<float>& out, uint32_t& frames) {
    out.clear();
    frames = 0;
    if (!m_impl || !m_impl->started || !m_impl->capture) return false;
    Impl* im = m_impl;
    if (!im->dueInit) {
        im->due = std::chrono::steady_clock::now();
        im->dueInit = true;
    }

    UINT32 packetFrames = 0;
    HRESULT hr = im->capture->GetNextPacketSize(&packetFrames);
    if (FAILED(hr) || packetFrames == 0) {
        // Nothing was captured. Silence from the desktop does not produce
        // packets at all, so pace the stream ourselves: the receiver needs a
        // continuous audio clock to anchor video to, and without it a silent
        // desktop would stall the whole session.
        const auto now = std::chrono::steady_clock::now();
        if (now - im->due > std::chrono::milliseconds(500)) im->due = now; // resync after a stall
        const auto aheadUs = im->due < now
                                 ? std::chrono::duration_cast<std::chrono::microseconds>(now -
                                                                                          im->due)
                                       .count()
                                 : 0;
        size_t want = (size_t)((double)aheadUs * (double)m_rate / 1e6);
        const size_t quantum = (size_t)m_rate / 100;        // 10 ms
        const size_t capDown = (size_t)m_rate * 4 / 100;    // never burst over 40 ms
        if (want < quantum) return false;
        if (want > capDown) want = capDown;
        out.assign(want * (size_t)m_channels, 0.0f);
        frames = (uint32_t)want;
        im->due += std::chrono::microseconds((long long)(want * 1000000ULL / (size_t)m_rate));
        return true;
    }

    BYTE* data = nullptr;
    UINT32 numFrames = 0;
    DWORD flags = 0;
    hr = im->capture->GetBuffer(&data, &numFrames, &flags, nullptr, nullptr);
    if (FAILED(hr)) return false;

    const WAVEFORMATEX* wf = im->mixFmt;
    const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
    const size_t srcCh = wf->nChannels;
    const bool isFloat = (wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) ||
                         (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                          ((WAVEFORMATEXTENSIBLE*)wf)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
    const size_t bytesPerSample = wf->wBitsPerSample / 8;
    const bool is16 = wf->wBitsPerSample == 16 && !isFloat;

    out.reserve((size_t)numFrames * m_channels);
    for (UINT32 i = 0; i < numFrames; i++) {
        for (int c = 0; c < m_channels; c++) {
            float v = 0.0f;
            if (!silent && data) {
                const uint8_t* p = data + ((size_t)i * srcCh + (size_t)(c % (int)srcCh)) * bytesPerSample;
                if (isFloat && bytesPerSample == 4) {
                    memcpy(&v, p, 4);
                } else if (is16) {
                    int16_t s;
                    memcpy(&s, p, 2);
                    v = (float)s / 32768.0f;
                } else if (bytesPerSample == 4) {
                    int32_t s;
                    memcpy(&s, p, 4);
                    v = (float)s / 2147483648.0f;
                } else if (bytesPerSample == 3) {
                    int32_t s = (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                                          ((uint32_t)p[2] << 16));
                    if (s & 0x800000) s |= ~0xffffff;
                    v = (float)s / 8388608.0f;
                } else if (bytesPerSample == 1) {
                    v = ((float)p[0] - 128.0f) / 128.0f;
                }
            }
            out.push_back(v);
        }
    }

    im->capture->ReleaseBuffer(numFrames);
    frames = numFrames;

    // Resample when the mix rate differs from what we send. Linear
    // interpolation is plenty for a desktop tap at 48k -> 44.1k.
    if ((int)wf->nSamplesPerSec != m_rate && numFrames > 1) {
        const double ratio = (double)wf->nSamplesPerSec / (double)m_rate;
        const size_t outFrames = (size_t)((double)numFrames / ratio);
        std::vector<float> rs;
        rs.reserve(outFrames * m_channels);
        for (size_t i = 0; i < outFrames; i++) {
            const double srcPos = (double)i * ratio;
            size_t i0 = (size_t)srcPos;
            size_t i1 = i0 + 1 < numFrames ? i0 + 1 : i0;
            const double frac = srcPos - (double)i0;
            for (int c = 0; c < m_channels; c++) {
                const float a = out[(size_t)i0 * m_channels + c];
                const float b = out[(size_t)i1 * m_channels + c];
                rs.push_back((float)(a + (b - a) * frac));
            }
        }
        out.swap(rs);
        frames = (UINT32)(out.size() / m_channels);
    }

    // Keep the silence pacing deadline aligned with the audio actually
    // delivered, so a later idle period resumes at the right point instead of
    // dumping a burst of padding.
    const auto now = std::chrono::steady_clock::now();
    im->due += std::chrono::microseconds((long long)(frames * 1000000ULL / (size_t)m_rate));
    if (now - im->due > std::chrono::milliseconds(250)) im->due = now;
    return true;
}

} // namespace fc
