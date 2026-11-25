# Experimental 60 fps mode

This is an **experimental, opt-in** option. It is off by default. It raises the frame rate from the roughly 30 fps
the original game settles at to a fixed 60 fps. It is capped deliberately: an unrestricted mode was tested and
rejected because the game's animations became visibly over-accelerated.

## Why the game runs at about 30 fps

There are three pacing layers, stacked on top of each other:

1. **Guest vblank wait.** The game calls `D3DDevice_BlockUntilVerticalBlank` from its present wrapper
   (`sub_29AAF0`, guest VA `0x29AAF0`). The host implements that call in `sdk/runtime/xdk/d3d8.cpp` as a 16 ms
   sleep. This is the main limiter.
2. **Host frame cap.** `D3DDevice_Swap` calls `app::Pace()` in `sdk/runtime/app/main.cpp`, which sleeps to a
   deadline derived from `fps_cap`. This is only a ceiling: `fps_cap = 0` removes the sleep but cannot make the
   guest draw faster than the 16 ms wait allows.
3. **Presentation.** `D3D12Backend::Present` honours V-Sync, and `Threaded::Present` deliberately waits for the
   previous present so the guest's measured frame times match the frames actually shown. A GPU-bound frame therefore
   also paces the game.

The synthetic raster state (`D3DDevice_GetRasterStatus`) already reports a 60 Hz clock, so the guest is not hard-wired
to 30 Hz there.

## What the option does

Set `experimental_60fps = true` in `settings.toml`, or tick **Experimental: 60 fps mode** in the Display tab of the
launcher or the F1 overlay. Two things change:

- `D3DDevice_BlockUntilVerticalBlank` returns immediately instead of sleeping 16 ms.
- `app::Pace()` ignores `fps_cap` and always uses the fixed 60 fps cap from `kExperimentalFps`.

The **Frame rate** setting is therefore ignored while the option is on. V-Sync is left as configured; the recommended
combination is the experimental mode on and V-Sync on, which gives a steady 60 fps.

An earlier version also allowed `fps_cap = 0` with V-Sync off; that reached about 200-230 fps but animations ran far
too fast, so that path was removed. The guest's clocks (`KeQueryPerformanceCounter`, `KeQueryInterruptTime` and
`KeTickCount`) are elapsed-time based, but some guest code evidently assumes the original frame cadence.

## Measured results

Local run on a GeForce RTX 4050 laptop, same scene, `XBE_PERF=1`:

| Configuration | Measured |
| --- | --- |
| Option off, `fps_cap = 60` | 29.5-30.9 fps |
| Option on, V-Sync on | 60.0 fps |
| Option on, `fps_cap = 0`, V-Sync on | 59.8-60.2 fps (the cap is overridden) |

No shader, D3D or guest errors were logged. This shows the frame rate itself; it does **not** prove that every game
system behaves correctly at 60 fps.

## Risks and known limitations

- Cutscenes are gated separately. `sdk/runtime/xdk/bink.cpp` advances one movie frame per game frame but uses
  `BinkWait`, which follows the movie's own audio/video clock. Movies may therefore stay near 30 Hz while the 3D scene
  runs at 60.
- Any guest logic that counts presents instead of elapsed time will run faster. If movement, timers or AI look wrong
  with the option on, turn it off.
- This mode is not covered by the compatibility testing done for normal play.

## How to check it

Enable **Show the frame rate**, or run with `XBE_PERF=1` and look for the `perf: <n> fps` line in the log, which is
written once a second by `LogPerf()` in `sdk/runtime/gpu/d3d12.cpp`. Compare the same scene with the option off and on.
