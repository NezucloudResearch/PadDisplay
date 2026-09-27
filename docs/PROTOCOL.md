# PadDisplay wire protocol (version 2)

There is one TCP connection per tablet. The tablet is the client and the host listens on **27183**, dual-stack. Over USB the tablet connects to `127.0.0.1:27183`, which `adb reverse` tunnels to the PC. The PIN is checked on every connection, USB included, unless the host sets `requireUsbPin=0`: the tunnel is reachable by any app on the tablet. All integers are **little-endian**.

Every message is framed as `u8 type | u32 payloadLength | payload`.

## Discovery
The host broadcasts the UDP datagram `PADDISPLAY <version> <tcpPort> <hostname>` once a second to port **27184**. It sends to 255.255.255.255 and to every interface's directed broadcast address.

## Handshake
1. The client sends `ClientHello`.
2. The host validates the version and the PIN. On failure it sends `Error` and closes the connection.
   - Wrong PINs are rate-limited: at most 5 per minute per source, and 20 per minute across all network sources. USB has its own limit.
   - While a source is locked out, it gets error code 6.
3. The host sends `HostHello`, then `Config`, then a stream of `Frame`s. The first frame is an IDR.

The host may send another `HostHello`, followed by a new `Config`, at any time, for example after a resolution change. The client then reconfigures its decoder.

## Host → client
| type | name | payload |
|---|---|---|
| 0x01 | HostHello | u16 version, u8 codec (1 = H.264, 2 = HEVC), u16 width, u16 height, u8 fps, u32 bitrateKbps |
| 0x02 | Config | Annex-B parameter sets (VPS/SPS/PPS); queue with `BUFFER_FLAG_CODEC_CONFIG` |
| 0x03 | Frame | u64 ptsUs, u8 flags (bit0 = keyframe), Annex-B access unit |
| 0x04 | Ping | u64 hostTimeUs, u32 lastRttUs, u32 currentBitrateKbps. The client must answer with `Pong` |
| 0x05 | Error | u8 code (1 bad PIN, 2 version, 3 no display, 4 encoder, 6 locked out after too many wrong PINs), UTF-8 text |

The stream has no B-frames and an infinite GOP, so IDRs are sent only on request. After any decode problem the client sends `KeyframeReq`.

## Client → host
| type | name | payload |
|---|---|---|
| 0x81 | ClientHello | u16 version, u16 screenW, u16 screenH, u16 dpi, u8 codecMask (bit0 H.264, bit1 HEVC), u8 maxTouch, u32 pin, u16 maxFps (the refresh rate chosen on the tablet; the host switches the virtual monitor to the closest mode), UTF-8 device name |
| 0x82 | Touch | u8 count, then count × { u8 pointerId, u8 kind (0 down, 1 move, 2 up, 3 cancel), u16 x, u16 y, u16 pressure (0–1024), u16 size } |
| 0x83 | Pen | u8 kind (0 hover, 1 down, 2 move, 3 up, 4 leave), u8 buttons (bit0 barrel, bit1 eraser), u16 x, u16 y, u16 pressure (0–1024), i8 tiltX, i8 tiltY (−90…90) |
| 0x84 | Mouse | u8 kind (0 move, 1 left down, 2 left up, 3 right down, 4 right up, 5 wheel), u16 x, u16 y, i16 wheelV, i16 wheelH (120 = one notch) |
| 0x85 | KeyframeReq | empty |
| 0x86 | Pong | u64 hostTimeUs, echoed from `Ping` |
| 0x87 | FrameAck | u64 ptsUs of a `Frame`, sent once the frame has been read and queued to the decoder (v2) |
| 0x88 | Keepalive | empty. Sent whenever the client has sent nothing for 20 ms. On ROMs that doze the Wi-Fi radio despite Wi-Fi locks (seen on HarmonyOS), the router holds or drops packets for the tablet until it transmits: without this, the PC → tablet direction lost 80% of packets and connecting took over 5 s |

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

## Flow control (v2)
The client acknowledges **every** `Frame`. The host stops encoding while `maxFramesInFlight` frames (default 3) are unacknowledged. Desktop updates made meanwhile are not lost; Desktop Duplication coalesces them into the next frame. This bounds queueing in the network to a few frames: over a slow link the frame rate drops instead of the delay growing. Adaptive bitrate uses the time spent waiting for acks and each frame's send-to-ack time.

v1 clients (no acks) are still accepted, but they only get RTT-based bitrate adaptation, which can let seconds of video queue up on Wi-Fi.

Coordinates are normalized to the video frame: 0–65535 across its width and height.

`Touch` messages carry **every** active finger. The host keeps held contacts alive and lifts everything when the connection drops. The host treats 10 s without any message from the client as a dead connection.
