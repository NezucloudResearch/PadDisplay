// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#include "server.h"
#include "log.h"
#include "text.h"

#include <ws2tcpip.h>
#include <algorithm>

namespace pd
{
    namespace
    {

        constexpr DWORD kHandshakeIoTimeoutMs = 5000;
        constexpr size_t kMaxHandshakesInProgress = 4;

        bool Refuse(Conn &conn, ErrorCode code, const char *text)
        {
            SendErrorMessage(conn, code, text);
            return false;
        }

    } // namespace
    bool Server::Start()
    {
        // Before listening: it reads (or makes) the key file, which no connection thread should ever wait for.
        // With pre-sign-in access on, use the machine-wide identity so this host and the SYSTEM sign-in-screen
        // service present one fingerprint to the tablet (see TlsIdentity).
        tls_ = TlsIdentity::LoadOrCreate(settings_.preSignInAccess);
        if (tls_)
            LOGI("server: TLS ready, fingerprint %s", tls_->Fingerprint().c_str());
        else
            LOGI("server: TLS unavailable - Wi-Fi connections cannot be encrypted");
        listen_ = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
        if (listen_ == INVALID_SOCKET)
            return false;
        NoInherit(listen_);
        DWORD off = 0;
        setsockopt(listen_, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<char *>(&off), sizeof(off));
        sockaddr_in6 a{};
        a.sin6_family = AF_INET6;
        a.sin6_port = htons(kTcpPort);
        a.sin6_addr = in6addr_any;
        // Bind can briefly fail with "address in use" during the sign-in handoff: the pre-sign-in logon service
        // stops its SYSTEM host (which holds this port) at the same moment the per-user tray host autostarts.
        // Retry on that one error for ~2 s so the handoff never leaves the port unserved. Deliberately NOT
        // SO_REUSEADDR: on Windows that lets any process bind a port already in use, which would let another
        // process steal this one - and it carries the Windows password. Startup path only; no hot-path cost, and
        // the normal (no-conflict) case binds on the first try with no delay.
        bool bound = false;
        for (int attempt = 0; attempt < 20 && !bound; ++attempt)
        {
            if (bind(listen_, reinterpret_cast<sockaddr *>(&a), sizeof(a)) == 0)
            {
                bound = true;
                break;
            }
            if (WSAGetLastError() != WSAEADDRINUSE)
                break; // a real error (e.g. a genuine second instance): fail fast
            Sleep(100);
        }
        if (!bound || listen(listen_, 4) != 0)
        {
            LOGI("server: cannot listen on port %u (%d) - is another PadDisplay running?", kTcpPort, WSAGetLastError());
            closesocket(listen_);
            listen_ = INVALID_SOCKET;
            return false;
        }
        LOGI("server: listening on port %u", kTcpPort);
        acceptThread_ = std::thread([this]
                                    { AcceptLoop(); });
        return true;
    }

    void Server::Stop(SessionEnd why)
    {
        stopping_ = true;
        if (acceptThread_.joinable())
            acceptThread_.join(); // it polls stopping_: no need to wake it by closing the socket
        if (listen_ != INVALID_SOCKET)
            closesocket(listen_), listen_ = INVALID_SOCKET;
        {
            // Each handshake thread uses this object to the end, so none may outlive it. Shutting their sockets down ends a
            // hello or PIN read at once; one that already passed its connection on is replacing the session, which ends.
            std::unique_lock lock(handshakeMutex_);
            for (auto &[id, h] : handshakes_)
                if (h.cuttable)
                    CutOff(h.s);
            handshakeDone_.wait(lock, [this]
                                { return handshakes_.empty(); });
        }
        std::lock_guard lock(sessionMutex_);
        StopSession(why);
        monitorOff_.RunNow(); // Exit puts the PC back: a monitor kept for a reconnect goes off now
        StopAudio();
        StopMic();
        StopCam();
    }

    void Server::StopMic()
    {
        {
            std::lock_guard lock(mutex_);
            if (mic_)
                mic_->Stop();
        }
        if (micThread_.joinable())
            micThread_.join();
        std::lock_guard lock(mutex_);
        mic_.reset();
    }

    void Server::StopCam()
    {
        {
            std::lock_guard lock(mutex_);
            if (cam_)
                cam_->Stop();
        }
        if (camThread_.joinable())
            camThread_.join();
        std::lock_guard lock(mutex_);
        cam_.reset();
    }

    void Server::StopAudio()
    {
        {
            std::lock_guard lock(mutex_);
            if (audio_)
                audio_->Stop();
        }
        if (audioThread_.joinable())
            audioThread_.join();
        std::lock_guard lock(mutex_);
        audio_.reset();
    }

    void Server::UpdateSettings(const Settings &s)
    {
        std::lock_guard lock(mutex_);
        settings_ = s;
        // Unticking "Accept the tablet's microphone" ends a microphone session that is running now, so the driver is not kept busy.
        // Stop() only flags it and shuts the socket down (the UI thread calls this); the session closes the driver by itself.
        if (!s.acceptTabletMic && mic_)
            mic_->Stop();
    }

    void Server::AcceptLoop()
    {
        while (!stopping_)
        {
            CutSlowHandshakes(); // on the wakeup that is here anyway: no timer of its own
            // Wait with a timeout rather than blocking in accept(): closing a socket does not reliably wake
            // a thread blocked in accept(), and Exit has to be able to stop this loop.
            fd_set rs;
            FD_ZERO(&rs);
            FD_SET(listen_, &rs);
            timeval tv{0, 200000};
            int ready = select(0, &rs, nullptr, nullptr, &tv);
            if (ready <= 0)
            {
                if (ready < 0)
                    Sleep(100);
                continue;
            }
            sockaddr_storage addr{};
            int len = sizeof(addr);
            SOCKET s = accept(listen_, reinterpret_cast<sockaddr *>(&addr), &len);
            if (s == INVALID_SOCKET)
            {
                if (stopping_)
                    break;
                Sleep(100);
                continue;
            }
            NoInherit(s);
            // Each hello/PIN exchange runs on its own thread, so a client that connects and stays silent
            // cannot block others (it is cut off after kHandshakeMs). Cap the number in progress.
            const uint64_t id = nextHandshakeId_++;
            {
                std::lock_guard lock(handshakeMutex_);
                if (handshakes_.size() >= kMaxHandshakesInProgress)
                {
                    closesocket(s);
                    continue;
                }
                handshakes_[id] = Handshake{s, GetTickCount64()};
            }
            std::thread([this, s, addr, id]
                        {
            const bool handedOver = Handle(s, addr, id);
            // Closed under the lock, together with forgetting it: a socket number is reused at once, and the accept loop
            // must never cut off a newer connection that got the same number. Notified under the lock too, so Stop cannot
            // return, and this object go, before this thread is done with it.
            std::lock_guard lock(handshakeMutex_);
            if (!handedOver) closesocket(s);
            handshakes_.erase(id);
            handshakeDone_.notify_all(); })
                .detach();
        }
    }

    // A whole handshake (TLS, hello, PIN) takes well under a second. Each read already gives up after 5 s of silence, but a client
    // sending one byte now and then would keep its slot for ever, and four of them would lock every tablet out.
    void Server::CutSlowHandshakes()
    {
        constexpr ULONGLONG kHandshakeMs = 10000;
        const ULONGLONG now = GetTickCount64();
        std::lock_guard lock(handshakeMutex_);
        for (auto &[id, h] : handshakes_)
        {
            if (!h.cuttable || now - h.startedMs < kHandshakeMs)
                continue;
            LOGI("server: a connection took more than %llu s to say hello, cut off", kHandshakeMs / 1000);
            CutOff(h.s); // its thread's read fails now, and it closes the socket
            h.cuttable = false;
        }
    }

    void Server::EndHandshakeDeadline(uint64_t id)
    {
        std::lock_guard lock(handshakeMutex_);
        handshakes_[id].cuttable = false;
    }

    ULONGLONG Server::LockedFor(const std::string &key, ULONGLONG now)
    {
        auto it = pinBuckets_.find(key);
        return it != pinBuckets_.end() && it->second.lockedUntil > now ? it->second.lockedUntil - now : 0;
    }

    void Server::RecordFailure(const std::string &key, ULONGLONG now, size_t limit)
    {
        auto &b = pinBuckets_[key];
        b.failures.push_back(now);
        while (!b.failures.empty() && now - b.failures.front() > 60000)
            b.failures.pop_front();
        if (b.failures.size() >= limit)
        {
            b.lockedUntil = now + 60000;
            b.failures.clear();
            LOGI("server: too many wrong PINs (%s) - locked for 60 s", key.c_str());
        }
    }

    void Server::StopSession(SessionEnd why)
    {
        {
            std::lock_guard lock(mutex_);
            if (session_)
                session_->Stop(why);
        }
        if (sessionThread_.joinable())
            sessionThread_.join();
        std::lock_guard lock(mutex_);
        session_.reset();
    }

    // The socket stays open on a refusal: the handshake thread closes it.
    bool Server::CheckPin(Conn &conn, const sockaddr_storage &addr, bool usb, uint32_t pin, const Settings &cfg)
    {
        if (!usb || cfg.requireUsbPin)
        {
            // Rate limit wrong PINs: 5/min per source, 20/min across all network sources; USB is its own bucket.
            std::string source = usb ? "usb" : AddrToString(addr);
            ULONGLONG now = GetTickCount64(), wait = 0;
            {
                std::lock_guard lock(pinMutex_);
                wait = std::max(LockedFor(source, now), usb ? 0 : LockedFor("*", now));
            }
            if (wait > 0)
            {
                std::string text = "Too many wrong PINs. Try again in " + std::to_string((wait + 999) / 1000) + " s.";
                return Refuse(conn, ErrLockedOut, text.c_str());
            }
            if (pin != cfg.pin)
            {
                {
                    std::lock_guard lock(pinMutex_);
                    RecordFailure(source, now, 5);
                    if (!usb)
                        RecordFailure("*", now, 20);
                }
                LOGI("server: %s rejected (wrong PIN)", source.c_str());
                if (status_)
                    status_(Widen("Error: wrong PIN from " + source + " (someone may be guessing)"));
                Sleep(1000); // slows guessing; this thread only serves this one connection
                return Refuse(conn, ErrBadPin, "Wrong PIN. Right-click the PadDisplay icon in the PC's tray to see it.");
            }
        }
        return true;
    }

    // What is known about a connection once its hello has been read.
    struct Server::Incoming
    {
        Conn &conn;
        const sockaddr_storage &addr;
        bool usb;
        uint64_t id;
        const std::string &link; // "Wi-Fi, TLS 1.2, 256-bit", for the log
        const Settings &cfg;

        std::string From() const { return usb ? "USB" : AddrToString(addr); }
    };

    bool Server::Handle(SOCKET s, const sockaddr_storage &addr, uint64_t id)
    {
        TuneSocket(s, true);
        DWORD timeout = kHandshakeIoTimeoutMs;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char *>(&timeout), sizeof(timeout));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<char *>(&timeout), sizeof(timeout));

        Settings cfg;
        {
            std::lock_guard lock(mutex_);
            cfg = settings_;
        }
        const bool usb = IsLoopback(addr);
        Conn conn;
        conn.s = s;
        // A TLS ClientHello starts with 0x16, a plain hello with 0x81 or 0x89: one peeked byte tells them apart,
        // so tablet apps from before TLS keep working and the message formats did not have to change.
        char firstByte = 0;
        if (recv(s, &firstByte, 1, MSG_PEEK) != 1)
            return false;
        std::string tlsInfo;
        if (firstByte == 0x16)
        {
            if (!tls_ || !(conn.tls = tls_->Accept(s, &tlsInfo)))
            {
                LOGI("server: TLS handshake with %s failed", AddrToString(addr).c_str());
                return false;
            }
        }
        const std::string link = std::string(usb ? "USB" : "Wi-Fi") + (conn.tls ? ", " + tlsInfo : ", plain");

        Msg type{};
        std::vector<uint8_t> p;
        if (!RecvMsg(conn, type, p, 1024))
            return false;
        // Answered only after the hello was read: closing a socket with unread data resets it, and the tablet would never see the error.
        if (!conn.tls && !usb && cfg.requireEncryption)
        {
            LOGI("server: refused a plain connection from %s (requireEncryption)", AddrToString(addr).c_str());
            return Refuse(conn, ErrEncryptionRequired, "This PC only accepts encrypted connections. Update the PadDisplay app on the tablet.");
        }
        const Incoming incoming{conn, addr, usb, id, link, cfg};
        if (type == Msg::AudioHello && p.size() >= 7)
            return StartAudio(incoming, p);
        // The tablet's microphone and camera (v4): each on its own connection, so neither can delay the picture or the input.
        if (type == Msg::MicHello && p.size() >= 7)
            return StartMic(incoming, p);
        if (type == Msg::CamHello && p.size() >= 12)
            return StartCam(incoming, p);
        if (type == Msg::ClientHello && p.size() >= 16)
            return StartSession(incoming, p);
        return false;
    }

    bool Server::StartAudio(const Incoming &in, const std::vector<uint8_t> &hello)
    {
        if (!CheckPin(in.conn, in.addr, in.usb, rd32(&hello[2]), in.cfg))
            return false;
        bool mutePc = (hello[6] & AudioMutePc) != 0;
        LOGI("server: audio connection from %s (%s)%s", in.From().c_str(), in.link.c_str(), mutePc ? " (mute PC)" : "");
        EndHandshakeDeadline(in.id);
        std::lock_guard replace(sessionMutex_);
        if (stopping_)
            return false;
        StopAudio(); // newest wins
        std::lock_guard lock(mutex_);
        audio_ = std::make_unique<AudioSession>(std::move(in.conn), mutePc);
        AudioSession *audio = audio_.get();
        audioThread_ = std::thread([audio]
                                   { audio->Run(); });
        return true;
    }

    bool Server::StartMic(const Incoming &in, const std::vector<uint8_t> &hello)
    {
        if (!CheckPin(in.conn, in.addr, in.usb, rd32(&hello[2]), in.cfg))
            return false;
        LOGI("server: microphone connection from %s (%s)", in.From().c_str(), in.link.c_str());
        EndHandshakeDeadline(in.id);
        std::lock_guard replace(sessionMutex_);
        if (stopping_)
            return false;
        StopMic(); // newest wins
        std::lock_guard lock(mutex_);
        mic_ = std::make_unique<MicSession>(std::move(in.conn), in.cfg, status_);
        MicSession *mic = mic_.get();
        micThread_ = std::thread([mic]
                                 { mic->Run(); });
        return true;
    }

    bool Server::StartCam(const Incoming &in, const std::vector<uint8_t> &hello)
    {
        if (!CheckPin(in.conn, in.addr, in.usb, rd32(&hello[2]), in.cfg))
            return false;
        const uint16_t w = rd16(&hello[7]), h = rd16(&hello[9]);
        const uint8_t fps = hello[11];
        LOGI("server: camera connection from %s (%s) %ux%u@%u", in.From().c_str(), in.link.c_str(), w, h, fps);
        EndHandshakeDeadline(in.id);
        std::lock_guard replace(sessionMutex_);
        if (stopping_)
            return false;
        StopCam();
        std::lock_guard lock(mutex_);
        cam_ = std::make_unique<CamSession>(std::move(in.conn), in.cfg, w, h, fps, status_);
        CamSession *cam = cam_.get();
        camThread_ = std::thread([cam]
                                 { cam->Run(); });
        return true;
    }

    bool Server::StartSession(const Incoming &in, const std::vector<uint8_t> &hello)
    {
        const uint8_t *p = hello.data();
        ClientHello h;
        h.version = rd16(&p[0]);
        h.screenW = rd16(&p[2]);
        h.screenH = rd16(&p[4]);
        h.dpi = rd16(&p[6]);
        h.codecMask = p[8];
        h.maxTouch = p[9];
        h.pin = rd32(&p[10]);
        h.maxFps = rd16(&p[14]);
        size_t nameAt = 16;
        if (h.version >= 3 && hello.size() > 16)
            h.flags = p[nameAt++];
        memcpy(h.name, p + nameAt, std::min<size_t>(hello.size() - nameAt, sizeof(h.name) - 1));

        if (h.version < kMinProtocolVersion || h.version > kProtocolVersion)
            return Refuse(in.conn, ErrVersion, "App and PC versions do not match. Update both.");
        if (!CheckPin(in.conn, in.addr, in.usb, h.pin, in.cfg))
            return false;
        DWORD zero = 0;
        setsockopt(in.conn.s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char *>(&zero), sizeof(zero));
        LOGI("server: client '%s' from %s (%s) screen %ux%u@%u dpi %u codecs %u%s%s%s", h.name, AddrToString(in.addr).c_str(),
             in.link.c_str(), h.screenW, h.screenH, h.maxFps, h.dpi, h.codecMask, h.flags & HelloGamingMode ? ", gaming mode" : "",
             h.flags & HelloOnlyScreen ? ", only screen" : "", h.flags & HelloReconnects ? ", reconnects" : "");
        EndHandshakeDeadline(in.id); // ending the previous session below can take seconds

        std::lock_guard replace(sessionMutex_);
        if (stopping_)
            return false;
        {
            std::mutex sendMutex; // nothing else sends on this connection yet
            WaitingPing waiting(in.conn, sendMutex);
            StopSession(SessionEnd::Replaced); // newest tablet wins
            monitorOff_.Cancel();              // a monitor kept for a reconnect is this session's now, windows and all
        }
        std::lock_guard lock(mutex_);
        session_ = std::make_unique<Session>(std::move(in.conn), h, in.usb, in.cfg, status_);
        Session *session = session_.get();
        tabletConnected_ = true;
        sessionThread_ = std::thread([this, session, grace = in.cfg.reconnectGraceSeconds]
                                     {
        session->Run();
        if (session->KeepsMonitorForReconnect()) monitorOff_.Schedule(grace, session->MonitorName());
        tabletConnected_ = false;
        if (!stopping_ && status_) status_(L"Waiting for tablet"); });
        return true;
    }

} // namespace pd
