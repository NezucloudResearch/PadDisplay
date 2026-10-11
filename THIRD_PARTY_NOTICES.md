# Third-party notices

PadDisplay by Nezucloud includes or uses the following third-party components. Each is used under its own license. The license texts are included in this repository at the paths listed.

| Component | Used for | License | Where |
|---|---|---|---|
| [Virtual Display Driver](https://github.com/VirtualDrivers/Virtual-Display-Driver) (MikeTheTech / VirtualDrivers), v11.30.4.434, signed by SignPath Foundation | The virtual monitor (IddCx driver). Redistributed unmodified | MIT | `host/driver/vdd/` (license: `host/driver/vdd/LICENSE`) |
| [nv-codec-headers](https://github.com/FFmpeg/nv-codec-headers) (NVIDIA Video Codec SDK headers, tag n13.0.19.0) | NVENC API definitions | MIT (header notice) | `host/third_party/nv-codec-headers/` |
| [NVAPI SDK](https://github.com/NVIDIA/nvapi) | Headers and `nvapi64.lib`, for the "prefer maximum performance" driver profile | MIT | `host/third_party/nvapi/` (license: `host/third_party/nvapi/License.txt`) |
| [ViGEmClient](https://github.com/nefarius/ViGEmClient) (Nefarius Software Solutions), commit `b66d02d` (after v1.21.222.0), source unmodified | Talks to the ViGEmBus driver to create virtual Xbox 360 controllers | MIT | `host/third_party/ViGEmClient/` (license: `host/third_party/ViGEmClient/LICENSE`) |
| [ViGEmBus](https://github.com/nefarius/ViGEmBus) (Nefarius Software Solutions), v1.22.0 (the final release), installer `ViGEmBus_1.22.0_x64_x86_arm64.exe`, SHA-256 `89220a7865076b342892f98865f3499fb7c4cfd673159e89d352c360fd014c6a`, signed by Nefarius Software Solutions e.U. | The virtual Xbox 360 controller bus driver. Redistributed unmodified; installed silently by the admin setup step after its hash and signature are checked | BSD-3-Clause | `host/driver/vigem/` (license: `host/driver/vigem/LICENSE`) |
| [Windows Camera samples, VirtualCamera](https://github.com/microsoft/Windows-Camera/tree/master/Samples/VirtualCamera) (Microsoft) | The structure of the virtual camera's media source (stream, source and activation contract for the Windows 11 Frame Server), rewritten without WIL and C++/WinRT. No source file is copied as is | MIT | `host/camera/camera.cpp` (credited in its header) |
| [Windows driver samples, ACX audio samples](https://github.com/microsoft/Windows-driver-samples/tree/main/audio/Acx) (Microsoft) | The structure of the PadDisplay Microphone driver (ACX circuit, pins and real-time stream engine) and its INF, rewritten for a single capture endpoint fed from a ring buffer. Parts derive from the AudioCodec sample | MS-PL | `host/third_party/padmic/driver/` ([acxmic](https://github.com/NezucloudResearch/acxmic), a git submodule; credited in the file headers; license text below) |
| [VB-CABLE](https://vb-cable.com) (VB-Audio Software, Vincent Burel), Pack45: `VBCABLE_Setup_x64.exe` SHA-256 `734c35dfa6d98f48782a451633ceb471166ec70d60482fd89a1123d0ee3c4f41`, signed by Vincent Burel; its driver is signed by Microsoft (WHQL) | The fallback virtual microphone on PCs where PadDisplay's own driver cannot load. **VB-CABLE comes from [www.vb-cable.com](https://www.vb-cable.com) and is donationware: please support VB-Audio there.** Shipped unmodified and installed only on request. VB-Audio's [licensing page](https://vb-audio.com/Services/licensing.htm) allows distributing the standard VB-CABLE package with an application, free or commercial, and embedding its silent installation, as long as users can see it is a VB-Audio product and can donate | Donationware (VB-Audio licence, see the package's `readme.txt`) | `host/driver/vbcable/` |
| [Inno Setup](https://jrsoftware.org/isinfo.php) 6 (Jordan Russell, Martijn Laan) | Builds the Windows installer; its setup and uninstall programs are part of `PadDisplay-Setup-<version>.exe` (not stored in this repository) | Inno Setup License | https://jrsoftware.org/files/is/license.txt |
| [Gradle Wrapper](https://gradle.org) | Android build bootstrap (`android/gradle/wrapper/gradle-wrapper.jar`) | Apache-2.0 | `android/gradle/wrapper/` |
| [Kotlin standard library](https://kotlinlang.org) | Linked into the Android app at build time (not stored in this repository) | Apache-2.0 | Maven Central |

The following are **not** bundled but are used at run time if present:
- **Android Debug Bridge (adb)** from Google's Android SDK Platform-Tools, for USB mode.
- **NVIDIA and Windows system components**: the NVENC driver (`nvEncodeAPI64.dll`), `nvapi64.dll`, Media Foundation, DXGI/Direct3D 11.

## Trademarks

NVIDIA, NVENC and GeForce are trademarks of NVIDIA Corporation. Windows and DirectX are trademarks of Microsoft Corporation. Android is a trademark of Google LLC. Huawei, MatePad and HarmonyOS are trademarks of Huawei Technologies Co., Ltd. They are used here only to describe compatibility. This project is not affiliated with, endorsed by, or sponsored by any of these companies.

"PadDisplay by Nezucloud" is an independent open-source project. It is **not related** to the iPadOS app "PadDisplay" by treastrain / Ryoga Tanaka.

## Microsoft Public License (MS-PL)

Applies to the parts of `host/third_party/padmic/driver/` (acxmic) derived from Microsoft's ACX audio samples.

Copyright (c) 2015 Microsoft

This license governs use of the accompanying software. If you use the software, you accept this license. If you do not accept the license, do not use the software.

1. Definitions
The terms "reproduce," "reproduction," "derivative works," and "distribution" have the same meaning here as under U.S. copyright law.
A "contribution" is the original software, or any additions or changes to the software.
A "contributor" is any person that distributes its contribution under this license.
"Licensed patents" are a contributor's patent claims that read directly on its contribution.

2. Grant of Rights
(A) Copyright Grant- Subject to the terms of this license, including the license conditions and limitations in section 3, each contributor grants you a non-exclusive, worldwide, royalty-free copyright license to reproduce its contribution, prepare derivative works of its contribution, and distribute its contribution or any derivative works that you create.
(B) Patent Grant- Subject to the terms of this license, including the license conditions and limitations in section 3, each contributor grants you a non-exclusive, worldwide, royalty-free license under its licensed patents to make, have made, use, sell, offer for sale, import, and/or otherwise dispose of its contribution in the software or derivative works of the contribution in the software.

3. Conditions and Limitations
(A) No Trademark License- This license does not grant you rights to use any contributors' name, logo, or trademarks.
(B) If you bring a patent claim against any contributor over patents that you claim are infringed by the software, your patent license from such contributor to the software ends automatically.
(C) If you distribute any portion of the software, you must retain all copyright, patent, trademark, and attribution notices that are present in the software.
(D) If you distribute any portion of the software in source code form, you may do so only under this license by including a complete copy of this license with your distribution. If you distribute any portion of the software in compiled or object code form, you may only do so under a license that complies with this license.
(E) The software is licensed "as-is." You bear the risk of using it. The contributors give no express warranties, guarantees or conditions. You may have additional consumer rights under your local laws which this license cannot change. To the extent permitted under your local laws, the contributors exclude the implied warranties of merchantability, fitness for a particular purpose and non-infringement.
