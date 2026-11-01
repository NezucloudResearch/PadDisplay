// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#include "micsink.h"
#include "audio/cablesink.h"
#include "log.h"
#include "micbackend.h"
#include "protocol.h"
#include "realtime.h"
#include "text.h"
#include "setup/vdd.h"

#include "../../third_party/padmic/include/padmic_public.h"

#include <windows.h>
#include <algorithm>
#include <cstring>
#include <vector>

namespace pd
{
    namespace
    {

        constexpr uint32_t kMaxMicPayload = 64 * 1024;
        constexpr size_t kMicDataHeader = 8;         // u64 ptsUs, then 48 kHz mono s16le: both outputs take it as it is
        constexpr DWORD kSilenceMeansGoneMs = 10000; // audio while an app records, a keep-alive every 2 s otherwise
        constexpr DWORD kDriverWriteWaitMs = 200;
        constexpr DWORD kRefusalDrainMs = 1000, kEndDrainMs = 300;

        const char *WhyDriverWillNotOpen(DWORD err)
        {
            const bool missing = err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND;
            if (missing && QueryMic() == VddState::Disabled)
                return "The PadDisplay microphone driver is switched off. In PadDisplay's tray menu tick \"Accept the tablet's microphone\", then switch the microphone off and on here.";
            if (missing)
                return "The PadDisplay microphone driver is not installed on the PC. In PadDisplay's tray menu choose \"Install microphone driver (admin)\", then switch the microphone off and on here.";
            if (err == ERROR_ACCESS_DENIED || err == ERROR_SHARING_VIOLATION)
                return "The PadDisplay microphone is already in use by another PadDisplay on the PC.";
            return "The PadDisplay microphone driver could not be opened. See the PC's log.";
        }

        void LogDriverEnd(HANDLE device, uint64_t packets, uint64_t lateWrites)
        {
            PADMIC_STATUS st{};
            DWORD got = 0;
            if (!DeviceIoControl(device, IOCTL_PADMIC_GET_STATUS, nullptr, 0, &st, sizeof(st), &got, nullptr) || got < PADMIC_STATUS_V1_SIZE)
            {
                LOGI("mic: ended (%llu packets, %llu late writes)", packets, lateWrites);
                return;
            }
            LOGI("mic: ended (%llu packets, %u underruns and %u drops in the driver, %llu late writes; streams opened %u, closed %u, running now %u, ticks %llu)", packets, st.Underruns, st.Drops, lateWrites, st.StreamsCreated, st.StreamsDestroyed, st.StreamsRunning, st.Ticks);
            if (got >= sizeof(st) && st.QpcFrequency && st.Ticks)
            { // interface version 2: what the driver itself spent
                const double us = 1e6 / double(st.QpcFrequency);
                LOGI("mic: driver tick busy avg %.1f us / max %.0f us, late avg %.0f us / max %.0f us, write avg %.1f us", st.TickBusyTotal * us / double(st.Ticks),
                     st.TickBusyMax * us, st.TickLateTotal * us / double(st.Ticks), st.TickLateMax * us, st.Writes ? st.WriteBusyTotal * us / double(st.Writes) : 0.0);
            }
        }

    } // namespace

    MicSession::MicSession(Conn conn, const Settings &settings, StatusFn status)
        : conn_(std::move(conn)), settings_(settings), status_(std::move(status)) {}

    MicSession::~MicSession()
    {
        Stop();
        CloseSocket();
    }

    void MicSession::Stop()
    {
        stop_ = true;
        std::lock_guard lock(socketMutex_);
        if (conn_.s != INVALID_SOCKET)
            CutOff(conn_.s);
    }

    void MicSession::Run()
    {
        MakeRealtimeThread(L"Pro Audio");
        if (!settings_.acceptTabletMic)
            return Refuse("The microphone is switched off in PadDisplay on the PC (settings.ini: acceptTabletMic).");

        const MicChoice choice = ChooseMicBackend(settings_);
        LOGI("mic: %ls", choice.detail.c_str());
        switch (choice.backend)
        {
        case MicBackend::Driver:
            return RunDriver();
        case MicBackend::Cable:
            return RunCable();
        case MicBackend::None:
            break;
        }
        OnMicSetupNeeded(); // the tray asks the user whether to install VB-CABLE
        Refuse("The PC has no virtual microphone yet. PadDisplay on the PC is asking to install one: accept it there, then switch the microphone off and on here.");
    }

    // The Notice goes out, then the connection ends cleanly so the tablet stops sending into nothing.
    void MicSession::Refuse(const char *text)
    {
        SendNotice(text);
        EndConnection(kRefusalDrainMs);
    }

    void MicSession::EndConnection(DWORD drainMs)
    {
        FinishConnection(conn_.s, drainMs);
        CloseSocket();
    }

    void MicSession::RunDriver()
    {
        // One writer at a time: the driver opens exclusive, so a second PadDisplay (or a stuck old one) is refused here.
        HANDLE dev = CreateFileW(PADMIC_USER_PATH, GENERIC_WRITE | GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (dev == INVALID_HANDLE_VALUE)
        {
            const DWORD err = GetLastError();
            LOGI("mic: cannot open the PadDisplay Microphone driver (%lu)", err);
            return Refuse(WhyDriverWillNotOpen(err));
        }
        LOGI("mic: playing the tablet's microphone into the PadDisplay Microphone");

        RecordingWatcher watcher;
        WatchRecording(watcher, RecordingDeviceId(L"PadDisplay"), nullptr);
        const bool driverGone = Pump([&](const uint8_t *pcm, size_t bytes)
                                     { return WriteToDriver(dev, pcm, bytes); });
        watcher.Stop(); // before the socket closes: it sends on it
        LogDriverEnd(dev, packets_, lateWrites_);
        CloseHandle(dev); // the driver empties its ring when the writer goes
        if (driverGone)
            SendNotice("The PadDisplay microphone driver stopped. See the PC's log.");
        EndConnection(kEndDrainMs);
    }

    void MicSession::RunCable()
    {
        CableOutput cable(settings_.micDevice, settings_.cableMinPeriod);
        std::wstring why;
        if (!cable.Open(why))
        {
            LOGI("mic: %ls", why.c_str());
            return Refuse("The virtual audio cable (VB-CABLE) could not be opened for playback. See the PC's log.");
        }
        RecordingWatcher watcher;
        WatchRecording(watcher, CableRecordingDeviceId(settings_.micDevice), [&](bool)
                       { cable.Restart(); });
        // The network thread only copies into the cable's ring; the render thread inside CableOutput plays it.
        const bool cableGone = Pump([&](const uint8_t *pcm, size_t bytes)
                                    {
        cable.Write(reinterpret_cast<const int16_t*>(pcm), bytes / 2); // buf.data() + 8 is 2-byte aligned
        return !cable.Failed(); });
        watcher.Stop(); // before the cable and the socket close: it uses both
        cable.Close();
        LOGI("mic: ended (%llu packets, %llu underruns, %llu samples dropped; audio arrived with a longest gap of %.1f ms, %llu gaps over 15 ms)", packets_, cable.Underruns(),
             cable.Dropped(), cable.MaxGapMs(), cable.GapsOver15Ms());
        if (cableGone)
            SendNotice("The virtual audio cable stopped. See the PC's log.");
        EndConnection(kEndDrainMs);
    }

    void MicSession::WatchRecording(RecordingWatcher &watcher, const std::wstring &deviceId, const std::function<void(bool)> &onChange)
    {
        const bool watching = watcher.Start(deviceId, [this, onChange](bool recording)
                                            {
        LOGI("mic: %s", recording ? "an app on the PC records from the microphone: the tablet's microphone is on"
                                  : "no app on the PC records from the microphone: the tablet's microphone is off");
        if (onChange) onChange(recording);
        SendState(recording); });
        if (watching)
            return;
        LOGI("mic: cannot tell when an app records from the microphone, so the tablet's microphone stays on while it is connected");
        SendState(true);
    }

    bool MicSession::Pump(const std::function<bool(const uint8_t *, size_t)> &sink)
    {
        DWORD timeout = kSilenceMeansGoneMs;
        setsockopt(conn_.s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char *>(&timeout), sizeof(timeout));
        Msg type{};
        std::vector<uint8_t> buf;
        while (!stop_ && RecvMsg(conn_, type, buf, kMaxMicPayload))
        {
            if (type != Msg::MicData || buf.size() < kMicDataHeader + sizeof(int16_t))
                continue;
            ++packets_;
            if (!sink(buf.data() + kMicDataHeader, (buf.size() - kMicDataHeader) & ~size_t(1)))
                return true;
        }
        return false;
    }

    bool MicSession::WriteToDriver(HANDLE device, const uint8_t *pcm, size_t bytes)
    {
        while (bytes >= sizeof(int16_t))
        {
            const DWORD chunk = DWORD(std::min<size_t>(bytes, PADMIC_MAX_WRITE_BYTES)) & ~1u; // whole samples
            if (!WriteChunkToDriver(device, pcm, chunk))
                return false;
            pcm += chunk;
            bytes -= chunk;
        }
        return true;
    }

    // The driver takes a write at once (it only copies into its ring), so this does not wait on anything slow; the bounded
    // wait and the cancel are only there so that a driver that is being removed cannot hang the session.
    bool MicSession::WriteChunkToDriver(HANDLE device, const uint8_t *pcm, DWORD bytes)
    {
        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ov.hEvent)
            return true;
        DWORD done = 0;
        bool ok = WriteFile(device, pcm, bytes, &done, &ov) != FALSE;
        if (!ok && GetLastError() == ERROR_IO_PENDING)
        {
            if (WaitForSingleObject(ov.hEvent, kDriverWriteWaitMs) != WAIT_OBJECT_0)
            {
                CancelIoEx(device, &ov);
                ++lateWrites_;
            }
            ok = GetOverlappedResult(device, &ov, &done, TRUE) != FALSE; // after a cancel this returns at once
        }
        const DWORD err = ok ? 0 : GetLastError();
        CloseHandle(ov.hEvent);
        if (ok || err == ERROR_OPERATION_ABORTED)
            return true;
        LOGI("mic: write to the driver failed (%lu)", err);
        return false;
    }

    void MicSession::SendNotice(const char *text)
    {
        {
            std::lock_guard lock(sendMutex_);
            SendMsg(conn_, Msg::Notice, text, uint32_t(strlen(text)));
        }
        if (status_)
            status_(L"Microphone: " + Widen(text));
    }

    void MicSession::SendState(bool recording)
    {
        const uint8_t v = recording ? 1 : 0;
        std::lock_guard lock(sendMutex_);
        SendMsg(conn_, Msg::MicState, &v, 1);
    }

    void MicSession::CloseSocket()
    {
        std::lock_guard lock(socketMutex_);
        if (conn_.s != INVALID_SOCKET)
            closesocket(conn_.s);
        conn_.s = INVALID_SOCKET;
    }

} // namespace pd
