// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#pragma once
#include "monitor_off.h"
#include "net/tls.h"
#include "session.h"

#include <deque>
#include <map>

namespace pd
{
    // Accepts tablets on TCP 27183 (dual-stack). A newly connected tablet replaces the current one.
    class Server
    {
    public:
        Server(const Settings &settings, StatusFn status) : settings_(settings), status_(std::move(status)) {}
        ~Server() { Stop(SessionEnd::HostExit); }
        bool Start();
        void Stop(SessionEnd why); // HostExit tells the tablet; HostRestart lets it reconnect to the next PadDisplay
        void UpdateSettings(const Settings &s);
        bool TabletConnected() const { return tabletConnected_; }
        // SHA-256 of the certificate tablets pin, "AB:CD:..."; empty when TLS is unavailable.
        std::string TlsFingerprint() const { return tls_ ? tls_->Fingerprint() : std::string(); }

    private:
        void AcceptLoop();
        void CutSlowHandshakes();
        void EndHandshakeDeadline(uint64_t id); // the connection passes to a session: it is no handshake to cut off any more
        struct Incoming;
        bool Handle(SOCKET s, const sockaddr_storage &addr, uint64_t id); // true = a session owns the socket now
        bool StartAudio(const Incoming &in, const std::vector<uint8_t> &hello);
        bool StartMic(const Incoming &in, const std::vector<uint8_t> &hello);
        bool StartCam(const Incoming &in, const std::vector<uint8_t> &hello);
        bool StartSession(const Incoming &in, const std::vector<uint8_t> &hello);
        void StopSession(SessionEnd why);
        void StopAudio();
        void StopMic();
        void StopCam();
        // PIN check shared by video and audio connections; sends the error itself. true = allowed.
        bool CheckPin(Conn &conn, const sockaddr_storage &addr, bool usb, uint32_t pin, const Settings &cfg);

        // Wrong-PIN rate limiting. Buckets: one per source address, one shared by all network
        // addresses ("*"), and USB ("usb") on its own so Wi-Fi attempts cannot lock out the USB tablet.
        struct PinBucket
        {
            std::deque<ULONGLONG> failures; // GetTickCount64() of recent wrong PINs
            ULONGLONG lockedUntil = 0;
        };
        ULONGLONG LockedFor(const std::string &key, ULONGLONG now); // ms remaining, 0 = open
        void RecordFailure(const std::string &key, ULONGLONG now, size_t limit);
        std::mutex pinMutex_;
        std::map<std::string, PinBucket> pinBuckets_;

        // Connections in the hello/PIN stage, each on its own thread. The accept loop cuts off those that take too long (a client
        // trickling one byte at a time would otherwise hold a slot for ever), and Stop waits until every one of those threads is done.
        struct Handshake
        {
            SOCKET s = INVALID_SOCKET;
            ULONGLONG startedMs = 0;
            bool cuttable = true;
        };
        std::mutex handshakeMutex_;
        std::condition_variable handshakeDone_;
        std::map<uint64_t, Handshake> handshakes_;
        uint64_t nextHandshakeId_ = 0; // accept thread only

        DeferredMonitorOff monitorOff_;
        std::atomic<bool> tabletConnected_{false};
        std::mutex sessionMutex_; // serializes "replace the running session"
        std::mutex mutex_;
        Settings settings_;
        StatusFn status_;
        SOCKET listen_ = INVALID_SOCKET;
        std::unique_ptr<TlsIdentity> tls_; // made in Start(), before any connection can arrive; null = no encryption
        std::thread acceptThread_;
        std::unique_ptr<Session> session_;
        std::thread sessionThread_;
        std::unique_ptr<AudioSession> audio_;
        std::thread audioThread_;
        std::unique_ptr<MicSession> mic_; // the tablet's microphone, into the PadDisplay Microphone driver (own connection, own thread)
        std::thread micThread_;
        std::unique_ptr<CamSession> cam_; // the tablet's camera, as the PC webcam
        std::thread camThread_;
        std::atomic<bool> stopping_{false};
    };

} // namespace pd
