# PocketGPU

English | [日本語](README.ja.md)

Thanks for reading this README. I'm Tsuruta.

PocketGPU is for people who have a powerful phone but a PC without a graphics card. Connect the phone to the PC with a cable, and the phone does the GPU work for the PC.

It works as a Vulkan driver on Windows and Linux. DirectX and OpenGL apps run through DXVK, vkd3d and zink. The phone side runs on iPhone and Android.

Built with [Claude Code](https://claude.com/claude-code). Most of the code here was written together with Claude.

- Setup and repair scripts for testers: [tsurutanmen/pocketgpu-setup](https://github.com/tsurutanmen/pocketgpu-setup)
- Discord: https://discord.gg/F3Mk8DBxqQ
- License: Apache-2.0 (`mc/pumpkin-ios` is GPL-3.0)

## What you need to install

These are not in this repository. They come from Apple, LunarG, Microsoft and others. The kit's `Setup-PocketGPU.cmd` checks them and offers to install them with winget.

| For | What | Where |
| --- | --- | --- |
| iPhone on Windows | Apple USB driver and Apple Mobile Device Service | [Apple Devices](https://apps.microsoft.com/detail/9np83lwlpz9k) from the Microsoft Store, or [iTunes](https://www.apple.com/itunes/) |
| Windows | Vulkan loader `vulkan-1.dll`, 64 and 32 bit | [Vulkan Runtime](https://vulkan.lunarg.com/sdk/home) from LunarG |
| Windows | Visual C++ runtime | [vc_redist.x64.exe](https://aka.ms/vs/17/release/vc_redist.x64.exe) |
| iPhone | A way to install the app with your own Apple ID | [AltStore](https://altstore.io), [Impactor](https://github.com/khcrysalis/Impactor/releases), [iloader](https://github.com/nab138/iloader/releases) or [xtool](https://github.com/xtool-org/xtool) |
| Android | adb | [Android platform-tools](https://developer.android.com/tools/releases/platform-tools) |
| iPhone on Linux | usbmuxd | Your distro's package |

## What is in here

| Folder | What |
| --- | --- |
| `vk/icd` | Vulkan driver (ICD) for the PC. It sends Vulkan calls to the phone |
| `vk/remote` | The receiver that runs the calls on the phone GPU (`server.c`, also built for Android) |
| `vk/tests` | Small Vulkan, D3D9/10/11/12 test programs |
| `vk/tools` | Tools used while measuring and debugging |
| `vk/third_party` | bcdec, lz4, stb_image |
| `Sources` | iPhone app (PocketGPU) and the camera-only app (PocketCam) |
| `android` | Android app (Java). It also carries the receiver from `vk/remote` |
| `pc` | PC tools: kit builder, USB forwarding, iPhone log and status tools |
| `pc/kit`, `pc/kit_linux` | Scripts that go into the Windows and Linux kits |
| `pc/app` | Windows app `PocketGPU.exe` (C#, WPF) |
| `pc/cam` | Windows webcam receiver, virtual camera and its installer |
| `pc/audio` | Windows mic receiver |
| `pc/livecap` | Live captions |
| `pc/linux_media` | Linux webcam and mic receivers |
| `pc/opengl` | OpenGL add-on pack: runs OpenGL apps through Mesa zink |
| `win` | Scripts that run games and samples on the phone GPU for testing |
| `mc` | Patches and glue to run the Pumpkin Minecraft server inside the iPhone app (optional) |
| `patches` | Patches applied to llama.cpp and whisper.cpp |
| `ci`, `.github/workflows`, `codemagic.yaml` | Build scripts |
| `server/kit-worker` | Cloudflare Worker for kit downloads, updates and tester logs |
| `server/discord` | Discord bot and the scripts that hand out kits and publish updates |
| `docs` | Plans and test notes (Japanese) |
| `docs/kit` | Install guide, FAQ and research notes for testers (Japanese and English) |

## Build

| Target | Command | Needs |
| --- | --- | --- |
| Windows driver and tools | `vk/build_win.sh <out>` | zig (`zig cc`). No Visual Studio |
| Linux driver (incl. WSL, Steam Deck) | `vk/build_linux.sh <out>` | gcc and the Vulkan headers |
| Android receiver | `vk/build_android.sh <out>` | Android NDK. No Android Studio |
| Android app (`.apk`) | `android/build_apk.sh` | Android SDK build-tools 35 and platform 34, JDK 17. No Gradle. Set `PGPU_SERVER_DIR` to the receiver built above |
| iPhone app (unsigned `.ipa`) | `ci/build_ios.sh` | macOS and Xcode 26.3. Runs on Codemagic or the `ios.yml` workflow |
| Windows app | `pc/app/build.cmd` | The C# compiler that comes with Windows (.NET Framework 4.8) |
| Webcam, mic and captions for Windows | `pc/cam/build.sh`, `pc/audio/build.sh`, `pc/livecap/build.sh` | zig |
| Pumpkin library for the iPhone app | `ci/build_pumpkin_ios.sh` | Rust. Only when `MINECRAFT=true` |
| Windows kit for one tester | `python pc/make_kit.py <name>` | The driver and app built above. Set `PGKIT_UPDATE_URL` to your own update server |
| Linux kit | `python pc/make_linux_kit.py <name>` | zig. Builds the Linux webcam and mic receivers too |
| Update server | `npx wrangler deploy` in `server/kit-worker` | A Cloudflare account. Fill in the `YOUR-...` values in `wrangler.toml` |

## Testers

- Toasty~ (`bl7ck_ic3`)
- BerBP (`berbp_but_now_its_benny`)
- Brewmasters (`brewmaegger_s2`)
- Josetruco1425 (`josetruco1425`)
- PabloC557 (`remythemice557`)
- ぴとん345 (`piton345`)
- Aurora (`tranzide`)
- 시유 (`seeu0001`)
- StarBite (`iwannabitestars`)
- Hi (`balzer0699`)

## License

| Part | License |
| --- | --- |
| Everything except the rows below | Apache-2.0 (`LICENSE`) |
| `mc/pumpkin-ios` | GPL-3.0 (`mc/pumpkin-ios/LICENSE`). An iPhone app built with `MINECRAFT=true` links Pumpkin and is GPL-3.0 as a whole |
| `vk/third_party/bcdec` | MIT or Unlicense |
| `vk/third_party/lz4` | BSD-2-Clause |
| `vk/third_party/stb` | MIT or public domain |

Not in this repository and fetched or built at build time: MoltenVK (Apache-2.0), llama.cpp (MIT), whisper.cpp (MIT), DXVK (zlib, the kit uses the dxvk-macOS async 1.10.3 build), vkd3d-proton 3.0.1 (LGPL-2.1, for the DirectX 12 tests), Mesa 26.2.4 from mesa-dist-win (MIT, for `pc/opengl`), Pumpkin (GPL-3.0).
