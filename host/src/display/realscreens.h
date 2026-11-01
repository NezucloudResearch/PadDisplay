// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
// The PC's own screens: remembering them, turning them off for only-screen mode, and bringing them back.
#pragma once
#include <string>
#include <vector>

namespace pd
{

    // Where the real (non-virtual) displays are.
    struct DisplayPlacement
    {
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
    bool ShowOnlyOn(const std::wstring &gdiName);
    // Back to the stored setup. true = the real screens are on (then RestoreRealScreens). false = none
    // came back, usually because a security prompt (UAC) is up: it keeps trying in the background and
    // puts the real screens back itself once they are on.
    bool ShowOnAllAgain();
    // A new session wants the screens its own way: stop that background attempt.
    void CancelScreenRestore();
    // At exit: give a running background attempt a moment to finish.
    void WaitScreenRestore(unsigned timeoutMs);

} // namespace pd
