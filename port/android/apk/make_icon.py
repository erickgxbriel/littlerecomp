#!/usr/bin/env python3
"""Generate crisp multi-DPI Miami Noir fallback PNG launcher icons for ScarfaceDroid."""
import math
import struct
import zlib
from pathlib import Path


def png_chunk(chunk_type: bytes, data: bytes) -> bytes:
    crc = zlib.crc32(chunk_type + data) & 0xFFFFFFFF
    return struct.pack(">I", len(data)) + chunk_type + data + struct.pack(">I", crc)


def mix(a, b, t):
    return int(round(a * (1.0 - t) + b * t))


def make_png(size: int) -> bytes:
    rows = bytearray()
    sub = 2
    sub_n = sub * sub
    for y in range(size):
        rows.append(0)
        for x in range(size):
            r_acc = g_acc = b_acc = a_acc = 0
            for sy in range(sub):
                for sx in range(sub):
                    px = (x + (sx + 0.5) / sub) / size
                    py = (y + (sy + 0.5) / sub) / size
                    # Coordinates in [0, 108] space matching vector foreground
                    vx, vy = px * 108.0, py * 108.0
                    cx, cy = vx - 54.0, vy - 54.0
                    dist = math.hypot(cx, cy)
                    if dist > 50.0:
                        continue  # transparent outside badge
                    # Split noir background (left white-ice, right deep charcoal)
                    if vx < 54.0:
                        cr, cg, cb = 242, 242, 242
                    else:
                        cr, cg, cb = 20, 20, 24

                    # Subtle crimson outer ring accent
                    if dist > 47.0:
                        cr, cg, cb = 200, 30, 43

                    # Palm trunk on left side
                    if 42.0 <= vx <= 47.5 and 38.0 <= vy <= 74.0:
                        t = (vy - 38.0) / 36.0
                        trunk_x = 46.5 - 2.0 * math.sin(t * 1.3)
                        if abs(vx - trunk_x) < 1.5 + 0.8 * t:
                            cr, cg, cb = 20, 20, 24

                    # Stylized palm fronds near (46, 39)
                    fx, fy = vx - 46.0, vy - 39.0
                    if -18.0 <= fx <= 16.0 and -14.0 <= fy <= 8.0:
                        d_crown = math.hypot(fx * 0.75, fy * 1.15)
                        if d_crown < 11.5 and (fy < 2.0 or abs(fx) > 3.0):
                            # Ribbed silhouette approximation
                            ang = math.atan2(fy, fx)
                            if math.sin(ang * 5.0) > -0.25:
                                cr, cg, cb = 20, 20, 24

                    # Mobile accent bars on right dark half
                    if 61.0 <= vx <= 77.0 and 63.0 <= vy <= 68.5:
                        cr, cg, cb = 242, 242, 242
                    elif 61.0 <= vx <= 73.0 and 71.5 <= vy <= 76.0:
                        cr, cg, cb = 200, 30, 43

                    # Bold crimson diagonal slash across center
                    # Line from (31, 80) to (74, 30)
                    dx, dy = 43.0, -50.0
                    d_len = math.hypot(dx, dy)
                    nx, ny = -dy / d_len, dx / d_len
                    perp = abs((vx - 31.0) * nx + (vy - 80.0) * ny)
                    proj = ((vx - 31.0) * dx + (vy - 80.0) * dy) / d_len
                    if 0.0 <= proj <= d_len and perp < 4.3:
                        cr, cg, cb = (233, 66, 79) if perp > 2.6 else (200, 30, 43)

                    r_acc += cr
                    g_acc += cg
                    b_acc += cb
                    a_acc += 255

            if a_acc == 0:
                rows.extend((0, 0, 0, 0))
            else:
                rows.extend((r_acc // sub_n, g_acc // sub_n, b_acc // sub_n, a_acc // sub_n))

    ihdr = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)
    idat = zlib.compress(bytes(rows), 9)
    return b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", ihdr) + png_chunk(b"IDAT", idat) + png_chunk(b"IEND", b"")


def main() -> None:
    res_dir = Path(__file__).resolve().parent / "res"
    densities = {
        "mipmap-mdpi": 48,
        "mipmap-hdpi": 72,
        "mipmap-xhdpi": 96,
        "mipmap-xxhdpi": 144,
        "mipmap-xxxhdpi": 192,
    }
    for folder, px in densities.items():
        data = make_png(px)
        out_dir = res_dir / folder
        out_dir.mkdir(parents=True, exist_ok=True)
        (out_dir / "ic_launcher.png").write_bytes(data)
        (out_dir / "ic_launcher_round.png").write_bytes(data)


if __name__ == "__main__":
    main()
