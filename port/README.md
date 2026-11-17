# Hercules Recompiled: the PC port

This folder is the PC port of *Disney's Hercules* (PS1, all five PAL
releases SCES-00891 to -00895), built on [psxrecomp](https://github.com/RetroPortingToolKit/psxrecomp).
The root builds the German release (SCES-00893), `versions/<id>/` each of the
others; the release downloads carry all five and the launcher starts the one
for the language you pick.
Downloads, what you need and build steps are in the
[main README](../README.md); every key is listed in [CONTROLS.md](CONTROLS.md).

| Folder | What |
|--------|------|
| `game.toml`, `game_options.toml` | the game's configuration for the framework (German release) |
| `versions/<id>/` | the other four PAL releases: their `game.toml`, the addresses the mods use (`hercules_addrs.h`) and a build entry, written by the decomp's `tools/port_version.py` |
| `mods/src`, `mods/preloaded` | the game's mods: HD sprites, widescreen, level select, skip intro / direct start / back to the launcher, NTSC speed, event log |
| `launcher_native/` | the launcher compiled into the game (menus, options, HD build, updates, in-game menu) |
| `launcher/` | the older Python launcher for source builds on Linux |
| `hd/hd_auto.py` | fetches the HD pack for the player's disc (or makes it from the disc when there is none) |
| `update/updater.py` | in-place updates for the release downloads |
| `update/events_post.py` | sends the event log, only if the player agreed (see Privacy in the main README) |
| `android/`, `scripts/build_android.sh` | the Android app (work in progress, not released yet) |
| `scripts/` | release builds for Linux and Windows (MinGW cross-build), all five releases |
| `patches/psxrecomp/` | framework changes this port needs that are not upstream |
