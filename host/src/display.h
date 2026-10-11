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

// Where the real (non-virtual) displays are.
struct DisplayPlacement {
    std::wstring gdiName;
    int x = 0, y = 0, width = 0, height = 0, hz = 0;
};
std::vector<DisplayPlacement> SnapshotRealDisplays();

// The PC's own screens as the user arranged them. When the virtual monitor appears or disappears, Windows
// re-applies a layout it stored for the new set of monitors, which can move a real screen or switch it off.
// So PadDisplay remembers the real screens while the virtual monitor is not on the desktop (right before it
// switches it on), and puts them back afterwards.
// - For this run it is Windows' own description of the screens (position, main display, resolution, refresh
//   rate, rotation), put back with one SetDisplayConfig: exact, and one mode switch instead of one per screen.
// - For the next start it is home-layout.txt (position and mode per \\.\DISPLAYn). A file can be out of date
//   (screens re-arranged or re-numbered since), so it is used only to bring back a screen that is off, or when
//   Windows no longer accepts what this run remembered.
void RememberRealScreens();
std::vector<DisplayPlacement> LoadHomeLayout(); // what the file holds; empty = nothing remembered yet
// Puts the real screens back as remembered and leaves the virtual monitor where it is. It watches for 1.5 s,
// because Windows applies its own layout a moment after a monitor came or went. Returns how often it had to.
int RestoreRealScreens();
// The end of only-screen mode: the real screens on as remembered and the virtual monitor off the desktop, in
// one step. false = nothing is remembered from this run or Windows refused (then use ShowOnAllAgain).
bool ShowRealScreensOnly();

// "Only screen" mode: the virtual monitor becomes the PC's only active display, so the real screens
// go dark. It is applied without saving it in Windows' display database: after a crash or a
// restart, Windows returns to the stored setup (the real screens) by itself.
bool ShowOnlyOn(const std::wstring& gdiName);
// Back to the stored setup. true = the real screens are on (then RestoreRealScreens). false = none
// came back, usually because a security prompt (UAC) is up: it keeps trying in the background and
// puts the real screens back itself once they are on.
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
