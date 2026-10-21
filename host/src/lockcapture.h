// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
// Host side of lock-screen capture (piece 3). When the PC is locked, Desktop Duplication in this
// (user) process is refused the Winlogon desktop, so the System helper (uacrelay.*) captures it and
// writes each frame into a shared keyed-mutex texture this class owns. The session then copies that
// texture into the encoder's input and streams it like any other frame. Only used while locked, so it
// never touches the normal capture hot path.
#pragma once
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <string>

namespace pd {

using Microsoft::WRL::ComPtr;

class LockCaptureReceiver {
public:
    ~LockCaptureReceiver();
    // Create the shared texture ([width]x[height], B8G8R8A8) and the frame event, both named (kLockTexName /
    // kLockFrameEvent) with a SYSTEM+administrators-only descriptor so the helper can open them. The texture
    // is placed on the GPU that renders the lock screen (see the .cpp). [excludeGdi], when set, is the virtual
    // monitor's GDI name (\\.\DISPLAYn): the lock screen is captured from a REAL display, never that virtual
    // one, because the sign-in screen moves to the real laptop screen when a tablet is the only screen and
    // indirect-display adapters cannot share a capture texture. false if it could not be set up.
    bool Start(ID3D11Device* device, int width, int height, const std::wstring& excludeGdi = L"");
    void Stop();
    bool Active() const { return shared_ != nullptr; }
    LUID AdapterLuid() const { return luid_; }
    int Width() const { return width_; }
    int Height() const { return height_; }

    // Wait up to [timeoutMs] for the helper to signal a fresh frame; if one came, copy the shared texture
    // into [dst] (the encoder input, same size). Returns true when [dst] was updated. Runs on the session
    // thread only. When the lock screen is rendered by a different GPU than the encoder, the frame is
    // bridged across adapters here (see the .cpp); otherwise it is a direct GPU copy.
    bool Next(ID3D11Device* device, unsigned timeoutMs, ID3D11Texture2D* dst, bool& fresh);

private:
    // The shared texture lives on the adapter that RENDERS the lock screen (the one owning the primary
    // output), because the SYSTEM helper must DuplicateOutput that output on the same adapter it writes to.
    // On a hybrid PC that is the display GPU, not the encode GPU; on a single-GPU PC they are the same.
    ComPtr<ID3D11Texture2D> shared_;
    ComPtr<IDXGIKeyedMutex> mutex_;
    ComPtr<ID3D11Device> lockDevice_;        // device on the lock-screen (primary output) adapter; owns shared_
    ComPtr<ID3D11DeviceContext> lockCtx_;
    bool bridge_ = false;                     // true when the lock-screen GPU differs from the encoder GPU
    ComPtr<ID3D11Texture2D> lockStaging_;     // CPU-readable copy on the lock-screen GPU (bridge only)
    ComPtr<ID3D11Texture2D> encStaging_;      // CPU-writable copy on the encoder GPU (bridge only)
    HANDLE frameEvent_ = nullptr;
    LUID luid_{};                             // the lock-screen adapter LUID, handed to the helper
    int width_ = 0, height_ = 0;
};

} // namespace pd
