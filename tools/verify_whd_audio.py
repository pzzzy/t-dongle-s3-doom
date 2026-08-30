#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Decode one compressed WHD sound and write a reference mono WAV.

This deliberately mirrors the allocation-free decoder used by the ESP32 build,
but parses the real input files independently.  It is useful before flashing an
audio profile and whenever the WHD generator changes.
"""

import argparse
import struct
import wave
from pathlib import Path


STEP_TABLE = (
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
    157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544,
    598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707,
    1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871,
    5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635,
    13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
)
INDEX_TABLE = (-1, -1, -1, -1, 2, 4, 6, 8)


def clip(value: int, low: int, high: int) -> int:
    return max(low, min(value, high))


def wad_lump_index(data: bytes, wanted: str) -> int:
    if data[:4] != b"IWAD":
        raise ValueError("input WAD is not an IWAD")
    count, directory = struct.unpack_from("<II", data, 4)
    wanted_bytes = wanted.upper().encode("ascii")
    for index in range(count):
        name = data[directory + index * 16 + 8:directory + index * 16 + 16]
        if name.rstrip(b"\0").upper() == wanted_bytes:
            return index
    raise ValueError(f"{wanted} is not in the WAD")


def whd_lump(data: bytes, index: int) -> bytes:
    if data[:4] not in (b"IWHD", b"IWHX"):
        raise ValueError("input file is not a WHD")
    count, offsets_at = struct.unpack_from("<II", data, 4)
    if index >= count:
        raise ValueError("WAD and WHD lump indices do not match")
    current, following = struct.unpack_from("<II", data, offsets_at + index * 4)
    padding = current >> 30
    start = current & 0x3FFFFFFF
    end = following & 0x3FFFFFFF
    return data[start:end - padding]


def decode_block(block: bytes) -> list[int]:
    if len(block) < 4 or block[2] > 88 or block[3] != 0:
        raise ValueError("invalid IMA-ADPCM block header")
    sample, index = struct.unpack_from("<hB", block)
    output = [sample]
    for packed in block[4:len(block) - (len(block) - 4) % 4]:
        for code in (packed & 15, packed >> 4):
            step = STEP_TABLE[index]
            delta = step >> 3
            if code & 1:
                delta += step >> 2
            if code & 2:
                delta += step >> 1
            if code & 4:
                delta += step
            if code & 8:
                delta = -delta
            sample = clip(sample + delta, -32768, 32767)
            index = clip(index + INDEX_TABLE[code & 7], 0, 88)
            output.append(sample)
    return output


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("wad", type=Path)
    parser.add_argument("whd", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--lump", default="DSPISTOL")
    args = parser.parse_args()

    index = wad_lump_index(args.wad.read_bytes(), args.lump)
    lump = whd_lump(args.whd.read_bytes(), index)
    if len(lump) < 12 or lump[:2] != b"\x03\x80":
        raise ValueError(f"{args.lump} is not a compressed WHD sound")
    rate = struct.unpack_from("<H", lump, 2)[0]
    samples: list[int] = []
    encoded = lump[8:]
    for offset in range(0, len(encoded), 128):
        samples.extend(decode_block(encoded[offset:offset + 128]))

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(args.output), "wb") as output:
        output.setnchannels(1)
        output.setsampwidth(2)
        output.setframerate(rate)
        output.writeframes(struct.pack(f"<{len(samples)}h", *samples))

    peak = max(map(abs, samples), default=0)
    print(f"{args.lump}: lump={index}, {rate} Hz, {len(encoded)} ADPCM bytes, "
          f"{len(samples)} decoded samples, peak={peak}, output={args.output}")


if __name__ == "__main__":
    main()
