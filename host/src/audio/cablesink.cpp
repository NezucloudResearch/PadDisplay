// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#include "cablesink.h"
#include "audiomix.h"
#include "comcall.h"
#include "log.h"
#include "realtime.h"

#include <windows.h>
#include <mmreg.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <propsys.h>
#include <wrl/client.h>
#include <functional>
#include <memory>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace pd
{
    namespace
    {

        constexpr wchar_t kCableName[] = L"CABLE Input"; // VB-CABLE's playback device: what it plays shows up on "CABLE Output"
        constexpr REFERENCE_TIME kBufferHns = 200000;    // 20 ms WASAPI buffer
        constexpr DWORD kOpenTimeoutMs = 3000;
        constexpr DWORD kWakeTimeoutMs = 100; // the event normally comes every engine period; this is only for noticing stop_ and a lost device
        constexpr uint64_t kLongGapUs = 15000;

        // PKEY_Device_FriendlyName, written out: the SDK's definitions header needs INITGUID in exactly one file.
        const PROPERTYKEY kFriendlyNameKey = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

        // PKEY_DeviceInterface_FriendlyName: the audio adapter a device belongs to ("VB-Audio Virtual Cable"). A cable's playback
        // device and its recording device have the same one.
        const PROPERTYKEY kAdapterNameKey = {{0x026e516e, 0xb814, 0x414b, {0x83, 0xcd, 0x85, 0x6d, 0x6f, 0xef, 0x48, 0x22}}, 2};

        std::wstring DeviceText(IMMDevice *dev, const PROPERTYKEY &key)
        {
            ComPtr<IPropertyStore> props;
            std::wstring text;
            if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props)))
            {
                PROPVARIANT v;
                PropVariantInit(&v);
                if (SUCCEEDED(props->GetValue(key, &v)) && v.vt == VT_LPWSTR && v.pwszVal)
                    text = v.pwszVal;
                PropVariantClear(&v);
            }
            return text;
        }

        std::wstring DeviceName(IMMDevice *dev) { return DeviceText(dev, kFriendlyNameKey); }

        std::wstring DeviceId(IMMDevice *dev)
        {
            std::wstring id;
            LPWSTR wid = nullptr;
            if (dev && SUCCEEDED(dev->GetId(&wid)))
            {
                id = wid;
                CoTaskMemFree(wid);
            }
            return id;
        }

        bool Contains(std::wstring text, std::wstring wanted)
        { // case-insensitive
            for (auto &c : text)
                c = static_cast<wchar_t>(towlower(c));
            for (auto &c : wanted)
                c = static_cast<wchar_t>(towlower(c));
            return text.find(wanted) != std::wstring::npos;
        }

        // The first active playback (eRender) or recording (eCapture) device that `matches`.
        ComPtr<IMMDevice> FindDevice(EDataFlow flow, const std::function<bool(IMMDevice *)> &matches)
        {
            ComPtr<IMMDeviceEnumerator> en;
            ComPtr<IMMDeviceCollection> list;
            if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))) ||
                FAILED(en->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &list)))
                return nullptr;
            UINT n = 0;
            list->GetCount(&n);
            for (UINT i = 0; i < n; ++i)
            {
                ComPtr<IMMDevice> dev;
                if (SUCCEEDED(list->Item(i, &dev)) && matches(dev.Get()))
                    return dev;
            }
            return nullptr;
        }

        ComPtr<IMMDevice> FindCable(const std::wstring &preferred)
        {
            const std::wstring wanted = preferred.empty() ? kCableName : preferred;
            return FindDevice(eRender, [&](IMMDevice *dev)
                              { return Contains(DeviceName(dev), wanted); });
        }

    } // namespace

    // An event-driven render stream on the cable, open but not yet started.
    struct CableStream
    {
        ComPtr<IMMDevice> device;
        ComPtr<IAudioClient> client;
        ComPtr<IAudioRenderClient> render;
        WAVEFORMATEX *mix = nullptr;
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        UINT32 bufferFrames = 0;
        bool isFloat = false;

        CableStream() = default;
        CableStream(const CableStream &) = delete;
        CableStream &operator=(const CableStream &) = delete;
        ~CableStream()
        {
            CoTaskMemFree(mix);
            if (event)
                CloseHandle(event);
        }
    };

    namespace
    {

        void LogFailure(const char *what, HRESULT hr) { LOGI("mic: %s failed 0x%08lx", what, static_cast<unsigned long>(hr)); }

        // IAudioClient3 lets a shared-mode stream use the engine's smallest period (128 frames, 2.7 ms) instead of the default
        // 10 ms: about 35 ms less delay end to end (measured with VB-CABLE: 58-66 ms against 93-106 ms for the cable alone). But the
        // engine then wakes 375 times a second and on a 2-core test VM that starved the network thread (gaps up to 105 ms, dropouts),
        // so it is opt-in (settings.ini: cableMinPeriod=1). Windows 10 1607 and later.
        bool InitAtMinimumPeriod(IAudioClient3 *client, const WAVEFORMATEX *mix)
        {
            UINT32 defaultPeriod = 0, fundamental = 0, minPeriod = 0, maxPeriod = 0;
            HRESULT hr = client->GetSharedModeEnginePeriod(mix, &defaultPeriod, &fundamental, &minPeriod, &maxPeriod);
            if (SUCCEEDED(hr))
                hr = client->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK, minPeriod, mix, nullptr);
            LOGI("mic: IAudioClient3 period %u frames (default %u): %s", minPeriod, defaultPeriod, SUCCEEDED(hr) ? "in use" : "not available, using the default period");
            if (FAILED(hr))
                LOGI("mic: IAudioClient3 failed 0x%08lx", static_cast<unsigned long>(hr));
            return SUCCEEDED(hr);
        }

        bool InitStream(CableStream &stream, bool minPeriod)
        {
            ComPtr<IAudioClient3> client3;
            if (minPeriod && SUCCEEDED(stream.client.As(&client3)))
            {
                if (InitAtMinimumPeriod(client3.Get(), stream.mix))
                    return true;
                // A failed InitializeSharedAudioStream leaves the client unusable: take a new one.
                client3.Reset();
                stream.client.Reset();
                const HRESULT reopened = stream.device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &stream.client);
                if (FAILED(reopened))
                {
                    LogFailure("reopen the cable", reopened);
                    return false;
                }
            }
            const HRESULT hr = stream.client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, kBufferHns, 0, stream.mix, nullptr);
            if (FAILED(hr))
                LogFailure("initialize the cable", hr);
            return SUCCEEDED(hr);
        }

        bool ConnectStream(CableStream &stream)
        {
            HRESULT hr = stream.client->SetEventHandle(stream.event);
            if (SUCCEEDED(hr))
                hr = stream.client->GetBufferSize(&stream.bufferFrames);
            if (SUCCEEDED(hr))
                hr = stream.client->GetService(IID_PPV_ARGS(&stream.render));
            if (FAILED(hr))
                LogFailure("set up the cable", hr);
            return SUCCEEDED(hr);
        }

        bool ChooseSampleFormat(CableStream &stream)
        {
            const WAVEFORMATEX &mix = *stream.mix;
            stream.isFloat = mix.wBitsPerSample == 32 && IsFloatFormat(mix);
            const bool isPcm16 = mix.wBitsPerSample == 16 && !stream.isFloat;
            if (!stream.isFloat && !isPcm16)
            {
                LogFailure("use the cable's format", E_NOTIMPL);
                return false;
            }
            LOGI("mic: cable format %lu Hz, %u ch, %s, %u frames per buffer", mix.nSamplesPerSec, mix.nChannels, stream.isFloat ? "float" : "16-bit", stream.bufferFrames);
            return true;
        }

        // Null when the cable cannot be used; the reason is logged.
        std::unique_ptr<CableStream> OpenCableStream(const std::wstring &deviceId, bool minPeriod)
        {
            auto stream = std::make_unique<CableStream>();
            ComPtr<IMMDeviceEnumerator> devices;
            HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&devices));
            if (SUCCEEDED(hr))
                hr = devices->GetDevice(deviceId.c_str(), &stream->device);
            if (SUCCEEDED(hr))
                hr = stream->device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &stream->client);
            if (SUCCEEDED(hr))
                hr = stream->client->GetMixFormat(&stream->mix);
            if (FAILED(hr))
            {
                LogFailure("open the cable", hr);
                return nullptr;
            }
            if (!InitStream(*stream, minPeriod) || !ConnectStream(*stream) || !ChooseSampleFormat(*stream))
                return nullptr;
            return stream;
        }

        // The tablet's mono audio on every channel of the device.
        template <class Sample, class Convert>
        void SpreadToChannels(const float *mono, UINT32 frames, UINT32 channels, Sample *out, Convert convert)
        {
            for (UINT32 frame = 0; frame < frames; ++frame)
            {
                const Sample sample = convert(mono[frame]);
                for (UINT32 channel = 0; channel < channels; ++channel)
                    *out++ = sample;
            }
        }

        void WriteToDevice(const CableStream &stream, const float *mono, UINT32 frames, BYTE *out)
        {
            const UINT32 channels = stream.mix->nChannels;
            if (stream.isFloat)
                SpreadToChannels(mono, frames, channels, reinterpret_cast<float *>(out), [](float sample)
                                 { return sample; });
            else
                SpreadToChannels(mono, frames, channels, reinterpret_cast<int16_t *>(out), ToS16);
        }

    } // namespace

    bool CableInstalled(const std::wstring &preferred, std::wstring *name)
    {
        ComForThisCall com;
        const ComPtr<IMMDevice> cable = FindCable(preferred);
        if (cable && name)
            *name = DeviceName(cable.Get());
        return cable != nullptr;
    }

    std::wstring CableRecordingDeviceId(const std::wstring &preferred)
    {
        ComForThisCall com;
        const ComPtr<IMMDevice> cable = FindCable(preferred);
        const std::wstring adapter = cable ? DeviceText(cable.Get(), kAdapterNameKey) : L"";
        if (adapter.empty())
            return L"";
        return DeviceId(FindDevice(eCapture, [&](IMMDevice *dev)
                                   { return DeviceText(dev, kAdapterNameKey) == adapter; })
                            .Get());
    }

    std::wstring RecordingDeviceId(const std::wstring &wanted)
    {
        ComForThisCall com;
        return DeviceId(FindDevice(eCapture, [&](IMMDevice *dev)
                                   { return Contains(DeviceName(dev), wanted); })
                            .Get());
    }

    CableOutput::CableOutput(std::wstring preferredName, bool minPeriod) : preferred_(std::move(preferredName)), minPeriod_(minPeriod)
    {
        started_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }

    CableOutput::~CableOutput()
    {
        Close();
        if (started_)
            CloseHandle(static_cast<HANDLE>(started_));
    }

    bool CableOutput::Open(std::wstring &why)
    {
        std::wstring id, name;
        {
            ComForThisCall com;
            const ComPtr<IMMDevice> cable = FindCable(preferred_);
            id = DeviceId(cable.Get());
            if (cable)
                name = DeviceName(cable.Get());
        }
        if (id.empty())
        {
            why = L"No virtual audio cable (VB-CABLE) is installed on the PC.";
            return false;
        }
        LOGI("mic: playing the tablet's microphone into '%ls'", name.c_str());
        thread_ = std::thread([this, id]
                              { RenderLoop(id); });
        // The render thread opens the device and starts the stream; the first packets should find it running.
        if (WaitForSingleObject(static_cast<HANDLE>(started_), kOpenTimeoutMs) != WAIT_OBJECT_0 || !startedOk_)
        {
            why = L"The virtual audio cable could not be opened for playback.";
            Close();
            return false;
        }
        return true;
    }

    void CableOutput::Close()
    {
        stop_ = true;
        if (thread_.joinable())
            thread_.join();
    }

    void CableOutput::Restart()
    {
        buffer_.Restart();
        paused_ = true;
    }

    void CableOutput::Write(const int16_t *samples, size_t count)
    {
        NoteArrival();
        buffer_.Write(samples, count);
    }

    void CableOutput::NoteArrival()
    {
        const uint64_t nowUs = NowUs();
        const bool afterPause = paused_.exchange(false); // the pause was no gap in the audio
        if (lastWriteUs_ && !afterPause)
        {
            const uint64_t gap = nowUs - lastWriteUs_;
            if (gap > maxGapUs_)
                maxGapUs_ = gap;
            if (gap > kLongGapUs)
                ++gaps15_;
        }
        lastWriteUs_ = nowUs;
    }

    void CableOutput::RenderLoop(std::wstring deviceId)
    {
        ComForThisCall com; // first, so the stream's COM objects are released while COM still runs
        MakeRealtimeThread(L"Pro Audio");
        const std::unique_ptr<CableStream> stream = OpenCableStream(deviceId, minPeriod_);
        if (!stream || !StartStream(*stream))
        {
            failed_ = true;
            ReportOpened(false);
            return;
        }
        ReportOpened(true);
        PlayUntilEnded(*stream);
        stream->client->Stop();
        if (!stop_)
            failed_ = true; // the loop ended by itself: the device went away
    }

    bool CableOutput::StartStream(const CableStream &stream)
    {
        buffer_.Start(stream.mix->nSamplesPerSec, GetTickCount64());
        const HRESULT hr = stream.client->Start();
        if (FAILED(hr))
            LogFailure("start the cable", hr);
        return SUCCEEDED(hr);
    }

    void CableOutput::ReportOpened(bool ok)
    {
        startedOk_ = ok;
        SetEvent(static_cast<HANDLE>(started_));
    }

    void CableOutput::PlayUntilEnded(const CableStream &stream)
    {
        std::vector<float> mono(stream.bufferFrames);
        while (!stop_)
        {
            const bool woken = WaitForSingleObject(stream.event, kWakeTimeoutMs) == WAIT_OBJECT_0;
            UINT32 padding = 0;
            if (FAILED(stream.client->GetCurrentPadding(&padding)))
                return; // asked on a timeout too: a lost device may never signal
            const UINT32 frames = stream.bufferFrames - padding;
            if (!woken || frames == 0)
                continue;
            BYTE *out = nullptr;
            if (FAILED(stream.render->GetBuffer(frames, &out)))
                return;
            buffer_.Read(mono.data(), frames, GetTickCount64());
            WriteToDevice(stream, mono.data(), frames, out);
            stream.render->ReleaseBuffer(frames, 0);
        }
    }

} // namespace pd
