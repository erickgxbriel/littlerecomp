# HUD glyphs and keyboard hotkeys

This note records the design, reverse-engineering and debugging work behind the PC key prompts shipped in `v1.1.0`.
The goal was to let a keyboard/mouse player read every HUD prompt without losing the original gamepad prompts for
players who use a pad. No game assets are added: the labels are plain text in the game's existing HUD font, and the
original controller artwork is hidden in memory by replacing its texture with a transparent one.

> [!NOTE]
> PC key prompts are now **experimental and off by default**. The table below records the original v1.1.0 design.
> The current label table is rebuilt from the keyboard-to-Xbox binding map in [controls.md](controls.md), so a
> rebind is shown in the HUD. The game's context-sensitive prompt text can still pair a word with the wrong pad glyph;
> original controller glyphs remain the default.

## Goals and constraints

- Keyboard/mouse and a real controller must keep working together. The last device to send input decides which prompt
  style is shown; neither input path is disabled.
- While keyboard/mouse is active (or before the first input), pad glyph tokens become bracketed key labels such as
  `[E] Exit`, `[Shift] Sprint` or `[Enter] Start`.
- A real pad must get the original glyphs, artwork and wording back. The original pad scheme and the optional modern
  gamepad preset are unchanged.
- Labels use brackets, not parentheses, and preserve each prompt's delimiter and letter case. They are UTF-16LE guest
  strings because the game's text constructor calls `wcslen` on the pointer it receives.
- Nothing is shipped from the game: no button images, keycap images or ISO data. The text is rendered with the HUD font
  already in the game.
- The PC bindings follow [controls.md](controls.md) and the official PC manual. In particular, **B** remains the horn
  and menu navigation stays arrow keys / **Enter** / **Esc**.

## User-visible behaviour

| Situation | Keyboard/mouse prompt | Gamepad prompt |
| --- | --- | --- |
| Interact / pick up / triangle-Y demand | `[E] ` before the original words | Original triangle glyph |
| Sprint | `[Shift]` | Original A glyph |
| Start / accept / confirm / cover | `[Enter]` | Original A or X glyph |
| Dialog close / cancel / back / create / exit | `[Esc]` | Original B glyph |
| Taunt / provoke | `[T]` | Original B glyph |
| Blind rage | `[V]` | Original B glyph |
| Tape player Play / Stop | `[T] ` before `PLAY` / `STOP` | Original B glyph |
| Tape player Shuffle | `[Enter] SHUFFLE ON/OFF` | Original wording with "TURN" and the A glyph |
| Mission start / profile / tutorial notices | `[Enter] Start`, `[Enter] Options and Profiles`, `[Esc] Exit Tutorial` | Original glyph and wording |

On the look tutorial page the keys follow the labels: **Enter** sends the pad's Start (Options and Profiles) and
**Esc** sends B (Exit Tutorial). Elsewhere Esc remains pause in the world and back in a menu.

The music library is a deliberate exception. It snapshots the active input device when its page opens and keeps that
style until another page is loaded, so changing device while the page is open does not mix key labels with pad glyphs.
Elsewhere, switching device rewrites cached prompts immediately.

## Prompt pipeline

The game expands localized strings in two passes. A string ID such as `[LOOK_TUTORIAL_X]` is first resolved to a wide
sentence, then nested `[BUTTON_GRAPHIC_*]` tokens inside that sentence are resolved to glyph strings. The host hooks
both passes.

1. **`sub_0024D9B0`** (nested-token extractor) saves a bounded narrow copy of the wide source sentence in a guest
   scratch page. This is what lets a token's own words decide its meaning.
2. **`sub_0024D900`** (token dispatcher) first calls the original function to obtain the real glyph string. If key
   prompts are active, it returns a UTF-16LE label from a persistent guest page instead. The original pointer is
   remembered beside the label.
3. **`sub_0024DF70`** (expander) records the source line around the original call, then rewrites the resulting wide
   buffer for prompts that do not go through a text-object constructor. On the pad path it removes injected labels and
   restores the original music wording. The second pass parses our own `[Enter]` / `[Esc]` text as tokens again, so
   those label words are ignored by the line rules to keep both tutorial prompts from collapsing to one key.
4. **`sub_002492A0` / `sub_00249300`** (wide and narrow text-object constructors) receive the final string. They keep
   both the plain and labeled variants, always hand the guest the longer labeled string so its buffer is large enough,
   and register the object for later device switches.
5. **`ApplyHudDeviceText()`** rewrites registered text buffers when `kbm::LastDevice()` changes, unless the music
   library has frozen the decision.
6. **`sub_00240570`** (HUD page loader) reads the page name. A page containing `TapePlayer`, `Tape_Player` or
   `Tape Player` freezes the prompt style until the next page.
7. **`gpu::texturedumper`** replaces the original controller artwork with a transparent BC2 texture while key labels
   are active. The swap is keyed by the original texture bytes, so no files are loaded from disk and no pad artwork is
   modified.

The recompiler connects strong host definitions through the import address table. The working slots are
`0xDC2268` for `sub_0024D900` (export RVA `0x145CF0`) and `0xDC2270` for `sub_0024DF70` (export RVA `0x146180`).
Earlier experiments at `0xDC22D8` / `0xDC22E0` did not replace the functions actually used by the HUD.

## Context and line rules

`kbm::PromptContextNow()` reads the active ActionMap stack and returns `OnFoot`, `Vehicle` or `Menu`. The token table in
`src/game/hooks.cpp` stores one label for each context. That alone was not enough:

- The title screen has no menu map yet, so an A token was labelled `[Shift]` instead of `[Enter]`.
- A tutorial notice can leave a dialog map on top, so "sprint" was labelled `[Enter]`.
- The same A token means accept in menus, sprint on foot and accelerate in a vehicle.
- Some scripts send bare tokens such as `A`, `B`, `START_MISSION`, `XSTART`, `TBACK` or `CLOSE` with no surrounding
  expander line.

The fix is a three-level decision:

1. **Line-level override.** The source sentence is scanned case-insensitively for known words. `sprint` / `run while`
   win over `start`; `start`, `accept`, `continue`, `select`, `[yes]`, `[load]` and `cover` mean `[Enter]`; `[close]`
   means `[Esc]` (pad B); `rage` / `[hold]` mean `[V]`; `taunt` / `provoke` mean `[T]`; cancel/back words mean `[Esc]`.
   Every triangle/Y demand maps to `[E]`, including plain captions where only the artwork was hidden.
2. **Bare-token fallback.** Well-known standalone tokens are classified without a source line.
3. **Context table.** If neither rule fires, the three-context mapping is used.

This is why wording and case are preserved instead of being normalized. The hooks only add or replace the button part
of a caption.

## Static artwork and tape-player captions

Some prompts have no button token at all. The game hides the triangle sprite but leaves the words on screen, so a text
token substitution alone would leave those lines without a key. Two more hooks cover them:

- `sub_00194A30` is the prompt sprite manager. Indices `2`, `3` and `4` are the R, R1 and L1 shoulder drawings; indices
  `0` and `1` are the curved navigation arrows and must stay visible. The hook lets the original select/cache/show
  handshake run, then hides only the shoulder drawing and clears the active index so its timer cannot show it again.
- The tape-player glyphs are identified by their UTF-16 code units: `0x00B9` for Play/Stop and `0x00BD` for Shuffle,
  followed by the original words. The glyph is replaced by `[T] ` for Play/Stop. Shuffle becomes `[Enter] SHUFFLE
  ON/OFF`: `Enter` is the key that toggles it in the PC menus, and "TURN" is removed only on the keyboard path because
  the full caption overlaps the neighboring tip. The gamepad path restores "TURN" and the original artwork.

The texture override hides 14 original artwork hashes, including menu-compressed copies of the face buttons, the Start
button, three tutorial shoulder drawings and the pause-menu pad icon. A fully transparent BC2 level works for both
alpha-tested and blended HUD draws. The override bumps the texture generation so every texture re-keys on the next
draw, which is what makes a pad bring the glyphs back immediately.

## Challenges and fixes

### Guest strings and the crash at `0x24C57A`

The text constructor calls an unbounded `wcslen` on its incoming pointer. Reusing a scratch string after its page was
rebuilt, or handing it an unterminated copy, produced `C0000005` reads such as guest `0x012BA1A0` with caller return
`0x24C57A`. The fix has several layers:

- All wide scans and copies check every UTF-16 unit's page with `mem::IsCommitted()` before reading it, not just the
  first unit. A string whose terminator crosses an uncommitted page is rejected.
- `SafeWideString()` runs immediately before the pointer is passed to the guest. Invalid plain or labeled pointers are
  discarded and the original caller-owned buffer is left in place.
- Copy helpers return zero when no terminated string is found, instead of creating a partial variant.
- The scratch tables were raised to 256 slots for text variants, and narrow/wide prefix and strip pages were separated
  so a byte copy cannot overwrite a UTF-16 unit.

The narrow constructor has the same checks because it converts an ASCII source to a wide string before the guest sees
it.

### Esc opening and immediately closing pause

A held **Esc** originally emitted a virtual Start every poll. Once the pause map became the top ActionMap, the same key
also became pad B, so the menu opened and closed in consecutive polls. This was also implicated in the pause crash.
`kbm::Pad()` now uses a one-shot edge latch: one physical press emits one virtual Start or B action. The latch resets
when the F1 overlay opens, when keyboard/mouse input is disabled and after the key is released.

The ActionMap test was also narrowed: only the **topmost** map decides whether Esc means back or pause. Previously a
stale `PauseActionMap` below the character map could make Esc send B while the player was still in the world.

### Cached music-library captions

The music library caches text objects and rebuilds Shuffle's caption when ON/OFF changes. Following the active device
live therefore left mixed labels, stale glyphs or the shortened Shuffle wording on the wrong device. Snapshotting at
page entry is done from `sub_00240570` by recognizing the two page-name reads at guest return addresses `0x24C26F` and
`0x24C9D1`. `hud_prompts::SetMusicLibraryPage()` stores the mode and freezes `UseKeyLabels()` until the next page.

### Cheat unlock hook

The first cheat implementation returned a fabricated script string from the shared evaluator bridge
`sub_0006FB50`, which poisoned unrelated script strings. It was replaced with a narrow hook in `sub_00099BC0` that
forces only the final `PauseIsRewardLocked` bool conversion at return address `0x1CC812` to `false`. This unlocks menu
entries without changing any text or any effect.

### Launcher gamepad A

The launcher's Play handler existed, but player presses were never seen because SDL controller input was only started
later for the game-code window. `Launcher()` now calls `input::Init()` after COM initialization. `PollGamepadPlay()`
uses the rising edge of `XINPUT_GAMEPAD_A` on any controller, leaves the mismatched-release popup in charge, and
activates the button labelled **Play (A)**. The game window's own Start (A) path is unchanged.

## Source map

| File | Responsibility |
| --- | --- |
| `src/game/hooks.cpp` | HUD hooks, prompt table, line rules, guest scratch memory, string validation, sprite and cheat hooks |
| `src/game/hud_prompts.h` | Effective prompt mode and music-library snapshot API |
| `src/common/kbm.cpp` | Active device, ActionMap context, Esc edge latch, device-change rewrite trigger |
| `sdk/runtime/gpu/texturedumper.cpp` | Original glyph hash table and transparent BC2 replacement |
| `sdk/runtime/app/launcher.cpp` | Launcher controller initialization, gamepad-A Play and the **Play (A)** label |
| `docs/controls.md` | Player-facing bindings that the labels name |

## Verification

- Release build succeeds after a clean rebuild.
- Repeated automated `--no-launcher` smokes and an Esc-input smoke complete without exceptions, probes or leftover
  processes.
- Launcher smoke confirms SDL starts, the controller is detected and **Play (A)** is rendered.
- Device switching, music-library snapshots, Shuffle wording, mission prompts, pause cheats and launcher gamepad A were
  confirmed in game by the maintainer.
- The automated smoke does not navigate to the music library, press Shuffle, open the pause cheats or reproduce every
  manual input path; those remain visually verified.

No temporary trace hooks or diagnostic artifacts are part of this feature.
