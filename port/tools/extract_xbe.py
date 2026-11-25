#!/usr/bin/env python3
"""Extract default.xbe from an Xbox XDVDFS (.iso / .xiso) image."""

import hashlib
import os
import struct
import sys
from pathlib import Path

SECTOR_SIZE = 2048
PARTITIONS = (0, 0x18300000)
VOLUME_OFFSET = 32 * SECTOR_SIZE
MAGIC = b"MICROSOFT*XBOX*MEDIA"
ENTRY_HEAD = 0x0E
ATTR_FOLDER = 0x10


def find_root_entry(f, target_name: str):
    for base in PARTITIONS:
        f.seek(base + VOLUME_OFFSET)
        vol = f.read(SECTOR_SIZE)
        if len(vol) != SECTOR_SIZE:
            continue
        if vol[: len(MAGIC)] != MAGIC or vol[0x7EC : 0x7EC + len(MAGIC)] != MAGIC:
            continue

        root_sector, root_size = struct.unpack_from("<II", vol, 0x14)
        if root_size == 0 or root_size > (16 << 20):
            continue

        f.seek(base + root_sector * SECTOR_SIZE)
        table = f.read(root_size)
        if len(table) != root_size:
            continue

        seen = set()
        todo = [0]
        while todo:
            at = todo.pop()
            if at + ENTRY_HEAD > len(table) or at in seen:
                continue
            seen.add(at)
            left, right, sector, size, attr, name_len = struct.unpack_from("<HHIIBB", table, at)
            if left == 0xFFFF:
                continue
            if at + ENTRY_HEAD + name_len > len(table):
                continue
            if left:
                todo.append(left * 4)
            if right:
                todo.append(right * 4)
            name = table[at + ENTRY_HEAD : at + ENTRY_HEAD + name_len].decode("ascii", errors="replace")
            if not (attr & ATTR_FOLDER) and name.lower() == target_name.lower():
                return base + sector * SECTOR_SIZE, size, name
    return None


def main() -> int:
    if len(sys.argv) < 2:
        print(f"usage: {sys.argv[0]} <iso-path> [out-xbe-path]", file=sys.stderr)
        return 2

    iso_path = Path(sys.argv[1])
    if len(sys.argv) >= 3:
        out_path = Path(sys.argv[2])
    else:
        out_path = Path(__file__).resolve().parents[2] / "assets" / "default.xbe"

    with iso_path.open("rb") as f:
        found = find_root_entry(f, "default.xbe")
        if not found:
            print(f"error: default.xbe not found in {iso_path}", file=sys.stderr)
            return 1
        offset, size, name = found
        f.seek(offset)
        data = f.read(size)
        if len(data) != size:
            print(f"error: short read ({len(data)} / {size} bytes)", file=sys.stderr)
            return 1

    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_bytes(data)
    sha1 = hashlib.sha1(data).hexdigest()
    print(f"Extracted {name} ({size} bytes) -> {out_path}")
    print(f"SHA-1: {sha1}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
