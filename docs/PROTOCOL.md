# PadDisplay wire protocol (version 3)

There is one TCP connection per tablet (encrypted with TLS over Wi-Fi: see *Transport security*). The tablet is the client and the host listens on **27183**, dual-stack. Over USB the tablet connects to `127.0.0.1:27183`, which `adb reverse` tunnels to the PC. The PIN is checked on every connection, USB included, unless the host sets `requireUsbPin=0`: the tunnel is reachable by any app on the tablet. All integers are **little-endian**.

Every message is framed as `u8 type | u32 payloadLength | payload`.

## Transport security
Over Wi-Fi the connection is TLS 1.2, and everything in this document, `ClientHello` included, travels inside it. Message formats and the protocol version did not change.
- **The host tells TLS from plain by the first byte** of the connection: 0x16 is a TLS handshake, while the plain protocol starts with 0x81 (`ClientHello`) or 0x89 (`AudioHello`). A host accepts both from any address, so clients from before TLS keep working.
- **The tablet uses TLS for every host except `127.0.0.1`** (USB through `adb reverse`, which never touches the network), and for both the video and the audio connection. It never falls back to plain over Wi-Fi.
- **Suites:** TLS 1.2 only, no renegotiation. The host's certificate is a self-signed ECDSA P-256 one, so the suites are ECDHE-ECDSA with AES-GCM. The tablet offers only `TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256`.
- **Identity:** the tablet pins the SHA-256 of the host certificate (DER) per host address, the first time it connects, and only after the host answered the PIN with something other than `Error`. It refuses a different certificate later. The video and audio connections must present the same one.
- **`requireEncryption=1`** (host setting) makes the host answer a plain connection from a non-loopback address with `Error` code 7 and close it.

## Discovery
The host broadcasts the UDP datagram `PADDISPLAY <version> <tcpPort> <hostname>` once a second to port **27184**. It sends to 255.255.255.255 and to every interface's directed broadcast address.

## Handshake
1. The client sends `ClientHello` (after the TLS handshake, over Wi-Fi).
2. The host validates the version and the PIN. On failure it sends `Error` and closes the connection.
   - Wrong PINs are rate-limited: at most 5 per minute per source, and 20 per minute across all network sources. USB has its own limit.
   - While a source is locked out, it gets error code 6.
3. The host sends `HostHello`, then `Config`, then a stream of `Frame`s. The first frame is an IDR. Preparing the screens can take several seconds (ending the previous session, switching the virtual monitor on); until `HostHello` the host sends a `Ping` twice a second, so a client's read timeout does not run out.

The host may send another `HostHello`, followed by a new `Config`, at any time, for example after a resolution change. The client then reconfigures its decoder.

## Host → client
| type | name | payload |
|---|---|---|
| 0x01 | HostHello | u16 version, u8 codec (1 = H.264, 2 = HEVC), u16 width, u16 height, u8 fps, u32 bitrateKbps |
| 0x02 | Config | Annex-B parameter sets (VPS/SPS/PPS); queue with `BUFFER_FLAG_CODEC_CONFIG` |
| 0x03 | Frame | u64 ptsUs, u8 flags (bit0 = keyframe), Annex-B access unit |
| 0x04 | Ping | u64 hostTimeUs, u32 lastRttUs, u32 currentBitrateKbps. The client must answer with `Pong`. hostTimeUs is 0 in the pings before `HostHello`: those only keep the connection alive |
| 0x05 | Error | u8 code (1 bad PIN, 2 version, 3 no display, 4 encoder, 6 locked out after too many wrong PINs, 7 the PC only accepts encrypted connections), UTF-8 text |
| 0x08 | Rumble | u8 controller index, u8 largeMotor, u8 smallMotor (0–255, 0 = off). The level holds until the next `Rumble` for that controller (v3) |
| 0x09 | Notice | UTF-8 text for the tablet to show. The session goes on. Used when a game controller cannot be created on the PC (v3) |
| 0x0D | LockState | u8 frozen: 1 = the PC is locked / at the sign-in screen and the tablet can't show it (the picture is frozen), so the tablet offers its password box; 0 = streaming normally again, hide it. Sent only on a change, and only to a v5+ client (v5) |

The stream has no B-frames and an infinite GOP, so IDRs are sent only on request. After any decode problem the client sends `KeyframeReq`.

## Client → host
| type | name | payload |
|---|---|---|
| 0x81 | ClientHello | u16 version, u16 screenW, u16 screenH, u16 dpi, u8 codecMask (bit0 H.264, bit1 HEVC), u8 maxTouch, u32 pin, u16 maxFps (the refresh rate chosen on the tablet; the host switches the virtual monitor to the closest mode), v3 only: u8 flags (bit0 = gaming mode, bit1 = only screen), UTF-8 device name |
| 0x82 | Touch | u8 count, then count × { u8 pointerId, u8 kind (0 down, 1 move, 2 up, 3 cancel), u16 x, u16 y, u16 pressure (0–1024), u16 size } |
| 0x83 | Pen | u8 kind (0 hover, 1 down, 2 move, 3 up, 4 leave), u8 buttons (bit0 barrel, bit1 eraser), u16 x, u16 y, u16 pressure (0–1024), i8 tiltX, i8 tiltY (−90…90) |
| 0x84 | Mouse | u8 kind (0 move, 1 left down, 2 left up, 3 right down, 4 right up, 5 wheel; v3: 6 middle down, 7 middle up, 8 X1/back down, 9 X1 up, 10 X2/forward down, 11 X2 up), u16 x, u16 y, i16 wheelV, i16 wheelH (120 = one notch) |
| 0x85 | KeyframeReq | empty |
| 0x86 | Pong | u64 hostTimeUs, echoed from `Ping` |
| 0x87 | FrameAck | u64 ptsUs of a `Frame`, sent once the frame has been read and queued to the decoder (v2) |
| 0x88 | Keepalive | empty. Sent whenever the client has sent nothing for 20 ms. On ROMs that doze the Wi-Fi radio despite Wi-Fi locks (seen on HarmonyOS), the router holds or drops packets for the tablet until it transmits: without this, the PC → tablet direction lost 80% of packets and connecting took over 5 s |

| 0x8A | Key | u8 flags (bit0 = down, else up), u16 scancode (v3). See *Keyboard* below |
| 0x8B | MouseRel | u8 kind (as for `Mouse`), i16 dx, i16 dy, i16 wheelV, i16 wheelH (v3). A captured mouse: kind 0 moves the pointer by dx, dy; buttons and the wheel act where the pointer is |
| 0x8C | Gamepad | u8 index (0–3), u8 flags (bit0 = connected), u16 buttons, u8 leftTrigger, u8 rightTrigger, i16 leftX, i16 leftY, i16 rightX, i16 rightY (v3). See *Game controllers* below |
| 0x92 | SecureText | u8 flags (bit0 = press Enter after), then utf-8 text (v5). The Windows password, typed into a box on the tablet, to be typed into the **locked sign-in screen**. The host honours it only over an encrypted (TLS) or USB link and only when lock-screen control is on; the SYSTEM helper types it only while the sign-in (Winlogon) desktop is the input desktop, so it can never reach a normal app. See *Typing the sign-in password* below |

## Hello flags (v3)
A v3 `ClientHello` has one more byte before the device name, with the choices made in the tablet app. v1 and v2 hellos don't have it.
- **bit0, gaming mode:** the host asks Windows for realtime GPU scheduling, so a game that uses the whole GPU can't starve capture and encoding. For older clients the host uses `gamingMode` from `settings.ini` instead.
- **bit1, only screen:** once the virtual monitor is attached, the host makes it the PC's only active display, so the PC's own screens go dark. When the session ends they come back. The host applies this without saving it in Windows' display database, so after a crash or a restart Windows returns to the stored setup by itself. If it can't be applied, the host sends a `Notice` and the tablet stays an extra screen.

## Keyboard (v3)
`Key` carries the key's **position**, as a PC scancode (set 1 make code, 1–0x7F), not a character: the PC applies its own keyboard layout, as for a keyboard plugged into it.
- Bit 8 (0x100) is set for keys that have the E0 prefix, such as the arrows, right Ctrl and right Alt.
- Pause is sent as 0x145, because its real code (E1 1D 45) doesn't fit this scheme.
- The client sends key repeats as further "down" messages; the host does not repeat keys itself.
- The host releases every key that is still down when the connection drops.

## Game controllers (v3)
`Gamepad` carries the whole state of one controller in the XInput layout, and is sent whenever it changes.
- Buttons: 0x0001 d-pad up, 0x0002 down, 0x0004 left, 0x0008 right, 0x0010 start, 0x0020 back, 0x0040 left stick, 0x0080 right stick, 0x0100 left shoulder, 0x0200 right shoulder, 0x0400 guide, 0x1000 A, 0x2000 B, 0x4000 X, 0x8000 Y.
- Sticks are −32768…32767 with up and right positive. Triggers are 0–255.
- The first message for an index plugs a virtual Xbox 360 controller into the PC. A message with the connected bit clear unplugs it, and so does the end of the session.
- This needs the ViGEmBus driver on the PC. Without it the host answers the first `Gamepad` with one `Notice` and ignores the rest.
- For a moment after a controller is plugged in, the host repeats its latest state to Windows: a state sent while Windows is still starting the controller is dropped.

## Audio connection (optional)
To receive PC audio, the tablet opens a **second** TCP connection to the same port and sends `AudioHello` first. The host checks the PIN exactly as for video, with the same rate limits. A newer audio connection replaces an older one. When audio is off, this connection is never opened and nothing is captured.

| dir | type | name | payload |
|---|---|---|---|
| client → host | 0x89 | AudioHello | u16 version, u32 pin, u8 flags (bit0 = mute the PC's speakers while connected) |
| host → client | 0x06 | AudioFormat | u32 sampleRate, u8 channels (2), u8 bitsPerSample (16) |
| host → client | 0x07 | AudioData | u64 ptsUs, interleaved s16le PCM (about 10 ms per packet, following the Windows mix period) |

- **Source:** the host captures the default output device with WASAPI loopback, which is taken before the PC's mute and volume, and down-mixes it to stereo.
- **Sending:** the host only sends when the socket is writable right away; otherwise it drops the packet. It never queues.
- **Client:** the client never sends on this connection, so the host treats "readable" as "closed" and restores the PC's mute state immediately.
- **Playback:** the tablet plays through an `AudioTrack` with a 15 ms start threshold and keeps its queue near an adaptive target.
  - The target starts at 30 ms. Each real underrun during continuous sound adds 10 ms, up to 80 ms, and it shrinks by 5 ms after every 20 s without one.
  - Drift and jitter are corrected with a linear resampler, changing the speed by up to +2 / −1%.
  - While the queue is above target, silent packets are skipped.
  - Packets are only dropped above 200 ms.

## Microphone connection (optional, v4)
To use the tablet's microphone as a PC microphone, the tablet opens another connection to the same port (TLS over Wi-Fi like the others) and sends `MicHello` first. The host checks the PIN exactly as for video, with the same rate limits. A newer microphone connection replaces an older one. Nothing is captured on the tablet until the user switches it on there, and then only while an app on the PC records from the virtual microphone.

| dir | type | name | payload |
|---|---|---|---|
| client → host | 0x8D | MicHello | u16 version, u32 pin, u8 flags (0) |
| client → host | 0x8E | MicData | u64 ptsUs, then s16le mono 48 kHz samples (10 ms per packet, 480 samples). Without samples it is a keep-alive |
| host → client | 0x0C | MicState | u8 active: 1 = an app on the PC records from the virtual microphone, so capture; 0 = nobody does, stop |

- **Only while it is used:** the host sends `MicState` as soon as the virtual microphone is open (within milliseconds of `MicHello`) and on every change. The tablet opens its microphone on 1 and closes it on 0, so Android shows the microphone in use only while a PC app listens. A start is passed on at once; a stop after the recording device was idle for 2 s, because apps close and reopen a microphone while they look at the devices. While the microphone is off the tablet sends a `MicData` without samples every 2 s: the host treats 10 s without a message as a lost tablet.
  - The host learns it from Windows' audio sessions on the recording device ("CABLE Output", or the PadDisplay microphone), by notification, without polling. If it cannot watch them it sends 1 once and the microphone runs for the whole connection.
  - A host from before `MicState` never sends it and expects audio all the time. A client that has not received a `MicState` 3 s after `MicHello` therefore captures anyway. A client from before `MicState` ignores the message and sends all the time; the host accepts that.

- **No codec:** raw PCM costs no encode delay and about 0.77 Mbps.
- **PC side:** Windows only offers microphones that a driver provides, so PadDisplay uses one of two: its own PadDisplay Microphone driver where Windows loads it, otherwise the free VB-CABLE that ships with it (the host chooses; see below). The driver ([acxmic](https://github.com/NezucloudResearch/acxmic), the submodule `host/third_party/padmic`). The host writes each packet's samples, as they are, to the driver's control device (`\\.\PadDisplayMic`, interface in `host/third_party/padmic/include/padmic_public.h`); apps choose "PadDisplay Virtual Microphone" (Windows shows it as "Microphone (PadDisplay Virtual Microphone)") as their microphone. The only buffer is the driver's: an app starts getting audio once 20 ms are waiting (silence before that, and again after the audio ran dry), and audio beyond 100 ms is dropped from the old end, so the delay is bounded. The host does no buffering or resampling of its own on this path. With VB-CABLE the host plays the packets into the cable's playback device ("CABLE Input") through WASAPI shared mode, with a small jitter buffer (20 ms to start, at most 120 ms, growing after underruns) and a resampler if the cable's rate is not 48 kHz; apps choose "CABLE Output". That path adds about 90-110 ms in all (the cable and Windows' audio engine alone take 93-106 ms), the driver path about 15 ms.
- **Refusals:** if the PC has no virtual microphone (neither PadDisplay's driver nor VB-CABLE; the PC then asks its user whether to install VB-CABLE), the control device is already open (another PadDisplay), or the PC has switched the microphone off (`acceptTabletMic`), it sends one `Notice` and ends the connection. The tablet shows the text and stops. Notice texts a tablet may see: "The PC has no virtual microphone yet. PadDisplay on the PC is asking to install one: accept it there, then switch the microphone off and on here.", "The virtual audio cable (VB-CABLE) could not be opened for playback. See the PC's log.", "The PadDisplay microphone driver is not installed on the PC…", "The PadDisplay microphone is already in use by another PadDisplay on the PC.", "The microphone is switched off in PadDisplay on the PC…", "The PadDisplay microphone driver stopped…".

## Camera connection (optional, v4, Windows 11 PC)
To use the tablet's camera as a PC webcam, the tablet opens a connection and sends `CamHello` first (same PIN check and TLS). The PC registers a virtual camera once (a setup step); apps then see "PadDisplay Camera".

| dir | type | name | payload |
|---|---|---|---|
| client → host | 0x8F | CamHello | u16 version, u32 pin, u8 codec (1 = H.264), u16 width, u16 height, u8 fps |
| client → host | 0x90 | CamConfig | Annex-B SPS/PPS |
| client → host | 0x91 | CamFrame | u64 ptsUs, u8 flags (bit0 = keyframe), Annex-B access unit |
| host → client | 0x0A | CamState | u8 active: 1 = an app on the PC has the webcam open, 0 = nobody does |
| host → client | 0x0B | CamKeyframeReq | empty: the host could not decode, send an IDR |

- **The camera runs only while someone uses the webcam.** After `CamHello` the host sends `CamState` 0. The tablet turns its camera and encoder on when it receives 1 and off (camera light off) when it receives 0. The host decides by watching whether the virtual camera's reader is asking for frames, about every 250 ms. Frames that arrive while the state is 0 are dropped.
- **Encoding:** hardware H.264 from the camera's surface, no B-frames, an IDR only on request, CBR (about 4 Mbps at 720p30, 8 Mbps at 1080p30). Sizes up to 1920×1080.
- **PC side:** the host decodes with the Media Foundation H.264 decoder in low-latency mode and writes the newest frame into a shared-memory slot that the virtual camera's media source (loaded by Windows' Frame Server) reads at 30 fps. There is no queue: a late frame is simply replaced. With no frame for 1 s, apps see a dark "no signal" picture.
- **Refusals:** before Windows 11, before the one-time setup, or with `acceptTabletCamera` off, the host sends one `Notice` and ends the connection.

`Notice` on these connections is not tied to `Gamepad`: they belong to the microphone or camera connection only.

## Typing the sign-in password (v5)
When the PC is locked (a mid-session lock, or the sign-in screen after a boot/restart), a normal process cannot capture the secure desktop, so the tablet may not be able to *show* it. As a fallback, the tablet can show its own password box and send what the user types as `SecureText`; the PC types it into the sign-in field.

- **Transport:** only over an **encrypted** Wi-Fi link (TLS) or a **USB** cable. A plain Wi-Fi link is refused with a `Notice` and the text is dropped — the password never crosses the network in the clear.
- **Gate:** honoured only when the PC's **lock-screen control** setting is on. The actual injection is done by the SYSTEM helper (`uacrelay`), which types only while the **sign-in (Winlogon) desktop** is the input desktop, so the text can never land in a normal app, even if it arrives at the wrong moment.
- **Encoding:** each UTF-16 code unit is injected with `KEYEVENTF_UNICODE` (independent of the PC's keyboard layout, so any character types correctly), then Enter when bit0 of the flags is set.
- **Handling:** the text is never written to any log, and the host wipes its copy as soon as it is handed to the helper. The tablet clears its box after sending.
- **When it is offered:** the tablet shows the password box only while the host has sent `LockState` 1 (the PC is locked and the picture is frozen because the tablet can't show that screen) — not during normal use, and not while the lock screen itself is streaming.

## Flow control (v2)
The client acknowledges **every** `Frame`. The host stops encoding while `maxFramesInFlight` frames (default 3) are unacknowledged. Desktop updates made meanwhile are not lost; Desktop Duplication coalesces them into the next frame. This bounds queueing in the network to a few frames: over a slow link the frame rate drops instead of the delay growing. Adaptive bitrate uses the time spent waiting for acks and each frame's send-to-ack time.

v1 clients (no acks) are still accepted, but they only get RTT-based bitrate adaptation, which can let seconds of video queue up on Wi-Fi.

## Versions
- **v1:** video, touch, pen, mouse gestures.
- **v2:** `FrameAck` (flow control).
- **v3:** `Key`, `MouseRel`, `Gamepad`, more `Mouse` buttons, `Rumble`, `Notice`, and the flags byte in `ClientHello`.
- **v4:** the microphone and camera connections: `MicHello`, `MicData`, `CamHello`, `CamConfig`, `CamFrame`, `CamState`, `CamKeyframeReq`. Nothing in the screen stream changed. A tablet app that sends version 4 needs a host of version 4 or later.
- **v5:** `SecureText` (type the Windows password into the locked sign-in screen) and `LockState` (the host tells the tablet when the picture is frozen on that screen, so the box is offered only then). Older clients never send `SecureText` and ignore `LockState`, which the host sends only to a v5+ client. Nothing else changed.

A host accepts every client version up to its own, and rejects a newer one with error code 2. It only sends `Rumble` or `Notice` to a client that has sent `Gamepad`.

Coordinates are normalized to the video frame: 0–65535 across its width and height.

`Touch` messages carry **every** active finger. The host keeps held contacts alive and lifts everything when the connection drops. The host treats 10 s without any message from the client as a dead connection.
