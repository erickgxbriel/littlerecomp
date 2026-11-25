# Credits & Attribution

**LittleRecomp** ([github.com/erickgxbriel/littlerecomp](https://github.com/erickgxbriel/littlerecomp)) is exclusively an **Android ARM64 port** of the PC static recompilation project **[efm-games/scarface-recomp](https://github.com/efm-games/scarface-recomp)**.

All credit for reverse-engineering, static recompilation of the original Xbox executable (`default.xbe`), Xbox kernel/XDK HLE architecture, game-specific hooks, lighting/combiner fixes, save conversion, and the core runtime belongs to the original authors and upstream projects below. Our work in **LittleRecomp** focuses strictly on porting the platform layer, mobile Vulkan renderer, Android audio/input/UI, and mobile hardware tuning.

---

## Original PC Port & Recompilation Author
- **[efm-games/scarface-recomp](https://github.com/efm-games/scarface-recomp)** — Creator of the original Windows x64 / D3D12 static recompilation of *Scarface: The World Is Yours*, reverse-engineering analysis, `src/game/hooks.cpp`, NV2A shader/combiner translation design, and PC/PS2/Xbox save converter.

## Recompiler Toolchain & Core SDK
- **[Xbox XBE Recomp (TGP482)](https://github.com/TGP482/XboxXBE-Recomp)** — Static Xbox x86 XBE recompiler, Xbox kernel runtime, and base SDK ([MIT License](sdk/LICENSE)).
- **[XenonRecomp (hedge-dev)](https://github.com/hedge-dev/XenonRecomp)** — Overridable function interposition architecture and x86/PPC static recompilation concepts.

## Android Port Scope (LittleRecomp)
- ARM64 (`aarch64`) cross-compilation (`fiber_switch_arm64.S`, `libScarface.so` symbol interposition)
- Native **Vulkan 1.1 TBDR** mobile renderer (`port/android/vk_backend.cpp`, `vk_shaders.cpp`, `vk_context.cpp`)
- Single-pass FP16 upscalers (Snapdragon GSR v1 & FSR 1.0 + Bilinear) (`vk_upscaler.cpp`)
- ARM64 NEON BC1–BC3 runtime decoder (only active when hardware BCn is absent) (`vk_tex_neon.cpp`)
- Google Oboe (AAudio / OpenSL ES) audio backend (`sdk/runtime/audio/oboe.cpp`)
- Android Storage Access Framework (SAF) zero-copy ISO reader (`sdk/runtime/kernel/disc.cpp`), virtual touch controls (`touch_controls.cpp`), dynamic big-core CPU affinity (`cpu_affinity.cpp`), Android ADPF/Thermal monitoring (`android_adpf.cpp`), and PT-BR / EN mobile UI (`i18n.cpp`, `android_app.cpp`).

## Open-Source Libraries
- **[SDL3](https://github.com/libsdl-org/SDL)** — Windowing, Android lifecycle, touch & gamepad input
- **[Google Oboe](https://github.com/google/oboe)** — Low-latency AAudio / OpenSL ES audio output
- **[Dear ImGui](https://github.com/ocornut/imgui)** — Mobile setup wizard, options overlay, and performance HUD
- **[shaderc](https://github.com/google/shaderc)** — On-device GLSL 450 -> SPIR-V shader compilation (cached to disk)
- **[Zydis](https://github.com/zyantific/zydis)** — x86 disassembler / tooling dependency
- **[toml++](https://github.com/marzer/tomlplusplus)** — Configuration persistence (`settings.toml`)

---

*Scarface: The World Is Yours* is a trademark of its respective owners. This is a non-commercial, fan-made BYOG (Bring Your Own Game) preservation project with **no copyrighted game assets or disc images included**.
