// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
// Injects tablet input into Windows: synthetic touch (multi-touch), synthetic pen
// (pressure/tilt/hover/eraser) and absolute mouse, mapped onto the virtual monitor, plus the
// keyboard and captured (relative) mouse connected to the tablet.
#pragma once
#include <windows.h>
#include <array>
#include <bitset>
#include <cstdint>
#include <mutex>

namespace pd {

class InputInjector {
public:
    InputInjector();
    ~InputInjector();

    void SetTarget(const RECT& desktopRect);
    void OnTouch(const uint8_t* p, size_t n);
    void OnPen(const uint8_t* p, size_t n);
    void OnMouse(const uint8_t* p, size_t n);
    void OnMouseRel(const uint8_t* p, size_t n);
    void OnKey(const uint8_t* p, size_t n);
    void Tick();       // call ~every 100 ms; refreshes held touch contacts
    void ReleaseAll(); // lift every contact / button / key (on disconnect)

private:
    struct Contact {
        bool active = false;
        uint8_t clientId = 0;
        POINTER_TYPE_INFO info{};
    };

    POINT Map(uint16_t x, uint16_t y) const;
    bool Inject(HSYNTHETICPOINTERDEVICE dev, const POINTER_TYPE_INFO* infos, UINT32 count, const char* what);
    void InjectTouchFrame(); // all active contacts as UPDATE
    void InjectPen(POINTER_FLAGS flags, POINTER_BUTTON_CHANGE_TYPE change);
    void SendMouse(DWORD flags, POINT pt, DWORD data = 0);
    void SendMouseRel(DWORD flags, LONG dx = 0, LONG dy = 0, DWORD data = 0); // no jump to a position
    void SendKey(uint16_t code, bool down);

    std::mutex mutex_;
    RECT rect_{0, 0, 1, 1};
    HSYNTHETICPOINTERDEVICE touchDev_ = nullptr;
    HSYNTHETICPOINTERDEVICE penDev_ = nullptr;
    std::array<Contact, 10> contacts_{};
    ULONGLONG lastTouchInject_ = 0;

    POINTER_TYPE_INFO pen_{};
    bool penInRange_ = false, penInContact_ = false;
    uint8_t buttonsDown_ = 0;    // bit per mouse button (see kButtons in input.cpp)
    std::bitset<0x200> keysDown_; // by wire scancode
    int injectErrors_ = 0;
};

} // namespace pd
