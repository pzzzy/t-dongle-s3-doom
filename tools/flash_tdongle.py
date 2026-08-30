#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Prepare private WAD-derived assets and flash a T-Dongle-S3 release."""

from __future__ import annotations

import argparse
import importlib.util
import subprocess
import sys
import tempfile
from contextlib import nullcontext
from pathlib import Path


APP_LIMIT = 0x110000
WHD_LIMIT = 0x5BA000
AUDIO_LIMIT = 0x926000


def require_file(path: Path, label: str, limit: int | None = None) -> Path:
    path = path.expanduser().resolve()
    if not path.is_file():
        raise SystemExit(f"{label} not found: {path}")
    if limit is not None and path.stat().st_size > limit:
        raise SystemExit(
            f"{label} is {path.stat().st_size:,} bytes; partition limit is "
            f"{limit:,} bytes"
        )
    return path


def run(command: list[str]) -> None:
    print("+", " ".join(command), flush=True)
    subprocess.run(command, check=True)


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Generate a private fidget WHD from your IWAD and flash it"
    )
    parser.add_argument("wad", type=Path, help="registered DOOM IWAD with E3M9")
    parser.add_argument("--port", help="serial port; required unless --dry-run")
    parser.add_argument("--firmware-dir", type=Path, required=True,
                        help="directory extracted from the GitHub firmware ZIP")
    parser.add_argument("--whd-gen", type=Path, required=True,
                        help="locally built whd_gen executable")
    parser.add_argument("--audio-pack", type=Path,
                        help="optional private pack from build_web_audio_pack.py")
    parser.add_argument("--output-dir", type=Path,
                        help="retain generated fidget WAD/WHD in this directory")
    parser.add_argument("--baud", type=int, default=921600)
    parser.add_argument("--dry-run", action="store_true",
                        help="prepare and validate assets without touching hardware")
    args = parser.parse_args()

    if not args.dry_run and not args.port:
        parser.error("--port is required unless --dry-run is used")

    repo_tools = Path(__file__).resolve().parent
    wad = require_file(args.wad, "IWAD")
    whd_gen = require_file(args.whd_gen, "whd_gen")
    firmware = args.firmware_dir.expanduser().resolve()
    bootloader = require_file(firmware / "bootloader.bin", "bootloader")
    partitions = require_file(firmware / "partition-table.bin", "partition table")
    app = require_file(firmware / "tdongle_doom.bin", "application", APP_LIMIT)
    audio = (require_file(args.audio_pack, "audio pack", AUDIO_LIMIT)
             if args.audio_pack else None)

    if args.output_dir:
        args.output_dir.mkdir(parents=True, exist_ok=True)
        workspace = nullcontext(args.output_dir.resolve())
    else:
        workspace = tempfile.TemporaryDirectory(prefix="tdongle-doom-")

    with workspace as work_value:
        work = Path(work_value)
        fidget_wad = work / "doom1-fidget.wad"
        fidget_whd = work / "doom1-fidget.whd"
        run([sys.executable, str(repo_tools / "build_fidget_wad.py"),
             str(wad), str(fidget_wad)])
        run([str(whd_gen), str(fidget_wad), str(fidget_whd),
             "-no-super-tiny"])
        require_file(fidget_whd, "generated WHD", WHD_LIMIT)

        print(
            f"validated: app={app.stat().st_size:,}/{APP_LIMIT:,}, "
            f"WHD={fidget_whd.stat().st_size:,}/{WHD_LIMIT:,} bytes"
        )
        if audio:
            print(f"validated: audio={audio.stat().st_size:,}/{AUDIO_LIMIT:,} bytes")
        if args.dry_run:
            print("dry run complete; no device was modified")
            return

        if importlib.util.find_spec("esptool") is None:
            raise SystemExit("esptool is missing; run: python -m pip install esptool")
        command = [
            sys.executable, "-m", "esptool", "--chip", "esp32s3",
            "--port", args.port, "--baud", str(args.baud),
            "--before", "default_reset", "--after", "hard_reset",
            "write_flash", "--flash_mode", "dio", "--flash_freq", "80m",
            "--flash_size", "16MB",
            "0x0", str(bootloader),
            "0x8000", str(partitions),
            "0x10000", str(app),
            "0x120000", str(fidget_whd),
        ]
        if audio:
            command.extend(("0x6DA000", str(audio)))
        run(command)
        print("flash complete; the IWAD and generated assets remained local")


if __name__ == "__main__":
    main()
