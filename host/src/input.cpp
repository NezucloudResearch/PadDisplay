// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#include "input.h"
#include "log.h"
#include "protocol.h"

#include <algorithm>

namespace pd {

InputInjector::InputInjector(bool elevated, std::function<void(const char*)> notice)
    : elevated_(elevated), notice_(std::move(notice)) {
    touchDev_ = CreateSyntheticPointerDevice(PT_TOUCH, UINT32(contacts_.size()), POINTER_FEEDBACK_DEFAULT);
    penDev_ = CreateSyntheticPointerDevice(PT_PEN, 1, POINTER_FEEDBACK_DEFAULT);
    if (!touchDev_ || !penDev_) LOGI("input: CreateSyntheticPointerDevice failed (%lu)", GetLastError());
    pen_.type = PT_PEN;
    pen_.penInfo.pointerInfo.pointerType = PT_PEN;
    pen_.penInfo.penMask = PEN_MASK_PRESSURE | PEN_MASK_TILT_X | PEN_MASK_TILT_Y;
}

InputInjector::~InputInjector() {
    ReleaseAll();
    if (touchDev_) DestroySyntheticPointerDevice(touchDev_);
    if (penDev_) DestroySyntheticPointerDevice(penDev_);
}

void InputInjector::SetTarget(const RECT& r) {
    std::lock_guard lock(mutex_);
    rect_ = r;
}

void InputInjector::SetUacForward(std::function<void(const UacForwardEvent&)> forward) {
    std::lock_guard lock(mutex_);
    uacForward_ = std::move(forward);
}

// Maps the wire scancode of one of the few keys allowed on a UAC prompt to its virtual-key code; 0
// for anything else, so no other key is ever forwarded.
static WORD UacVkFromWire(uint16_t code) {
    const bool ext = (code & kKeyExtended) != 0;
    switch (code & 0xFF) {
    case 0x1C: return VK_RETURN;
    case 0x01: return VK_ESCAPE;
    case 0x0F: return VK_TAB;
    case 0x39: return VK_SPACE;
    case 0x48: return ext ? VK_UP : 0;
    case 0x50: return ext ? VK_DOWN : 0;
    case 0x4B: return ext ? VK_LEFT : 0;
    case 0x4D: return ext ? VK_RIGHT : 0;
    default: return 0;
    }
}

// Synthetic pointer (touch/pen) injection takes coordinates relative to the top-left of the
// virtual screen, not screen coordinates: with a monitor left of the primary they differ.
static POINT ToVirtualScreen(POINT pt) {
    return {pt.x - GetSystemMetrics(SM_XVIRTUALSCREEN), pt.y - GetSystemMetrics(SM_YVIRTUALSCREEN)};
}

// The classic on-screen keyboard (osk.exe). WindowFromPoint sends no message to another thread's window, so a hung
// app under the finger cannot stall the input thread.
static bool OnScreenKeyboardAt(POINT screen) {
    wchar_t cls[16] = {};
    return GetClassNameW(GetAncestor(WindowFromPoint(screen), GA_ROOT), cls, int(std::size(cls))) && wcscmp(cls, L"OSKMainClass") == 0;
}

POINT InputInjector::Map(uint16_t x, uint16_t y) const {
    LONG w = std::max<LONG>(1, rect_.right - rect_.left), h = std::max<LONG>(1, rect_.bottom - rect_.top);
    return {rect_.left + LONG((int64_t(x) * (w - 1) + kCoordMax / 2) / kCoordMax),
            rect_.top + LONG((int64_t(y) * (h - 1) + kCoordMax / 2) / kCoordMax)};
}

bool InputInjector::Inject(HSYNTHETICPOINTERDEVICE dev, const POINTER_TYPE_INFO* infos, UINT32 count, const char* what) {
    if (!dev || count == 0) return false;
    if (InjectSyntheticPointerInput(dev, infos, count)) { NoteInjectResult(true, 0); return true; }
    const DWORD err = GetLastError();
    if (injectErrors_++ < 20) LOGI("input: inject %s failed (%lu)", what, err);
    NoteInjectResult(false, err);
    return false;
}

// UIPI drops mouse/keyboard (SendInput) and touch/pen (InjectSyntheticPointerInput) from a process to
// a window of higher integrity; the call fails with ERROR_ACCESS_DENIED. Game controllers are
// unaffected: ViGEmBus feeds them in as a virtual HID device, below UIPI. Two cases:
//   - Not elevated, blocked by an elevated (High) window (Task Manager, an app run as administrator):
//     running PadDisplay as administrator fixes it, so the tablet is told to.
//   - Elevated and still blocked: the window is above High, i.e. System integrity. In practice that is
//     a Windows security prompt (UAC / consent.exe), which no High-level process can click. If the
//     System-level UAC injector is active (uacForward_ set) it will click it, so say nothing; otherwise
//     tell the user to answer it on the PC.
// Told once; the flag clears on the next injection that lands, so moving between windows reminds again.
void InputInjector::NoteInjectResult(bool ok, DWORD err) {
    if (ok) { toldElevation_ = false; return; }
    if (toldElevation_ || !notice_ || err != ERROR_ACCESS_DENIED) return;
    toldElevation_ = true;
    if (elevated_) {
        if (uacForward_) return; // the UAC injector handles this click; no notice needed
        notice_("Windows is blocking the tablet from clicking this. It is a Windows security prompt "
                "(User Account Control), which runs above administrator level, so not even PadDisplay "
                "running as administrator can click it from the tablet. Answer it on the PC.");
    } else
        notice_("An app running as administrator has focus, so Windows is blocking the tablet's mouse, "
                "keyboard, touch and pen from it (game controllers still work). To control it, run "
                "PadDisplay as administrator: right-click the PadDisplay icon in the PC's tray and choose "
                "\"Run as administrator\".");
}

void InputInjector::OnTouch(const uint8_t* p, size_t n) {
    if (n < 1) return;
    size_t count = p[0];
    if (n < 1 + count * 10) return;
    std::lock_guard lock(mutex_);

    std::array<POINTER_TYPE_INFO, 10> frame{};
    std::array<int, 10> frameSlot{};
    std::array<POINT, 10> downPts{}; // screen points of contacts that begin (go down) in this frame
    UINT32 used = 0, downs = 0;
    for (size_t i = 0; i < count && used < frame.size(); ++i) {
        const uint8_t* e = p + 1 + i * 10;
        uint8_t id = e[0], kind = e[1];
        POINT screen = Map(rd16(e + 2), rd16(e + 4));
        POINT pt = ToVirtualScreen(screen);
        uint32_t pressure = std::min<uint32_t>(rd16(e + 6), kPressureMax);

        int slot = -1;
        for (int s = 0; s < int(contacts_.size()); ++s)
            if (contacts_[s].active && contacts_[s].clientId == id) slot = s;
        if (slot < 0) {
            if (kind == TouchUp || kind == TouchCancel) continue; // unknown contact
            for (int s = 0; s < int(contacts_.size()) && slot < 0; ++s)
                if (!contacts_[s].active) slot = s;
            if (slot < 0) continue;
            kind = TouchDown; // a contact must begin with DOWN
        } else if (kind == TouchDown) {
            kind = TouchMove;
        }

        Contact& c = contacts_[slot];
        if (kind == TouchDown) c.onKeyboard = OnScreenKeyboardAt(screen);
        if (c.onKeyboard) {
            PressKeyboardWithMouse(slot, kind, screen);
            c.active = kind == TouchDown || kind == TouchMove;
            c.clientId = id;
            continue;
        }
        auto& ti = c.info.touchInfo;
        c.info.type = PT_TOUCH;
        ti.pointerInfo.pointerType = PT_TOUCH;
        ti.pointerInfo.pointerId = UINT32(slot);
        ti.pointerInfo.ptPixelLocation = pt;
        ti.touchMask = TOUCH_MASK_CONTACTAREA | TOUCH_MASK_PRESSURE;
        ti.pressure = pressure;
        ti.rcContact = {pt.x - 4, pt.y - 4, pt.x + 4, pt.y + 4};
        switch (kind) {
        case TouchDown: ti.pointerInfo.pointerFlags = POINTER_FLAG_DOWN | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT; break;
        case TouchMove: ti.pointerInfo.pointerFlags = POINTER_FLAG_UPDATE | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT; break;
        case TouchUp: ti.pointerInfo.pointerFlags = POINTER_FLAG_UP; break;
        default: ti.pointerInfo.pointerFlags = POINTER_FLAG_UP | POINTER_FLAG_CANCELED; break;
        }
        if (kind == TouchDown) downPts[downs++] = screen; // may become a UAC click if this frame is blocked
        c.active = true;
        c.clientId = id;
        frameSlot[used] = slot;
        frame[used++] = c.info;
    }
    // Every active contact must appear in each frame.
    for (int s = 0; s < int(contacts_.size()) && used < frame.size(); ++s) {
        if (!contacts_[s].active || contacts_[s].onKeyboard || std::find(frameSlot.begin(), frameSlot.begin() + used, s) != frameSlot.begin() + used)
            continue;
        auto info = contacts_[s].info;
        info.touchInfo.pointerInfo.pointerFlags = POINTER_FLAG_UPDATE | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT;
        frameSlot[used] = s;
        frame[used++] = info;
    }
    Inject(touchDev_, frame.data(), used, "touch");
    lastTouchInject_ = GetTickCount64();
    // Synthetic touch does not actuate a UAC prompt (the inject returns success but Windows drops it), so
    // offer each new contact to the UAC click path too. The client ignores it unless a prompt is up, and
    // the injector re-checks the window under the point, so a normal tap costs only an atomic read.
    if (uacForward_)
        for (UINT32 i = 0; i < downs; ++i) uacForward_({UacForwardEvent::Click, downPts[i].x, downPts[i].y});

    for (UINT32 i = 0; i < used; ++i) {
        Contact& c = contacts_[frameSlot[i]];
        if (frame[i].touchInfo.pointerInfo.pointerFlags & POINTER_FLAG_UP) c.active = false;
        else c.info.touchInfo.pointerInfo.pointerFlags = POINTER_FLAG_UPDATE | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT;
    }
}

// osk.exe types on a mouse press but on a touch release (measured: a tapped key arrived as the finger lifted, a held one
// only after it), so a finger that lands on it is sent as the mouse, and the key comes as the finger touches. A mouse has
// one left button: a second finger takes it over, so typing with overlapping fingers still gives each its key.
void InputInjector::PressKeyboardWithMouse(int slot, uint8_t kind, POINT screen) {
    if (kind == TouchDown) {
        if (keyboardPress_ >= 0) SendMouse(MOUSEEVENTF_LEFTUP, screen);
        SendMouse(MOUSEEVENTF_LEFTDOWN, screen);
        keyboardPress_ = slot;
        buttonsDown_ |= 1; // ReleaseAll lifts it on disconnect
    } else if (keyboardPress_ == slot) {
        if (kind == TouchMove) {
            SendMouse(0, screen);
            return;
        }
        SendMouse(MOUSEEVENTF_LEFTUP, screen);
        keyboardPress_ = -1;
        buttonsDown_ &= ~1;
    }
}

void InputInjector::InjectTouchFrame() {
    std::array<POINTER_TYPE_INFO, 10> frame{};
    UINT32 used = 0;
    for (auto& c : contacts_)
        if (c.active && !c.onKeyboard) frame[used++] = c.info;
    if (used) Inject(touchDev_, frame.data(), used, "touch refresh");
    lastTouchInject_ = GetTickCount64();
}

void InputInjector::InjectPen(POINTER_FLAGS flags, POINTER_BUTTON_CHANGE_TYPE change) {
    pen_.penInfo.pointerInfo.pointerFlags = flags;
    pen_.penInfo.pointerInfo.ButtonChangeType = change;
    if (pen_.penInfo.penFlags & PEN_FLAG_BARREL) pen_.penInfo.pointerInfo.pointerFlags |= POINTER_FLAG_SECONDBUTTON;
    Inject(penDev_, &pen_, 1, "pen");
    penInRange_ = (flags & POINTER_FLAG_INRANGE) != 0;
    penInContact_ = (flags & POINTER_FLAG_INCONTACT) != 0;
}

void InputInjector::OnPen(const uint8_t* p, size_t n) {
    if (n < 10) return;
    std::lock_guard lock(mutex_);
    uint8_t kind = p[0], buttons = p[1];
    auto& pi = pen_.penInfo;
    pi.pointerInfo.ptPixelLocation = ToVirtualScreen(Map(rd16(p + 2), rd16(p + 4)));
    pi.pressure = std::min<uint32_t>(rd16(p + 6), kPressureMax);
    pi.tiltX = std::clamp<INT32>(int8_t(p[8]), -90, 90);
    pi.tiltY = std::clamp<INT32>(int8_t(p[9]), -90, 90);
    pi.penFlags = PEN_FLAG_NONE;
    if (buttons & PenBarrel) pi.penFlags |= PEN_FLAG_BARREL;
    if (buttons & PenEraser) pi.penFlags |= (kind == PenDown || kind == PenMove) ? PEN_FLAG_ERASER : PEN_FLAG_INVERTED;

    const POINTER_FLAGS hover = POINTER_FLAG_UPDATE | POINTER_FLAG_INRANGE;
    const POINTER_FLAGS contact = POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT | POINTER_FLAG_FIRSTBUTTON;
    switch (kind) {
    case PenHover:
        if (penInContact_) InjectPen(POINTER_FLAG_UP | POINTER_FLAG_INRANGE, POINTER_CHANGE_FIRSTBUTTON_UP);
        InjectPen(hover, POINTER_CHANGE_NONE);
        break;
    case PenDown:
    case PenMove:
        if (!penInContact_) {
            if (!penInRange_) InjectPen(hover, POINTER_CHANGE_NONE);
            pi.pressure = std::min<uint32_t>(rd16(p + 6), kPressureMax);
            InjectPen(POINTER_FLAG_DOWN | contact, POINTER_CHANGE_FIRSTBUTTON_DOWN);
        } else {
            InjectPen(POINTER_FLAG_UPDATE | contact, POINTER_CHANGE_NONE);
        }
        break;
    case PenUp:
        if (penInContact_) {
            pi.pressure = 0;
            InjectPen(POINTER_FLAG_UP | POINTER_FLAG_INRANGE, POINTER_CHANGE_FIRSTBUTTON_UP);
        }
        break;
    case PenLeave:
        if (penInContact_) {
            pi.pressure = 0;
            InjectPen(POINTER_FLAG_UP | POINTER_FLAG_INRANGE, POINTER_CHANGE_FIRSTBUTTON_UP);
        }
        if (penInRange_) InjectPen(POINTER_FLAG_UPDATE, POINTER_CHANGE_NONE);
        break;
    }
}

void InputInjector::SendMouse(DWORD flags, POINT pt, DWORD data) {
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = std::max(2, GetSystemMetrics(SM_CXVIRTUALSCREEN)), vh = std::max(2, GetSystemMetrics(SM_CYVIRTUALSCREEN));
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dx = LONG((int64_t(pt.x - vx) * 65535 + (vw - 1) / 2) / (vw - 1));
    in.mi.dy = LONG((int64_t(pt.y - vy) * 65535 + (vh - 1) / 2) / (vh - 1));
    in.mi.dwFlags = flags | MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    in.mi.mouseData = data;
    const UINT sent = SendInput(1, &in, sizeof(in));
    NoteInjectResult(sent != 0, GetLastError());
}

void InputInjector::SendMouseRel(DWORD flags, LONG dx, LONG dy, DWORD data) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dx = dx;
    in.mi.dy = dy;
    in.mi.dwFlags = flags;
    in.mi.mouseData = data;
    const UINT sent = SendInput(1, &in, sizeof(in));
    NoteInjectResult(sent != 0, GetLastError());
}

struct MouseButton {
    DWORD down, up, data;
};
constexpr MouseButton kButtons[] = {
    {MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP, 0},     {MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP, 0},
    {MOUSEEVENTF_MIDDLEDOWN, MOUSEEVENTF_MIDDLEUP, 0}, {MOUSEEVENTF_XDOWN, MOUSEEVENTF_XUP, XBUTTON1},
    {MOUSEEVENTF_XDOWN, MOUSEEVENTF_XUP, XBUTTON2},
};

// Maps a button MouseKind to its index in kButtons. The kinds come in down/up pairs.
static bool ButtonOf(uint8_t kind, int& button, bool& down) {
    int first = 0;
    if (kind >= MouseLeftDown && kind <= MouseRightUp) first = MouseLeftDown, button = 0;
    else if (kind >= MouseMiddleDown && kind <= MouseX2Up) first = MouseMiddleDown, button = 2;
    else return false;
    button += (kind - first) / 2;
    down = (kind - first) % 2 == 0;
    return true;
}

void InputInjector::OnMouse(const uint8_t* p, size_t n) {
    if (n < 9) return;
    std::lock_guard lock(mutex_);
    POINT pt = Map(rd16(p + 1), rd16(p + 3));
    int16_t wheelV = int16_t(rd16(p + 5)), wheelH = int16_t(rd16(p + 7));
    int button = 0;
    bool down = false;
    if (ButtonOf(p[0], button, down)) {
        SendMouse(down ? kButtons[button].down : kButtons[button].up, pt, kButtons[button].data);
        buttonsDown_ = uint8_t(down ? buttonsDown_ | (1 << button) : buttonsDown_ & ~(1 << button));
        // Left-button press: offer it to the UAC click path. Windows does not refuse injection into a
        // consent prompt (it silently drops it, no ERROR_ACCESS_DENIED), so this cannot key off the
        // inject result; the client ignores it unless a prompt is actually up and the injector re-checks
        // the window under the point, so a normal press is a cheap atomic read and nothing more.
        if (button == 0 && down && uacForward_) uacForward_({UacForwardEvent::Click, pt.x, pt.y});
    } else if (p[0] == MouseMove) {
        SendMouse(0, pt);
        if (uacForward_) uacForward_({UacForwardEvent::MouseMove, pt.x, pt.y}); // dropped unless the sign-in screen is up
    } else if (p[0] == MouseWheel) {
        if (wheelV) SendMouse(MOUSEEVENTF_WHEEL, pt, DWORD(int32_t(wheelV)));
        if (wheelH) SendMouse(MOUSEEVENTF_HWHEEL, pt, DWORD(int32_t(wheelH)));
    }
}

void InputInjector::OnMouseRel(const uint8_t* p, size_t n) {
    if (n < 9) return;
    std::lock_guard lock(mutex_);
    int16_t dx = int16_t(rd16(p + 1)), dy = int16_t(rd16(p + 3));
    int16_t wheelV = int16_t(rd16(p + 5)), wheelH = int16_t(rd16(p + 7));
    int button = 0;
    bool down = false;
    if (ButtonOf(p[0], button, down)) {
        SendMouseRel(down ? kButtons[button].down : kButtons[button].up, 0, 0, kButtons[button].data);
        buttonsDown_ = uint8_t(down ? buttonsDown_ | (1 << button) : buttonsDown_ & ~(1 << button));
    } else if (p[0] == MouseMove) {
        if (dx || dy) SendMouseRel(MOUSEEVENTF_MOVE, dx, dy);
    } else if (p[0] == MouseWheel) {
        if (wheelV) SendMouseRel(MOUSEEVENTF_WHEEL, 0, 0, DWORD(int32_t(wheelV)));
        if (wheelH) SendMouseRel(MOUSEEVENTF_HWHEEL, 0, 0, DWORD(int32_t(wheelH)));
    }
}

// Scancodes, not virtual keys: the PC's own keyboard layout applies, as for a keyboard plugged into it.
void InputInjector::SendKey(uint16_t code, bool down) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wScan = WORD(code & 0xFF);
    if (code == kKeyPause) {
        in.ki.wVk = VK_PAUSE; // its real scancode has an E1 prefix, which SendInput cannot express
    } else {
        in.ki.dwFlags = KEYEVENTF_SCANCODE | (code & kKeyExtended ? KEYEVENTF_EXTENDEDKEY : 0);
    }
    if (!down) in.ki.dwFlags |= KEYEVENTF_KEYUP;
    const UINT sent = SendInput(1, &in, sizeof(in));
    NoteInjectResult(sent != 0, GetLastError());
    keysDown_[code] = down;
}

void InputInjector::OnKey(const uint8_t* p, size_t n) {
    if (n < 3) return;
    uint16_t code = rd16(p + 1);
    if (code >= keysDown_.size() || (code & 0xFF) == 0 || (code & 0xFF) >= 0x80) return; // not a make code
    std::lock_guard lock(mutex_);
    const bool down = (p[0] & KeyDown) != 0;
    SendKey(code, down);
    if (uacForward_) {
        // Enter / Esc / Tab / arrows: offer to the UAC prompt (dropped unless one is up). Not gated on the
        // inject result, since injection into a consent prompt returns success but is dropped.
        if (down) { WORD vk = UacVkFromWire(code); if (vk) uacForward_({UacForwardEvent::KeyVk, 0, 0, vk}); }
        // Every key, down and up: offer to the lock / sign-in screen as a full scancode so the PC's own
        // layout and modifiers apply (typing the password). Dropped on the helper side unless the Windows
        // sign-in screen is up, so a normal keypress costs only a cheap atomic read there.
        uacForward_({UacForwardEvent::KeyScan, 0, 0, 0, code, down});
    }
}

void InputInjector::Tick() {
    std::lock_guard lock(mutex_);
    if (GetTickCount64() - lastTouchInject_ < 100) return;
    if (std::any_of(contacts_.begin(), contacts_.end(), [](auto& c) { return c.active; })) InjectTouchFrame();
}

void InputInjector::ReleaseAll() {
    std::lock_guard lock(mutex_);
    std::array<POINTER_TYPE_INFO, 10> frame{};
    UINT32 used = 0;
    for (auto& c : contacts_) {
        if (c.active && !c.onKeyboard) {
            frame[used] = c.info;
            frame[used++].touchInfo.pointerInfo.pointerFlags = POINTER_FLAG_UP | POINTER_FLAG_CANCELED;
        }
        c.active = false;
    }
    keyboardPress_ = -1; // its button is lifted with the others below
    if (used) Inject(touchDev_, frame.data(), used, "touch release");
    if (penInContact_) InjectPen(POINTER_FLAG_UP | POINTER_FLAG_INRANGE, POINTER_CHANGE_FIRSTBUTTON_UP);
    if (penInRange_) InjectPen(POINTER_FLAG_UPDATE, POINTER_CHANGE_NONE);
    for (int b = 0; b < int(std::size(kButtons)); ++b)
        if (buttonsDown_ & (1 << b)) SendMouseRel(kButtons[b].up, 0, 0, kButtons[b].data);
    buttonsDown_ = 0;
    for (size_t code = 0; code < keysDown_.size(); ++code)
        if (keysDown_[code]) SendKey(uint16_t(code), false);
}

} // namespace pd
