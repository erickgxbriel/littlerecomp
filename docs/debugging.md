# Debugging

The renderer keeps a record of what the game asked it to draw, and of the state writes that led there.
Two keys and a few environment variables turn that into files beside the executable.

## Keys

| Key | What it does |
| --- | --- |
| **F2** | Saves the frame on screen as a BMP in `screenshots\`, plus `*_history.txt` with the state writes behind it. |
| **F3** | Captures the next frame in full under the capture folder: every draw, every target, plus `history.txt`. |

F3 writes a folder per frame. `frame.txt` holds one line per call, with the state the draw saw: shaders,
blend and depth states, the texture stage table (`s0 c<ColorOp>/<Arg0>,<Arg1>,<Arg2> a<AlphaOp>/...`),
the texture factor, all assembled vertices and the shader constants. `history.txt` holds the state writes in
order, with the address in the game that made them, interleaved with the draws.

## Environment variables

| Variable | What it does |
| --- | --- |
| `XBE_TRACE_TSS=1` | Prints every texture stage state, texture factor, texture and pixel shader write, with its caller. |
| `XBE_CAPTURE_EVERY=1` | Saves a 180x120 copy of the render target before every draw (`NNNN_now_<target>.bmp`). |
| `XBE_CAPTURE_TEX=0` | Leaves the texture images out of a capture. |
| `XBE_CAPTURE_HLSL=0` | Leaves the generated HLSL and shader state out of a capture. |
| `XBE_CAPTURE_FRAMES=N` | Captures N frames in a row instead of one. |
| `XBE_CAPTURE_DIR=<name>` | Capture folder under the executable, `capture` by default. |

## Reading a capture

`history.txt` is the one to look at first when a draw sees the wrong state:

```
000447 tex  s0 00000000 -> 834A5000                     pc 0026B847
000448 tss  s0 ColorOp(12) 4 -> 2                        pc 0026B0A8
000449 draw prim 6 s0 c2/1,2,2 tf FF4A6F60
```

Each entry carries the write count from the start of the capture, so the lines above say that the stage 0
colour op was left at `SELECTARG1` by the game itself at `0026B0A8` right before the draw. `mirror` lines
are writes the game made straight into its own deferred state table, which reach a draw without an API call.

Pair a white frame with a clean one and diff the two `history.txt` files: the first line that differs is the
state change that causes the bug.
