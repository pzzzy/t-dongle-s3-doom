# Browser audio architecture

## Design target

The ESP32-S3 continues to run the authoritative DOOM 1.9 game and sound
logic. A connected browser is only a renderer: it receives the same eight
channel operations that a native `I_Sound` backend receives, then mixes them
through Web Audio to the phone, tablet, or computer speaker. This keeps audio
decoding and buffering out of the dongle's critically small internal RAM and
does not disturb the 35 Hz game clock or LCD DMA path.

The shipped build uses a 9,589,541-byte `DWAP` bank in the flash partition
named `audio`. It contains:

- 68 available Ultimate DOOM sound effects as the original unsigned 8-bit
  DMX PCM, including vanilla's observable 16-sample trimming at both ends.
- All 32 `musicenum_t` music IDs. Duplicate MUS lumps share one encoded blob,
  leaving 29 unique tracks and about 73 minutes of rendered music.
- Music rendered offline with libADLMIDI's Bobby Prince v1 DMX instrument
  bank, one DOSBox OPL2 chip, the DMX volume model, mono output, and 15 kbit/s
  constrained-VBR WebM Opus. WebM is intentional: Safari supported Opus in
  WebM years before it supported the Ogg container, so this bank works on
  modern iPhones without sacrificing the compact Opus encoding.

## Runtime signal path

`esp_sound_web.c` implements the engine's normal sound and music interface.
It preserves engine channel number, SFX identity, volume, stereo separation,
pitch, stop/replacement operations, music identity, looping, pause/resume,
volume, and elapsed playback time. Sound duration is computed from the WHD
ADPCM metadata so `I_SoundIsPlaying()` retains engine-correct behavior even
though playback occurs elsewhere.

Events cross a fixed, allocation-free 64-entry ring in
`esp_web_control.c`. The game task never performs socket I/O. A queued HTTP
server work item drains binary WebSocket frames, and an overflowing queue
drops its oldest observation so current state wins over stale positional
updates.

The browser builds separate master, music, and SFX gain buses. Each of the
eight SFX channels has explicit left/right gain nodes and a channel merger;
the gains implement DOOM/SDL's linear separation law rather than Web Audio's
equal-power panner. Pitch is `pitch / 127`, and moving-source parameters use a
short smoothing constant to remove browser zipper noise without making the
game feel detached.

Music is fetched one track at a time, decoded by the browser, and started at
the engine-reported elapsed offset. Only one decoded music track is retained.
SFX PCM is fetched lazily on first use, expanded into mono `AudioBuffer`
objects, coalesced when several channels request the same uncached sample, and
then retained. There is no monolithic SFX preload before audio becomes ready.
Large reads are divided into checked 8 KiB HTTP ranges with a 1.8-second
deadline, bounded retry, and an event-loop yield between pieces. This lets the
single low-memory HTTP task service control acknowledgements and status traffic
between media pieces instead of allowing a large music response to monopolize
the server.

On iOS, the `AudioContext` is created and primed synchronously inside the
first pointer gesture. Every later game-control gesture is also an audio
resume opportunity. The browser explicitly suspends audio when the page moves
to the background and, after the next foreground gesture, reloads the desired
track at the engine-reported elapsed position. This handles Safari's
foreground-only audio lifecycle while preserving game/music synchronization.

## WebSocket protocol

Every server-to-browser audio message begins with `0xDA`.

| Type | Payload after magic/type |
|---:|---|
| 0 | reset |
| 1 | channel, SFX id, volume, separation, pitch |
| 2 | channel stop |
| 3 | channel, volume, separation |
| 4 | music id, loop, volume, elapsed milliseconds LE32 |
| 5 | music stop |
| 6 | music pause |
| 7 | music resume |
| 8 | music volume |

A new WebSocket connection receives reset, all currently active SFX channels,
and the current music state. This makes reload/reconnect converge without
restarting the game or the soundtrack.

Each valid four-byte browser input frame receives a one-byte binary `0xAC`
acknowledgement. The client tracks only an actually outstanding frame; if it
remains unacknowledged for 1.8 seconds it tears down that socket and reconnects.
Connection attempts begin after 100 ms and cap at one second, stale socket
callbacks are ignored by generation, and a touch, `pageshow`, or `online`
event can request an immediate link. The firmware separately expires remote
input after 1.2 seconds, so a lost phone can never leave movement held.

## DWAP v1 format

All integers are little-endian. The 16-byte header is `DWAP`, version,
SFX-count, music-count, entry-size, and table-size. It is followed by 110 SFX
entries and 33 music entries, each four `uint32_t` values. SFX values are
offset, byte length, sample rate, and sample count. Music values are offset,
byte length, duration milliseconds, and flags. Blobs are four-byte aligned.

The HTTP endpoint `/audio.pack` supports byte ranges and immutable caching.
The firmware validates the header, table geometry, and every referenced end
offset before exposing the bank.

## Rebuilding the bank

The generator consumes the user's original IWAD directly:

```sh
tools/build_web_audio_pack.py /path/to/DOOM1.WAD \
  outputs/doom1-web-audio.pack \
  --mus2mid /path/to/mus2mid \
  --adlmidiplay /path/to/adlmidiplay \
  --manifest outputs/doom1-web-audio.json
```

It also requires `ffmpeg` and `ffprobe` with libopus. `mus2mid` can be built
from this source tree. `adlmidiplay` is supplied by libADLMIDI. The generator
fails rather than emitting an image larger than the `0x926000` audio
partition.

## Operational notes

- A browser user gesture is required by mobile autoplay policy. Tap `AUDIO` or
  any game control once. `AUDIO…` means assets are loading; `AUDIO ON` means
  the context is running. After returning from another app or locking the
  phone, tap any control once to resume Safari audio.
- The controller uses `viewport-fit=cover`, all four iPhone safe-area insets,
  dynamic viewport height, scroll-contained settings sheets, and 44-point
  minimum interactive targets in both portrait and landscape.
- Master, music, and SFX levels are stored in that browser. Engine music
  volume remains multiplicative and authoritative.
- The AP accepts one station. Multiple tabs on that station may connect, but
  the newest WebSocket is the audio sink.
- This backend is mutually exclusive with the experimental on-dongle PDM
  backend in Kconfig.
- Status polling is single-flight, skipped in the background, limited to a
  900 ms request, and runs every five seconds. It cannot accumulate behind
  audio requests or consume all HTTP sockets.
