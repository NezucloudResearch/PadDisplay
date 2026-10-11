// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
// The tablet's microphone through a virtual audio cable (VB-CABLE, shipped with PadDisplay and installed on request). Windows
// only shows microphones that a signed kernel driver provides; PadDisplay's own driver is not Microsoft-signed, so on a PC with
// Secure Boot the audio is played into the cable's playback device ("CABLE Input") and apps record from its recording
// device ("CABLE Output"). Used when PadDisplay's own driver cannot load (see micbackend.h).
#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pd {

// Is a playback device whose name contains `preferred` (default "CABLE Input") active? name = its full name when it is.
// Any thread; initialises COM for the call.
bool CableInstalled(const std::wstring& preferred, std::wstring* name = nullptr);
// The ID of the cable's other end, the recording device that apps pick as their microphone ("CABLE Output"): the recording
// device of the same audio adapter as the playback device above. Empty = not found. Any thread.
std::wstring CableRecordingDeviceId(const std::wstring& preferred);
// The ID of the first active recording device whose name contains `wanted`. Empty = not found. Any thread.
std::wstring RecordingDeviceId(const std::wstring& wanted);

class CableOutput {
public:
    CableOutput(std::wstring preferredName, bool minPeriod);
    ~CableOutput();
    // Finds the cable, starts the render thread and waits until the stream plays. false = why says what went wrong.
    bool Open(std::wstring& why);
    // The tablet's audio (48 kHz mono s16), from the network thread. Never blocks on the audio side.
    void Write(const int16_t* samples, size_t count);
    // The tablet's microphone stops or starts (nobody records / somebody records again). Any thread. The audio left in the
    // buffer is dropped and the next audio builds its cushion first, without that counting as an underrun or a gap.
    void Restart();
    bool Failed() const { return failed_; } // the audio side ended (the device went away, or could not be used)
    void Close();
    uint64_t Underruns() const { return underruns_; }
    uint64_t Dropped() const { return dropped_; }
    // How regularly the tablet's audio arrived (milliseconds between Write calls): a gap longer than the cushion is an underrun.
    double MaxGapMs() const { return maxGapUs_ / 1000.0; }
    uint64_t GapsOver15Ms() const { return gaps15_; }

private:
    void RenderLoop(std::wstring deviceId);

    std::wstring preferred_;
    bool minPeriod_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> failed_{false};
    void* started_ = nullptr; // an event handle; signalled once the render thread knows whether the stream runs
    std::atomic<bool> startedOk_{false};
    std::atomic<bool> restart_{false}; // Restart(), for the render thread
    std::atomic<bool> paused_{false};  // Restart(), for the network thread

    // The jitter buffer between the network thread and the audio thread: mono, 48 kHz, as received. A fixed ring: no
    // allocation per packet. The lock is only ever held for a memcpy-sized amount of work.
    static constexpr size_t kRing = 24000; // 500 ms
    std::mutex mutex_;
    std::vector<float> ring_ = std::vector<float>(kRing);
    uint64_t head_ = 0, tail_ = 0; // total samples written / read; fill = head - tail
    std::atomic<uint64_t> underruns_{0}, dropped_{0};
    uint64_t lastWriteUs_ = 0;                 // network thread only
    std::atomic<uint64_t> maxGapUs_{0}, gaps15_{0};
};

} // namespace pd
