# Controls

A gamepad is the default. Xbox, PlayStation and Switch controllers are detected automatically, with hot plugging and
rumble. Keyboard and mouse are opt-in: enable **Keyboard and mouse (opt-in)** in the launcher, the F1 options overlay,
or `settings.toml`, then edit the fixed pad bindings in the Controls tab.

The keyboard layer does not reinterpret keys for each game context. Each bound key or mouse button sends one Xbox pad
control, and the game decides what that control means in a menu, on foot or in a vehicle. Mouse look always drives the
right stick. This is the safest layout because the game keeps its own context-sensitive input handling.

## Default keyboard to Xbox pad bindings

| Xbox pad control | Default key | Typical use |
| --- | --- | --- |
| Left stick up/down/left/right | W / S / A / D | Move / steer |
| Right stick | Mouse move | Look / camera |
| D-pad up/down/left/right | Arrow keys | Menu selection and weapon selection |
| A | Space | Confirm, sprint, wall cover, accelerate |
| B | T | Back, taunt, rage, handbrake |
| X | R | Reload, brake |
| Y | E | Interact, enter or exit a vehicle |
| LB (white) | Tab | SAT phone |
| RB (black) | F | Alternate fire, look back |
| LT | Caps Lock | Lock target |
| RT | Left click | Fire |
| L3 | Left Ctrl | Crouch |
| R3 | C | Recentre or change camera |
| Back | Backspace | Back button |
| Start | Esc | Pause |

The mouse wheel no longer sends a pad control. The old PC-manual context rules (for example W meaning both “move
forward” and “accelerate”) are replaced by this fixed pad map, so the keys in a prompt are the keys that actually send
the pad control the game asks for.

### Rebinding

In the launcher or F1 Controls tab, find **Keyboard to Xbox pad bindings**:

1. Turn on Keyboard and mouse if it is off.
2. Click the key cell for a pad control.
3. Press the keyboard key or mouse button you want. Esc cancels without changing anything.
4. Use **Reset** on a row, or **Reset all bindings to Xbox defaults**, to restore the table above.

An asterisk marks a changed row. Bindings are written to `settings.toml` under `[keybindings]`; an absent table means
the defaults above. A settings file from an older build keeps keyboard enabled and its existing mouse sensitivity.

## Experimental options

Both options below are off by default. They remain available for players who want them, but they are not the default
because the game's prompts and vehicle inputs are context-sensitive.

### Modern gamepad controls (GTA-style)

Set `modern_pad = true` in `settings.toml`, or tick **Experimental: Modern gamepad controls (GTA-style)** in the
Controls tab. It remaps only a physical pad, and only while driving. The keyboard bindings above are unchanged.

| Pad input | Action while driving |
| --- | --- |
| RT | Accelerate |
| LT | Brake / reverse |
| A | Handbrake |
| B | Horn |
| RB | Fire from the vehicle |
| LB | Look back |
| L3 | Lock target |
| Back | SAT phone menu |
| X | Original brake, still available as a secondary pedal |
| Left stick | Steer |
| Right stick | Camera |
| Y / d-pad / R3 | Enter or exit / weapons / camera view as before |

On foot the original layout already follows the twin-stick convention (left stick move, right stick camera, RT fire,
LT lock), so it is left unchanged. The game has no separate horn button: the horn is the same pad input as accelerate,
so pressing B also gives a brief throttle input.

### HUD key prompts

Set `hud_key_prompts = true`, or tick **Experimental: PC key prompts in the HUD**. Pad glyph tokens become bracketed
names of the keys bound to those pad controls while keyboard/mouse is active. Touching a controller restores the
original glyphs; the music library snapshots the active input when its page opens and keeps that style until you leave.

These labels still depend on the game's context-sensitive prompt text. If a prompt describes an action with the wrong
pad glyph, the key name will follow that glyph, so the original controller glyphs remain the default.

## Any time

| Key | Action |
| --- | --- |
| F1 | Open or close the options overlay |
| F2 | Save the current frame as a BMP in the `screenshots` folder beside the executable |
| F3 | Capture the next frame in full under the `capture` folder, for debugging (see [debugging.md](debugging.md)) |

## Implementation notes

- **Fixed emulation:** `src/common/kbm.cpp` keeps one atomic key code per Xbox pad control. `kbm::Pad()` turns those
  codes into a normal pad report; it no longer reads the guest ActionMap stack to reinterpret keys. The recompiler's
  game code still reads ActionMaps normally to decide what each pad control does.
- **Simultaneous gamepad + keyboard/mouse:** Real controller inputs and keyboard/mouse inputs augment each other.
  Digital buttons are OR'd, analog triggers take `max()`, and stick inputs are clamped additions. The active input
  device switches on the fly.
- **Mouse look:** Raw X and Y deltas use the separate `kbm_sensitivity_x` and `kbm_sensitivity_y` sliders; `invert_y`
  flips only the vertical sign. The old single `kbm_sensitivity` setting is migrated to both axes.
- **Overlay capture:** While the ImGui options overlay (F1) is open, keyboard and mouse input is neutralized so an edit
  cannot leak into the game. Key capture swallows the key press and release used to set a binding.
- **Settings compatibility:** The `[keybindings]` table is optional. Bindings are saved with the other settings; an
  old file without the table uses the defaults. Reserved runtime keys (F1/F2/F3 and the diagnostic F8/F9) can be seen
  by the capture but should not be assigned as game bindings.

Sources: Official *Scarface: The World Is Yours* PC Manual (page 3, "Default Controls"), used as a historical reference. The fixed binding table above is this port's emulator-style layout.
