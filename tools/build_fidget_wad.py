#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Replace registered DOOM E3M9 with the one-button fidget arena.

The output remains an IWAD containing the user's original assets. Only the ten
standard map lumps following E3M9 are replaced. The arena is a single convex
sector, so its valid BSP consists of four segs, one subsector, and no nodes.
"""

from __future__ import annotations

import argparse
import struct
from pathlib import Path


MAP_NAMES = (
    "THINGS", "LINEDEFS", "SIDEDEFS", "VERTEXES", "SEGS",
    "SSECTORS", "NODES", "SECTORS", "REJECT", "BLOCKMAP",
)


def name8(text: str) -> bytes:
    encoded = text.upper().encode("ascii")
    if len(encoded) > 8:
        raise ValueError(text)
    return encoded.ljust(8, b"\0")


def sidedef(middle: str) -> bytes:
    return struct.pack("<hh8s8s8sH", 0, 0, name8("-"), name8("-"),
                       name8(middle), 0)


def build_map() -> dict[str, bytes]:
    # Player stands at the west end facing an eastward, front-only horde.
    things = struct.pack("<hhhhh", -384, 0, 0, 1, 7)
    vertices_xy = ((-512, -256), (-512, 256), (512, 256), (512, -256))
    vertices = b"".join(struct.pack("<hh", x, y) for x, y in vertices_xy)

    # Clockwise walls place the sector on every linedef's right/front side.
    lines = ((0, 1), (1, 2), (2, 3), (3, 0))
    linedefs = b"".join(struct.pack("<HHHHHHH", a, b, 1, 0, 0, i, 0xFFFF)
                        for i, (a, b) in enumerate(lines))
    sidedefs = b"".join(sidedef("STARTAN3") for _ in lines)

    # Angles are binary-angle 16-bit values: north, east, south, west.
    seg_angles = (16384, 0, -16384, -32768)
    segs = b"".join(struct.pack("<HHhHHh", a, b, angle, i, 0, 0)
                    for i, ((a, b), angle) in enumerate(zip(lines, seg_angles)))
    ssectors = struct.pack("<HH", 4, 0)
    # whd_gen requires a non-empty NODES lump. Both sides of this harmless
    # dummy partition resolve to the arena's sole subsector (0x8000).
    nodes = struct.pack("<hhhhhhhhhhhhHH", 0, 0, 0, 1,
                        -256, 256, -512, 512,
                        -256, 256, -512, 512,
                        0x8000, 0x8000)
    sectors = struct.pack("<hh8s8shhh", 0, 128, name8("FLOOR4_8"),
                          name8("CEIL3_5"), 255, 0, 0)
    reject = b"\0"

    # All 9x5 blocks share one conservative list containing all four walls.
    columns, rows = 9, 5
    shared_offset = 4 + columns * rows
    blockmap_words = [-512, -256, columns, rows]
    blockmap_words += [shared_offset] * (columns * rows)
    blockmap_words += [0, 0, 1, 2, 3, -1]
    blockmap = struct.pack(f"<{len(blockmap_words)}h", *blockmap_words)

    return dict(zip(MAP_NAMES, (things, linedefs, sidedefs, vertices, segs,
                                ssectors, nodes, sectors, reject, blockmap)))


def read_wad(path: Path) -> list[tuple[str, bytes]]:
    data = path.read_bytes()
    magic, count, directory = struct.unpack_from("<4sII", data)
    if magic != b"IWAD":
        raise ValueError("input must be an IWAD")
    lumps: list[tuple[str, bytes]] = []
    for i in range(count):
        offset, size, raw_name = struct.unpack_from("<II8s", data,
                                                    directory + i * 16)
        name = raw_name.rstrip(b"\0").decode("ascii")
        lumps.append((name, data[offset:offset + size]))
    return lumps


def write_wad(path: Path, lumps: list[tuple[str, bytes]]) -> None:
    body = bytearray()
    directory: list[tuple[int, int, str]] = []
    for name, data in lumps:
        offset = 12 + len(body)
        body.extend(data)
        directory.append((offset, len(data), name))
    directory_offset = 12 + len(body)
    result = bytearray(struct.pack("<4sII", b"IWAD", len(lumps),
                                   directory_offset))
    result.extend(body)
    for offset, size, name in directory:
        result.extend(struct.pack("<II8s", offset, size, name8(name)))
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(result)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()

    lumps = read_wad(args.input)
    marker = next((i for i, (name, _) in enumerate(lumps) if name == "E3M9"),
                  None)
    if marker is None:
        raise ValueError("registered DOOM E3M9 marker not found")
    expected = tuple(name for name, _ in lumps[marker + 1:marker + 11])
    if expected != MAP_NAMES:
        raise ValueError(f"unexpected E3M9 map lump order: {expected}")
    custom = build_map()
    for i, name in enumerate(MAP_NAMES, marker + 1):
        lumps[i] = (name, custom[name])
    write_wad(args.output, lumps)
    print(f"wrote {args.output}: {args.output.stat().st_size:,} bytes; "
          "E3M9 is the one-sector fidget arena")


if __name__ == "__main__":
    main()
