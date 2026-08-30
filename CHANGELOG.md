# Changelog

This project follows [Semantic Versioning](https://semver.org/).

## [1.4.0] — 2026-08-30

### Added

- Native one-button fidget horde using a custom, generated E3M9 arena.
- Kill-driven pistol, shotgun, chaingun, rocket, plasma, and BFG progression.
- Bounded monster/corpse lifecycle for indefinite embedded operation.
- Horde state, kill count, and weapon telemetry in the web controller.
- Reproducible bring-your-own-WAD map, audio, and flashing tools.

### Changed

- Mobile control transport now acknowledges every input frame, uses a 1.2 s
  firmware input lease, and reconnects within a bounded 100–1000 ms window.
- Browser audio lazily loads SFX and cooperatively streams music in 8 KiB ranges
  so synchronous low-memory HTTP serving cannot starve control traffic.
- Phone layout, safe areas, touch cancellation, and Safari audio lifecycle were
  tightened for current iPhones.

### Fixed

- Stuck directional input after pointer cancellation or a dropped socket.
- Multi-second control recovery after Safari reloads.
- Safari audio activation and foreground resume behavior.
- Low-resolution menu, status bar, weapon placement, clipping, and palette
  artifacts accumulated during the ESP32-S3 renderer port.

[1.4.0]: https://github.com/pzzzy/t-dongle-s3-doom/releases/tag/v1.4.0
