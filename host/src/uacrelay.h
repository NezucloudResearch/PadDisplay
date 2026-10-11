// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
//
// Clicking the Windows UAC / consent prompt from the tablet (only-screen mode).
//
// The consent prompt (consent.exe) runs at System integrity, one level above an elevated (High)
// process, so neither PadDisplay nor any other High-level program can inject input into it. The only
// thing that can is a *System-integrity process in the user's interactive session*. This module gets
// one there and feeds it a tightly limited set of clicks:
//
//   PadDisplay (elevated, High)                      --uac-relay (SYSTEM, session 0)
//     registers + triggers the SYSTEM task  ───────▶   duplicates its token into the active console
//     connects to the injector's pipe                  session and starts --uac-inject there
//     forwards "click at (x,y)" / "key"    ◀────────▶ --uac-inject (SYSTEM, the user's session)
//                                                        validates the foreground is consent.exe,
//                                                        then injects the click; nothing else.
//
// Everything here is OFF unless the user turned on "Let the tablet answer UAC prompts" AND the host is
// elevated AND the tablet is the only screen. The injector forwards input ONLY while the UAC prompt is
// the foreground window, and only a left click or Enter / Esc / Tab / arrow keys - never arbitrary
// input. See SECURITY.md.
#pragma once
#include <windows.h>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace pd {

// Fixed 12-byte command frames on the pipe (no variable-length parsing).
enum class UacCmd : uint32_t { Click = 1, Key = 2 };
#pragma pack(push, 1)
struct UacFrame {
    uint32_t cmd; // UacCmd
    int32_t a;    // Click: screen x; Key: virtual-key code
    int32_t b;    // Click: screen y; Key: 0
};
#pragma pack(pop)

constexpr wchar_t kUacRelayTask[] = L"\\PadDisplay\\UacClickRelay";
constexpr wchar_t kUacPipeName[] = L"\\\\.\\pipe\\PadDisplayUacClick";

// --- used by the main (elevated) PadDisplay ------------------------------------------------------

// Registers the SYSTEM scheduled task that launches the relay. Needs administrator rights; a no-op
// that returns true if the task already exists. false + message on failure.
bool EnsureUacRelayTask(std::wstring& message);
void RemoveUacRelayTask(); // for uninstall; whoever registered it

// Client the session uses while a UAC prompt is up. Click()/Key() are called from the input thread
// and only enqueue; a private worker thread does the slow part (starting the relay task, waiting for
// the injector's pipe, writing) so the input thread never blocks. [notice] is called (from the worker)
// at most once if the helper cannot be reached, so the user is not left tapping a prompt that never
// responds. Safe to construct even when the feature is off; then it is simply never fed.
class UacClickClient {
public:
    explicit UacClickClient(std::function<void(const char*)> notice);
    ~UacClickClient();
    // [x],[y] in virtual-desktop pixels (the same screen coordinates InputInjector::Map returns).
    void Click(int x, int y);
    // [vk] is checked against the small allow-list on the injector side too.
    void Key(WORD vk);

private:
    void Worker();
    bool Ensure();          // task triggered + pipe connected (worker thread only)
    bool Send(const UacFrame& f);

    std::function<void(const char*)> notice_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<UacFrame> queue_;
    bool stop_ = false;
    bool warned_ = false;   // the "could not reach the helper" notice was sent
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    bool triedTask_ = false;
};

// --- process entry points (dispatched from wWinMain by command line) -----------------------------

int RunUacRelayBootstrap(); // --uac-relay : SYSTEM in session 0, starts the injector in the user's session
int RunUacInjector();       // --uac-inject: SYSTEM in the user's session, the pipe server that injects

} // namespace pd
