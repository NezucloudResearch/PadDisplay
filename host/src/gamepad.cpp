// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#include "gamepad.h"
#include "log.h"
#include "vdd.h"

#include <ViGEm/Client.h>

#include <cstring>

namespace pd {

GamepadBridge::GamepadBridge(RumbleFn rumble, NoticeFn notice) : rumble_(std::move(rumble)), notice_(std::move(notice)) {
    worker_ = std::thread([this] { Run(); });
}

GamepadBridge::~GamepadBridge() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    cv_.notify_one();
    worker_.join(); // unplugs the controllers on its way out
    if (client_) {
        vigem_disconnect(client_);
        vigem_free(client_);
    }
}

void GamepadBridge::OnState(const uint8_t* p, size_t n) {
    if (n < 14 || p[0] >= kMaxGamepads) return;
    {
        std::lock_guard lock(mutex_);
        Mail& m = mail_[p[0]];
        m.pending = true;
        m.connected = (p[1] & GamepadConnected) != 0;
        memcpy(m.report.data(), p + 2, m.report.size());
    }
    cv_.notify_one();
}

void GamepadBridge::Run() {
    std::array<Mail, kMaxGamepads> work; // taken from mail_; stays pending while the driver is unusable
    for (;;) {
        ULONGLONG now = GetTickCount64();
        bool settling = false, retrying = false;
        for (int i = 0; i < kMaxGamepads; ++i) {
            settling |= pads_[i].target && now < pads_[i].settleUntil;
            retrying |= work[i].pending;
        }
        {
            std::unique_lock lock(mutex_);
            auto ready = [&] {
                if (stop_) return true;
                for (auto& m : mail_)
                    if (m.pending) return true;
                return false;
            };
            if (settling) cv_.wait_for(lock, std::chrono::milliseconds(10), ready);
            else if (retrying) cv_.wait_for(lock, std::chrono::milliseconds(100), ready);
            else cv_.wait(lock, ready);
            if (stop_) break;
            for (int i = 0; i < kMaxGamepads; ++i) {
                if (!mail_[i].pending) continue;
                work[i] = mail_[i];
                mail_[i].pending = false;
            }
        }
        for (int i = 0; i < kMaxGamepads; ++i)
            if (work[i].pending && Apply(i, work[i])) work[i].pending = false;
        now = GetTickCount64();
        for (const Pad& pad : pads_) // a report sent while Windows starts a controller is dropped: repeat it
            if (pad.target && now < pad.settleUntil) Submit(pad);
    }
    for (int i = 0; i < kMaxGamepads; ++i) Remove(i);
}

bool GamepadBridge::Connect() {
    if (client_) return true;
    ULONGLONG now = GetTickCount64();
    if (now < retryAt_) return false;
    retryAt_ = now + 500;
    client_ = vigem_alloc();
    VIGEM_ERROR err = client_ ? vigem_connect(client_) : VIGEM_ERROR_BUS_NOT_FOUND;
    if (VIGEM_SUCCESS(err)) {
        LOGI("gamepad: connected to ViGEmBus");
        return true;
    }
    if (client_) vigem_free(client_);
    client_ = nullptr;
    if (reported_ || now < enableDeadline_) return false; // told already, or the driver is coming up

    VddState state = QueryVigem();
    // Installed but switched off: switch it on, as for the virtual monitor. The next attempts retry.
    if (state == VddState::Disabled && !enableDeadline_ && EnableVigem()) {
        enableDeadline_ = now + 5000;
        return false;
    }
    reported_ = true;
    LOGI("gamepad: ViGEmBus unavailable (0x%08x), driver %ls", unsigned(err), VddStateText(state));
    notice_(state == VddState::NotInstalled
                ? "Game controllers need the ViGEmBus driver on the PC. Right-click the PadDisplay tray icon and choose 'Get game controller driver'."
            : state == VddState::Ready
                ? "The game controller driver (ViGEmBus) on the PC did not respond. See the PadDisplay log."
                : "The game controller driver (ViGEmBus) is switched off on the PC. Right-click the PadDisplay tray icon and choose 'Enable game controller driver'.");
    return false;
}

bool GamepadBridge::Apply(int index, const Mail& mail) {
    Pad& pad = pads_[index];
    if (!mail.connected) {
        Remove(index);
        return true;
    }
    if (!Connect()) return false;
    if (!pad.target) {
        PVIGEM_TARGET target = vigem_target_x360_alloc();
        VIGEM_ERROR err = target ? vigem_target_add(client_, target) : VIGEM_ERROR_NO_FREE_SLOT;
        if (!VIGEM_SUCCESS(err)) {
            LOGI("gamepad: cannot add controller %d (0x%08x)", index + 1, unsigned(err));
            if (target) vigem_target_free(target);
            return true; // the next state change tries again
        }
        pad.target = target;
        // Each call waits for the next rumble change; it fails once the controller is removed.
        pad.rumble = std::thread([this, index, target] {
            XUSB_OUTPUT_DATA out{};
            while (VIGEM_SUCCESS(vigem_target_x360_get_output(client_, target, &out)))
                rumble_(uint8_t(index), out.LargeMotor, out.SmallMotor);
        });
        pad.settleUntil = GetTickCount64() + 1500;
        LOGI("gamepad: controller %d connected", index + 1);
    }
    pad.report = mail.report;
    Submit(pad);
    return true;
}

void GamepadBridge::Submit(const Pad& pad) {
    const uint8_t* p = pad.report.data();
    XUSB_REPORT report{};
    report.wButtons = rd16(p);
    report.bLeftTrigger = p[2];
    report.bRightTrigger = p[3];
    report.sThumbLX = SHORT(rd16(p + 4));
    report.sThumbLY = SHORT(rd16(p + 6));
    report.sThumbRX = SHORT(rd16(p + 8));
    report.sThumbRY = SHORT(rd16(p + 10));
    vigem_target_x360_update(client_, pad.target, report);
}

void GamepadBridge::Remove(int index) {
    Pad& pad = pads_[index];
    if (!pad.target) return;
    vigem_target_remove(client_, pad.target); // unplugs it, which also ends the rumble wait
    if (pad.rumble.joinable()) pad.rumble.join();
    vigem_target_free(pad.target);
    pad.target = nullptr;
    LOGI("gamepad: controller %d disconnected", index + 1);
}

} // namespace pd
