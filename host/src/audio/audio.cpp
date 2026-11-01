// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#include "audio.h"
#include "audiomix.h"
#include "comcall.h"
#include "log.h"
#include "realtime.h"
#include "net/net.h"

#include <windows.h>
#include <audioclient.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
#include <atomic>
#include <memory>
#include <optional>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace pd
{
    namespace
    {

        constexpr REFERENCE_TIME kBufferHns = 200000; // 20 ms WASAPI buffer; we drain it every ~5 ms
        constexpr uint64_t kPollUs = 5000;            // half the ~10 ms Windows mix period, so a mixed packet waits here at most 5 ms
        constexpr int kSendBufferBytes = 32 * 1024;   // small: if the network backs up we drop audio instead of queueing it (latency cap)
        constexpr DWORD kSendTimeoutMs = 200;         // a stalled network must not hold the capture (and the PC mute) open
        constexpr DWORD kOpenRetryMs = 1000;
        constexpr DWORD kReopenDelayMs = 500;

        constexpr uint8_t kSentChannels = 2;
        constexpr uint8_t kSentBitsPerSample = 16;
        constexpr size_t kSentFrameBytes = kSentChannels * kSentBitsPerSample / 8;
        constexpr size_t kPtsBytes = sizeof(uint64_t);

        // Loopback is taken before the endpoint mute, so the PC can be silenced while the tablet plays.
        class ScopedPcMute
        {
        public:
            explicit ScopedPcMute(IMMDevice *device)
            {
                if (FAILED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, &volume_)))
                    return;
                volume_->GetMute(&wasMuted_);
                volume_->SetMute(TRUE, nullptr);
            }
            ~ScopedPcMute()
            {
                if (volume_)
                    volume_->SetMute(wasMuted_, nullptr);
            }
            ScopedPcMute(const ScopedPcMute &) = delete;
            ScopedPcMute &operator=(const ScopedPcMute &) = delete;

        private:
            ComPtr<IAudioEndpointVolume> volume_;
            BOOL wasMuted_ = FALSE;
        };

        // A stream opened through IMMDevice stays on that device when the user picks another default output, without any error, so
        // only this notification tells. Windows calls it on its own thread and it must not block: it raises a flag and nothing more.
        class DefaultDeviceWatcher final : public IMMNotificationClient
        {
        public:
            bool Changed() const { return changed_; }

            STDMETHODIMP QueryInterface(REFIID iid, void **out) override
            {
                if (iid != __uuidof(IUnknown) && iid != __uuidof(IMMNotificationClient))
                {
                    *out = nullptr;
                    return E_NOINTERFACE;
                }
                *out = this;
                AddRef();
                return S_OK;
            }
            STDMETHODIMP_(ULONG)
            AddRef() override { return ++refs_; }
            STDMETHODIMP_(ULONG)
            Release() override
            {
                const ULONG left = --refs_;
                if (left == 0)
                    delete this;
                return left;
            }

            STDMETHODIMP OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override
            {
                if (flow == eRender && role == eConsole)
                    changed_ = true;
                return S_OK;
            }
            STDMETHODIMP OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
            STDMETHODIMP OnDeviceAdded(LPCWSTR) override { return S_OK; }
            STDMETHODIMP OnDeviceRemoved(LPCWSTR) override { return S_OK; }
            STDMETHODIMP OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

        private:
            ~DefaultDeviceWatcher() = default;

            std::atomic<ULONG> refs_{1};
            std::atomic<bool> changed_{false};
        };

    } // namespace

    // The capture of the default output device for one run. `devices` is declared first so it outlives the watcher it unregisters.
    struct AudioLoopback
    {
        ComPtr<IMMDeviceEnumerator> devices;
        ComPtr<DefaultDeviceWatcher> watcher;
        ComPtr<IMMDevice> device;
        ComPtr<IAudioClient> client;
        ComPtr<IAudioCaptureClient> capture;
        std::optional<StereoMixer> mixer;
        uint32_t rate = 0;
        int channels = 0;

        ~AudioLoopback()
        {
            if (watcher)
                devices->UnregisterEndpointNotificationCallback(watcher.Get());
        }
    };

    namespace
    {

        HRESULT InitCapture(AudioLoopback &loopback, const WAVEFORMATEX &format)
        {
            HRESULT hr = loopback.client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, kBufferHns, 0, &format, nullptr);
            if (SUCCEEDED(hr))
                hr = loopback.client->GetService(IID_PPV_ARGS(&loopback.capture));
            if (FAILED(hr))
                return hr;
            loopback.mixer.emplace(format);
            loopback.rate = format.nSamplesPerSec;
            loopback.channels = format.nChannels;
            return S_OK;
        }

        // Null when there is nothing to capture right now; the reason is logged.
        std::unique_ptr<AudioLoopback> OpenDefaultLoopback()
        {
            auto loopback = std::make_unique<AudioLoopback>();
            WAVEFORMATEX *format = nullptr;
            if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&loopback->devices))))
            {
                // Watching comes before asking for the default, so a change in between still counts.
                loopback->watcher.Attach(new DefaultDeviceWatcher);
                loopback->devices->RegisterEndpointNotificationCallback(loopback->watcher.Get());
            }
            if (!loopback->devices || FAILED(loopback->devices->GetDefaultAudioEndpoint(eRender, eConsole, &loopback->device)) ||
                FAILED(loopback->device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &loopback->client)) ||
                FAILED(loopback->client->GetMixFormat(&format)))
            {
                LOGI("audio: no default output device");
                return nullptr;
            }
            const HRESULT hr = InitCapture(*loopback, *format);
            CoTaskMemFree(format);
            if (FAILED(hr))
            {
                LOGI("audio: loopback init failed 0x%08lx", hr);
                return nullptr;
            }
            return loopback;
        }

        // Appends everything captured so far to `packet`. A failure is returned as it is: the caller decides what it means.
        HRESULT DrainCapture(IAudioCaptureClient *capture, const StereoMixer &mixer, std::vector<uint8_t> &packet)
        {
            UINT32 nextFrames = 0;
            HRESULT hr;
            while (SUCCEEDED(hr = capture->GetNextPacketSize(&nextFrames)) && nextFrames > 0)
            {
                BYTE *data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                if (FAILED(hr = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr)))
                    break;
                const size_t at = packet.size();
                packet.resize(at + frames * kSentFrameBytes); // zeroes it, which is all a silent buffer needs
                if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT))
                    mixer.ToStereoS16(data, frames, reinterpret_cast<int16_t *>(packet.data() + at));
                capture->ReleaseBuffer(frames);
            }
            return hr;
        }

        void CapSendQueue(SOCKET s)
        {
            int sendBuffer = kSendBufferBytes;
            setsockopt(s, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<char *>(&sendBuffer), sizeof(sendBuffer));
            DWORD sendTimeout = kSendTimeoutMs;
            setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<char *>(&sendTimeout), sizeof(sendTimeout));
        }

        fd_set SetOf(SOCKET s)
        {
            fd_set set;
            FD_ZERO(&set);
            FD_SET(s, &set);
            return set;
        }

        // These ask the socket and not the TLS layer: there is nothing to read from the tablet, and the send has its own timeout.
        bool ReadableNow(SOCKET s)
        {
            fd_set set = SetOf(s);
            timeval now{0, 0};
            return select(0, &set, nullptr, nullptr, &now) != 0;
        }

        bool WritableNow(SOCKET s)
        {
            fd_set set = SetOf(s);
            timeval now{0, 0};
            return select(0, nullptr, &set, nullptr, &now) == 1;
        }

    } // namespace

    AudioSession::AudioSession(Conn conn, bool mutePc)
        : conn_(std::move(conn)), mutePc_(mutePc), stopEvent_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}

    AudioSession::~AudioSession()
    {
        Stop();
        closesocket(conn_.s);
        CloseHandle(stopEvent_);
    }

    void AudioSession::Stop()
    {
        stop_ = true;
        SetEvent(stopEvent_);
        shutdown(conn_.s, SD_BOTH);
    }

    void AudioSession::Run()
    {
        ComForThisCall com;
        MakeRealtimeThread(L"Audio");
        CapSendQueue(conn_.s);
        while (!stop_ && !PeerClosed())
        {
            if (Stream() != StreamEnd::DeviceLost)
                break;
            WaitForStop(kReopenDelayMs);
        }
        LOGI("audio: ended (%llu packets sent, %llu dropped)", sentPackets_, droppedPackets_);
    }

    void AudioSession::WaitForStop(DWORD ms) { WaitForSingleObject(stopEvent_, ms); }

    // The tablet never sends on the audio connection, so "readable" means it closed it.
    bool AudioSession::PeerClosed()
    {
        if (!ReadableNow(conn_.s))
            return false;
        char c;
        return recv(conn_.s, &c, 1, MSG_PEEK) <= 0;
    }

    AudioSession::StreamEnd AudioSession::Stream()
    {
        const std::unique_ptr<AudioLoopback> loopback = OpenDefaultLoopback();
        if (!loopback)
        {
            WaitForStop(kOpenRetryMs);
            return StreamEnd::DeviceLost;
        }
        std::optional<ScopedPcMute> mute;
        if (mutePc_)
            mute.emplace(loopback->device.Get());
        LOGI("audio: streaming %u Hz, %d ch -> stereo s16%s", loopback->rate, loopback->channels, mutePc_ ? " (PC muted)" : "");
        const StreamEnd end = SendFormat(loopback->rate) ? PumpUntilEnded(*loopback) : StreamEnd::SendFailed;
        loopback->client->Stop();
        LOGI("audio: capture run ended (%s)", Describe(end));
        return end;
    }

    bool AudioSession::SendFormat(uint32_t rate)
    {
        uint8_t format[6];
        wr32(format, rate);
        format[4] = kSentChannels;
        format[5] = kSentBitsPerSample;
        return SendMsg(conn_, Msg::AudioFormat, format, sizeof(format));
    }

    AudioSession::StreamEnd AudioSession::PumpUntilEnded(const AudioLoopback &loopback)
    {
        HRESULT hr = loopback.client->Start();
        if (FAILED(hr))
        {
            LOGI("audio: capture start failed 0x%08lx", hr);
            return StreamEnd::DeviceLost;
        }
        std::vector<uint8_t> packet;
        const uint64_t start = NowUs();
        while (!stop_)
        {
            SleepUs(kPollUs);
            // Checked every loop so the PC is unmuted at once even when nothing is playing.
            if (PeerClosed())
                return StreamEnd::TabletClosed;
            if (loopback.watcher->Changed())
                return StreamEnd::DeviceLost;
            packet.assign(kPtsBytes, 0);
            wr64(packet.data(), NowUs() - start);
            hr = DrainCapture(loopback.capture.Get(), *loopback.mixer, packet);
            if (FAILED(hr))
            {
                LOGI("audio: output device lost 0x%08lx", hr);
                return StreamEnd::DeviceLost;
            }
            const bool nothingPlaying = packet.size() == kPtsBytes;
            if (!nothingPlaying && !SendOrDrop(packet))
                return StreamEnd::SendFailed;
        }
        return StreamEnd::Stopped;
    }

    // Latency cap: only send if the socket can take it right now; otherwise drop this packet. False means the connection failed.
    bool AudioSession::SendOrDrop(const std::vector<uint8_t> &packet)
    {
        if (!WritableNow(conn_.s))
        {
            ++droppedPackets_;
            return true;
        }
        if (!SendMsg(conn_, Msg::AudioData, packet.data(), uint32_t(packet.size())))
            return false;
        ++sentPackets_;
        return true;
    }

    const char *AudioSession::Describe(StreamEnd end)
    {
        switch (end)
        {
        case StreamEnd::DeviceLost:
            return "device changed or lost";
        case StreamEnd::TabletClosed:
            return "tablet disconnected";
        case StreamEnd::SendFailed:
            return "send failed";
        default:
            return "stopped";
        }
    }

} // namespace pd
