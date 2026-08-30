#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build the browser-audio flash image from an Ultimate DOOM IWAD.

The pack deliberately keeps sound effects as the original unsigned 8-bit DMX
samples.  Music is converted from MUS and rendered once through an OPL2/DMX
pipeline, then stored as independently seekable WebM Opus tracks.  The ESP only
serves byte ranges; decoding and mixing happen on the controller device.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path


MAGIC = b"DWAP"
VERSION = 1
SFX_COUNT = 110
MUSIC_COUNT = 33
ENTRY_SIZE = 16
HEADER_SIZE = 16

# This order is the sfxenum_t/S_sfx order in src/doom/sounds.{h,c}.
SFX_NAMES = (
    "", "pistol", "shotgn", "sgcock", "dshtgn", "dbopn", "dbcls",
    "dbload", "plasma", "bfg", "sawup", "sawidl", "sawful", "sawhit",
    "rlaunc", "rxplod", "firsht", "firxpl", "pstart", "pstop", "doropn",
    "dorcls", "stnmov", "swtchn", "swtchx", "plpain", "dmpain", "popain",
    "vipain", "mnpain", "pepain", "slop", "itemup", "wpnup", "oof",
    "telept", "posit1", "posit2", "posit3", "bgsit1", "bgsit2", "sgtsit",
    "cacsit", "brssit", "cybsit", "spisit", "bspsit", "kntsit", "vilsit",
    "mansit", "pesit", "sklatk", "sgtatk", "skepch", "vilatk", "claw",
    "skeswg", "pldeth", "pdiehi", "podth1", "podth2", "podth3", "bgdth1",
    "bgdth2", "sgtdth", "cacdth", "skldth", "brsdth", "cybdth", "spidth",
    "bspdth", "vildth", "kntdth", "pedth", "skedth", "posact", "bgact",
    "dmact", "bspact", "bspwlk", "vilact", "noway", "barexp", "punch",
    "hoof", "metal", "chgun", "tink", "bdopn", "bdcls", "itmbk", "flame",
    "flamst", "getpow", "bospit", "boscub", "bossit", "bospn", "bosdth",
    "manatk", "mandth", "sssit", "ssdth", "keenpn", "keendt", "skeact",
    "skesit", "skeatk", "radio", "",  # NUM_SFX has one spare legacy slot.
)

# sfx_chgun is a pitch/volume-linked alias of sfx_pistol.
SFX_LINKS = {86: 1}

# This order is musicenum_t/S_music. Ultimate DOOM E4 intentionally reuses
# tracks from the first three episodes and therefore needs no additional IDs.
MUSIC_NAMES = (
    "", "e1m1", "e1m2", "e1m3", "e1m4", "e1m5", "e1m6", "e1m7", "e1m8",
    "e1m9", "e2m1", "e2m2", "e2m3", "e2m4", "e2m5", "e2m6", "e2m7",
    "e2m8", "e2m9", "e3m1", "e3m2", "e3m3", "e3m4", "e3m5", "e3m6",
    "e3m7", "e3m8", "e3m9", "inter", "intro", "bunny", "victor", "introa",
)


def run(command: list[str], *, cwd: Path | None = None) -> None:
    subprocess.run(command, cwd=cwd, check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)


def read_wad(path: Path) -> dict[str, bytes]:
    data = path.read_bytes()
    if data[:4] not in (b"IWAD", b"PWAD"):
        raise ValueError(f"{path} is not a WAD")
    count, directory = struct.unpack_from("<II", data, 4)
    lumps: dict[str, bytes] = {}
    for index in range(count):
        position, size, raw_name = struct.unpack_from(
            "<II8s", data, directory + index * 16)
        name = raw_name.rstrip(b"\0").decode("ascii").upper()
        # WAD lookup semantics select the last lump with a duplicate name.
        lumps[name] = data[position:position + size]
    return lumps


def command(name: str, supplied: Path | None) -> str:
    value = str(supplied) if supplied else shutil.which(name)
    if not value:
        raise RuntimeError(f"required program not found: {name}")
    return value


def duration_ms(ffprobe: str, path: Path) -> int:
    result = subprocess.run(
        [ffprobe, "-v", "error", "-show_entries", "format=duration",
         "-of", "default=nk=1:nw=1", str(path)], check=True,
        capture_output=True, text=True)
    return round(float(result.stdout.strip()) * 1000)


def render_music(mus: bytes, stem: str, work: Path, mus2mid: str,
                 adlmidiplay: str, ffmpeg: str, ffprobe: str,
                 bitrate: int) -> tuple[bytes, int]:
    mus_path = work / f"{stem}.mus"
    mid_path = work / f"{stem}.mid"
    mus_path.write_bytes(mus)
    run([mus2mid, str(mus_path), str(mid_path)])

    # Bank 16 is libADLMIDI's Bobby Prince v1 DMX bank.  A single DOSBox OPL2
    # chip, zero four-operator channels, and the DMX volume model reproduce the
    # Sound Blaster/AdLib presentation instead of modern General MIDI.
    run([adlmidiplay, str(mid_path), "-mono", "-s16", "--emu-dosbox-opl2",
         "-vm", "3", "--gain", "1.0", "16", "1", "0"], cwd=work)
    wav_path = Path(f"{mid_path}.wav")
    opus_path = work / f"{stem}.webm"
    run([ffmpeg, "-hide_banner", "-loglevel", "error", "-i", str(wav_path),
         "-map_metadata", "-1", "-vn", "-c:a", "libopus", "-b:a",
         f"{bitrate}k", "-vbr", "constrained", "-compression_level", "10",
         "-application", "audio", "-f", "webm", str(opus_path)])
    return opus_path.read_bytes(), duration_ms(ffprobe, opus_path)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("wad", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--mus2mid", type=Path)
    parser.add_argument("--adlmidiplay", type=Path)
    parser.add_argument("--ffmpeg", type=Path)
    parser.add_argument("--ffprobe", type=Path)
    parser.add_argument("--music-bitrate", type=int, default=15)
    parser.add_argument("--max-bytes", type=int, default=0x926000)
    parser.add_argument("--manifest", type=Path)
    args = parser.parse_args()

    if len(SFX_NAMES) != SFX_COUNT or len(MUSIC_NAMES) != MUSIC_COUNT:
        raise AssertionError("audio enum table length changed")
    lumps = read_wad(args.wad)
    mus2mid = command("mus2mid", args.mus2mid)
    adlmidiplay = command("adlmidiplay", args.adlmidiplay)
    ffmpeg = command("ffmpeg", args.ffmpeg)
    ffprobe = command("ffprobe", args.ffprobe)

    table_size = HEADER_SIZE + (SFX_COUNT + MUSIC_COUNT) * ENTRY_SIZE
    blobs = bytearray(b"\0" * table_size)
    sfx_entries: list[tuple[int, int, int, int]] = []
    music_entries: list[tuple[int, int, int, int]] = []
    manifest: dict[str, object] = {"version": VERSION, "sfx": {}, "music": {}}

    # Four-byte alignment makes range inspection and direct parsing pleasant.
    def append(payload: bytes) -> tuple[int, int]:
        while len(blobs) & 3:
            blobs.append(0)
        offset = len(blobs)
        blobs.extend(payload)
        return offset, len(payload)

    for sfx_id, name in enumerate(SFX_NAMES):
        source_id = SFX_LINKS.get(sfx_id, sfx_id)
        source_name = SFX_NAMES[source_id]
        lump = lumps.get(f"DS{source_name}".upper()) if source_name else None
        if not lump or len(lump) < 8 or lump[:2] != b"\x03\x00":
            sfx_entries.append((0, 0, 0, 0))
            continue
        rate, sample_count = struct.unpack_from("<HI", lump, 2)
        if sample_count > len(lump) - 8:
            raise ValueError(f"invalid DS{source_name.upper()} sample count")
        # DMX drops the first and last 16 samples. This is observable vanilla
        # behavior retained by Chocolate Doom and avoids padded clicks.
        pcm = lump[8 + 16:8 + sample_count - 16]
        offset, length = append(pcm)
        sfx_entries.append((offset, length, rate, len(pcm)))
        manifest["sfx"][str(sfx_id)] = {
            "name": source_name, "offset": offset, "bytes": length,
            "rate": rate, "samples": len(pcm),
        }

    with tempfile.TemporaryDirectory(prefix="tdoom-web-audio-") as temp:
        work = Path(temp)
        rendered: dict[str, tuple[int, int, int, int]] = {}
        for music_id, name in enumerate(MUSIC_NAMES):
            if not name:
                music_entries.append((0, 0, 0, 0))
                continue
            lump_name = f"D_{name}".upper()
            mus = lumps.get(lump_name)
            if not mus or not mus.startswith(b"MUS\x1a"):
                music_entries.append((0, 0, 0, 0))
                continue
            content_key = hashlib.sha256(mus).hexdigest()
            if content_key not in rendered:
                opus, duration = render_music(
                    mus, name, work, mus2mid, adlmidiplay, ffmpeg, ffprobe,
                    args.music_bitrate)
                offset, length = append(opus)
                rendered[content_key] = (offset, length, duration, 2)
            entry = rendered[content_key]
            music_entries.append(entry)
            manifest["music"][str(music_id)] = {
                "name": name, "offset": entry[0], "bytes": entry[1],
                "durationMs": entry[2], "codec": "webm-opus",
            }

    struct.pack_into("<4sHHHHI", blobs, 0, MAGIC, VERSION, SFX_COUNT,
                     MUSIC_COUNT, ENTRY_SIZE, table_size)
    cursor = HEADER_SIZE
    for entry in sfx_entries + music_entries:
        struct.pack_into("<IIII", blobs, cursor, *entry)
        cursor += ENTRY_SIZE

    if len(blobs) > args.max_bytes:
        raise RuntimeError(
            f"audio pack is {len(blobs):,} bytes; limit is {args.max_bytes:,}")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(blobs)
    manifest.update({"bytes": len(blobs), "limit": args.max_bytes,
                     "sha256": __import__("hashlib").sha256(blobs).hexdigest()})
    if args.manifest:
        args.manifest.parent.mkdir(parents=True, exist_ok=True)
        args.manifest.write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"wrote {args.output}: {len(blobs):,}/{args.max_bytes:,} bytes, "
          f"{len(manifest['sfx'])} SFX, {len(manifest['music'])} music IDs")


if __name__ == "__main__":
    main()
