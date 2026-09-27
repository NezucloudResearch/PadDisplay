# Third-party notices

PadDisplay by Nezucloud includes or uses the following third-party components. Each is used under its own license. The license texts are included in this repository at the paths listed.

| Component | Used for | License | Where |
|---|---|---|---|
| [Virtual Display Driver](https://github.com/VirtualDrivers/Virtual-Display-Driver) (MikeTheTech / VirtualDrivers), v11.30.4.434, signed by SignPath Foundation | The virtual monitor (IddCx driver). Redistributed unmodified | MIT | `host/driver/vdd/` (license: `host/driver/vdd/LICENSE`) |
| [nv-codec-headers](https://github.com/FFmpeg/nv-codec-headers) (NVIDIA Video Codec SDK headers, tag n13.0.19.0) | NVENC API definitions | MIT (header notice) | `host/third_party/nv-codec-headers/` |
| [NVAPI SDK](https://github.com/NVIDIA/nvapi) | Headers and `nvapi64.lib`, for the "prefer maximum performance" driver profile | MIT | `host/third_party/nvapi/` (license: `host/third_party/nvapi/License.txt`) |
| [Gradle Wrapper](https://gradle.org) | Android build bootstrap (`android/gradle/wrapper/gradle-wrapper.jar`) | Apache-2.0 | `android/gradle/wrapper/` |
| [Kotlin standard library](https://kotlinlang.org) | Linked into the Android app at build time (not stored in this repository) | Apache-2.0 | Maven Central |

The following are **not** bundled but are used at run time if present:
- **Android Debug Bridge (adb)** from Google's Android SDK Platform-Tools, for USB mode.
- **NVIDIA and Windows system components**: the NVENC driver (`nvEncodeAPI64.dll`), `nvapi64.dll`, Media Foundation, DXGI/Direct3D 11.

## Trademarks

NVIDIA, NVENC and GeForce are trademarks of NVIDIA Corporation. Windows and DirectX are trademarks of Microsoft Corporation. Android is a trademark of Google LLC. Huawei, MatePad and HarmonyOS are trademarks of Huawei Technologies Co., Ltd. They are used here only to describe compatibility. This project is not affiliated with, endorsed by, or sponsored by any of these companies.

"PadDisplay by Nezucloud" is an independent open-source project. It is **not related** to the iPadOS app "PadDisplay" by treastrain / Ryoga Tanaka.
