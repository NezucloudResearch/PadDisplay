// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#include "cablesink.h"
#include "log.h"
#include "net.h" // MakeRealtimeThread

#include <windows.h>
#include <mmreg.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <propsys.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <functional>

using Microsoft::WRL::ComPtr;

namespace pd {
namespace {

constexpr uint32_t kInputRate = 48000;       // what the tablet sends
constexpr size_t kStartFill = 960;           // 20 ms: the cushion before playing starts
constexpr wchar_t kCableName[] = L"CABLE Input"; // VB-CABLE's playback device: what it plays shows up on "CABLE Output"

// PKEY_Device_FriendlyName, written out: the SDK's definitions header needs INITGUID in exactly one file.
const PROPERTYKEY kFriendlyNameKey = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

// PKEY_DeviceInterface_FriendlyName: the audio adapter a device belongs to ("VB-Audio Virtual Cable"). A cable's playback
// device and its recording device have the same one.
const PROPERTYKEY kAdapterNameKey = {{0x026e516e, 0xb814, 0x414b, {0x83, 0xcd, 0x85, 0x6d, 0x6f, 0xef, 0x48, 0x22}}, 2};

std::wstring DeviceText(IMMDevice* dev, const PROPERTYKEY& key) {
    ComPtr<IPropertyStore> props;
    std::wstring text;
    if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(props->GetValue(key, &v)) && v.vt == VT_LPWSTR && v.pwszVal) text = v.pwszVal;
        PropVariantClear(&v);
    }
    return text;
}

std::wstring DeviceName(IMMDevice* dev) { return DeviceText(dev, kFriendlyNameKey); }

std::wstring DeviceId(IMMDevice* dev) {
    std::wstring id;
    LPWSTR wid = nullptr;
    if (dev && SUCCEEDED(dev->GetId(&wid))) {
        id = wid;
        CoTaskMemFree(wid);
    }
    return id;
}

bool Contains(std::wstring text, std::wstring wanted) { // case-insensitive
    for (auto& c : text) c = static_cast<wchar_t>(towlower(c));
    for (auto& c : wanted) c = static_cast<wchar_t>(towlower(c));
    return text.find(wanted) != std::wstring::npos;
}

// The first active playback (eRender) or recording (eCapture) device that `matches`.
ComPtr<IMMDevice> FindDevice(EDataFlow flow, const std::function<bool(IMMDevice*)>& matches) {
    ComPtr<IMMDeviceEnumerator> en;
    ComPtr<IMMDeviceCollection> list;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))) ||
        FAILED(en->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &list)))
        return nullptr;
    UINT n = 0;
    list->GetCount(&n);
    for (UINT i = 0; i < n; ++i) {
        ComPtr<IMMDevice> dev;
        if (SUCCEEDED(list->Item(i, &dev)) && matches(dev.Get())) return dev;
    }
    return nullptr;
}

ComPtr<IMMDevice> FindCable(const std::wstring& preferred) {
    const std::wstring wanted = preferred.empty() ? kCableName : preferred;
    return FindDevice(eRender, [&](IMMDevice* dev) { return Contains(DeviceName(dev), wanted); });
}

// COM for the length of one call, on whatever thread asks.
struct ComForThisCall {
    const bool started = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    ~ComForThisCall() {
        if (started) CoUninitialize();
    }
};

} // namespace

bool CableInstalled(const std::wstring& preferred, std::wstring* name) {
    ComForThisCall com;
    const ComPtr<IMMDevice> cable = FindCable(preferred);
    if (cable && name) *name = DeviceName(cable.Get());
    return cable != nullptr;
}

std::wstring CableRecordingDeviceId(const std::wstring& preferred) {
    ComForThisCall com;
    const ComPtr<IMMDevice> cable = FindCable(preferred);
    const std::wstring adapter = cable ? DeviceText(cable.Get(), kAdapterNameKey) : L"";
    if (adapter.empty()) return L"";
    return DeviceId(FindDevice(eCapture, [&](IMMDevice* dev) { return DeviceText(dev, kAdapterNameKey) == adapter; }).Get());
}

std::wstring RecordingDeviceId(const std::wstring& wanted) {
    ComForThisCall com;
    return DeviceId(FindDevice(eCapture, [&](IMMDevice* dev) { return Contains(DeviceName(dev), wanted); }).Get());
}

CableOutput::CableOutput(std::wstring preferredName, bool minPeriod) : preferred_(std::move(preferredName)), minPeriod_(minPeriod) {
    started_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

CableOutput::~CableOutput() {
    Close();
    if (started_) CloseHandle(static_cast<HANDLE>(started_));
}

bool CableOutput::Open(std::wstring& why) {
    std::wstring id, name;
    {
        ComForThisCall com;
        const ComPtr<IMMDevice> cable = FindCable(preferred_);
        id = DeviceId(cable.Get());
        if (cable) name = DeviceName(cable.Get());
    }
    if (id.empty()) {
        why = L"No virtual audio cable (VB-CABLE) is installed on the PC.";
        return false;
    }
    LOGI("mic: playing the tablet's microphone into '%ls'", name.c_str());
    thread_ = std::thread([this, id] { RenderLoop(id); });
    // The render thread opens the device and starts the stream; the first packets should find it running.
    if (WaitForSingleObject(static_cast<HANDLE>(started_), 3000) != WAIT_OBJECT_0 || !startedOk_) {
        why = L"The virtual audio cable could not be opened for playback.";
        Close();
        return false;
    }
    return true;
}

void CableOutput::Close() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
}

void CableOutput::Restart() {
    restart_ = true;
    paused_ = true;
}

void CableOutput::Write(const int16_t* samples, size_t count) {
    LARGE_INTEGER q, f;
    QueryPerformanceCounter(&q);
    QueryPerformanceFrequency(&f);
    const uint64_t nowUs = uint64_t(q.QuadPart) * 1000000ull / uint64_t(f.QuadPart);
    if (lastWriteUs_) {
        const uint64_t gap = nowUs - lastWriteUs_;
        if (gap > maxGapUs_) maxGapUs_ = gap;
        if (gap > 15000) ++gaps15_;
    }
    lastWriteUs_ = nowUs;
    if (paused_.exchange(false)) lastWriteUs_ = 0; // the next packet starts anew: the pause was no gap in the audio
    std::lock_guard lock(mutex_);
    for (size_t i = 0; i < count; ++i) ring_[head_++ % kRing] = samples[i] * (1.0f / 32768.0f);
    const uint64_t fill = head_ - tail_;
    constexpr size_t kMaxFill = 5760; // 120 ms: above this the oldest audio is dropped, to cap the delay
    if (fill > kMaxFill) { // the audio side fell behind: drop the oldest, keep the delay bounded
        dropped_ += fill - kStartFill;
        tail_ = head_ - kStartFill;
    }
}

void CableOutput::RenderLoop(std::wstring deviceId) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MakeRealtimeThread(L"Pro Audio");
    ComPtr<IMMDeviceEnumerator> en;
    ComPtr<IMMDevice> dev;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioClient3> client3;
    ComPtr<IAudioRenderClient> render;
    WAVEFORMATEX* mix = nullptr;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    auto fail = [&](const char* what, HRESULT hr) {
        LOGI("mic: %s failed 0x%08lx", what, static_cast<unsigned long>(hr));
        failed_ = true;
        startedOk_ = false;
        SetEvent(static_cast<HANDLE>(started_));
        if (mix) CoTaskMemFree(mix);
        if (event) CloseHandle(event);
        CoUninitialize();
    };
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en));
    if (SUCCEEDED(hr)) hr = en->GetDevice(deviceId.c_str(), &dev);
    if (SUCCEEDED(hr)) hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client);
    if (SUCCEEDED(hr)) hr = client->GetMixFormat(&mix);
    if (FAILED(hr)) return fail("open the cable", hr);

    // IAudioClient3 lets a shared-mode stream use the engine's smallest period (128 frames, 2.7 ms) instead of the default
    // 10 ms: about 35 ms less delay end to end (measured with VB-CABLE: 58-66 ms against 93-106 ms for the cable alone). But the
    // engine then wakes 375 times a second and on a 2-core test VM that starved the network thread (gaps up to 105 ms, dropouts),
    // so it is opt-in (settings.ini: cableMinPeriod=1). Windows 10 1607 and later.
    bool ok = false;
    if (minPeriod_ && SUCCEEDED(client.As(&client3))) {
        UINT32 def = 0, fundamental = 0, minPeriod = 0, maxPeriod = 0;
        HRESULT hrPeriod = client3->GetSharedModeEnginePeriod(mix, &def, &fundamental, &minPeriod, &maxPeriod);
        HRESULT hrInit = SUCCEEDED(hrPeriod) ? client3->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK, minPeriod, mix, nullptr) : hrPeriod;
        ok = SUCCEEDED(hrInit);
        LOGI("mic: IAudioClient3 period %u frames (default %u, minimum %u): %s", minPeriod, def, minPeriod,
             ok ? "in use" : "not available, using the default period");
        if (!ok) LOGI("mic: IAudioClient3 failed 0x%08lx", static_cast<unsigned long>(hrInit));
    }
    if (!ok) {
        if (client3) { // a failed InitializeSharedAudioStream leaves the client unusable: take a new one
            client3.Reset();
            client.Reset();
            hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client);
            if (FAILED(hr)) return fail("reopen the cable", hr);
        }
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 200000, 0, mix, nullptr);
        if (FAILED(hr)) return fail("initialize the cable", hr);
    }
    UINT32 bufferFrames = 0;
    hr = client->SetEventHandle(event);
    if (SUCCEEDED(hr)) hr = client->GetBufferSize(&bufferFrames);
    if (SUCCEEDED(hr)) hr = client->GetService(IID_PPV_ARGS(&render));
    if (FAILED(hr)) return fail("set up the cable", hr);

    // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, written out so this file does not need the KS GUID definitions.
    const GUID kFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
    const bool isFloat = mix->wBitsPerSample == 32 &&
                         (mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                          (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                           IsEqualGUID(reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mix)->SubFormat, kFloat)));
    const bool isPcm16 = mix->wBitsPerSample == 16 && !isFloat;
    if (!isFloat && !isPcm16) return fail("use the cable's format", E_NOTIMPL);
    const UINT32 channels = mix->nChannels;
    const double step = double(kInputRate) / double(mix->nSamplesPerSec); // input samples consumed per output frame
    LOGI("mic: cable format %lu Hz, %u ch, %s, %u frames per buffer", mix->nSamplesPerSec, channels, isFloat ? "float" : "16-bit", bufferFrames);

    // Fills the device buffer; true while there is a stream. The cushion (fill) grows by 10 ms after an underrun, up to
    // 80 ms, so a jittery Wi-Fi link ends up with a buffer that rides it out instead of crackling.
    size_t target = kStartFill;
    bool playing = false;
    double pos = 0; // fractional read position (input samples), for the resampler
    uint64_t lastUnderrun = GetTickCount64();
    client->Start();
    startedOk_ = true;
    SetEvent(static_cast<HANDLE>(started_));
    while (!stop_) {
        if (WaitForSingleObject(event, 100) != WAIT_OBJECT_0) continue;
        UINT32 padding = 0;
        if (FAILED(client->GetCurrentPadding(&padding))) break;
        const UINT32 frames = bufferFrames - padding;
        if (frames == 0) continue;
        BYTE* out = nullptr;
        if (FAILED(render->GetBuffer(frames, &out))) break;
        {
            std::lock_guard lock(mutex_);
            if (restart_.exchange(false)) { // see Restart: drop what is left and build the cushion again; no underrun
                tail_ = head_;
                playing = false;
                pos = 0;
            }
            if (!playing && head_ - tail_ >= target) playing = true;
            for (UINT32 f = 0; f < frames; ++f) {
                float s = 0;
                if (playing) {
                    const uint64_t need = uint64_t(pos) + 2; // one input sample ahead for the interpolation
                    if (head_ - tail_ >= need) {
                        const uint64_t i0 = tail_ + uint64_t(pos);
                        const float frac = float(pos - std::floor(pos));
                        s = ring_[i0 % kRing] * (1 - frac) + ring_[(i0 + 1) % kRing] * frac;
                        // Input and output run at the same rate, so a cushion left over from the start would stay for ever.
                        // Reading up to 2% faster while it is above the target works it off (inaudible for speech), the same
                        // correction the tablet's own audio playback uses.
                        const double excess = double(head_ - tail_) - double(target);
                        const double speedUp = excess > 0 ? std::min(0.02, excess / 2400.0 * 0.02) : 0.0;
                        pos += step * (1.0 + speedUp);
                        const uint64_t whole = uint64_t(pos);
                        tail_ += whole;
                        pos -= double(whole);
                    } else { // ran dry: stop, refill to a bigger cushion
                        ++underruns_;
                        playing = false;
                        pos = 0;
                        const uint64_t now = GetTickCount64();
                        if (now - lastUnderrun < 20000) target = std::min<size_t>(target + 480, 3840);
                        lastUnderrun = now;
                    }
                }
                if (isFloat) {
                    auto* p = reinterpret_cast<float*>(out) + size_t(f) * channels;
                    for (UINT32 c = 0; c < channels; ++c) p[c] = s;
                } else {
                    auto* p = reinterpret_cast<int16_t*>(out) + size_t(f) * channels;
                    const int16_t v = static_cast<int16_t>(std::clamp(s, -1.f, 1.f) * 32767.f);
                    for (UINT32 c = 0; c < channels; ++c) p[c] = v;
                }
            }
        }
        render->ReleaseBuffer(frames, 0);
        if (target > kStartFill && GetTickCount64() - lastUnderrun > 20000) {
            target = std::max<size_t>(kStartFill, target - 240); // calm again: shrink the cushion slowly
            lastUnderrun = GetTickCount64();
        }
    }
    client->Stop();
    if (!stop_) failed_ = true; // the loop ended by itself: the device went away
    CoTaskMemFree(mix);
    CloseHandle(event);
    CoUninitialize();
}

} // namespace pd
