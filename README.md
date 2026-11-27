# LittleRecomp — *Scarface: The World Is Yours* (Android ARM64 Port)

`v0.4.0-beta` | [Leia em Português (BR)](README.pt-BR.md) | [Credits & Attribution](CREDITS.md)

**LittleRecomp** ([github.com/erickgxbriel/littlerecomp](https://github.com/erickgxbriel/littlerecomp)) is an unofficial native **Android (`arm64-v8a`) + Vulkan 1.1 TBDR** port of **[efm-games/scarface-recomp](https://github.com/efm-games/scarface-recomp)** (static recompilation of the Original Xbox version of *Scarface: The World Is Yours*).

> [!IMPORTANT]
> **Full Credits to the Original PC Port Author:**
> All static recompilation work, Xbox reverse engineering, game-specific hooks, save conversion, and the core Xbox runtime were created by **[efm-games/scarface-recomp](https://github.com/efm-games/scarface-recomp)** (built with [Xbox XBE Recomp](https://github.com/TGP482/XboxXBE-Recomp) and [XenonRecomp](https://github.com/hedge-dev/XenonRecomp)).
> **LittleRecomp** is exclusively the Android ARM64 porting layer (mobile Vulkan 1.1 TBDR renderer, Oboe audio, SDL3 touch/gamepad controls, ADPF thermal management, and PT-BR/EN setup UI).

> [!NOTE]
> **BYOG (Bring Your Own Game) — Zero Game Files Included:**
> This public repository contains **source code only**. Neither this repository nor generated source code packages contain game assets, textures, audio, ISOs, or `default.xbe`. You must supply your own legally acquired copy of the **Original Xbox** release.

---

## Requirements (Android)
- **OS:** Android 11+ (API 30+), `arm64-v8a` device.
- **GPU:** Vulkan 1.1+ compatible GPU (tested on ARM Mali-G68 MP4 / Exynos 1280).
- **Game Copy:** Your own Original Xbox `.iso`, `.xiso`, or extracted folder containing `default.xbe` (USA/Europe `default.xbe` SHA-1: `9367786d1b3bc9b35208a29a7ddca29487af0f5d`). **PS2 and PC versions are NOT compatible.**

---

## Installation & Setup (100% On-Device, No PC Needed After Installing APK)

1. Install the `LittleRecomp` APK on your Android phone (allow unknown sources if prompted).
2. Copy your Original Xbox `.iso` / `.xiso` (or extracted game folder) to your phone's internal storage or SD card.
   *(Do **not** leave it inside a cloud drive stream like Google Drive without downloading it locally first.)*
3. Open **LittleRecomp**:
   - The Setup Assistant opens automatically in **Português (BR)** or **English** (with a language switcher at the top).
   - Tap **Select ISO / XISO** (recommended — uses Android Storage Access Framework to stream directly from the `.iso`/`.xiso` without copying 3.2 GB and without needing extra storage permissions!).
   - Alternatively, tap **Select Game Folder** to pick an extracted folder with `default.xbe` (requires "All Files Access" permission).
4. Play with the on-screen touch controls or connect any Bluetooth/USB controller (Xbox / PlayStation / Switch).
5. Tap **MENU** in the **top-right corner** at any time to adjust:
   - **Graphics:** Internal resolution scale (`0.6x / 0.75x / 1.0x / 1.5x / 2.0x / 2.25x`), aspect mode (**True Ultrawide** default, Fullscreen Stretch, 16:9), single-pass FP16 upscaler (**Snapdragon GSR v1**, **FSR 1.0**, **Bilinear**), **Anisotropic Filtering** (`2x–16x`), **VSync**, **Swapchain Pre-Rotation**, **Light Textures (-75% VRAM)**, **Low-End Phone Preset**, optional **Fast Aux Effects (`0.5x`)** & **Smart DRS (FPS + Thermal)**, **Experimental 60 FPS**, big-core CPU pinning, and **Performance HUD** (**Off**, **FPS Only**, **Basic**, **Advanced**).
   - **Controls:** Virtual touch opacity/toggle, **Touch Layout Editor**, **Touch Auto-Aim + Fire**, **Auto-Hide Touch With Gamepad**, **Controller Rumble**, and GTA-style driving preset (`modern_pad`).
   - **Saves:** In-app Save Manager with slot metadata viewer, 2-tap deletion, and **Export/Import Backup (`.zip`)** via Android SAF.
   - **Achievements:** 45 offline achievements (500 pts, aligned with RA #20684), O(1) 2 Hz RAM polling (0% FPS impact), **Hardcore `[HC]`** badge detection, retroactive save sync, and animated toast banners.
   - **Game & Language:** UI language (`Auto / PT-BR / EN`), **Hybrid PT-BR Localization** (native `TextBible` menus + HD TrueType subtitle overlay + external `mods/ptbr/*.tsv` packs), **`mods/` File Override Layer**, **Import Mod / PT-BR Pack (`.zip`)**, Skip Intro, and Pause Cheats.

---

## Mobile Features Implemented in LittleRecomp
- **Native Vulkan 1.1 / 1.2+ TBDR Renderer:** `LAZILY_ALLOCATED` + `TRANSIENT_ATTACHMENT` depth buffers (zero DRAM spills), render-pass coalescing, **True Ultrawide 20:9** perspective correction, Vulkan 1.2/1.3 `VK_KHR_timeline_semaphore` fast path (with 1.1 fence fallback), pre-pass `upload_cmd` texture streaming, `TexSlabAllocator` (16 MB VRAM slabs), `MemSigFast()` 64-bit texture hashing, persistent `VkPipelineCache` + SPIR-V disk cache (`spv_cache.bin`), and 512 KB ISO LRU cache.
- **Universal Compatibility:** Dynamic `features.textureCompressionBC` check (ARM64 NEON BC1–BC3 decode runs only on mobile GPUs lacking native BCn), dynamic `cpuinfo_max_freq` heterogeneous CPU big-core detection, and dynamic `libandroid.so` ADPF / Thermal status loading.
- **FP16 Single-Pass Upscaling:** Snapdragon GSR v1 (default), FSR 1.0 EASU+RCAS, and Bilinear with hardware gamma LUT.
- **Low-Latency Audio & Controls:** Google Oboe (AAudio / OpenSL ES) mixing DirectSound + Xbox ADPCM, arcade vector touch overlay with custom layout editor, and physical controller support.

---

## Building From Source (Linux x86_64 -> Android ARM64)

This repository does **not** ship `generated/` files (they are generated locally from your own `default.xbe`).

### Prerequisites
- Linux x86_64 with CMake 3.25+, Ninja, Python 3, Clang/LLVM
- **Android NDK** installed at `/opt/android-ndk` (e.g. NDK r27+ with `aarch64` sysroot and `sources/third_party/shaderc`)
- Your Original Xbox `default.xbe` placed in `scarface-recomp/assets/` (and generated `generated/game/x86_recomp.*.cpp` built via `xbe_recomp` from [XboxXBE-Recomp](https://github.com/TGP482/XboxXBE-Recomp))

### Configure & Build APK
```bash
cmake -S port/android -B out/android-arm64-phase1 -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=/opt/android-ndk/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-30 \
  -DSCARFACE_BUILD_ANDROID_APP=ON \
  -DCMAKE_BUILD_TYPE=Release

# Build debug APK:
bash port/android/build_apk.sh

# Build release APK (debuggable=false, stripped, 16 KB page-aligned):
BUILD_TYPE=release bash port/android/build_apk.sh
```

Custom release signing can be configured via `SCARFACE_KEYSTORE`, `SCARFACE_KEY_ALIAS`, and `SCARFACE_KEY_PASS`.

---

## Upstream PC Port

For the original Windows Direct3D 12 port, XBE recompilation tooling notes, save format documentation, and Windows releases, visit **[efm-games/scarface-recomp](https://github.com/efm-games/scarface-recomp)**.
