# Project notes

Background notes that are not a work queue. `v1.3.2` is the current release. With the graphics defect below resolved,
further work is limited to bug fixes and the explicitly approved input changes recorded here.

## First-mission lighting/texture flash

A flash was reported in the first mission at Tony's mansion. The player's captures `20261118_232001_827.bmp` (bright)
and `20261118_232002_978.bmp` (the same view looking normal a second later) show the same scene. The bright frame has
mean RGB `[103.5, 85.5, 64.0]`, the normal frame `[30.6, 22.4, 14.9]`, and the full-frame difference mean is
`[77.1, 66.5, 51.8]`.

The F2 draw histories isolate the cause. The bright frame draws the world with stage 0 set to `SELECTARG2` and
`arg2 = TEXTURE` (`c3/1,1,2`), which selects the raw texture and drops the vertex diffuse that carries the console's
lighting. Every fixed-function `c3` draw in that frame has no pixel shader and a texture factor sweeping `B4...` to
`F7...`. The normal frame draws the same geometry with stage 0 disabled (`c1/1,1,2`, texture factor `FE000000`), which
the renderer already turns into `MODULATE(diffuse, texture)`. The other ten captured frames have no fixed-function
`c3` draws at all, so the pattern is unique to the flash. Moving Tony returns the state to the steady `c1` path.

The fix is in `MakePixelKey()` in `sdk/runtime/gpu/shaders.cpp`: stage 0 with a bound texture, `ColorOp = SELECTARG2`,
`arg1 = CURRENT` and `arg2 = TEXTURE` is now rewritten to `MODULATE(CURRENT, TEXTURE)`, exactly like the existing
disabled-stage and white-UI-panel rules. The sweep itself is not applied by those draws, so it does not need changing;
only the combiner that discarded the lighting did.

## Multi-monitor cursor escape

The game hides the Windows cursor with `SetCursor(nullptr)` but never confined it. On a multi-monitor desktop the
hidden pointer can slide outside the game window; a click then activates another window, sends `WM_ACTIVATE` with
`WA_INACTIVE`, clears input and stops raw mouse look until the game window is clicked again. A synthetic `mouse_event`
injected from another process bypasses the game thread's clip and is not a valid test of the fix.

`kbm::ApplyCursorClip()` now clips the pointer to the window's client rectangle while the game owns it. It is applied
on `WM_SETCURSOR`, `WM_SIZE` and `WM_MOVE`, and released on deactivate, overlay, minimize and destroy. Because
`ClipCursor` is thread-affine, only the window thread calls it. Windowed-mode playtesting confirmed the fix; the dev
machine has one monitor, so the exact two-monitor escape could not be reproduced locally.

## Fixed keyboard-to-Xbox bindings

The earlier keyboard layer reinterpreted a key from the guest ActionMap stack: for example W moved on foot but
accelerated in a car, and Space fired on foot but handbraked in a vehicle. That is convenient but ambiguous, and it
made a HUD key prompt describe an action rather than the key actually being sent.

`src/common/kbm.cpp` now stores one atomic key code per Xbox pad control. `kbm::Pad()` emits the corresponding pad
report without reading ActionMaps, so the game keeps its own context-sensitive meaning for A, B, X, Y, triggers and the
d-pad. Mouse look remains a raw right-stick emulation and is intentionally outside the binding table. The shared
Controls tab exposes a game-owned editor through `app::GameInfo` callbacks in both the launcher and the F1 overlay,
with capture, Esc cancel, per-row reset and reset-all.

Bindings persist under `[keybindings]` in `settings.toml`; an absent table means the defaults. A settings file from an
older build keeps keyboard enabled and its existing sensitivity values. Gamepad is the default, and the GTA-style pad
preset and PC key prompts are experimental and off by default. PC prompt labels are rebuilt from the binding table when
it changes, so a rebind is visible in the HUD, with a mutex guarding the label refresh against text expansion.

## Declined: DLSS 5

DLSS 5 is not planned for this project. The official SDK is not publicly available, and a proper integration would
require renderer changes this project does not intend to make, including engine motion vectors, jitter,
exposure/HDR data, semantic and engine masks, NVIDIA runtime packaging, and hardware gating. External user-installed
overlay solutions are outside this project: they are not bundled, supported, or required, and the base renderer keeps
working without an NVIDIA card or extra redistributables.

## Save import and conversion

PC saves use the same named-variable format as Xbox saves, but some releases have 191 variables instead of the
originally handled 188; PS2 releases have 169, 170, 174 or 332. The converter maps values by name into the Xbox save layout:
it uses a game-created profile when one exists and otherwise builds a template from the source, so the extra variables
are ignored and no Xbox profile has to exist first. CodeBreaker `.cbs` saves are RC4-decrypted and inflated in memory;
Max Drive `.max` uses LZARI, SharkPort/X-Port `.sps`/`.xps` stores raw members, EMS/uLaunchELF `.psu` stores a small
PS2 directory, and GameFAQs' `.zip` archives hold a PSV virtual-memory-card image. All are decoded in memory before
conversion, leaving the downloaded file untouched. A 332-variable CodeBreaker save used to crash after
"Profile loads successfully": PS2 value banks insert four-byte gaps before variables 5 and 196, and synthesized
Xbox templates are capped to 191 variables so late values cannot overwrite the signature areas. The save header's
four bytes at `0x0F` are the in-game profile identity, not part of
the date or the variable table. The game compares them with the fixed profile folder id, so an identity copied from
another profile makes the save look damaged. Original-Xbox imports are therefore rebound to the selected slot: the
identity is replaced, the three HMAC-SHA1 signature blocks are recomputed with the game's zero key, and the label is
rewritten. The save manager shows all four in-game profiles in one table: every row is always visible, empty rows offer
separate **Import From File** and **Import From Folder** buttons (Windows uses different dialogs for the two), and
occupied rows offer open and delete. Source detection is automatic. Profiles 1-3 use the
game's fixed folder ids and identities. Profile 4 was confirmed from the save the game created in the empty fourth slot:
folder `267AFF5B38C3`, identity `15 11 1a 30`, `SaveMeta.xbx` `Name=Profile  4`, and three valid signatures. The launcher
keeps that identity in private metadata and prefers the live save's identity if the game rewrites it, so the row still
imports after the save is deleted. Every row, including Profile 4, offers import while empty. There is no first-free
fallback, and the import/convert code rejects an occupied target, so the player must delete the existing save first.
`User_Slot_0`/Profile 1 is only written when the player explicitly targets that empty slot. The launcher and the in-game
options overlay share the save-manager tab, but they are different modes. The pre-game launcher passes `game_running =
false` and can manage saves; the F1 overlay passes `true`, shows a warning and disables every command (import, open
folder and delete) because the game is already running behind it. Command-line import remains a pre-game operation.
