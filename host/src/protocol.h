// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
// Wire protocol shared with the Android client (see docs/PROTOCOL.md).
// Every message: [u8 type][u32 payloadLen][payload], little-endian.
#pragma once
#include <cstdint>

namespace pd {

// v2 adds FrameAck (flow control); v3 adds keyboard, relative mouse and game controllers.
// Older clients are still accepted.
constexpr uint16_t kProtocolVersion = 3;
constexpr uint16_t kMinProtocolVersion = 1;
constexpr uint16_t kTcpPort = 27183;
constexpr uint16_t kDiscoveryPort = 27184;
constexpr uint32_t kHeaderSize = 5;
constexpr uint32_t kMaxClientPayload = 64 * 1024;

enum class Msg : uint8_t {
    // host -> client
    HostHello = 0x01,   // u16 version, u8 codec, u16 width, u16 height, u8 fps, u32 bitrateKbps
    Config = 0x02,      // Annex-B parameter sets (VPS/SPS/PPS)
    Frame = 0x03,       // u64 ptsUs, u8 flags (bit0 keyframe), Annex-B access unit
    Ping = 0x04,        // u64 hostTimeUs, u32 lastRttUs, u32 bitrateKbps
    Error = 0x05,       // u8 code, utf-8 message
    AudioFormat = 0x06, // audio connection: u32 sampleRate, u8 channels, u8 bitsPerSample
    AudioData = 0x07,   // audio connection: u64 ptsUs, interleaved s16le PCM
    Rumble = 0x08,      // u8 index, u8 largeMotor, u8 smallMotor (0 = off); from a game on the PC (v3)
    Notice = 0x09,      // utf-8 text for the tablet to show, e.g. the controller driver is missing (v3)
    // client -> host
    ClientHello = 0x81, // u16 version, u16 screenW, u16 screenH, u16 dpi, u8 codecMask,
                        // u8 maxTouch, u32 pin, u16 maxFps, [v3: u8 flags,] utf-8 device name
    Touch = 0x82,       // u8 count, count x {u8 id, u8 kind, u16 x, u16 y, u16 pressure, u16 size}
    Pen = 0x83,         // u8 kind, u8 buttons, u16 x, u16 y, u16 pressure, i8 tiltX, i8 tiltY
    Mouse = 0x84,       // u8 kind, u16 x, u16 y, i16 wheelV, i16 wheelH
    KeyframeReq = 0x85, // empty
    Pong = 0x86,        // u64 echoed hostTimeUs
    FrameAck = 0x87,    // u64 ptsUs of a Frame the client has read and queued to its decoder (v2)
    Keepalive = 0x88,   // empty; sent by the client every 20 ms when idle to keep its Wi-Fi radio awake
    AudioHello = 0x89,  // first message of an (optional) audio connection: u16 version, u32 pin, u8 flags
    Key = 0x8A,         // u8 flags (bit0 down), u16 scancode: PC set-1 make code, bit 8 = E0-extended (v3)
    MouseRel = 0x8B,    // u8 kind, i16 dx, i16 dy, i16 wheelV, i16 wheelH: a captured mouse; buttons and
                        // the wheel act where the cursor is (v3)
    Gamepad = 0x8C,     // u8 index, u8 flags (bit0 connected), u16 buttons, u8 leftTrigger, u8 rightTrigger,
                        // i16 leftX, i16 leftY, i16 rightX, i16 rightY: the XInput layout (v3)
};

enum Codec : uint8_t { CodecH264 = 1, CodecHEVC = 2 };
enum CodecMask : uint8_t { MaskH264 = 1, MaskHEVC = 2 };
enum AudioFlags : uint8_t { AudioMutePc = 1 };
// Choices made in the tablet app (v3). OnlyScreen: the PC's own screens are off while connected.
enum HelloFlags : uint8_t { HelloGamingMode = 1, HelloOnlyScreen = 2 };
enum ErrorCode : uint8_t { ErrBadPin = 1, ErrVersion = 2, ErrNoDisplay = 3, ErrEncoder = 4, ErrLockedOut = 6 };
enum TouchKind : uint8_t { TouchDown = 0, TouchMove = 1, TouchUp = 2, TouchCancel = 3 };
enum PenKind : uint8_t { PenHover = 0, PenDown = 1, PenMove = 2, PenUp = 3, PenLeave = 4 };
enum PenButtons : uint8_t { PenBarrel = 1, PenEraser = 2 };
enum MouseKind : uint8_t {
    MouseMove = 0, MouseLeftDown = 1, MouseLeftUp = 2, MouseRightDown = 3, MouseRightUp = 4, MouseWheel = 5,
    MouseMiddleDown = 6, MouseMiddleUp = 7, MouseX1Down = 8, MouseX1Up = 9, MouseX2Down = 10, MouseX2Up = 11 // v3
};
enum KeyFlags : uint8_t { KeyDown = 1 };
enum GamepadFlags : uint8_t { GamepadConnected = 1 };
constexpr uint16_t kKeyExtended = 0x100; // the scancode has the E0 prefix (arrows, right Ctrl, ...)
constexpr uint16_t kKeyPause = 0x145;    // Pause, whose real code (E1 1D 45) does not fit this scheme
constexpr int kMaxGamepads = 4;          // as many as XInput reports

// Coordinates are normalized to the video frame: 0..65535 across the width/height.
constexpr uint32_t kCoordMax = 65535;
constexpr uint32_t kPressureMax = 1024;

struct ClientHello {
    uint16_t version = 0;
    uint16_t screenW = 0, screenH = 0, dpi = 0;
    uint8_t codecMask = MaskH264;
    uint8_t maxTouch = 10;
    uint32_t pin = 0;
    uint16_t maxFps = 60;
    uint8_t flags = 0;
    char name[64] = {};
};

// Little-endian readers/writers for message payloads.
inline uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
inline uint32_t rd32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
inline uint64_t rd64(const uint8_t* p) { return uint64_t(rd32(p)) | (uint64_t(rd32(p + 4)) << 32); }
inline void wr16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }
inline void wr32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i)); }
inline void wr64(uint8_t* p, uint64_t v) { wr32(p, uint32_t(v)); wr32(p + 4, uint32_t(v >> 32)); }

} // namespace pd
