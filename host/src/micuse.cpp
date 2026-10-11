// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#include "micuse.h"
#include "log.h"

#include <windows.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
#include <atomic>
#include <map>

using Microsoft::WRL::ComPtr;

namespace pd {
namespace {

constexpr ULONGLONG kLingerMs = 2000; // see RecordingWatcher::Start

// Windows calls these on its own threads. They only wake the watcher thread, which then reads the sessions itself: no COM
// call is made from inside a notification, so nothing here can deadlock with the audio service.
class SessionAlarm final : public IAudioSessionNotification, public IAudioSessionEvents {
public:
    SessionAlarm() : event_(CreateEventW(nullptr, FALSE, FALSE, nullptr)) {}
    HANDLE Event() const { return event_; }

    STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IAudioSessionNotification)) *out = static_cast<IAudioSessionNotification*>(this);
        else if (iid == __uuidof(IAudioSessionEvents)) *out = static_cast<IAudioSessionEvents*>(this);
        else return *out = nullptr, E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG left = --refs_;
        if (left == 0) delete this;
        return left;
    }

    STDMETHODIMP OnSessionCreated(IAudioSessionControl*) override { return Ring(); }
    STDMETHODIMP OnStateChanged(AudioSessionState) override { return Ring(); }
    STDMETHODIMP OnSessionDisconnected(AudioSessionDisconnectReason) override { return Ring(); }
    STDMETHODIMP OnDisplayNameChanged(LPCWSTR, LPCGUID) override { return S_OK; }
    STDMETHODIMP OnIconPathChanged(LPCWSTR, LPCGUID) override { return S_OK; }
    STDMETHODIMP OnSimpleVolumeChanged(float, BOOL, LPCGUID) override { return S_OK; }
    STDMETHODIMP OnChannelVolumeChanged(DWORD, float[], DWORD, LPCGUID) override { return S_OK; }
    STDMETHODIMP OnGroupingParamChanged(LPCGUID, LPCGUID) override { return S_OK; }

private:
    ~SessionAlarm() { CloseHandle(event_); }
    HRESULT Ring() {
        SetEvent(event_);
        return S_OK;
    }

    std::atomic<ULONG> refs_{1};
    HANDLE event_;
};

using Sessions = std::map<std::wstring, ComPtr<IAudioSessionControl>>; // by session instance ID

// Is a stream running in any session of the device? Also starts listening to sessions that are new, and stops listening to
// the ones that are gone. Listening comes before reading the state, so a change in between still rings the alarm.
bool AnySessionActive(IAudioSessionManager2* manager, SessionAlarm* alarm, Sessions& listening) {
    ComPtr<IAudioSessionEnumerator> list;
    int count = 0;
    if (FAILED(manager->GetSessionEnumerator(&list)) || FAILED(list->GetCount(&count))) return false;
    bool active = false;
    Sessions present;
    for (int i = 0; i < count; ++i) {
        ComPtr<IAudioSessionControl> session;
        ComPtr<IAudioSessionControl2> session2;
        LPWSTR id = nullptr;
        if (FAILED(list->GetSession(i, &session)) || FAILED(session.As(&session2)) || FAILED(session2->GetSessionInstanceIdentifier(&id))) continue;
        const std::wstring key = id;
        CoTaskMemFree(id);
        auto known = listening.find(key);
        if (known != listening.end()) session = known->second; // the object the alarm is registered on has to stay alive
        else if (FAILED(session->RegisterAudioSessionNotification(alarm))) continue;
        AudioSessionState state = AudioSessionStateInactive;
        if (FAILED(session->GetState(&state)) || state == AudioSessionStateExpired) {
            session->UnregisterAudioSessionNotification(alarm);
            continue;
        }
        active |= state == AudioSessionStateActive;
        present[key] = session;
    }
    for (auto& [key, session] : listening)
        if (!present.count(key)) session->UnregisterAudioSessionNotification(alarm);
    listening.swap(present);
    return active;
}

} // namespace

RecordingWatcher::RecordingWatcher() : stop_(CreateEventW(nullptr, TRUE, FALSE, nullptr)), started_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}

RecordingWatcher::~RecordingWatcher() {
    Stop();
    CloseHandle(static_cast<HANDLE>(stop_));
    CloseHandle(static_cast<HANDLE>(started_));
}

bool RecordingWatcher::Start(const std::wstring& deviceId, Changed changed) {
    if (deviceId.empty() || thread_.joinable()) return false;
    changed_ = std::move(changed);
    thread_ = std::thread([this, deviceId] { Run(deviceId); });
    WaitForSingleObject(static_cast<HANDLE>(started_), INFINITE);
    if (!watching_) Stop();
    return watching_;
}

void RecordingWatcher::Stop() {
    SetEvent(static_cast<HANDLE>(stop_));
    if (thread_.joinable()) thread_.join();
}

void RecordingWatcher::Run(std::wstring deviceId) {
    const bool comStarted = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    {
        ComPtr<IMMDeviceEnumerator> devices;
        ComPtr<IMMDevice> device;
        ComPtr<IAudioSessionManager2> manager;
        ComPtr<SessionAlarm> alarm;
        alarm.Attach(new SessionAlarm());
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&devices));
        if (SUCCEEDED(hr)) hr = devices->GetDevice(deviceId.c_str(), &device);
        if (SUCCEEDED(hr)) hr = device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, &manager);
        if (SUCCEEDED(hr)) hr = manager->RegisterSessionNotification(alarm.Get());
        watching_ = SUCCEEDED(hr);
        if (!watching_) LOGI("mic: cannot watch who records from the virtual microphone (0x%08lx)", static_cast<unsigned long>(hr));
        SetEvent(static_cast<HANDLE>(started_));

        Sessions listening;
        bool told = false, toldOnce = false;
        ULONGLONG idleSince = 0;
        while (watching_) {
            const bool recording = AnySessionActive(manager.Get(), alarm.Get(), listening);
            const ULONGLONG now = GetTickCount64();
            if (recording) idleSince = 0;
            else if (!idleSince) idleSince = now;
            const bool lingering = told && !recording && now - idleSince < kLingerMs;
            const bool state = recording || lingering;
            if (!toldOnce || state != told) {
                told = state;
                toldOnce = true;
                changed_(state);
            }
            const HANDLE events[2] = {static_cast<HANDLE>(stop_), alarm->Event()};
            const DWORD timeout = lingering ? DWORD(kLingerMs - (now - idleSince)) : INFINITE;
            if (WaitForMultipleObjects(2, events, FALSE, timeout) == WAIT_OBJECT_0) break;
        }
        for (auto& [key, session] : listening) session->UnregisterAudioSessionNotification(alarm.Get());
        if (watching_) manager->UnregisterSessionNotification(alarm.Get());
    }
    if (comStarted) CoUninitialize();
}

} // namespace pd
