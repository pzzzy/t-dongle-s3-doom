# One-button fidget horde

## User model

The first falling edge on the T-Dongle-S3's GPIO0 button leaves normal demo
play and loads a purpose-built E3M9 arena. After the arena appears, the same
button is the complete control scheme: hold or tap it to fire. The player is
fixed in place and facing down one firing lane while a bounded, endlessly
recycled horde advances from the front.

The mode deliberately retains DOOM's real 35 Hz ticker, hitscan/projectile
logic, monster AI, damage states, death animations, weapon cadence, palette,
sound events, and kill accounting. This is not an animation or separate mini
engine; it is a constrained game mode inside the native engine.

## Rules and progression

- God mode and zero momentum are enforced every tic.
- Health is restored to 100, every DOOM 1 weapon is owned, and every ammo pool
  is replenished every tic.
- Movement, strafing, turning, use, menu, and manual weapon bits are removed
  from the final `ticcmd`; only `BT_ATTACK` survives.
- Facing angle and player momentum are fixed each tic, so monster impacts
  cannot slowly move or turn the player.
- Weapon progression is kill-driven: pistol at 0, shotgun at 4, chaingun at
  12, rocket launcher at 28, plasma rifle at 52, and BFG 9000 at 84 kills.
- Difficulty rises from imps/sergeants through cacodemons and barons. The live
  target population grows from three to six.

## Bounded infinite horde

An embedded system cannot leak a thinker for every historical corpse. The
spawner therefore owns exactly eight monster slots. It spawns at most once
every 12 tics, maintains no more than six living monsters, and reclaims a dead
monster after 50 tics. Removed thinkers are detected before reuse. This keeps
the mode indefinitely playable without heap growth or thinker exhaustion.

All spawn points share the player's exact sightline but use staggered
distances. The arrangement makes every weapon useful with no aim input while
preserving collisions and the visual sense of a queue advancing toward the
screen.

## Custom map image

`tools/build_fidget_wad.py` copies the user's registered DOOM IWAD and replaces
only E3M9's ten map lumps with a compact, valid arena. It emits conventional
vertices, linedefs, sidedefs, sector, things, segs, subsector, node, reject,
and blockmap data. The normal `whd_gen` pipeline then converts it into the
low-memory image consumed by the dongle.

```sh
tools/build_fidget_wad.py /path/to/DOOM1.WAD outputs/doom1-fidget.wad
build-native/src/whd_gen/whd_gen \
  outputs/doom1-fidget.wad outputs/doom1-fidget.whd -no-super-tiny
```

The release package contains the generated WHD and the generator, but not the
original copyrighted IWAD.

## Diagnostic trigger and telemetry

For bench testing, lowercase `h` on USB serial takes the same request path as
the GPIO falling edge. `/api/status` exposes `fidget`, `kills`, and `weapon`,
and the mobile controller replaces general heap telemetry with `HORDE`, the
live kill count, and the current weapon while the mode is active.

Hardware validation on the release image held fire for 45 seconds, consumed
380/380 acknowledged WebSocket inputs, progressed from pistol through shotgun
to chaingun (14 kills), retained 35 Hz, and finished with exactly the same free
heap and largest free block with which it began.
