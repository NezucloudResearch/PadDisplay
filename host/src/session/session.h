// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#pragma once
#include "audio/audio.h"
#include "camera/camsink.h"
#include "capture/adapters.h"
#include "capture/capture.h"
#include "capture/lockcapture.h"
#include "display/display.h"
#include "encoder/encoder.h"
#include "input/gamepad.h"
#include "input/input.h"
#include "mic/micsink.h"
#include "net/net.h"
#include "protocol.h"
#include "settings.h"
#include "secure/uacrelay.h"

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace pd
{

    using StatusFn = std::function<void(const std::wstring &)>;

    // The payload of a Msg::Error.
    std::vector<uint8_t> ErrorPayload(ErrorCode code, const char *text);
    void SendErrorMessage(Conn &conn, ErrorCode code, const char *text);

    // Pings the tablet twice a second, from its own thread, while a session thread is busy with slow blocking work that sends
    // nothing: ending the previous session, switching the virtual monitor on. The tablet gives up after 10 s of silence, and
    // together those steps can take that long (a tablet that reconnects right after leaving). It lives for those seconds only.
    class WaitingPing
    {
    public:
        WaitingPing(Conn &conn, std::mutex &sendMutex);
        ~WaitingPing();

    private:
        std::mutex mutex_;
        std::condition_variable done_;
        bool stop_ = false;
        std::thread thread_;
    };

    // Why a session ended. It decides whether the tablet is told (so it does not reconnect) and whether the virtual monitor stays on.
    enum class SessionEnd
    {
        None,
        TabletLeft,
        Lost,
        Replaced,
        HostExit,
        HostRestart,
        Failed
    };
    const char *SessionEndName(SessionEnd why);

    // One connected tablet: captures the virtual display, encodes, streams, and injects its input.
    // With conn.s == INVALID_SOCKET and a dump file it runs the capture/encode pipeline only (test mode).
    class Session
    {
    public:
        Session(Conn conn, const ClientHello &hello, bool usb, const Settings &settings, StatusFn status);
        ~Session();
        void Run(FILE *dump = nullptr, int dumpSeconds = 0);
        void Stop(SessionEnd why);
        // After Run: the tablet lost its link and reconnects by itself, so the virtual monitor was left on for it.
        bool KeepsMonitorForReconnect() const { return keepMonitor_; }
        const std::wstring &MonitorName() const { return gdiName_; }

    private:
        enum class Told
        {
            None,
            Locked,
            Uac
        }; // what the tablet has been told about a frozen picture

        bool PrepareDisplay(); // false = the session cannot go on; the tablet has been told
        void ChooseStreamFormat();
        bool BringUpMonitor(VirtualDisplay &vd, bool &switchedOn);
        bool AddTabletModeToDriver(const DisplayMode &want, VirtualDisplay &vd);
        bool AttachMonitor(const DisplayMode &want, DisplayMode &active);
        void StartSecureDesktopHelp();
        void WatchLayoutWhileStarting();
        void TagStreamForWifi();
        void Serve(); // streams and injects input until the session ends
        void StartInputThreads();
        bool WaitOutSecureDesktop(Told &told);
        bool StreamLockScreen(Told reason, bool &streamed);
        void Teardown(); // always last: the threads end, the PC's screens and the virtual monitor are put back
        void EndOnlyScreenMode(bool monitorStaysOn);
        void EndAs(SessionEnd why); // the first reason given is kept
        void SendGoodbye(SessionEnd why);
        bool GamingMode() const; // the tablet app decides; an app from before v3 cannot, so settings.ini does
        void ChooseMonitorGpu(); // before the virtual monitor is switched on: which GPU renders it, and so encodes it
        int SetupPipeline();     // 1 ready, 0 retry later, -1 fatal
        bool ChooseCaptureAdapter(CaptureTarget &t);
        bool EnsureDevice(const CaptureTarget &t, bool wgc);
        bool StartEncoder(UINT vendorId);
        bool DeviceLost() const; // the GPU was reset or its driver replaced: the device and all made on it are gone
        bool StreamLoop();       // true = rebuild the pipeline, false = session over
        bool OnCaptureLost(uint64_t now);
        enum class Emit
        {
            Sent,
            EncoderFailed,
            SendFailed,
            NeedIdr
        };
        Emit EmitFrame(bool &pendingIdr);
        bool Heartbeat(uint64_t now); // pings the tablet; false = it stopped answering or the connection is gone
        void RecvLoop();
        void OnFrameAck(const std::vector<uint8_t> &message, uint64_t now);
        void OnPong(const std::vector<uint8_t> &message, uint64_t now);
        void OnSecureText(std::vector<uint8_t> &message);
        void InputLoop();
        bool Send(Msg type, const void *payload, uint32_t len);
        void SendError(ErrorCode code, const char *text);
        void SendNotice(const char *text); // shown on the tablet and as a tray warning; the session goes on
        void SendLockState(bool frozen);   // tell a v5+ tablet whether the picture is frozen on the locked sign-in screen (offer the password box); sent only on change
        void AdaptBitrate();
        void ReportStatus(uint64_t now);

        Conn conn_;
        ClientHello hello_;
        bool usb_;
        Settings settings_;
        StatusFn status_;
        FILE *dump_ = nullptr;
        std::mutex sendMutex_; // frames, rumble and notices are sent from different threads

        std::atomic<bool> stop_{false};
        std::atomic<SessionEnd> end_{SessionEnd::None};
        bool monitorUsed_ = false; // session thread only: the virtual monitor was found or switched on for this session
        bool keepMonitor_ = false; // set by Teardown, see KeepsMonitorForReconnect
        std::atomic<bool> keyframeRequested_{true};
        std::atomic<uint32_t> lastRttUs_{0};
        std::atomic<uint64_t> rttSumUs_{0};
        std::atomic<uint32_t> rttCount_{0};
        std::atomic<uint64_t> lastRecvUs_{0};

        // Flow control (protocol v2): frames in flight = sent - acked.
        bool flowControl_ = false;
        uint64_t framesSent_ = 0;
        std::atomic<uint64_t> framesAcked_{0};
        std::mutex inflightMutex_;
        std::deque<std::pair<uint64_t, uint64_t>> inflight_; // (ptsUs, sendUs)
        std::condition_variable ackCv_;                      // with inflightMutex_: an ack arrived
        std::atomic<uint64_t> latencySumUs_{0};
        std::atomic<uint32_t> latencyCount_{0};
        std::atomic<uint32_t> lastLatencyUs_{0};
        uint64_t minLatencyUs_ = UINT64_MAX;
        uint64_t windowMinUs_ = UINT64_MAX;
        int windowSeconds_ = 0;
        uint64_t blockedUsThisSecond_ = 0;
        uint32_t lastBlockedMs_ = 0;
        HANDLE qos_ = nullptr;
        uint32_t qosFlow_ = 0;
        std::thread recvThread_;

        // Input is injected on its own thread so a slow injection cannot delay the acks.
        std::thread inputThread_;
        std::mutex inputMutex_;
        std::condition_variable inputCv_;
        std::deque<std::pair<Msg, std::vector<uint8_t>>> inputQueue_;
        std::unique_ptr<GamepadBridge> gamepads_; // made by the input thread when a controller first reports

        std::thread layoutThread_;   // puts the real screens back while the stream starts
        bool onlyScreen_ = false;    // the tablet asked to be the PC's only screen: the real ones are off
        VirtualPlacement placement_; // where the virtual monitor was attached, before only-screen mode moved it to (0,0)
        bool havePlacement_ = false;
        std::atomic<bool> layoutChanged_{false}; // it moved one: the capture target must be re-read

        std::wstring gdiName_;
        std::wstring chosenGpu_;          // session thread only: the GPU the virtual monitor was asked to be rendered on (empty: not chosen)
        std::wstring fallbackGpu_;        // session thread only: another GPU to try if the monitor does not come up on the chosen one
        bool wgcOnly_ = false;            // session thread only: Desktop Duplication did not work here, capture with Windows.Graphics.Capture
        int captureLostInRow_ = 0;        // session thread only: captures lost one right after the other (throttles the retries)
        int captureLostNoFrame_ = 0;      // session thread only: captures lost since the last frame that arrived
        uint64_t captureLostSinceUs_ = 0; // session thread only: when the first of those was lost
        CaptureTarget target_;
        ComPtr<ID3D11Device> device_;
        ComPtr<ID3D11DeviceContext> ctx_;
        DesktopCapture capture_;
        std::unique_ptr<Encoder> encoder_;
        std::unique_ptr<InputInjector> input_;
        bool uacClickable_ = false;                // only-screen + elevated + opted in: the tablet may click UAC prompts
        bool lockControl_ = false;                 // elevated + opted in: the tablet may see and type into the locked sign-in screen
        std::unique_ptr<UacClickClient> uacClick_; // the client side of the System-level secure-desktop helper (uacrelay.*)
        LockCaptureReceiver lockCap_;              // shared texture the helper writes the locked sign-in screen into (piece 3)
        std::vector<uint8_t> au_, msg_;            // reused encode/send buffers
        bool pendingIdr_ = false;                  // session thread only: outlives StreamLoop, so a pipeline rebuild cannot lose a keyframe request
        Codec codec_ = CodecHEVC;
        int width_ = 0, height_ = 0, fps_ = 60;
        int bitrateKbps_ = 0, maxBitrateKbps_ = 0;
        bool configSent_ = false;

        uint64_t startUs_ = 0;
        uint64_t deadlineUs_ = 0;
        uint64_t minRttUs_ = UINT64_MAX;
        int stableSeconds_ = 0;
        uint64_t adaptHoldUntilUs_ = 0; // no bitrate adaptation while the tablet restarts its decoder
        uint32_t framesThisSecond_ = 0;
        uint64_t bytesThisSecond_ = 0;
        uint64_t encodeUsThisSecond_ = 0;
        uint64_t composeUsThisSecond_ = 0;
        ComPtr<ID3D11Query> probe_;
        double copiedThisSecond_ = 0;
        uint64_t lastSecondUs_ = 0;
        uint64_t lastPingUs_ = 0;    // session thread only
        bool secureDesktop_ = false; // Windows is showing a UAC prompt, which cannot be captured
        int lockStateSent_ = -1;     // session thread only: last LockState sent to the tablet (-1 none, 0/1), to send only on change
        uint32_t statusCount_ = 0;
    };

} // namespace pd
