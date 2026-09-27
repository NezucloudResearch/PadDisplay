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

// Logs every display device and DXGI output (for --list and diagnostics).
void LogDisplays();

} // namespace pd
