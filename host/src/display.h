// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
// Locating, attaching and configuring the virtual (IddCx) monitor.
#pragma once
#include <windows.h>
#include <string>
#include <vector>

namespace pd {

struct VirtualDisplay {
    std::wstring gdiName;     // \\.\DISPLAYn
    std::wstring adapterName; // e.g. "Virtual Display Driver"
    bool attached = false;
};

struct DisplayMode { int width, height, hz; };

// Finds the virtual display's GDI source. overrideName forces a specific \\.\DISPLAYn.
bool FindVirtualDisplay(const std::wstring& overrideName, VirtualDisplay& out);

std::vector<DisplayMode> ListModes(const std::wstring& gdiName);

// The user's own setup of the virtual monitor (position, resolution, refresh), remembered per tablet
// (keyed by the tablet's screen size) so a reconnect restores it instead of the defaults.
struct VirtualPlacement {
    int x = 0, y = 0;
    DisplayMode mode{};
};
bool CurrentPlacement(const std::wstring& gdiName, VirtualPlacement& out); // false if not attached
void SaveVirtualPlacement(int tabletW, int tabletH, const VirtualPlacement& p);
bool LoadVirtualPlacement(int tabletW, int tabletH, VirtualPlacement& out);

// Makes sure the virtual monitor is on the desktop.
// - saved (the user's setup for this tablet, recorded when their last session ended): put it at that
//   position and resolution, also if Windows already attached it somewhere else. Only the refresh
//   rate follows want.hz (the tablet app's explicit 60/120 Hz choice) when that mode exists.
// - no saved setup (first connection): the tablet's resolution if matchResolution, placed to the
//   right of the existing monitors.
bool EnsureAttached(const std::wstring& gdiName, DisplayMode want, bool matchResolution, const VirtualPlacement* saved,
                    DisplayMode& active);

void Detach(const std::wstring& gdiName);

// Where the real (non-virtual) displays are. When the virtual monitor appears or disappears,
// Windows re-applies a stored layout for the new set of monitors, which can switch a real screen
// off or move it; RestoreRealDisplays puts them back and saves that as the layout for this set.
struct DisplayPlacement {
    std::wstring gdiName;
    int x = 0, y = 0, width = 0, height = 0, hz = 0;
};
std::vector<DisplayPlacement> SnapshotRealDisplays();
int RestoreRealDisplays(const std::vector<DisplayPlacement>& saved); // returns how many were fixed

// "Home layout": the user's arrangement of real screens. Recorded only while the virtual monitor is
// not on the desktop (at start-up, before a session attaches it), because Windows shifts real
// screens when the virtual one is added or removed.
void SaveHomeLayout(const std::vector<DisplayPlacement>& layout);
std::vector<DisplayPlacement> LoadHomeLayout();
// Puts the real screens back to the home layout (waits briefly for Windows to settle first).
int RestoreHomeLayout();

// "Only screen" mode: the virtual monitor becomes the PC's only active display, so the real screens
// go dark. It is applied without saving it in Windows' display database: after a crash or a
// restart, Windows returns to the stored setup (the real screens) by itself.
bool ShowOnlyOn(const std::wstring& gdiName);
// Back to the stored setup. true = the real screens are on (then restore the home layout). false = none
// came back, usually because a security prompt (UAC) is up: it keeps trying in the background and
// restores the home layout itself once they are on.
bool ShowOnAllAgain();
// A new session wants the screens its own way: stop that background attempt.
void CancelScreenRestore();
// At exit: give a running background attempt a moment to finish.
void WaitScreenRestore(unsigned timeoutMs);

// Only-screen mode + an elevated host: optionally move UAC prompts off the (uncapturable) secure
// desktop onto the normal desktop, so the tablet can show and click them. This is a machine-wide
// Windows security setting (see SECURITY.md), so it is applied only while a tablet is the PC's only
// screen and always put back. The previous value is kept in a marker file, so a crash cannot leave
// the secure desktop disabled: RestoreSecureDesktop (also called at start-up) recovers it.
bool AllowUacClicksOnTablet(); // true = UAC prompts are now on the normal desktop (or already were)
void RestoreSecureDesktop();   // undo AllowUacClicksOnTablet; a no-op if nothing was changed

// Logs every display device and DXGI output (for --list and diagnostics).
void LogDisplays();

} // namespace pd
