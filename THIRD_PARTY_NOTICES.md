# Third-party notices

This repository is a derivative and collective work containing components under
several compatible open-source licenses. Source-file notices remain
authoritative.

- **DOOM source release** — id Software; GNU General Public License version 2
  or later. DOOM is a trademark of id Software/Bethesda/ZeniMax. The current
  source tree and inspected `v1.4.0` firmware archive contain no WAD, WHX, WHD,
  or generated audio pack. A commercial-data-derived `doom1.whx` was present in
  earlier Git history and remains retrievable from those commits; removing it
  from the current branch does not rewrite history or invalidate existing
  clones and forks. No commercial game data should be redistributed from this
  repository.
- **Chocolate Doom** — Chocolate Doom contributors; generally GPL-2.0-or-later.
  See `README-chocolate.md` and notices in `src/`.
- **RP2040 Doom** — Graham Sanderson and contributors. Engine-derived portions
  retain GPL terms; independently written RP2040 support is generally BSD-3-
  Clause. See `README-RP2040.md` and individual files.
- **ESP-IDF** — Espressif Systems and contributors; primarily Apache-2.0 with
  component-specific exceptions and notices supplied by ESP-IDF.
- **usb_host_hid managed component** — Espressif Systems; Apache-2.0. Its
  manifest declares the dependency and ESP-IDF's component manager fetches it
  during a clean build.
- **WebM/Opus, FFmpeg, libADLMIDI, and mus2mid** are build-time tools or formats;
  they are not vendored into release firmware by this repository. Users are
  responsible for complying with the licenses of locally installed tools.

`LICENSE` contains GPL version 2. Independently licensed files do not lose their
original license merely by being stored in this repository.
