# Contributing

Thanks for helping make tiny DOOM stranger and better.

## Before opening a change

- Search existing issues and keep proposals focused on this ESP32-S3 port.
- Never attach, commit, or link commercial WADs, generated WHD images, extracted
  sprites/sounds/music, or DWAP audio banks.
- For vulnerabilities, use the private process in `SECURITY.md`.
- For hardware bugs, include the board revision, power source, firmware commit,
  serial log, WAD edition/hash (not the WAD), and exact reproduction steps.

## Development checks

Use ESP-IDF 5.3.x and build the release profile:

```sh
. /path/to/esp-idf/export.sh
idf.py -C esp-idf build
python -m py_compile tools/*.py
```

When changing the embedded web controller, extract its script and run a syntax
check as CI does:

```sh
awk '/<script>/{on=1;next}/<\/script>/{on=0}on' \
  esp-idf/main/web/index.html > /tmp/tdongle-ui.js
node --check /tmp/tdongle-ui.js
```

Renderer, memory-layout, and game-loop changes should be tested on real
hardware. Report the 5-second FPS telemetry and free/largest heap before and
after a representative stress run. Web-control changes should run
`tools/stress_web_control.py` against the dongle.

## Pull requests

- Keep commits reviewable and explain the embedded-memory tradeoff.
- Update documentation and `CHANGELOG.md` for user-visible behavior.
- Add an SPDX identifier to new source files.
- Preserve upstream copyright notices and license boundaries.
- Confirm that `git status` contains no generated assets or build directories.

By contributing, you agree that your contribution is distributed under the
license already governing the file or component you modify.
