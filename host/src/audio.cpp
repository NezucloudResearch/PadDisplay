// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#include "audio.h"
#include "log.h"
#include "net.h"

#include <windows.h>
#include <audioclient.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <ksmedia.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace pd {
namespace {

constexpr REFERENCE_TIME kBufferHns = 200000; // 20 ms WASAPI buffer; we drain it every ~5 ms
constexpr uint64_t kPollUs = 5000;

// Sample reader for the device mix format: float32 or 16/24/32-bit integer PCM.
struct SampleReader {
    bool isFloat = false;
    int bytes = 4;
    float Read(const BYTE* p) const {
        if (isFloat) return *reinterpret_cast<const float*>(p);
        switch (bytes) {
        case 2: return *reinterpret_cast<const int16_t*>(p) / 32768.f;
        case 3: return float(int32_t(uint32_t(p[0]) << 8 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 24)) / 2147483648.f;
        default: return float(*reinterpret_cast<const int32_t*>(p)) / 2147483648.f;
        }
    }
};

SampleReader ReaderFor(const WAVEFORMATEX* f) {
    SampleReader r;
    r.bytes = f->wBitsPerSample / 8;
    if (f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) r.isFloat = true;
    if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        auto* x = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(f);
        r.isFloat = x->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        r.bytes = f->wBitsPerSample / 8; // container size
    }
    return r;
}

int16_t ToS16(float v) { return int16_t(std::clamp(v, -1.f, 1.f) * 32767.f); }

} // namespace

AudioSession::AudioSession(SOCKET sock, bool mutePc) : sock_(sock), mutePc_(mutePc) {}

AudioSession::~AudioSession() {
    Stop();
    closesocket(sock_);
}

void AudioSession::Stop() {
    stop_ = true;
    shutdown(sock_, SD_BOTH);
}

void AudioSession::Run() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MakeRealtimeThread(L"Audio");
    // Small send buffer: if the network backs up we drop audio instead of queueing it (latency cap).
    int sndbuf = 32 * 1024;
    setsockopt(sock_, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<char*>(&sndbuf), sizeof(sndbuf));
    DWORD sendTimeout = 200; // a stalled network must not hold the capture (and the PC mute) open
    setsockopt(sock_, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<char*>(&sendTimeout), sizeof(sendTimeout));
    while (!stop_ && !PeerClosed()) {
        if (!Stream()) break;
        Sleep(500); // default device changed or was lost: pick up the new one
    }
    LOGI("audio: ended (%llu packets sent, %llu dropped)", sentPackets_, droppedPackets_);
    CoUninitialize();
}

// The tablet never sends on the audio connection, so "readable" means it closed it.
bool AudioSession::PeerClosed() {
    fd_set rs;
    FD_ZERO(&rs);
    FD_SET(sock_, &rs);
    timeval none{0, 0};
    if (select(0, &rs, nullptr, nullptr, &none) == 0) return false;
    char c;
    return recv(sock_, &c, 1, MSG_PEEK) <= 0;
}

bool AudioSession::Stream() {
    ComPtr<IMMDeviceEnumerator> en;
    ComPtr<IMMDevice> dev;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;
    WAVEFORMATEX* fmt = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))) ||
        FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) ||
        FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client)) || FAILED(client->GetMixFormat(&fmt))) {
        LOGI("audio: no default output device");
        Sleep(1000);
        return !stop_;
    }
    HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, kBufferHns, 0, fmt, nullptr);
    if (SUCCEEDED(hr)) hr = client->GetService(IID_PPV_ARGS(&capture));
    if (FAILED(hr)) {
        LOGI("audio: loopback init failed 0x%08lx", hr);
        CoTaskMemFree(fmt);
        Sleep(1000);
        return !stop_;
    }
    const int channels = fmt->nChannels;
    const uint32_t rate = fmt->nSamplesPerSec;
    const SampleReader reader = ReaderFor(fmt);
    const int frameBytes = fmt->nBlockAlign;
    CoTaskMemFree(fmt);

    // Loopback is taken before the endpoint mute, so the PC can be silenced while the tablet plays.
    ComPtr<IAudioEndpointVolume> volume;
    BOOL wasMuted = FALSE;
    if (mutePc_ && SUCCEEDED(dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, &volume))) {
        volume->GetMute(&wasMuted);
        volume->SetMute(TRUE, nullptr);
    }

    uint8_t format[6];
    wr32(format, rate);
    format[4] = 2;  // channels sent
    format[5] = 16; // bits per sample
    bool ok = SendMsg(sock_, Msg::AudioFormat, format, sizeof(format));
    LOGI("audio: streaming %u Hz, %d ch -> stereo s16%s", rate, channels, mutePc_ ? " (PC muted)" : "");
    client->Start();

    std::vector<uint8_t> packet;
    bool closed = false;
    const uint64_t start = NowUs();
    while (ok && !stop_) {
        SleepUs(kPollUs);
        // Checked every loop so the PC is unmuted at once even when nothing is playing.
        if (PeerClosed()) {
            closed = true;
            break;
        }
        packet.assign(8, 0);
        wr64(packet.data(), NowUs() - start);
        UINT32 next = 0;
        while (SUCCEEDED(hr = capture->GetNextPacketSize(&next)) && next > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            if (FAILED(hr = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
            size_t at = packet.size();
            packet.resize(at + size_t(frames) * 4);
            auto* out = reinterpret_cast<int16_t*>(packet.data() + at);
            for (UINT32 i = 0; i < frames; ++i) {
                float l = 0, r = 0;
                if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
                    const BYTE* f = data + size_t(i) * frameBytes;
                    l = reader.Read(f);
                    r = channels > 1 ? reader.Read(f + reader.bytes) : l;
                    // Surround down-mix: centre to both sides, extra channels alternately left/right.
                    if (channels > 2) {
                        float c = reader.Read(f + 2 * reader.bytes) * 0.707f;
                        l += c, r += c;
                        for (int ch = 4; ch < channels; ++ch) (ch % 2 ? r : l) += 0.5f * reader.Read(f + ch * reader.bytes);
                    }
                }
                out[2 * i] = ToS16(l);
                out[2 * i + 1] = ToS16(r);
            }
            capture->ReleaseBuffer(frames);
        }
        if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
            LOGI("audio: output device changed");
            break;
        }
        if (packet.size() <= 8) continue; // nothing playing on the PC
        // Latency cap: only send if the socket can take it right now; otherwise drop this packet.
        fd_set ws;
        FD_ZERO(&ws);
        FD_SET(sock_, &ws);
        timeval zero{0, 0};
        if (select(0, nullptr, &ws, nullptr, &zero) == 1) {
            ok = SendMsg(sock_, Msg::AudioData, packet.data(), uint32_t(packet.size()));
            ++sentPackets_;
        } else {
            ++droppedPackets_;
        }
    }
    client->Stop();
    if (volume) volume->SetMute(wasMuted, nullptr);
    LOGI("audio: capture run ended (%s)", closed ? "tablet disconnected" : !ok ? "send failed" : stop_ ? "stopped" : "device changed");
    return ok && !stop_ && !closed;
}

} // namespace pd
