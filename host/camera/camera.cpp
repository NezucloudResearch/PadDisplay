// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
//
// PadDisplay's virtual camera: a custom media source that Windows 11's Frame Server loads for the camera
// registered with MFCreateVirtualCamera. It hands out the newest frame PadDisplay.exe wrote to the shared
// mailbox (camshared.h), or a "no signal" picture when there is none.
//
// Structure and the Frame Server contract (sensor profiles, stream attributes, provided sample allocator,
// IMFMediaStream2) follow Microsoft's VirtualCamera sample from github.com/microsoft/Windows-Camera
// (Copyright (C) Microsoft Corporation, MIT License), rewritten without WIL and C++/WinRT.
#include "camshared.h"

#include <windows.h>
#include <unknwn.h>
#include <ole2.h>
#include <initguid.h>
#include <ks.h>
#include <ksproxy.h>
#include <ksmedia.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mferror.h>
#include <mfvirtualcamera.h>
#include <sddl.h>
#include <wrl/client.h>
#include <wrl/implements.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#pragma comment(lib, "mfuuid")
#pragma comment(lib, "mf")
#pragma comment(lib, "mfplat")
#pragma comment(lib, "mfsensorgroup")
#pragma comment(lib, "advapi32")

using Microsoft::WRL::ChainInterfaces;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Make;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;
using Microsoft::WRL::ClassicCom;

namespace {

// {A3C1D2F4-5B6E-4F70-8A91-6D2E0B7C4E15}: the CLSID the host passes to MFCreateVirtualCamera.
const GUID CLSID_PadDisplayCamera = {0xa3c1d2f4, 0x5b6e, 0x4f70, {0x8a, 0x91, 0x6d, 0x2e, 0x0b, 0x7c, 0x4e, 0x15}};
const wchar_t kClsidText[] = L"{A3C1D2F4-5B6E-4F70-8A91-6D2E0B7C4E15}";
const wchar_t kFriendlyName[] = L"PadDisplay Camera";

HMODULE g_module = nullptr;
std::atomic<long> g_objects{0}; // for DllCanUnloadNow: the Frame Server unloads the DLL when this is 0

struct ModuleRef {
    ModuleRef() { ++g_objects; }
    ~ModuleRef() { --g_objects; }
};

constexpr DWORD kStreamId = 0;
constexpr UINT32 kFps = 30;

struct Mode { UINT32 w, h; };
const Mode kModes[] = {{1280, 720}, {1920, 1080}}; // the host decodes to exactly these; anything else is scaled

// ---------------------------------------------------------------------------------------------------------
// The mailbox from the host's side of the glass.

class Mailbox {
public:
    ~Mailbox() {
        if (view_) UnmapViewOfFile(view_);
        if (map_) CloseHandle(map_);
    }

    // Fills dst (NV12, pitch bytes per row, UV plane at h * pitch) with the newest frame. false = no signal.
    bool Read(BYTE* dst, LONG pitch, UINT32 w, UINT32 h) {
        if (!Open()) return false;
        const uint64_t now = GetTickCount64();
        view_->readerTick = static_cast<int64_t>(now); // tells the host that someone is watching
        if (now - static_cast<uint64_t>(view_->writeTick) > pd::kCamFrameStaleMs) return false;

        // Seqlock read: copy, then check the writer did not touch the slot meanwhile. A torn copy is thrown away and
        // the previous good frame is shown again.
        const int64_t s1 = view_->seq;
        if (!(s1 & 1)) {
            const UINT32 sw = static_cast<UINT32>(view_->width), sh = static_cast<UINT32>(view_->height);
            if (sw >= 2 && sh >= 2 && sw <= pd::kCamMaxWidth && sh <= pd::kCamMaxHeight && (sw | sh) % 2 == 0) {
                std::atomic_thread_fence(std::memory_order_acquire);
                const size_t bytes = size_t(sw) * sh * 3 / 2;
                if (back_.size() < pd::kCamCapacity) back_.resize(pd::kCamCapacity);
                memcpy(back_.data(), pd::CamPixels(view_), bytes);
                std::atomic_thread_fence(std::memory_order_acquire);
                if (view_->seq == s1) {
                    back_.swap(front_);
                    fw_ = sw;
                    fh_ = sh;
                }
            }
        }
        if (fw_ == 0) return false;
        Blit(dst, pitch, w, h);
        return true;
    }

    void NoSignal(BYTE* dst, LONG pitch, UINT32 w, UINT32 h) {
        // Dark grey with a bar sweeping across, so a frozen picture is never mistaken for a live one.
        for (UINT32 y = 0; y < h; ++y) memset(dst + size_t(y) * pitch, 0x28, w);
        for (UINT32 y = 0; y < h / 2; ++y) memset(dst + size_t(h + y) * pitch, 0x80, w);
        const UINT32 barW = std::max<UINT32>(16, w / 24);
        const UINT32 x0 = static_cast<UINT32>((GetTickCount64() / 8) % (w - barW));
        for (UINT32 y = h / 3; y < h / 3 + h / 12; ++y) memset(dst + size_t(y) * pitch + x0, 0x90, barW);
    }

private:
    // Creates the mapping (this process is a service, so it may use the Global\ namespace) or finds the host's.
    bool Open() {
        if (view_) return true;
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, FALSE};
        PSECURITY_DESCRIPTOR sd = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(pd::kCamMapSddl, SDDL_REVISION_1, &sd, nullptr)) return false;
        sa.lpSecurityDescriptor = sd;
        map_ = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, pd::kCamMapBytes, pd::kCamMapName);
        const bool fresh = map_ && GetLastError() != ERROR_ALREADY_EXISTS;
        LocalFree(sd);
        if (!map_) return false;
        view_ = static_cast<pd::CamShared*>(MapViewOfFile(map_, FILE_MAP_ALL_ACCESS, 0, 0, pd::kCamMapBytes));
        if (!view_) {
            CloseHandle(map_);
            map_ = nullptr;
            return false;
        }
        if (fresh) { // pages are zero already; only the identity needs writing
            view_->version = 1;
            view_->capacity = pd::kCamCapacity;
            view_->magic = pd::kCamMagic;
        }
        return view_->magic == pd::kCamMagic;
    }

    // Copies (or nearest-neighbour scales when the sizes differ) the front frame into the sample buffer.
    void Blit(BYTE* dst, LONG pitch, UINT32 w, UINT32 h) {
        const BYTE* srcY = front_.data();
        const BYTE* srcUV = srcY + size_t(fw_) * fh_;
        if (fw_ == w && fh_ == h) {
            for (UINT32 y = 0; y < h; ++y) memcpy(dst + size_t(y) * pitch, srcY + size_t(y) * w, w);
            for (UINT32 y = 0; y < h / 2; ++y) memcpy(dst + size_t(h + y) * pitch, srcUV + size_t(y) * w, w);
            return;
        }
        for (UINT32 y = 0; y < h; ++y) {
            const BYTE* s = srcY + size_t(uint64_t(y) * fh_ / h) * fw_;
            BYTE* d = dst + size_t(y) * pitch;
            for (UINT32 x = 0; x < w; ++x) d[x] = s[uint64_t(x) * fw_ / w];
        }
        for (UINT32 y = 0; y < h / 2; ++y) {
            const BYTE* s = srcUV + size_t(uint64_t(y) * (fh_ / 2) / (h / 2)) * fw_;
            BYTE* d = dst + size_t(h + y) * pitch;
            for (UINT32 x = 0; x < w; x += 2) { // U and V stay together
                const UINT32 sx = static_cast<UINT32>(uint64_t(x / 2) * (fw_ / 2) / (w / 2)) * 2;
                d[x] = s[sx];
                d[x + 1] = s[sx + 1];
            }
        }
    }

    HANDLE map_ = nullptr;
    pd::CamShared* view_ = nullptr;
    std::vector<BYTE> front_, back_;
    UINT32 fw_ = 0, fh_ = 0;
};

// ---------------------------------------------------------------------------------------------------------
// The stream.

class CamSource;

class CamStream : public RuntimeClass<RuntimeClassFlags<ClassicCom>, ChainInterfaces<IMFMediaStream2, IMFMediaStream, IMFMediaEventGenerator>> {
public:
    HRESULT Initialize(CamSource* source);
    ~CamStream() override { StopThread(); }

    // IMFMediaEventGenerator
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* cb, IUnknown* state) override {
        ComPtr<IMFMediaEventQueue> q = Queue();
        return q ? q->BeginGetEvent(cb, state) : MF_E_SHUTDOWN;
    }
    STDMETHODIMP EndGetEvent(IMFAsyncResult* r, IMFMediaEvent** ev) override {
        ComPtr<IMFMediaEventQueue> q = Queue();
        return q ? q->EndGetEvent(r, ev) : MF_E_SHUTDOWN;
    }
    STDMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** ev) override {
        ComPtr<IMFMediaEventQueue> q = Queue(); // GetEvent may block: never hold the lock across it
        return q ? q->GetEvent(flags, ev) : MF_E_SHUTDOWN;
    }
    STDMETHODIMP QueueEvent(MediaEventType t, REFGUID ext, HRESULT hr, const PROPVARIANT* v) override {
        ComPtr<IMFMediaEventQueue> q = Queue();
        return q ? q->QueueEventParamVar(t, ext, hr, v) : MF_E_SHUTDOWN;
    }

    // IMFMediaStream
    STDMETHODIMP GetMediaSource(IMFMediaSource** out) override;
    STDMETHODIMP GetStreamDescriptor(IMFStreamDescriptor** out) override {
        if (!out) return E_POINTER;
        std::lock_guard lock(mutex_);
        if (shutdown_ || !desc_) return MF_E_SHUTDOWN;
        desc_.CopyTo(out);
        return S_OK;
    }
    STDMETHODIMP RequestSample(IUnknown* token) override {
        std::lock_guard lock(mutex_);
        if (shutdown_) return MF_E_SHUTDOWN;
        if (state_ != MF_STREAM_STATE_RUNNING) return MF_E_INVALIDREQUEST;
        // The pump answers these at the frame rate. Old requests are dropped, never queued without bound.
        if (tokens_.size() >= 8) tokens_.pop_front();
        tokens_.push_back(token);
        return S_OK;
    }

    // IMFMediaStream2
    STDMETHODIMP SetStreamState(MF_STREAM_STATE state) override {
        std::lock_guard lock(mutex_);
        if (shutdown_) return MF_E_SHUTDOWN;
        if (state == state_) return S_OK;
        switch (state) {
        case MF_STREAM_STATE_PAUSED:
            if (state_ != MF_STREAM_STATE_RUNNING) return MF_E_INVALID_STATE_TRANSITION;
            state_ = state;
            return S_OK;
        case MF_STREAM_STATE_RUNNING: return StartLocked(false, nullptr);
        case MF_STREAM_STATE_STOPPED: return StopLocked(false);
        default: return MF_E_INVALID_STATE_TRANSITION;
        }
    }
    STDMETHODIMP GetStreamState(MF_STREAM_STATE* out) override {
        if (!out) return E_INVALIDARG;
        std::lock_guard lock(mutex_);
        if (shutdown_) return MF_E_SHUTDOWN;
        *out = state_;
        return S_OK;
    }

    // Called by the source.
    HRESULT Start(IMFMediaType* type) {
        std::lock_guard lock(mutex_);
        if (!type) return E_INVALIDARG;
        if (!type_) type_ = type;
        return StartLocked(true, type);
    }
    HRESULT Stop(bool sendEvent) {
        std::lock_guard lock(mutex_);
        if (shutdown_) return MF_E_SHUTDOWN;
        return StopLocked(sendEvent);
    }
    HRESULT SetAllocator(IMFVideoSampleAllocator* a) {
        std::lock_guard lock(mutex_);
        if (shutdown_) return MF_E_SHUTDOWN;
        if (state_ == MF_STREAM_STATE_RUNNING) return MF_E_INVALIDREQUEST;
        allocator_ = a;
        return S_OK;
    }
    IMFAttributes* Attributes() { return attrs_.Get(); }
    void Shutdown() {
        StopThread();
        std::lock_guard lock(mutex_);
        shutdown_ = true;
        source_.Reset();
        if (queue_) {
            queue_->Shutdown();
            queue_.Reset();
        }
        attrs_.Reset();
        desc_.Reset();
        allocator_.Reset();
        tokens_.clear();
        state_ = MF_STREAM_STATE_STOPPED;
    }

private:
    ComPtr<IMFMediaEventQueue> Queue() {
        std::lock_guard lock(mutex_);
        return shutdown_ ? nullptr : queue_;
    }

    HRESULT StartLocked(bool sendEvent, IMFMediaType* newType) {
        BOOL match = FALSE;
        if (type_ && newType) {
            type_->Compare(newType, MF_ATTRIBUTES_MATCH_ALL_ITEMS, &match);
            if (!match) type_ = newType;
        }
        if (state_ != MF_STREAM_STATE_RUNNING || !match) {
            if (!allocator_) return E_POINTER; // the Frame Server hands one over before it starts the stream
            HRESULT hr = allocator_->InitializeSampleAllocator(6, type_.Get());
            if (FAILED(hr)) return hr;
            hr = MFGetAttributeSize(type_.Get(), MF_MT_FRAME_SIZE, &w_, &h_);
            if (FAILED(hr)) return hr;
        }
        if (sendEvent) {
            HRESULT hr = queue_->QueueEventParamVar(MEStreamStarted, GUID_NULL, S_OK, nullptr);
            if (FAILED(hr)) return hr;
        }
        state_ = MF_STREAM_STATE_RUNNING;
        if (!thread_.joinable()) {
            stop_ = false;
            thread_ = std::thread([this] { Pump(); });
        }
        return S_OK;
    }

    HRESULT StopLocked(bool sendEvent) {
        state_ = MF_STREAM_STATE_STOPPED;
        tokens_.clear(); // a stopped stream must not answer old requests
        if (sendEvent && queue_) return queue_->QueueEventParamVar(MEStreamStopped, GUID_NULL, S_OK, nullptr);
        return S_OK;
    }

    void StopThread() {
        stop_ = true;
        if (wake_) SetEvent(wake_);
        if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) thread_.join();
        else if (thread_.joinable()) thread_.detach();
    }

    // One sample per tick, only while someone asked for one. A waitable timer rather than a sleep loop: the Frame
    // Server polls at the frame rate and the wait costs no CPU in between.
    void Pump() {
        HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
        LARGE_INTEGER due;
        due.QuadPart = -10000LL * 1000 / kFps; // first tick one frame from now (100 ns units, negative = relative)
        SetWaitableTimer(timer, &due, 1000 / kFps, nullptr, nullptr, FALSE);
        HANDLE handles[2] = {timer, wake_};
        while (!stop_) {
            if (WaitForMultipleObjects(2, handles, FALSE, INFINITE) != WAIT_OBJECT_0) continue;
            std::lock_guard lock(mutex_);
            if (shutdown_ || state_ != MF_STREAM_STATE_RUNNING || tokens_.empty()) continue;
            ComPtr<IUnknown> token = std::move(tokens_.front());
            tokens_.pop_front();
            Deliver(token.Get());
        }
        CloseHandle(timer);
    }

    void Deliver(IUnknown* token) {
        ComPtr<IMFSample> sample;
        if (FAILED(allocator_->AllocateSample(&sample))) return;
        ComPtr<IMFMediaBuffer> buffer;
        ComPtr<IMF2DBuffer2> buf2d;
        if (FAILED(sample->GetBufferByIndex(0, &buffer)) || FAILED(buffer.As(&buf2d))) return;
        BYTE* data = nullptr;
        BYTE* start = nullptr;
        LONG pitch = 0;
        DWORD length = 0;
        if (FAILED(buf2d->Lock2DSize(MF2DBuffer_LockFlags_Write, &data, &pitch, &start, &length))) return;
        if (pitch > 0 && size_t(pitch) * h_ * 3 / 2 <= length) {
            if (!mailbox_.Read(data, pitch, w_, h_)) mailbox_.NoSignal(data, pitch, w_, h_);
        }
        buf2d->Unlock2D();
        sample->SetSampleTime(MFGetSystemTime());
        sample->SetSampleDuration(10000000 / kFps);
        if (token) sample->SetUnknown(MFSampleExtension_Token, token);
        if (queue_) queue_->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, sample.Get());
    }

    std::mutex mutex_;
    ComPtr<IMFMediaSource> source_;
    ComPtr<IMFMediaEventQueue> queue_;
    ComPtr<IMFStreamDescriptor> desc_;
    ComPtr<IMFAttributes> attrs_;
    ComPtr<IMFMediaType> type_;
    ComPtr<IMFVideoSampleAllocator> allocator_;
    std::deque<ComPtr<IUnknown>> tokens_;
    MF_STREAM_STATE state_ = MF_STREAM_STATE_STOPPED;
    bool shutdown_ = false;
    UINT32 w_ = 0, h_ = 0;
    Mailbox mailbox_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    HANDLE wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ModuleRef ref_;
};

// ---------------------------------------------------------------------------------------------------------
// The source.

class CamSource : public RuntimeClass<RuntimeClassFlags<ClassicCom>, ChainInterfaces<IMFMediaSourceEx, IMFMediaSource, IMFMediaEventGenerator>,
                                      IMFGetService, IKsControl, IMFSampleAllocatorControl> {
public:
    HRESULT Initialize(IMFAttributes* activateAttributes) {
        std::lock_guard lock(mutex_);
        HRESULT hr = MFCreateAttributes(&attrs_, 4);
        if (FAILED(hr)) return hr;
        if (activateAttributes) activateAttributes->CopyAllItems(attrs_.Get());
        hr = AddSensorProfiles();
        if (FAILED(hr)) return hr;
        hr = MFCreateEventQueue(&queue_);
        if (FAILED(hr)) return hr;
        stream_ = Make<CamStream>();
        hr = stream_->Initialize(this);
        if (FAILED(hr)) return hr;
        ComPtr<IMFStreamDescriptor> desc;
        hr = stream_->GetStreamDescriptor(&desc);
        if (FAILED(hr)) return hr;
        IMFStreamDescriptor* list[1] = {desc.Get()};
        hr = MFCreatePresentationDescriptor(1, list, &pd_);
        if (FAILED(hr)) return hr;
        state_ = State::Stopped;
        return S_OK;
    }

    ~CamSource() override = default;

    // IMFMediaEventGenerator
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* cb, IUnknown* state) override {
        ComPtr<IMFMediaEventQueue> q = Queue();
        return q ? q->BeginGetEvent(cb, state) : MF_E_SHUTDOWN;
    }
    STDMETHODIMP EndGetEvent(IMFAsyncResult* r, IMFMediaEvent** ev) override {
        ComPtr<IMFMediaEventQueue> q = Queue();
        return q ? q->EndGetEvent(r, ev) : MF_E_SHUTDOWN;
    }
    STDMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** ev) override {
        ComPtr<IMFMediaEventQueue> q = Queue();
        return q ? q->GetEvent(flags, ev) : MF_E_SHUTDOWN;
    }
    STDMETHODIMP QueueEvent(MediaEventType t, REFGUID ext, HRESULT hr, const PROPVARIANT* v) override {
        ComPtr<IMFMediaEventQueue> q = Queue();
        return q ? q->QueueEventParamVar(t, ext, hr, v) : MF_E_SHUTDOWN;
    }

    // IMFMediaSource
    STDMETHODIMP CreatePresentationDescriptor(IMFPresentationDescriptor** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        std::lock_guard lock(mutex_);
        if (state_ == State::Shutdown) return MF_E_SHUTDOWN;
        return pd_->Clone(out);
    }
    STDMETHODIMP GetCharacteristics(DWORD* out) override {
        if (!out) return E_POINTER;
        *out = MFMEDIASOURCE_IS_LIVE;
        return S_OK;
    }
    STDMETHODIMP Pause() override { return MF_E_INVALID_STATE_TRANSITION; }
    STDMETHODIMP Shutdown() override {
        std::lock_guard lock(mutex_);
        state_ = State::Shutdown;
        attrs_.Reset();
        pd_.Reset();
        if (queue_) {
            queue_->Shutdown();
            queue_.Reset();
        }
        if (stream_) {
            stream_->Shutdown();
            stream_.Reset();
        }
        return S_OK;
    }
    STDMETHODIMP Start(IMFPresentationDescriptor* pdesc, const GUID* timeFormat, const PROPVARIANT* startPos) override {
        std::lock_guard lock(mutex_);
        if (state_ == State::Shutdown) return MF_E_SHUTDOWN;
        if (!pdesc || !startPos) return E_INVALIDARG;
        if (timeFormat && *timeFormat != GUID_NULL) return MF_E_UNSUPPORTED_TIME_FORMAT;
        DWORD count = 0;
        HRESULT hr = pdesc->GetStreamDescriptorCount(&count);
        if (FAILED(hr)) return hr;
        if (count != 1) return E_INVALIDARG;
        BOOL selected = FALSE;
        ComPtr<IMFStreamDescriptor> sd;
        hr = pdesc->GetStreamDescriptorByIndex(0, &selected, &sd);
        if (FAILED(hr)) return hr;
        BOOL wasSelected = FALSE;
        pd_->GetStreamDescriptorByIndex(0, &wasSelected, &sd);
        PROPVARIANT startTime;
        PropVariantInit(&startTime);
        startTime.vt = VT_I8;
        startTime.hVal.QuadPart = MFGetSystemTime();
        if (selected) {
            pd_->SelectStream(0);
            ComPtr<IMFMediaTypeHandler> handler;
            ComPtr<IMFMediaType> type;
            ComPtr<IMFStreamDescriptor> asked;
            pdesc->GetStreamDescriptorByIndex(0, &selected, &asked);
            hr = asked->GetMediaTypeHandler(&handler);
            if (SUCCEEDED(hr)) hr = handler->GetCurrentMediaType(&type);
            if (FAILED(hr)) return hr;
            ComPtr<IUnknown> unk;
            stream_.As(&unk);
            queue_->QueueEventParamUnk(wasSelected ? MEUpdatedStream : MENewStream, GUID_NULL, S_OK, unk.Get());
            hr = stream_->Start(type.Get()); // sends MEStreamStarted
            if (FAILED(hr)) return hr;
        } else if (wasSelected) {
            pd_->DeselectStream(0);
            stream_->Stop(false);
        }
        queue_->QueueEventParamVar(MESourceStarted, GUID_NULL, S_OK, &startTime);
        state_ = State::Started;
        return S_OK;
    }
    STDMETHODIMP Stop() override {
        std::lock_guard lock(mutex_);
        if (state_ == State::Shutdown) return MF_E_SHUTDOWN;
        if (state_ != State::Started) return MF_E_INVALID_STATE_TRANSITION;
        state_ = State::Stopped;
        PROPVARIANT stopTime;
        PropVariantInit(&stopTime);
        stopTime.vt = VT_I8;
        stopTime.hVal.QuadPart = MFGetSystemTime();
        stream_->Stop(true);
        pd_->DeselectStream(0);
        return queue_->QueueEventParamVar(MESourceStopped, GUID_NULL, S_OK, &stopTime);
    }

    // IMFMediaSourceEx
    STDMETHODIMP GetSourceAttributes(IMFAttributes** out) override {
        if (!out) return E_POINTER;
        std::lock_guard lock(mutex_);
        if (state_ == State::Shutdown) return MF_E_SHUTDOWN;
        attrs_.CopyTo(out);
        return S_OK;
    }
    STDMETHODIMP GetStreamAttributes(DWORD id, IMFAttributes** out) override {
        if (!out) return E_POINTER;
        std::lock_guard lock(mutex_);
        if (state_ == State::Shutdown) return MF_E_SHUTDOWN;
        if (id != kStreamId) return MF_E_NOT_FOUND;
        *out = stream_->Attributes();
        (*out)->AddRef();
        return S_OK;
    }
    STDMETHODIMP SetD3DManager(IUnknown*) override { return E_NOTIMPL; } // system-memory frames only

    // IMFGetService
    STDMETHODIMP GetService(REFGUID, REFIID, LPVOID* out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        return MF_E_UNSUPPORTED_SERVICE;
    }

    // IKsControl: no camera controls. The same answer a driver without a handler gives.
    STDMETHODIMP KsProperty(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG*) override { return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND); }
    STDMETHODIMP KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG*) override { return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND); }
    STDMETHODIMP KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG*) override { return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND); }

    // IMFSampleAllocatorControl
    STDMETHODIMP SetDefaultAllocator(DWORD id, IUnknown* allocator) override {
        std::lock_guard lock(mutex_);
        if (state_ == State::Shutdown) return MF_E_SHUTDOWN;
        if (id != kStreamId) return MF_E_NOT_FOUND;
        ComPtr<IMFVideoSampleAllocator> a;
        HRESULT hr = allocator->QueryInterface(IID_PPV_ARGS(&a));
        return FAILED(hr) ? hr : stream_->SetAllocator(a.Get());
    }
    STDMETHODIMP GetAllocatorUsage(DWORD id, DWORD* inputId, MFSampleAllocatorUsage* usage) override {
        if (!inputId || !usage) return E_POINTER;
        *inputId = id;
        *usage = MFSampleAllocatorUsage_UsesProvidedAllocator;
        return S_OK;
    }

private:
    enum class State { Invalid, Stopped, Started, Shutdown };

    ComPtr<IMFMediaEventQueue> Queue() {
        std::lock_guard lock(mutex_);
        return state_ == State::Shutdown ? nullptr : queue_;
    }

    // The legacy profile is mandatory: apps that do not know about profiles still need to work.
    HRESULT AddSensorProfiles() {
        ComPtr<IMFSensorProfileCollection> collection;
        ComPtr<IMFSensorProfile> profile;
        HRESULT hr = MFCreateSensorProfileCollection(&collection);
        if (FAILED(hr)) return hr;
        hr = MFCreateSensorProfile(KSCAMERAPROFILE_Legacy, 0, nullptr, &profile);
        if (FAILED(hr)) return hr;
        hr = profile->AddProfileFilter(kStreamId, L"((RES==;FRT<=30,1;SUT==))");
        if (FAILED(hr)) return hr;
        hr = collection->AddProfile(profile.Get());
        if (FAILED(hr)) return hr;
        return attrs_->SetUnknown(MF_DEVICEMFT_SENSORPROFILE_COLLECTION, collection.Get());
    }

    std::mutex mutex_;
    State state_ = State::Invalid;
    ComPtr<IMFMediaEventQueue> queue_;
    ComPtr<IMFPresentationDescriptor> pd_;
    ComPtr<IMFAttributes> attrs_;
    ComPtr<CamStream> stream_;
    ModuleRef ref_;
};

HRESULT CamStream::Initialize(CamSource* source) {
    std::lock_guard lock(mutex_);
    source_ = static_cast<IMFMediaSource*>(source);
    std::vector<ComPtr<IMFMediaType>> types;
    for (const Mode& m : kModes) {
        ComPtr<IMFMediaType> t;
        HRESULT hr = MFCreateMediaType(&t);
        if (FAILED(hr)) return hr;
        t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        t->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        t->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
        MFSetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, m.w, m.h);
        MFSetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, kFps, 1);
        MFSetAttributeRatio(t.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        t->SetUINT32(MF_MT_DEFAULT_STRIDE, m.w);
        t->SetUINT32(MF_MT_AVG_BITRATE, UINT32(uint64_t(m.w) * m.h * 3 / 2 * 8 * kFps));
        types.push_back(t);
    }
    std::vector<IMFMediaType*> raw;
    for (auto& t : types) raw.push_back(t.Get());
    HRESULT hr = MFCreateAttributes(&attrs_, 8);
    if (FAILED(hr)) return hr;
    hr = MFCreateEventQueue(&queue_);
    if (FAILED(hr)) return hr;
    hr = MFCreateStreamDescriptor(kStreamId, DWORD(raw.size()), raw.data(), &desc_);
    if (FAILED(hr)) return hr;
    ComPtr<IMFMediaTypeHandler> handler;
    hr = desc_->GetMediaTypeHandler(&handler);
    if (FAILED(hr)) return hr;
    hr = handler->SetCurrentMediaType(raw[0]);
    if (FAILED(hr)) return hr;
    // The same attributes on the stream and on its descriptor, as the Frame Server expects of a camera source.
    for (IMFAttributes* a : {attrs_.Get(), static_cast<IMFAttributes*>(desc_.Get())}) {
        a->SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, PINNAME_VIDEO_CAPTURE);
        a->SetUINT32(MF_DEVICESTREAM_STREAM_ID, kStreamId);
        a->SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 1);
        a->SetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, MFFrameSourceTypes_Color);
    }
    return S_OK;
}

STDMETHODIMP CamStream::GetMediaSource(IMFMediaSource** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    std::lock_guard lock(mutex_);
    if (shutdown_ || !source_) return MF_E_SHUTDOWN;
    source_.CopyTo(out);
    return S_OK;
}

// ---------------------------------------------------------------------------------------------------------
// Activation: the Frame Server creates this class (by CLSID) and calls ActivateObject for the media source.

#define FWD(name, decl, call) STDMETHODIMP name decl override { return attrs_->name call; }

class CamActivate : public RuntimeClass<RuntimeClassFlags<ClassicCom>, ChainInterfaces<IMFActivate, IMFAttributes>> {
public:
    CamActivate() { MFCreateAttributes(&attrs_, 4); }

    // IMFActivate
    STDMETHODIMP ActivateObject(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        ComPtr<CamSource> source = Make<CamSource>();
        HRESULT hr = source->Initialize(attrs_.Get());
        return FAILED(hr) ? hr : source->QueryInterface(riid, ppv);
    }
    STDMETHODIMP ShutdownObject() override { return S_OK; }
    STDMETHODIMP DetachObject() override { return S_OK; }

    // IMFAttributes: all of it belongs to the attribute store the Frame Server fills in.
    FWD(GetItem, (REFGUID k, PROPVARIANT* v), (k, v))
    FWD(GetItemType, (REFGUID k, MF_ATTRIBUTE_TYPE* t), (k, t))
    FWD(CompareItem, (REFGUID k, REFPROPVARIANT v, BOOL* r), (k, v, r))
    FWD(Compare, (IMFAttributes* o, MF_ATTRIBUTES_MATCH_TYPE t, BOOL* r), (o, t, r))
    FWD(GetUINT32, (REFGUID k, UINT32* v), (k, v))
    FWD(GetUINT64, (REFGUID k, UINT64* v), (k, v))
    FWD(GetDouble, (REFGUID k, double* v), (k, v))
    FWD(GetGUID, (REFGUID k, GUID* v), (k, v))
    FWD(GetStringLength, (REFGUID k, UINT32* n), (k, n))
    FWD(GetString, (REFGUID k, LPWSTR s, UINT32 cch, UINT32* n), (k, s, cch, n))
    FWD(GetAllocatedString, (REFGUID k, LPWSTR* s, UINT32* n), (k, s, n))
    FWD(GetBlobSize, (REFGUID k, UINT32* n), (k, n))
    FWD(GetBlob, (REFGUID k, UINT8* b, UINT32 cb, UINT32* n), (k, b, cb, n))
    FWD(GetAllocatedBlob, (REFGUID k, UINT8** b, UINT32* n), (k, b, n))
    FWD(GetUnknown, (REFGUID k, REFIID i, LPVOID* p), (k, i, p))
    FWD(SetItem, (REFGUID k, REFPROPVARIANT v), (k, v))
    FWD(DeleteItem, (REFGUID k), (k))
    FWD(DeleteAllItems, (), ())
    FWD(SetUINT32, (REFGUID k, UINT32 v), (k, v))
    FWD(SetUINT64, (REFGUID k, UINT64 v), (k, v))
    FWD(SetDouble, (REFGUID k, double v), (k, v))
    FWD(SetGUID, (REFGUID k, REFGUID v), (k, v))
    FWD(SetString, (REFGUID k, LPCWSTR s), (k, s))
    FWD(SetBlob, (REFGUID k, const UINT8* b, UINT32 cb), (k, b, cb))
    FWD(SetUnknown, (REFGUID k, IUnknown* u), (k, u))
    FWD(LockStore, (), ())
    FWD(UnlockStore, (), ())
    FWD(GetCount, (UINT32* n), (n))
    FWD(GetItemByIndex, (UINT32 i, GUID* k, PROPVARIANT* v), (i, k, v))
    FWD(CopyAllItems, (IMFAttributes* d), (d))

private:
    ComPtr<IMFAttributes> attrs_;
    ModuleRef ref_;
};

class Factory : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IClassFactory> {
public:
    STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        return Make<CamActivate>()->QueryInterface(riid, ppv);
    }
    STDMETHODIMP LockServer(BOOL lock) override {
        lock ? ++g_objects : --g_objects;
        return S_OK;
    }
};

// ---------------------------------------------------------------------------------------------------------
// COM registration (HKLM: the Frame Server is a service and cannot see a user's HKCU).

HRESULT SetKey(const std::wstring& path, const wchar_t* name, const std::wstring& value) {
    HKEY key = nullptr;
    LSTATUS s = RegCreateKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, nullptr, 0, KEY_WRITE | KEY_WOW64_64KEY, nullptr, &key, nullptr);
    if (s != ERROR_SUCCESS) return HRESULT_FROM_WIN32(s);
    s = RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), DWORD((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return HRESULT_FROM_WIN32(s);
}

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (clsid != CLSID_PadDisplayCamera) return CLASS_E_CLASSNOTAVAILABLE;
    return Make<Factory>()->QueryInterface(riid, ppv);
}

STDAPI DllCanUnloadNow() { return g_objects.load() == 0 ? S_OK : S_FALSE; }

STDAPI DllRegisterServer() {
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(g_module, path, MAX_PATH)) return HRESULT_FROM_WIN32(GetLastError());
    const std::wstring base = std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + kClsidText;
    HRESULT hr = SetKey(base, nullptr, kFriendlyName);
    if (SUCCEEDED(hr)) hr = SetKey(base + L"\\InprocServer32", nullptr, path);
    if (SUCCEEDED(hr)) hr = SetKey(base + L"\\InprocServer32", L"ThreadingModel", L"Both");
    return hr;
}

STDAPI DllUnregisterServer() {
    const std::wstring base = std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + kClsidText;
    LSTATUS s = RegDeleteTreeW(HKEY_LOCAL_MACHINE, base.c_str());
    return s == ERROR_SUCCESS || s == ERROR_FILE_NOT_FOUND ? S_OK : HRESULT_FROM_WIN32(s);
}
