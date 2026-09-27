// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#pragma once
#include "audio.h"
#include "capture.h"
#include "encoder.h"
#include "input.h"
#include "net.h"
#include "settings.h"

#include <atomic>
#include <cstdio>
#include <deque>
#include <map>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace pd {

using StatusFn = std::function<void(const std::wstring&)>;

// One connected tablet: captures the virtual display, encodes, streams, and injects its input.
// With sock == INVALID_SOCKET and a dump file it runs the capture/encode pipeline only (test mode).
class Session {
public:
    Session(SOCKET sock, const ClientHello& hello, bool usb, const Settings& settings, StatusFn status);
    ~Session();
    void Run(FILE* dump = nullptr, int dumpSeconds = 0);
    void Stop();

private:
    int SetupPipeline();  // 1 ready, 0 retry later, -1 fatal
    bool StreamLoop();    // true = rebuild the pipeline, false = session over
    void RecvLoop();
    bool Send(Msg type, const void* payload, uint32_t len);
    void SendError(ErrorCode code, const char* text);
    void AdaptBitrate();
    void ReportStatus(uint64_t now);

    SOCKET sock_;
    ClientHello hello_;
    bool usb_;
    Settings settings_;
    StatusFn status_;
    FILE* dump_ = nullptr;

    std::atomic<bool> stop_{false};
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

    std::wstring gdiName_;
    CaptureTarget target_;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> ctx_;
    DesktopCapture capture_;
    std::unique_ptr<Encoder> encoder_;
    std::unique_ptr<InputInjector> input_;
    Codec codec_ = CodecHEVC;
    int width_ = 0, height_ = 0, fps_ = 60;
    int bitrateKbps_ = 0, maxBitrateKbps_ = 0;
    bool configSent_ = false;

    uint64_t startUs_ = 0;
    uint64_t deadlineUs_ = 0;
    uint64_t minRttUs_ = UINT64_MAX;
    int stableSeconds_ = 0;
    uint32_t framesThisSecond_ = 0;
    uint64_t bytesThisSecond_ = 0;
    uint64_t encodeUsThisSecond_ = 0;
    uint64_t composeUsThisSecond_ = 0;
    uint64_t captureUsThisSecond_ = 0;
    ComPtr<ID3D11Query> probe_;
    double copiedThisSecond_ = 0;
    uint64_t lastSecondUs_ = 0;
    uint32_t statusCount_ = 0;
};

// Accepts tablets on TCP 27183 (dual-stack). A newly connected tablet replaces the current one.
class Server {
public:
    Server(const Settings& settings, StatusFn status) : settings_(settings), status_(std::move(status)) {}
    ~Server() { Stop(); }
    bool Start();
    void Stop();
    void UpdateSettings(const Settings& s);

private:
    void AcceptLoop();
    void Handle(SOCKET s, const sockaddr_storage& addr);
    void StopSession();
    void StopAudio();
    // PIN check shared by video and audio connections; sends the error itself. true = allowed.
    bool CheckPin(SOCKET s, const sockaddr_storage& addr, bool usb, uint32_t pin, const Settings& cfg);

    // Wrong-PIN rate limiting. Buckets: one per source address, one shared by all network
    // addresses ("*"), and USB ("usb") on its own so Wi-Fi attempts cannot lock out the USB tablet.
    struct PinBucket {
        std::deque<ULONGLONG> failures; // GetTickCount64() of recent wrong PINs
        ULONGLONG lockedUntil = 0;
    };
    ULONGLONG LockedFor(const std::string& key, ULONGLONG now);          // ms remaining, 0 = open
    void RecordFailure(const std::string& key, ULONGLONG now, size_t limit);
    std::mutex pinMutex_;
    std::map<std::string, PinBucket> pinBuckets_;

    std::atomic<int> handshakes_{0}; // connections still in the hello/PIN stage (own threads)
    std::mutex sessionMutex_;        // serializes "replace the running session"
    std::mutex mutex_;
    Settings settings_;
    StatusFn status_;
    SOCKET listen_ = INVALID_SOCKET;
    std::thread acceptThread_;
    std::unique_ptr<Session> session_;
    std::thread sessionThread_;
    std::unique_ptr<AudioSession> audio_;
    std::thread audioThread_;
    std::atomic<bool> stopping_{false};
};

// Collects VPS/SPS/PPS NAL units (with start codes) from an Annex-B access unit.
std::vector<uint8_t> ExtractParameterSets(const std::vector<uint8_t>& au, Codec codec);

} // namespace pd
