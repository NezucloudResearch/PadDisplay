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
#include <functional>
#include <mutex>

namespace pd {

// One input event offered to the secure-desktop helper (uacrelay.*) when the host's own injection
// cannot reach the target: a UAC prompt (High->System) or the locked Windows sign-in screen (the
// Winlogon desktop). The helper runs as System in the session and injects there; it gates every event
// by the window/desktop under it, so anything not on a secure desktop is dropped on its side.
struct UacForwardEvent {
    enum Kind : uint8_t {
        Click,     // left click at (x,y) screen pixels - a UAC button or a lock-screen control
        KeyVk,     // a UAC-navigation virtual key (Enter/Esc/Tab/Space/arrows); for consent prompts
        KeyScan,   // a full key (wire scancode + down), for typing into the lock / sign-in screen
        MouseMove, // move the cursor to (x,y); for the lock / sign-in screen
    };
    Kind kind;
    int x = 0, y = 0;
    WORD vk = 0;       // KeyVk
    uint16_t code = 0; // KeyScan: wire scancode, bit 8 = E0-extended
    bool down = false; // KeyScan
};

class InputInjector {
public:
    // [elevated]: this process has administrator rights, so Windows (UIPI) lets its input reach
    // elevated windows. When it does not, [notice] is called once with a message for the tablet
    // the first time an injection is blocked, so the user learns why nothing is reaching the app.
    InputInjector(bool elevated, std::function<void(const char*)> notice);
    ~InputInjector();

    void SetTarget(const RECT& desktopRect);
    // While set, input that the host may need to replay on a secure desktop is offered here: clicks,
    // cursor moves and keys. The session points this at the System-level helper (see uacrelay.*), which
    // drops anything not on a UAC prompt or the locked sign-in screen. Windows does not refuse injection
    // into those from the host (it silently drops it), so this cannot key off the inject result. Null =
    // no forwarding; set only when the UAC-click or lock-screen feature is on.
    void SetUacForward(std::function<void(const UacForwardEvent&)> forward);
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
        bool onKeyboard = false; // began on the on-screen keyboard: sent as the mouse, never as touch
        uint8_t clientId = 0;
        POINTER_TYPE_INFO info{};
    };

    POINT Map(uint16_t x, uint16_t y) const;
    bool Inject(HSYNTHETICPOINTERDEVICE dev, const POINTER_TYPE_INFO* infos, UINT32 count, const char* what);
    void InjectTouchFrame(); // all active contacts as UPDATE
    void PressKeyboardWithMouse(int slot, uint8_t kind, POINT screen);
    void InjectPen(POINTER_FLAGS flags, POINTER_BUTTON_CHANGE_TYPE change);
    void SendMouse(DWORD flags, POINT pt, DWORD data = 0);
    void SendMouseRel(DWORD flags, LONG dx = 0, LONG dy = 0, DWORD data = 0); // no jump to a position
    void SendKey(uint16_t code, bool down);
    // UIPI blocks input to a higher-integrity (elevated) window from a normal process; the call then
    // fails with ERROR_ACCESS_DENIED. [ok] is the SendInput / InjectSyntheticPointerInput result and
    // [err] the GetLastError captured right after it (before any other call could overwrite it).
    void NoteInjectResult(bool ok, DWORD err);

    bool elevated_;
    std::function<void(const char*)> notice_;
    std::function<void(const UacForwardEvent&)> uacForward_; // set only when a secure-desktop feature is on
    bool toldElevation_ = false; // the elevation notice was sent; cleared by the next injection that lands
    std::mutex mutex_;
    RECT rect_{0, 0, 1, 1};
    HSYNTHETICPOINTERDEVICE touchDev_ = nullptr;
    HSYNTHETICPOINTERDEVICE penDev_ = nullptr;
    std::array<Contact, 10> contacts_{};
    int keyboardPress_ = -1; // the contact holding the left button down on the on-screen keyboard
    ULONGLONG lastTouchInject_ = 0;

    POINTER_TYPE_INFO pen_{};
    bool penInRange_ = false, penInContact_ = false;
    uint8_t buttonsDown_ = 0;    // bit per mouse button (see kButtons in input.cpp)
    std::bitset<0x200> keysDown_; // by wire scancode
    int injectErrors_ = 0;
};

} // namespace pd
