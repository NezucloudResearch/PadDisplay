// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#include "uacrelay.h"
#include "display.h"  // SessionIsLocked
#include "protocol.h" // kKeyExtended, kKeyPause
#include "log.h"
#include "net.h"      // RunCapture
#include "setup.h"    // OnlyAdminsCanChange
#include "vdd.h"      // CreateTaskFromXml

#include <windows.h>
#include <wtsapi32.h>
#include <sddl.h>
#include <userenv.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <thread>

namespace pd {

using Microsoft::WRL::ComPtr;

namespace {

// The relay and injector run as SYSTEM, so their AppDataDir() is the SYSTEM profile (admin-only and
// confusing). Log instead under %ProgramData%\PadDisplay, which users can read.
std::wstring UacLogPath() {
    wchar_t pd[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"ProgramData", pd, MAX_PATH);
    std::wstring dir = (n > 0 && n < MAX_PATH) ? std::wstring(pd) : std::wstring(L"C:\\ProgramData");
    dir += L"\\PadDisplay";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\uac-relay.log";
}

// What WindowImage returns for a process the caller may not open (a System process such as consent.exe
// is closed even to an elevated administrator).
const wchar_t* const kOpenDenied = L"<open-denied>";

// The executable basename of [win]'s process ("<...>" markers if it cannot be read). Logged on an
// ignored command so a window that is not consent.exe (a differently named secure UI) can be spotted.
std::wstring WindowImage(HWND win) {
    if (!win) return L"<none>";
    DWORD pid = 0;
    GetWindowThreadProcessId(win, &pid);
    if (!pid) return L"<no-pid>";
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return kOpenDenied; // the System helper can open consent.exe; the elevated host cannot
    wchar_t path[MAX_PATH] = {};
    DWORD n = MAX_PATH;
    bool ok = QueryFullProcessImageNameW(proc, 0, path, &n) != 0;
    CloseHandle(proc);
    if (!ok) return L"<query-failed>";
    const wchar_t* slash = wcsrchr(path, L'\\');
    return slash ? slash + 1 : path;
}

bool IsConsent(HWND win) { return _wcsicmp(WindowImage(win).c_str(), L"consent.exe") == 0; }

// The Windows lock-screen curtain (LockApp) sits on the normal desktop; a tap or key there is what dismisses
// it and hands off to the secure sign-in desktop (Winlogon), where the PIN/password field lives. Allowing
// input onto LockApp - and nothing else on that desktop - lets the tablet get past the curtain to the field
// without ever reaching the user's own apps behind the lock.
bool IsLockScreenAt(int x, int y) {
    HWND root = GetAncestor(WindowFromPoint(POINT{x, y}), GA_ROOT);
    return _wcsicmp(WindowImage(root).c_str(), L"LockApp.exe") == 0;
}

// The top-level consent window under the screen point, or null. The prompt opens in the background and
// is created full-screen for an instant before it shrinks, so checking the window under the exact point
// (not the foreground window, which is still the app behind it) is what tells us the tap hit the prompt.
HWND ConsentRootAt(int x, int y) {
    HWND w = WindowFromPoint(POINT{x, y});
    if (!w) return nullptr;
    HWND root = GetAncestor(w, GA_ROOT);
    return IsConsent(root ? root : w) ? (root ? root : w) : nullptr;
}

BOOL CALLBACK FindConsentProc(HWND w, LPARAM lp) {
    if (IsWindowVisible(w) && IsConsent(w)) { *reinterpret_cast<HWND*>(lp) = w; return FALSE; }
    return TRUE;
}

// A visible top-level consent.exe window, or null. By process, not window class, so it does not depend
// on a class name that can differ between Windows builds.
HWND FindConsentWindow() {
    HWND found = nullptr;
    EnumWindows(FindConsentProc, reinterpret_cast<LPARAM>(&found));
    return found;
}

// Only these keys are forwarded to a UAC prompt. Anything else is dropped, so the channel cannot be
// used to type arbitrary input as System.
bool KeyAllowed(WORD vk) {
    switch (vk) {
    case VK_RETURN:
    case VK_ESCAPE:
    case VK_TAB:
    case VK_LEFT:
    case VK_RIGHT:
    case VK_UP:
    case VK_DOWN:
    case VK_SPACE:
        return true;
    default:
        return false;
    }
}

// Bring the consent window forward before injecting, so the first click presses the button instead of
// only activating a background window. As SYSTEM in the session this is permitted.
void Activate(HWND consent) {
    if (!consent) return;
    SetForegroundWindow(consent);
    SetActiveWindow(consent);
    Sleep(30); // let the activation take effect before the click
}

void InjectClick(int x, int y) {
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = (std::max)(2, GetSystemMetrics(SM_CXVIRTUALSCREEN)), vh = (std::max)(2, GetSystemMetrics(SM_CYVIRTUALSCREEN));
    std::array<INPUT, 3> in{};
    const LONG nx = LONG((int64_t(x - vx) * 65535 + (vw - 1) / 2) / (vw - 1));
    const LONG ny = LONG((int64_t(y - vy) * 65535 + (vh - 1) / 2) / (vh - 1));
    for (auto& i : in) {
        i.type = INPUT_MOUSE;
        i.mi.dx = nx;
        i.mi.dy = ny;
        i.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    }
    in[1].mi.dwFlags |= MOUSEEVENTF_LEFTDOWN;
    in[2].mi.dwFlags |= MOUSEEVENTF_LEFTUP;
    SendInput(UINT(in.size()), in.data(), sizeof(INPUT));
}

void InjectKey(WORD vk) {
    INPUT in[2] = {};
    in[0].type = in[1].type = INPUT_KEYBOARD;
    in[0].ki.wVk = in[1].ki.wVk = vk;
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
}

// One key, down or up, as a scancode so the PC's own layout applies - the same encoding the host's
// InputInjector::SendKey uses. For typing the password into the sign-in screen.
void InjectKeyScan(uint16_t code, bool down) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    if (code == kKeyPause) {
        in.ki.wVk = VK_PAUSE; // its real scancode has an E1 prefix SendInput cannot express
    } else {
        in.ki.wScan = WORD(code & 0xFF);
        in.ki.dwFlags = KEYEVENTF_SCANCODE | ((code & kKeyExtended) ? KEYEVENTF_EXTENDEDKEY : 0);
    }
    if (!down) in.ki.dwFlags |= KEYEVENTF_KEYUP;
    SendInput(1, &in, sizeof(in));
}

// One character typed as Unicode code unit(s), independent of the PC's keyboard layout (the tablet does not
// know that layout, and a password may hold any character). SendInput delivers it to the focused control as
// a WM_CHAR. A character outside the BMP is two UTF-16 units (a surrogate pair): send both in one SendInput
// call (lo != 0) so they stay adjacent and compose into the one character; sending them as separate calls
// would deliver two lone surrogates. Used only for typing the password into the sign-in screen; never logged.
void InjectUnicode(wchar_t hi, wchar_t lo) {
    INPUT in[4] = {};
    int n = 0;
    for (int up = 0; up < 2; ++up) { // all downs first, then all ups, keeping the pair together
        const DWORD flags = KEYEVENTF_UNICODE | (up ? KEYEVENTF_KEYUP : 0);
        in[n].type = INPUT_KEYBOARD; in[n].ki.wScan = hi; in[n].ki.dwFlags = flags; ++n;
        if (lo) { in[n].type = INPUT_KEYBOARD; in[n].ki.wScan = lo; in[n].ki.dwFlags = flags; ++n; }
    }
    SendInput(n, in, sizeof(INPUT));
}

void InjectMove(int x, int y) {
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = (std::max)(2, GetSystemMetrics(SM_CXVIRTUALSCREEN)), vh = (std::max)(2, GetSystemMetrics(SM_CYVIRTUALSCREEN));
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dx = LONG((int64_t(x - vx) * 65535 + (vw - 1) / 2) / (vw - 1));
    in.mi.dy = LONG((int64_t(y - vy) * 65535 + (vh - 1) / 2) / (vh - 1));
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    SendInput(1, &in, sizeof(in));
}

std::wstring DesktopName(HDESK d) {
    wchar_t name[128] = {};
    DWORD need = 0;
    if (d && GetUserObjectInformationW(d, UOI_NAME, name, sizeof(name), &need)) return name;
    return L"";
}

// SendInput reaches only the desktop the calling thread is attached to. The input desktop switches to
// Winlogon when the PC locks or a UAC prompt appears and back to Default afterwards, so follow it before
// every injection (measured: without this, injection onto the locked sign-in screen fails with error 5).
// Returns true while the current input desktop is Winlogon (the lock / sign-in / secure desktop).
bool AttachToInputDesktop() {
    static HDESK current = nullptr;   // the desktop this thread is attached to (kept alive)
    static std::wstring currentName;  // its name, to notice a switch cheaply
    HDESK in = OpenInputDesktop(0, FALSE, GENERIC_ALL);
    if (!in) return _wcsicmp(currentName.c_str(), L"Winlogon") == 0; // transient: keep the last known
    std::wstring name = DesktopName(in);
    if (name != currentName) {
        if (SetThreadDesktop(in)) {
            if (current) CloseDesktop(current);
            current = in;
            currentName = name;
        } else {
            CloseDesktop(in); // could not switch (owns a window/hook): stay where we are
        }
    } else {
        CloseDesktop(in);
    }
    return _wcsicmp(currentName.c_str(), L"Winlogon") == 0;
}

// ---- lock-screen capture (producer side of piece 3) --------------------------------------------
// The injector is the one process that can both inject into and capture the Winlogon (sign-in) desktop.
// On request it captures that desktop with Desktop Duplication and copies each frame into the host's
// shared keyed-mutex texture (kLockTexName), signalling kLockFrameEvent, so the host can encode and
// stream it. It runs on its own thread and only while Winlogon is the input desktop.

std::atomic<bool> g_injExpired{false};
std::atomic<ULONGLONG> g_lastActive{0}; // command received or frame captured; the watchdog reads it
std::atomic<bool> g_capStop{false};
std::thread g_capThread;
LUID g_capLuid{};
std::atomic<int> g_capW{0}, g_capH{0};

// A full-screen scaling blit (letterbox). The sign-in (Winlogon) desktop can be a different resolution
// than the stream the tablet negotiated (measured: a VM lock screen at 1024x768 under a 1920x1080
// desktop), and CopyResource cannot resize, so the captured frame is drawn into the shared texture at
// its aspect ratio with black bars. Built once per capture session.
struct ScaledBlitter {
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11RenderTargetView> rtv;
    ID3D11Texture2D* rtvFor = nullptr;

    bool Init(ID3D11Device* dev) {
        static const char* kVs =
            "struct VOut{float4 p:SV_Position;float2 uv:TEXCOORD0;};"
            "VOut main(uint id:SV_VertexID){VOut o;float2 t=float2((id<<1)&2,id&2);o.uv=t;"
            "o.p=float4(t*float2(2,-2)+float2(-1,1),0,1);return o;}";
        static const char* kPs =
            "Texture2D tx:register(t0);SamplerState sm:register(s0);"
            "float4 main(float4 p:SV_Position,float2 uv:TEXCOORD0):SV_Target{return tx.Sample(sm,uv);}";
        ComPtr<ID3DBlob> vb, pb, err;
        if (FAILED(D3DCompile(kVs, strlen(kVs), nullptr, nullptr, nullptr, "main", "vs_4_0", 0, 0, &vb, &err)) ||
            FAILED(D3DCompile(kPs, strlen(kPs), nullptr, nullptr, nullptr, "main", "ps_4_0", 0, 0, &pb, &err)) ||
            FAILED(dev->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), nullptr, &vs)) ||
            FAILED(dev->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, &ps)))
            return false;
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        return SUCCEEDED(dev->CreateSamplerState(&sd, &sampler));
    }

    void Draw(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* srcSrv,
              int srcW, int srcH, ID3D11Texture2D* dst, int dstW, int dstH) {
        if (rtvFor != dst) { rtv.Reset(); if (SUCCEEDED(dev->CreateRenderTargetView(dst, nullptr, &rtv))) rtvFor = dst; }
        if (!rtv) return;
        const float black[4] = {0, 0, 0, 1};
        ctx->ClearRenderTargetView(rtv.Get(), black);
        float scale = (std::min)(float(dstW) / float(std::max(1, srcW)), float(dstH) / float(std::max(1, srcH)));
        D3D11_VIEWPORT vp{};
        vp.Width = srcW * scale;
        vp.Height = srcH * scale;
        vp.TopLeftX = (dstW - vp.Width) / 2;
        vp.TopLeftY = (dstH - vp.Height) / 2;
        vp.MaxDepth = 1;
        ID3D11RenderTargetView* rt = rtv.Get();
        ctx->OMSetRenderTargets(1, &rt, nullptr);
        ctx->RSSetViewports(1, &vp);
        ctx->VSSetShader(vs.Get(), nullptr, 0);
        ctx->PSSetShader(ps.Get(), nullptr, 0);
        ID3D11SamplerState* s = sampler.Get();
        ctx->PSSetSamplers(0, 1, &s);
        ctx->PSSetShaderResources(0, 1, &srcSrv);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->IASetInputLayout(nullptr);
        ctx->Draw(3, 0);
    }
};

ComPtr<ID3D11Device> CreateDeviceOnLuid(LUID luid) {
    ComPtr<IDXGIFactory1> f;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) return nullptr;
    ComPtr<IDXGIAdapter1> a;
    for (UINT i = 0; f->EnumAdapters1(i, &a) == S_OK; ++i) {
        DXGI_ADAPTER_DESC1 d{};
        a->GetDesc1(&d);
        if (d.AdapterLuid.LowPart == luid.LowPart && d.AdapterLuid.HighPart == luid.HighPart) {
            ComPtr<ID3D11Device> dev;
            if (SUCCEEDED(D3D11CreateDevice(a.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, nullptr)))
                return dev;
            return nullptr;
        }
    }
    return nullptr;
}

void LockCaptureLoop() {
    ComPtr<ID3D11Device> dev = CreateDeviceOnLuid(g_capLuid);
    if (!dev) { LOGI("uac-inject: lock capture - no device on the encode adapter %08lx:%08lx", g_capLuid.HighPart, g_capLuid.LowPart); return; }
    ComPtr<ID3D11DeviceContext> ctx;
    dev->GetImmediateContext(&ctx);
    ComPtr<ID3D11Device1> dev1;
    ComPtr<ID3D11Texture2D> shared;
    ComPtr<IDXGIKeyedMutex> mutex;
    if (FAILED(dev.As(&dev1)) ||
        FAILED(dev1->OpenSharedResourceByName(kLockTexName, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, IID_PPV_ARGS(&shared))) ||
        FAILED(shared.As(&mutex))) {
        LOGI("uac-inject: lock capture - could not open the shared texture");
        return;
    }
    HANDLE frameEvent = OpenEventW(EVENT_MODIFY_STATE, FALSE, kLockFrameEvent);
    if (!frameEvent) { LOGI("uac-inject: lock capture - could not open the frame event (%lu)", GetLastError()); return; }

    ScaledBlitter blit;
    if (!blit.Init(dev.Get())) { LOGI("uac-inject: lock capture - could not build the scaling blit"); return; }

    HDESK myDesk = nullptr;
    std::wstring myDeskName;
    ComPtr<IDXGIOutputDuplication> dupl;
    ComPtr<ID3D11Texture2D> interm; // SRV-bindable copy of the duplicated frame (frames are not SRV-bindable)
    ComPtr<ID3D11ShaderResourceView> intermSrv;
    int intermW = 0, intermH = 0;
    while (!g_capStop) {
        // Follow the input desktop; capture only while it is the sign-in (Winlogon) desktop.
        HDESK in = OpenInputDesktop(0, FALSE, GENERIC_ALL);
        std::wstring name = in ? DesktopName(in) : L"";
        const bool winlogon = _wcsicmp(name.c_str(), L"Winlogon") == 0;
        if (in && name != myDeskName) {
            if (SetThreadDesktop(in)) { if (myDesk) CloseDesktop(myDesk); myDesk = in; myDeskName = name; dupl.Reset(); }
            else CloseDesktop(in);
        } else if (in) {
            CloseDesktop(in);
        }
        if (!winlogon) { dupl.Reset(); Sleep(100); continue; }

        if (!dupl) {
            ComPtr<IDXGIDevice> dxgiDev;
            ComPtr<IDXGIAdapter> ad;
            if (FAILED(dev.As(&dxgiDev)) || FAILED(dxgiDev->GetAdapter(&ad))) { Sleep(200); continue; }
            // Capture the PRIMARY output (the one at the desktop origin): the sign-in screen (LogonUI) shows
            // there. Output 0 is NOT always the primary - on a multi-monitor PC the primary can be any index
            // (a real PC's sign-in screen was \\.\DISPLAY5), and grabbing the wrong monitor gave a black frame,
            // so the tablet saw no password field. Fall back to the first attached output if none claims (0,0).
            ComPtr<IDXGIOutput> out, first;
            for (UINT i = 0;; ++i) {
                ComPtr<IDXGIOutput> o;
                if (ad->EnumOutputs(i, &o) != S_OK) break;
                DXGI_OUTPUT_DESC od{};
                o->GetDesc(&od);
                if (!od.AttachedToDesktop) continue;
                if (!first) first = o;
                if (od.DesktopCoordinates.left == 0 && od.DesktopCoordinates.top == 0) { out = o; break; }
            }
            if (!out) out = first;
            ComPtr<IDXGIOutput1> out1;
            if (!out || FAILED(out.As(&out1)) || FAILED(out1->DuplicateOutput(dev.Get(), &dupl))) {
                Sleep(200); // not on the secure desktop yet, or no output: try again shortly
                continue;
            }
            DXGI_OUTPUT_DESC od{};
            out->GetDesc(&od);
            LOGI("uac-inject: lock capture on %ls (%ldx%ld at %ld,%ld)", od.DeviceName,
                 od.DesktopCoordinates.right - od.DesktopCoordinates.left, od.DesktopCoordinates.bottom - od.DesktopCoordinates.top,
                 od.DesktopCoordinates.left, od.DesktopCoordinates.top);
        }
        DXGI_OUTDUPL_FRAME_INFO fi{};
        ComPtr<IDXGIResource> res;
        HRESULT hr = dupl->AcquireNextFrame(100, &fi, &res);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;
        if (FAILED(hr)) { dupl.Reset(); Sleep(100); continue; }
        ComPtr<ID3D11Texture2D> frame;
        if (SUCCEEDED(res.As(&frame))) {
            D3D11_TEXTURE2D_DESC fd{};
            frame->GetDesc(&fd);
            // Keep an SRV-bindable copy of the (possibly differently sized) sign-in frame to sample from.
            if (int(fd.Width) != intermW || int(fd.Height) != intermH) {
                interm.Reset();
                intermSrv.Reset();
                D3D11_TEXTURE2D_DESC id = fd;
                id.Usage = D3D11_USAGE_DEFAULT;
                id.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                id.CPUAccessFlags = 0;
                id.MiscFlags = 0;
                if (SUCCEEDED(dev->CreateTexture2D(&id, nullptr, &interm)) &&
                    SUCCEEDED(dev->CreateShaderResourceView(interm.Get(), nullptr, &intermSrv))) {
                    intermW = int(fd.Width);
                    intermH = int(fd.Height);
                } else {
                    intermW = intermH = 0;
                }
            }
            if (interm && intermSrv && mutex->AcquireSync(0, 100) == S_OK) {
                ctx->CopyResource(interm.Get(), frame.Get()); // frame -> SRV-bindable copy (same size)
                blit.Draw(dev.Get(), ctx.Get(), intermSrv.Get(), intermW, intermH, shared.Get(), g_capW.load(), g_capH.load());
                ctx->Flush();
                mutex->ReleaseSync(0);
                SetEvent(frameEvent);
                g_lastActive = GetTickCount64(); // keep the injector alive while capturing
            }
        }
        dupl->ReleaseFrame();
    }
    dupl.Reset();
    if (myDesk) CloseDesktop(myDesk);
    CloseHandle(frameEvent);
    LOGI("uac-inject: lock capture stopped");
}

void StartLockCapture() {
    if (g_capThread.joinable()) return; // already running
    g_capStop = false;
    g_capThread = std::thread(LockCaptureLoop);
    LOGI("uac-inject: lock capture started (%dx%d)", g_capW.load(), g_capH.load());
}

void StopLockCapture() {
    g_capStop = true;
    if (g_capThread.joinable()) g_capThread.join();
}

// A security descriptor that lets only SYSTEM and administrators touch the pipe. The client (the
// elevated PadDisplay) is an administrator; a normal-rights program cannot connect.
PSECURITY_DESCRIPTOR MakePipeSd() {
    PSECURITY_DESCRIPTOR sd = nullptr;
    // D: owner SYSTEM; allow generic all to SYSTEM (SY) and Administrators (BA). No one else.
    ConvertStringSecurityDescriptorToSecurityDescriptorW(
        L"D:(A;;GA;;;SY)(A;;GA;;;BA)", SDDL_REVISION_1, &sd, nullptr);
    return sd;
}

} // namespace

// ================= main (elevated) side ==========================================================

// Click gate (hot path): one window hit-test + a single process-name read on the window under the tap.
// consent.exe runs as System, and OpenProcess on it is denied to the elevated host (an administrator
// token, not System), so a window whose process cannot be opened counts as "maybe the prompt" and is
// forwarded. The System injector then decides by process (ConsentRootAt) and drops anything else, so
// this only costs one ignored pipe message for a tap on some other protected window.
bool WindowAtPointIsConsent(int x, int y, std::wstring* image) {
    HWND root = GetAncestor(WindowFromPoint(POINT{x, y}), GA_ROOT);
    const std::wstring name = WindowImage(root);
    if (image) *image = name;
    return root && (_wcsicmp(name.c_str(), L"consent.exe") == 0 || name == kOpenDenied);
}

// Key gate (hot path, used for the few allowed keys incl. auto-repeat): a cheap top-level class lookup,
// no per-window process reads. The class can differ between Windows builds, so keys may not reach the
// prompt where clicks (point + process, robust) still do; the injector re-validates by process anyway.
bool ConsentPromptOpen() { return FindWindowW(L"Credential Dialog Xaml Host", nullptr) != nullptr; }

bool EnsureUacRelayTask(std::wstring& message) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    // The task starts this exe as System and any signed-in user may start the task. If a normal program could
    // replace the exe (or put a DLL next to it), it could run its own code as System with one `schtasks /run`.
    // Same rule as the autostart task: only an exe, folder and parents that administrators alone can change
    // (Program Files, where the installer puts it) may be registered; a portable copy in the user's folders may not.
    if (!OnlyAdminsCanChange(exe)) {
        RemoveUacRelayTask(); // a task registered from here (or any path) earlier must not stay usable
        message = L"Answering UAC prompts from the tablet needs PadDisplay installed with its installer (Program Files): "
                  L"programs without administrator rights can change this copy.";
        LOGI("uac-relay: not registering the System task for %ls, because programs without administrator rights can change it", exe);
        return false;
    }
    // Always (re)register: CreateTaskFromXml uses schtasks /create /f, which overwrites any existing
    // task of this name, so the action always points at the exe running now - never a stale path.

    // SYSTEM principal (System integrity), startable on demand by interactive users (GRGX;;;IU), like
    // the virtual-monitor on/off tasks. Its action is fixed: this exe with --uac-relay and nothing else.
    std::wstring xml =
        L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n"
        L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n"
        L"  <RegistrationInfo>\r\n"
        L"    <Author>PadDisplay</Author>\r\n"
        L"    <Description>PadDisplay: start the System helper that lets the tablet click UAC prompts (only-screen mode).</Description>\r\n"
        L"    <SecurityDescriptor>D:(A;;FA;;;SY)(A;;FA;;;BA)(A;;GRGX;;;IU)</SecurityDescriptor>\r\n"
        L"  </RegistrationInfo>\r\n"
        L"  <Principals><Principal id=\"System\"><UserId>S-1-5-18</UserId><RunLevel>HighestAvailable</RunLevel></Principal></Principals>\r\n"
        L"  <Settings>\r\n"
        L"    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>\r\n"
        L"    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>\r\n"
        L"    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>\r\n"
        L"    <AllowStartOnDemand>true</AllowStartOnDemand>\r\n"
        L"    <Hidden>true</Hidden>\r\n"
        L"    <ExecutionTimeLimit>PT2M</ExecutionTimeLimit>\r\n"
        L"    <Priority>4</Priority>\r\n"
        L"  </Settings>\r\n"
        L"  <Actions Context=\"System\"><Exec>\r\n"
        // The Command element is the bare executable path - Task Scheduler must not see surrounding
        // quotes, or it looks for a file whose name literally contains them and the task fails to start.
        L"    <Command>" + XmlEscape(exe) + L"</Command>\r\n"
        L"    <Arguments>--uac-relay</Arguments>\r\n"
        L"  </Exec></Actions>\r\n"
        L"</Task>\r\n";
    const bool ok = CreateTaskFromXml(kUacRelayTask, xml, message);
    LOGI("uac-relay: task %s", ok ? "registered" : "FAILED to register");
    return ok;
}

void RemoveUacRelayTask() {
    DWORD code = 1;
    RunCapture(std::wstring(L"schtasks.exe /delete /f /tn \"") + kUacRelayTask + L"\"", 10000, &code);
}

UacClickClient::UacClickClient(std::function<void(const char*)> notice, bool allowLockScreen)
    : notice_(std::move(notice)), allowLockScreen_(allowLockScreen) {
    thread_ = std::thread([this] { Worker(); });
}

UacClickClient::~UacClickClient() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    if (pipe_ != INVALID_HANDLE_VALUE) CloseHandle(pipe_);
}

// Called on the input thread on a tap / allowed key. The consent test returns at once for a normal tap
// (the overwhelming case), so this costs a window hit-test and - only when a prompt is actually there -
// a process-name read; nothing is forwarded for ordinary input. Enqueue is the only thing passed to the
// worker; the slow task/pipe work is all on the worker. Tiny bound, drop oldest (human-paced clicks).
void UacClickClient::Enqueue(const UacFrame& f) {
    {
        std::lock_guard lock(mutex_);
        // Coalesce a run of cursor moves: a move only matters as the latest position, so replace a move
        // already at the back instead of letting move spam push clicks and keys out of the bounded queue.
        if (f.cmd == uint32_t(UacCmd::MouseMove) && !queue_.empty() && queue_.back().cmd == f.cmd)
            queue_.back() = f;
        else {
            if (queue_.size() >= 64) queue_.pop_front();
            queue_.push_back(f);
        }
    }
    cv_.notify_one();
}

void UacClickClient::Click(int x, int y) {
    // When lock-screen control is on and the session is locked, the sign-in screen is on the Winlogon
    // desktop, which the host cannot hit-test (WindowFromPoint only sees the host's own desktop), so
    // forward the click unconditionally; the injector gates by the input desktop. Otherwise this is the
    // UAC case: forward only when the point is on a consent prompt.
    if (!(allowLockScreen_ && SessionIsLocked())) {
        std::wstring img;
        if (!WindowAtPointIsConsent(x, y, &img)) {
            // Diagnostic: say why a tap was not forwarded, naming the window actually under it. Rate-limited
            // so a stream of normal taps cannot flood the log or cost the input thread.
            static ULONGLONG lastLog = 0;
            ULONGLONG now = GetTickCount64();
            if (now - lastLog > 500) { lastLog = now; LOGI("uac-relay: tap at %d,%d not forwarded - window there is %ls", x, y, img.c_str()); }
            return;
        }
    }
    Enqueue(UacFrame{uint32_t(UacCmd::Click), x, y});
}

void UacClickClient::Key(WORD vk) {
    if (!ConsentPromptOpen()) return;
    Enqueue(UacFrame{uint32_t(UacCmd::Key), int32_t(vk), 0});
}

void UacClickClient::KeyScan(uint16_t code, bool down) {
    if (!allowLockScreen_ || !SessionIsLocked()) return; // typing the password is a lock-screen-only capability
    Enqueue(UacFrame{uint32_t(UacCmd::KeyScan), int32_t(code), down ? 1 : 0});
}

void UacClickClient::TypeUnicode(const std::wstring& text, bool submit) {
    if (!allowLockScreen_) return; // typing the password is a lock-screen-only capability
    // Not gated on SessionIsLocked(): at the boot sign-in screen the host (a SYSTEM logon-host) gets no
    // session-lock notification, so that flag stays false. The injector's own "is the sign-in (Winlogon)
    // desktop the input desktop" check is the gate that keeps this safe, and it covers both the boot sign-in
    // screen and a mid-session lock. Push the whole run under one lock, bypassing Enqueue's drop-oldest cap:
    // a password is a single deliberate, length-capped (by the caller) burst, so no character may be dropped
    // or reordered. Drained at once by the worker, so it never grows the queue for long.
    {
        std::lock_guard lock(mutex_);
        for (size_t i = 0; i < text.size(); ++i) {
            wchar_t hi = text[i];
            int32_t lo = 0; // a non-BMP character is a high+low surrogate pair: carry both units in one frame
            if (hi >= 0xD800 && hi <= 0xDBFF && i + 1 < text.size() &&
                text[i + 1] >= 0xDC00 && text[i + 1] <= 0xDFFF)
                lo = int32_t(uint16_t(text[++i]));
            queue_.push_back(UacFrame{uint32_t(UacCmd::UnicodeChar), int32_t(uint16_t(hi)), lo});
        }
        if (submit) { // Enter = set-1 make code 0x1C (not extended); reuse the scancode path
            queue_.push_back(UacFrame{uint32_t(UacCmd::KeyScan), 0x1C, 1});
            queue_.push_back(UacFrame{uint32_t(UacCmd::KeyScan), 0x1C, 0});
        }
    }
    cv_.notify_one();
}

void UacClickClient::MouseMove(int x, int y) {
    if (!allowLockScreen_ || !SessionIsLocked()) return;
    Enqueue(UacFrame{uint32_t(UacCmd::MouseMove), x, y});
}

void UacClickClient::StartCapture(LUID luid, int w, int h) {
    Enqueue(UacFrame{uint32_t(UacCmd::CaptureLuid), int32_t(luid.LowPart), luid.HighPart});
    Enqueue(UacFrame{uint32_t(UacCmd::StartCapture), w, h});
}

void UacClickClient::StopCapture() {
    Enqueue(UacFrame{uint32_t(UacCmd::StopCapture), 0, 0});
}

void UacClickClient::Worker() {
    for (;;) {
        UacFrame f{};
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this] { return stop_ || !queue_.empty(); }); // purely event-driven: no idle poll
            if (stop_ && queue_.empty()) return;
            f = queue_.front();
            queue_.pop_front();
        }
        LOGI("uac-relay: forwarding %s to the injector", f.cmd == uint32_t(UacCmd::Click) ? "a click" : "a key");
        if (!Send(f) && !warned_) {
            warned_ = true;
            if (notice_) notice_("Could not reach the PC helper that clicks UAC prompts. Answer this prompt on the PC. "
                                 "(It is set up when the tablet is the only screen and PadDisplay runs as administrator.)");
        }
    }
}

bool UacClickClient::Ensure() {
    if (pipe_ != INVALID_HANDLE_VALUE) return true;
    if (!triedTask_) {
        triedTask_ = true;
        DWORD code = 1;
        RunCapture(std::wstring(L"schtasks.exe /run /tn \"") + kUacRelayTask + L"\"", 10000, &code);
        if (code != 0) { LOGI("uac-relay: could not start the task (%lu)", code); return false; }
    }
    // The relay spawns the injector, which creates the pipe: wait briefly for it to appear. On the
    // worker thread, so this wait never touches the input path.
    for (int i = 0; i < 30 && pipe_ == INVALID_HANDLE_VALUE; ++i) {
        if (WaitNamedPipeW(kUacPipeName, 100)) {
            HANDLE h = CreateFileW(kUacPipeName, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (h != INVALID_HANDLE_VALUE) { pipe_ = h; break; }
        }
        Sleep(100);
    }
    if (pipe_ == INVALID_HANDLE_VALUE) LOGI("uac-relay: injector pipe did not open");
    return pipe_ != INVALID_HANDLE_VALUE;
}

bool UacClickClient::Send(const UacFrame& f) {
    // Two tries: the injector self-exits after its 2-minute cap, so a later click finds it gone and the
    // first write fails; drop the dead pipe and re-launch transparently before giving up. Both the
    // re-trigger and the pipe wait run on this worker thread, never on the input thread.
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!Ensure()) return false;
        DWORD wrote = 0;
        if (WriteFile(pipe_, &f, sizeof(f), &wrote, nullptr) && wrote == sizeof(f)) {
            warned_ = false; // it is working
            return true;
        }
        CloseHandle(pipe_);
        pipe_ = INVALID_HANDLE_VALUE;
        triedTask_ = false; // next Ensure re-triggers the task
    }
    return false;
}

// ================= SYSTEM relay / injector =======================================================

int RunUacRelayBootstrap() {
    LogInit(UacLogPath(), false, true);
    // We are SYSTEM in session 0. Put a copy of our token into the active console session and start the
    // injector there, so its SendInput reaches the user's desktop (and the consent prompt on it).
    DWORD session = WTSGetActiveConsoleSessionId();
    if (session == 0xFFFFFFFF) { LOGI("uac-relay: no active console session"); return 1; }

    HANDLE self = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ASSIGN_PRIMARY | TOKEN_ADJUST_DEFAULT | TOKEN_ADJUST_SESSIONID,
                          &self)) {
        LOGI("uac-relay: OpenProcessToken failed (%lu)", GetLastError());
        return 1;
    }
    HANDLE dup = nullptr;
    BOOL okDup = DuplicateTokenEx(self, MAXIMUM_ALLOWED, nullptr, SecurityImpersonation, TokenPrimary, &dup);
    CloseHandle(self);
    if (!okDup) { LOGI("uac-relay: DuplicateTokenEx failed (%lu)", GetLastError()); return 1; }
    if (!SetTokenInformation(dup, TokenSessionId, &session, sizeof(session)))
        LOGI("uac-relay: SetTokenInformation(session) failed (%lu)", GetLastError()); // try to start anyway

    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring cmd = std::wstring(L"\"") + exe + L"\" --uac-inject";

    LPVOID env = nullptr;
    CreateEnvironmentBlock(&env, dup, FALSE);
    STARTUPINFOW si{sizeof(si)};
    si.lpDesktop = const_cast<wchar_t*>(L"winsta0\\default");
    PROCESS_INFORMATION pi{};
    const DWORD flags = CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW;
    BOOL ok = CreateProcessAsUserW(dup, exe, cmd.data(), nullptr, nullptr, FALSE, flags, env, nullptr, &si, &pi);
    if (env) DestroyEnvironmentBlock(env);
    CloseHandle(dup);
    if (!ok) { LOGI("uac-relay: CreateProcessAsUser failed (%lu)", GetLastError()); return 1; }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    LOGI("uac-relay: injector started in session %lu", session);
    return 0;
}

int RunUacInjector() {
    LogInit(UacLogPath(), false, true);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2); // match the host's coordinate mapping

    PSECURITY_DESCRIPTOR sd = MakePipeSd();
    SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};
    // One client at a time (the local PadDisplay). Message mode, 12-byte frames.
    HANDLE pipe = CreateNamedPipeW(kUacPipeName, PIPE_ACCESS_INBOUND,
                                   PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                   1, 0, sizeof(UacFrame) * 4, 0, sd ? &sa : nullptr);
    if (sd) LocalFree(sd);
    if (pipe == INVALID_HANDLE_VALUE) { LOGI("uac-inject: CreateNamedPipe failed (%lu)", GetLastError()); return 1; }
    LOGI("uac-inject: ready (SYSTEM, this session); waiting for the local client");

    // Idle lifetime cap: ConnectNamedPipe/ReadFile block, and the task does not bound this detached child,
    // so a watchdog cancels the blocked I/O once no command has arrived for 2 minutes and the SYSTEM
    // process exits - re-launched transparently when a later command needs it, so it is never a long-lived
    // SYSTEM process. Idle, not a hard cap, so it survives a long stay on the sign-in screen while the user
    // is typing. static (never the returning function's stack): the detached thread may outlive the loop.
    g_injExpired = false;
    g_lastActive = GetTickCount64();
    std::thread([pipe] {
        while (!g_injExpired) {
            Sleep(5000);
            if (GetTickCount64() - g_lastActive.load() > 2 * 60 * 1000) { g_injExpired = true; CancelIoEx(pipe, nullptr); break; }
        }
    }).detach();

    while (!g_injExpired) {
        if (!ConnectNamedPipe(pipe, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) break;
        g_lastActive = GetTickCount64();
        for (;;) {
            UacFrame f{};
            DWORD got = 0;
            if (!ReadFile(pipe, &f, sizeof(f), &got, nullptr) || got != sizeof(f)) break;
            g_lastActive = GetTickCount64();
            // Follow the desktop Windows is showing input on (Default, Winlogon for lock/sign-in/UAC) before
            // injecting; onWinlogon gates everything the sign-in screen allows (see AttachToInputDesktop).
            const bool onWinlogon = AttachToInputDesktop();
            if (f.cmd == uint32_t(UacCmd::Click)) {
                // A consent prompt anywhere under the point is the UAC case (any desktop). Otherwise a click
                // is allowed only onto the locked sign-in screen (Winlogon), where there is no consent window.
                HWND consent = ConsentRootAt(f.a, f.b);
                if (consent) {
                    Activate(consent);
                    InjectClick(f.a, f.b);
                    LOGI("uac-inject: clicked the UAC prompt at %d,%d", f.a, f.b);
                } else if (onWinlogon) {
                    InjectClick(f.a, f.b); // LogonUI already owns its desktop; no activation needed
                    LOGI("uac-inject: clicked the sign-in screen at %d,%d", f.a, f.b);
                } else if (IsLockScreenAt(f.a, f.b)) {
                    InjectClick(f.a, f.b); // dismiss the lock curtain so the secure sign-in field comes up
                    LOGI("uac-inject: dismissed the lock screen at %d,%d", f.a, f.b);
                } else {
                    LOGI("uac-inject: ignored a click at %d,%d - window there is %ls, not a secure desktop",
                         f.a, f.b, WindowImage(GetAncestor(WindowFromPoint(POINT{f.a, f.b}), GA_ROOT)).c_str());
                }
            } else if (f.cmd == uint32_t(UacCmd::Key) && f.a >= 0 && f.a <= 0xFFFF && KeyAllowed(WORD(f.a))) {
                // Navigation key: into a consent prompt (bring it forward first), or onto the sign-in screen.
                HWND consent = FindConsentWindow();
                if (consent) {
                    Activate(consent);
                    InjectKey(WORD(f.a));
                    LOGI("uac-inject: sent key 0x%02x to the UAC prompt", unsigned(f.a));
                } else if (onWinlogon) {
                    InjectKey(WORD(f.a));
                } else {
                    LOGI("uac-inject: ignored a key - no UAC prompt and the sign-in screen is not up");
                }
            } else if (f.cmd == uint32_t(UacCmd::KeyScan)) {
                // Typing the Windows password: only onto the sign-in screen, never elsewhere. The key itself
                // is never logged (it is the password); only that typing happened, once per connection.
                if (onWinlogon) {
                    InjectKeyScan(uint16_t(f.a), f.b != 0);
                    static bool noted = false;
                    if (!noted) { noted = true; LOGI("uac-inject: typing into the sign-in screen"); }
                }
            } else if (f.cmd == uint32_t(UacCmd::UnicodeChar)) {
                // Typing the Windows password as Unicode (a box on the tablet): only onto the sign-in screen,
                // never elsewhere. The character is never logged - only that typing happened, once per connection.
                if (onWinlogon) {
                    InjectUnicode(wchar_t(uint16_t(f.a)), wchar_t(uint16_t(f.b))); // f.b = low surrogate, or 0
                    static bool noted = false;
                    if (!noted) { noted = true; LOGI("uac-inject: typing into the sign-in screen"); }
                }
            } else if (f.cmd == uint32_t(UacCmd::MouseMove)) {
                // Move the cursor on the sign-in screen, or over the lock curtain so the user can aim a tap.
                if (onWinlogon || IsLockScreenAt(f.a, f.b)) InjectMove(f.a, f.b);
            } else if (f.cmd == uint32_t(UacCmd::CaptureLuid)) {
                g_capLuid.LowPart = DWORD(f.a);
                g_capLuid.HighPart = f.b;
            } else if (f.cmd == uint32_t(UacCmd::StartCapture)) {
                g_capW = f.a;
                g_capH = f.b;
                StartLockCapture();
            } else if (f.cmd == uint32_t(UacCmd::StopCapture)) {
                StopLockCapture();
            }
        }
        DisconnectNamedPipe(pipe);
    }
    StopLockCapture();
    // Not closed here: the watchdog thread may still CancelIoEx on this handle, and the process exits
    // immediately after returning, so the OS closes it. Closing here could race the watchdog.
    LOGI("uac-inject: exiting");
    return 0;
}

} // namespace pd
