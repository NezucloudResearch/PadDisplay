// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
// The Windows security settings that only-screen mode and the lock screen run into.
#pragma once

namespace pd
{

    // Only-screen mode + an elevated host: optionally move UAC prompts off the (uncapturable) secure
    // desktop onto the normal desktop, so the tablet can show and click them. This is a machine-wide
    // Windows security setting (see SECURITY.md), so it is applied only while a tablet is the PC's only
    // screen and always put back. The previous value is kept in a marker file, so a crash cannot leave
    // the secure desktop disabled: RestoreSecureDesktop (also called at start-up) recovers it.
    bool AllowUacClicksOnTablet(); // true = UAC prompts are now on the normal desktop (or already were)
    bool HostIsElevated();         // the PadDisplay process runs with administrator rights
    void RestoreSecureDesktop();   // undo AllowUacClicksOnTablet; a no-op if nothing was changed

    // Whether this interactive session is locked (Win+L, timeout, Ctrl+Alt+Del lock). The tray window
    // learns it from WM_WTSSESSION_CHANGE and reports it here. A locked session, unlike a UAC prompt,
    // moves the desktop to Winlogon: Desktop Duplication and injection are both refused to the host
    // there (E_ACCESSDENIED / ERROR_ACCESS_DENIED), and only a System helper on that desktop can reach
    // it. The session uses this to tell the tablet the truth ("the PC is locked") instead of blaming a
    // UAC prompt, and to decide whether to hand capture and input to that helper.
    void SetSessionLocked(bool locked); // called from the tray window proc only
    bool SessionIsLocked();

} // namespace pd
