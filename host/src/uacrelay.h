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
//     a tap whose window is consent.exe  ───────────▶   duplicates its token into the active console
//     is forwarded as "click at (x,y)"                  session and starts --uac-inject there
//     connects to the injector's pipe      ◀────────▶ --uac-inject (SYSTEM, the user's session)
//                                                        re-checks the window under the point is
//                                                        consent.exe, then clicks it; nothing else.
//
// Everything here is OFF unless the user turned on "Let the tablet answer UAC prompts" AND the host is
// elevated AND the tablet is the only screen. A click is injected ONLY when the window directly under it
// is the consent prompt (consent.exe), a key ONLY while a consent prompt is open, and only a left click
// or Enter / Esc / Tab / Space / arrow keys - never arbitrary input. See SECURITY.md.
#pragma once
#include <windows.h>
#include <atomic>
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

// (Re)registers the SYSTEM scheduled task that launches the relay, overwriting any existing one so its
// action always points at the running exe. Needs administrator rights. false + message on failure.
bool EnsureUacRelayTask(std::wstring& message);
void RemoveUacRelayTask(); // for uninstall; whoever registered it

// Consent-prompt tests used by the client gate (and the injector). The elevated host can read the
// consent process; from a normal-rights process these return false. Cheap enough for a tap/key DOWN.
bool WindowAtPointIsConsent(int x, int y); // the top-level window under the screen point is consent.exe
bool ConsentPromptOpen();                  // some top-level consent.exe window exists right now

// Client the session uses while a UAC prompt is up. Click()/Key() are called from the input thread on a
// tap/allowed key. They do a cheap consent test (WindowAtPointIsConsent / ConsentPromptOpen) and return
// at once for a normal tap; only a tap that actually lands on a consent prompt is enqueued. A private
// worker thread then does the slow part (starting the relay task, waiting for the injector's pipe,
// writing) so the input thread never blocks. [notice] is called (from the worker) at most once if the
// helper cannot be reached. Safe to construct when the feature is off; then it is simply never fed.
class UacClickClient {
public:
    explicit UacClickClient(std::function<void(const char*)> notice);
    ~UacClickClient();
    // [x],[y] in virtual-desktop pixels (the same screen coordinates InputInjector::Map returns). Dropped
    // unless the window under the point is a consent prompt; the injector re-checks before it clicks.
    void Click(int x, int y);
    // [vk] is checked against the small allow-list on the injector side too. Dropped unless a prompt is up.
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
