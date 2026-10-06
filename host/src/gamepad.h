// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
// Game controllers connected to the tablet, shown to Windows as Xbox 360 controllers through the
// ViGEmBus driver (which the user installs; see vdd.h). Rumble from games goes back to the tablet.
#pragma once
#include "protocol.h"

#include <windows.h>
#include <array>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

struct _VIGEM_CLIENT_T;
struct _VIGEM_TARGET_T;

namespace pd {

class GamepadBridge {
public:
    using RumbleFn = std::function<void(uint8_t index, uint8_t largeMotor, uint8_t smallMotor)>; // on other threads
    using NoticeFn = std::function<void(const char* text)>;                                      // the driver is unusable

    GamepadBridge(RumbleFn rumble, NoticeFn notice); // starts the worker thread
    ~GamepadBridge();                                // unplugs every controller

    // A Gamepad message. Never waits for the driver: the newest state of each controller is handed to
    // the worker thread. Plugging a virtual controller in takes Windows ~40 ms, and a missing or
    // switched-off driver takes far longer; none of that may delay touch, pen, key or mouse input.
    void OnState(const uint8_t* p, size_t n);

private:
    struct Pad { // worker thread only
        _VIGEM_TARGET_T* target = nullptr;
        std::thread rumble; // waits for the games' rumble requests
        std::array<uint8_t, 12> report{};
        ULONGLONG settleUntil = 0; // Windows is still starting the controller: keep repeating the state
    };
    struct Mail { // the newest state of one controller; older ones are not worth applying
        bool pending = false;
        bool connected = false;
        std::array<uint8_t, 12> report{}; // the Gamepad message from its third byte on
    };

    void Run();
    bool Apply(int index, const Mail& mail); // false = the driver is not usable yet: try again
    bool Connect();
    void Submit(const Pad& pad);
    void Remove(int index);

    RumbleFn rumble_;
    NoticeFn notice_;

    std::mutex mutex_; // guards mail_ and stop_
    std::condition_variable cv_;
    std::array<Mail, kMaxGamepads> mail_;
    bool stop_ = false;

    // Worker thread only.
    _VIGEM_CLIENT_T* client_ = nullptr;
    std::array<Pad, kMaxGamepads> pads_;
    ULONGLONG retryAt_ = 0;        // do not hammer a missing driver
    ULONGLONG enableDeadline_ = 0; // the switch-on task was started: give the driver until then
    bool reported_ = false;        // the user has been told once
    std::thread worker_;
};

} // namespace pd
