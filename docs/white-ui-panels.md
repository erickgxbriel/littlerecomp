# White UI panels

This note records the cause and fix of the white UI backdrop bug seen in early previews (`v0.1.0`): the pause
menu and some other menus drew solid white panels instead of the intended tinted, translucent ones, which made
the text hard to read. The fix shipped in `v0.1.1` and was completed in `v0.2.0`.

## Symptom

- The pause menu backdrop was white instead of red.
- Other panels (ammo counters, dialog backgrounds) were either solid white or fully opaque.
- The 3D scene and text itself were fine; only the panel quads were wrong.

## Diagnosis

The bug was traced with the frame-capture tools described in [debugging.md](debugging.md). An **F3** capture of a
bad frame and a good frame, followed by a diff of their `history.txt` files, showed that the first differing
state write was the game leaving texture stage 0 in one of two unexpected combinations:

1. `ColorOp` disabled (`<= 1`) while a texture was still bound.
2. `ColorOp = SELECTARG1` with `arg1 = TEXTURE` — the texture was selected directly, bypassing the vertex tint.

The renderer's fixed-function shader key is built in `MakePixelKey()` in `sdk/runtime/gpu/shaders.cpp`. Its
"disabled stage" path already rewrote stage 0 to `MODULATE(CURRENT, TEXTURE)` for the video blit, but the
explicit `SELECTARG1/TEXTURE` case was not covered.

## Root cause

The game's UI atlases are white shapes: the actual panel colour and translucency live in the vertex diffuse
colour, not in the texture. On the Xbox's fixed-function pipeline the title expects stage 0 to multiply the
sampled white texel by the diffuse tint. When a menu selects the texture outright, or leaves stage 0 disabled
while still binding the atlas, the host renderer passed the white texel through unchanged, producing a solid
white rectangle. A second issue appeared once the colour was fixed: the atlas has no meaningful alpha, so
selecting alpha straight from the texture made panels opaque instead of translucent.

## Fix

In `MakePixelKey()` (`sdk/runtime/gpu/shaders.cpp`), when stage 0 has a bound texture and the game selects it
directly:

```cpp
if (i == 0 && tex[0].kind && t[kTssColorOp] == kTopSelectArg1 && (t[kTssColorArg1] & 0xF) == kTaTexture) {
    k.ff[0][0] = 4;  // D3DTOP_MODULATE
    k.ff[0][1] = kTaCurrent;
    k.ff[0][2] = kTaTexture;
    k.ff[0][4] = 4;  // D3DTOP_MODULATE (alpha)
    k.ff[0][5] = kTaCurrent;
    k.ff[0][6] = kTaTexture;
}
```

This makes both menus follow the same path: stage 0 modulates the white atlas by the diffuse colour AND the
diffuse alpha. Later stages still run, so effects that build on stage 0 are unaffected. The change is part of
the shader key, so it applies to every draw that uses the same texture-stage state.

## History

- `v0.1.1` (`5a042fa`): added the colour modulation for the `SELECTARG1/TEXTURE` case and removed the temporary
  debug bindings used while investigating. The "white backdrop" known issue was dropped from the README.
- `v0.2.0` (`5508395`): added the matching alpha modulation, fixing translucent ammo and pause panes.
