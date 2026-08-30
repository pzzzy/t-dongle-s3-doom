# Firmware-only release bundle

These binaries contain the open-source ESP32-S3 application and its ESP-IDF
bootloader/partition table. They intentionally contain no DOOM WAD, generated
WHD, sprites, sounds, or music.

Use `tools/flash_tdongle.py` from the source repository with your legally
obtained registered DOOM IWAD. Full instructions are in the repository README:

https://github.com/pzzzy/t-dongle-s3-doom#install-a-release

Files:

- `bootloader.bin` → flash offset `0x000000`
- `partition-table.bin` → flash offset `0x008000`
- `tdongle_doom.bin` → flash offset `0x010000`
- `SHA256SUMS` → integrity hashes for the three binaries

The application expects a user-generated WHD at `0x120000`; phone audio is
optional and expects a user-generated DWAP bank at `0x6DA000`.
