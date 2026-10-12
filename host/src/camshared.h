// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
// The one-slot mailbox between the host (writes decoded camera frames) and the virtual camera's media source,
// which runs inside Windows' Frame Server (a service in session 0, so it cannot see this process's memory
// any other way). Included by both PadDisplay.exe and PadDisplayCamera.dll.
#pragma once
#include <cstdint>

namespace pd {

// The Frame Server (a service) creates the mapping when an app opens the camera: only services may create
// objects in the Global\ namespace, and a normal user's process cannot. The host opens it for writing.
constexpr wchar_t kCamMapName[] = L"Global\\PadDisplayCameraFrame";
// Anyone using the PC interactively may write frames; the service's own account and administrators have full control.
constexpr wchar_t kCamMapSddl[] = L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;LS)(A;;GRGW;;;IU)";

constexpr uint32_t kCamMagic = 0x4D434450; // 'PDCM'
constexpr uint32_t kCamHeaderBytes = 4096;
constexpr uint32_t kCamMaxWidth = 1920, kCamMaxHeight = 1080;
constexpr uint32_t kCamCapacity = kCamMaxWidth * kCamMaxHeight * 3 / 2; // NV12, tightly packed (pitch = width)
constexpr uint32_t kCamMapBytes = kCamHeaderBytes + kCamCapacity;

// A reader that has not asked for a frame for this long is gone (an app that closed the camera, or a crashed Frame Server).
constexpr uint64_t kCamReaderStaleMs = 1500;
// A frame older than this is not shown: the tablet stopped, so the camera shows "no signal" instead of a frozen picture.
constexpr uint64_t kCamFrameStaleMs = 1000;

struct CamShared {
    uint32_t magic;
    uint32_t version;
    uint32_t capacity;
    uint32_t reserved;
    volatile int64_t readerTick; // GetTickCount64() when the media source last wanted a frame (written by the service)
    volatile int64_t seq;        // even = stable; odd = the host is writing (seqlock: no lock, no waiting)
    volatile int64_t ptsUs;
    volatile int64_t writeTick;  // GetTickCount64() of the last complete write
    volatile int32_t width;
    volatile int32_t height;
};
static_assert(sizeof(CamShared) <= kCamHeaderBytes, "header must fit before the pixels");

inline uint8_t* CamPixels(CamShared* s) { return reinterpret_cast<uint8_t*>(s) + kCamHeaderBytes; }

} // namespace pd
