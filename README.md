# Hercules Recompiled

A native PC (Windows and Linux) port of the 1997 PlayStation action game *Disney's
Hercules* (all five PAL releases: English, French, German, Italian, Spanish), plus the decompilation work
behind it. The game logic runs unchanged as statically recompiled code; only
the platform layer is replaced: 1080p, true 16:9, HD sprites, level textures
and HUD, a CRT simulation, save states, and a launcher styled after the
game's own title menus (in German, English, French, Spanish and Italian)
that boots straight into the level you pick.

**This repository contains no game data.** You need your own copy of the
game. Every graphic, sound and code byte of the game is read from your disc
image at build or run time, and everything made from it (extracted files,
disassembly, HD texture packs, launcher artwork) stays out of git. The
prebuilt downloads contain the game's program code translated to x86-64, but
no graphics, sound, video or level data: those still come from your disc.
The optional HD texture packs (upscaled from the game's graphics) are
published separately as the [hd-packs](https://github.com/obaidr1/hercules-recompiled/releases/tag/hd-packs)
release; the launcher can also make them from your disc instead.

Fan project, not affiliated with or endorsed by Disney or Sony.

## Download

Prebuilt for players who don't want to build it themselves:

| System | Download |
|--------|----------|
| Windows 10/11 (x64) | [**hercules-recompiled-windows-x64.zip**](https://github.com/obaidr1/hercules-recompiled/releases/latest/download/hercules-recompiled-windows-x64.zip) |
| Linux (x64, glibc 2.35+: Ubuntu 22.04, Debian 12, Fedora 36 or newer) | [**hercules-recompiled-linux-x64.zip**](https://github.com/obaidr1/hercules-recompiled/releases/latest/download/hercules-recompiled-linux-x64.zip) |

Checksums and release notes: [latest release](https://github.com/obaidr1/hercules-recompiled/releases/latest).

1. Unzip into a folder of its own (the archive has no top-level folder).
2. Start `Hercules_Recompiled.exe` (Windows) or `./Hercules_Recompiled` (Linux).
3. On first start, pick your disc image (`.cue` + `.bin`, `.chd`, `.bin`,
   `.img` or `.iso`) of one of the **PAL releases** SCES-00891 (English),
   -00892 (French), -00893 (German), -00894 (Italian) or -00895 (Spanish).
   The US release (SLUS-01029) is not supported. The launcher's
   language picks the release; it asks for that release's disc and remembers
   it (`disc.cfg`, `disc-<id>.cfg`) next to the program.
4. Choose a level in the launcher. Keyboard, mouse and gamepads work; F11
   toggles fullscreen.
5. In the game, **Esc** (gamepad: **Select + Start**) opens the menu:
   HD on/off, CRT picture, 16:9 or 4:3, volume, the controls and more,
   all switched live. Every key is listed in [CONTROLS.md](port/CONTROLS.md).

Windows shows a SmartScreen warning because the program is not signed:
click **More info → Run anyway**.

**Updates come by themselves** (from 0.5.0 on): on start the launcher checks
for a new release, shows its changelog and installs it on request. Settings,
saves, the disc path and the HD graphics stay. *Options → General → Auto
updates* turns the check off. Coming from 0.4.0 or older: unzip the new
download over your existing folder once, so your HD graphics and saves stay.

**HD graphics are downloaded after your first game** (from 0.5.1 on): the
launcher fetches the pack for your release from the
[hd-packs](https://github.com/obaidr1/hercules-recompiled/releases/tag/hd-packs)
release in the background, about 1.6 GB, checked against SHA256SUMS.txt; a
second language then only needs the difference, a few to 40 MB. The
progress shows on the main menu and under *Options → Graphics → HD
graphics*; once done, HD is switched on.

If that release has no pack for your disc, the launcher **makes the pack on your PC,
from your disc**, at low priority: about 4 hours on a laptop CPU, while you
play in the original graphics. It stops when you close the launcher and
continues on the next start. For this it downloads, once, about 400 MB from the official sources, each
checked against a fixed SHA-256: a portable Python
([python-build-standalone](https://github.com/astral-sh/python-build-standalone)),
PyTorch (CPU), [spandrel](https://github.com/chaiNNer-org/spandrel) and
chaiNNer's image helpers, and the upscaling models from their authors'
releases ([IllustrationJaNai](https://github.com/the-database/MangaJaNai),
CC BY-NC-SA 4.0; [HDcube](https://github.com/Venomalia/HDcube)). It needs
6 GB of free disk space while it runs; afterwards only the finished pack
(in `mods/texture-packs/`) stays next to the program.

## Good to know

- **Speed and frame rate.** From 0.5.5 the PAL releases run at the speed of
  the US release (SLUS-01029) by default: 60 Hz timing, so the game takes 30
  steps and draws 30 pictures per second instead of 25. Music and movies keep
  their speed. On a 60 Hz screen every picture now stays exactly two
  refreshes, so the slight stutter of 25 pictures on 60 Hz is gone. For the
  original PAL speed set *General → Game speed → PAL* in the launcher.
  60 fps is not possible: both the PAL and the US game wait a fixed two video
  frames per step, and generated in-between frames (blending, optical flow,
  AI interpolation) were tried and looked worse than the original pictures.
- **Widescreen.** In 16:9 the game draws the extra width itself: scenery,
  far background and the HUD at the screen edges, and from 0.5.4 also the
  characters and objects in the side margins.
- **Problems?** Open an [issue](https://github.com/obaidr1/hercules-recompiled/issues)
  with the version (shown in the launcher), your disc's ID (SCES-0089x),
  Windows or Linux, and a screenshot if you can.

## Layout

| Folder | What |
|--------|------|
| [`decomp/`](decomp) | Decompilation of the main executable and the overlays (splat + GCC 2.7.2 / maspsx, byte-identical rebuild), file-format notes (levels, sprites, videos), tools for extraction, HD upscaling and texture packs |
| [`port/`](port) | The PC port, built on [psxrecomp](https://github.com/RetroPortingToolKit/psxrecomp): game configuration, game-specific mods (HD sprites, widescreen, level select, skip intro / direct start, NTSC speed) and the launcher |
| `port/psxrecomp` | Submodule: the upstream framework |
| [`port/patches/psxrecomp`](port/patches/psxrecomp) | Twelve framework changes this port uses that are not upstream (F3/F4 debug overlays and texture-pack tracking across save states; CRT simulation in the GL present path; a hook that lets the game build in its own launcher; a hook for the game's own in-game menu with live settings; the keyboard alongside the gamepad for Player 1; extra builds per disc release sharing one toolchain; a clean debug-server shutdown; the CRT simulation on blended frames; freezing the F4 inspector's picture to mark objects; semi-transparent draws keeping the original texture; NTSC timing for a PAL game as a mod option; an aarch64 overlay-toolchain Python for Linux arm64 releases) |

## Getting started

The five **PAL releases** (SCES-00891 to -00895) work: the root of `port/`
builds the German one, `port/versions/<id>/` each of the others
(`cmake -S versions/<id> -B build-<id>`). US and Japanese versions are not
supported. Tested on Linux (Fedora) and Windows (Wine);
`port/scripts/build_windows.sh` cross-builds the Windows release with MinGW-w64.

Needs GCC/G++, CMake ≥ 3.20, Ninja, pkg-config, git, Python 3 with numpy and
Pillow, and for the launcher PyGObject with GTK 4, libadwaita and
WebKitGTK 6.0.

```sh
git clone --recursive https://github.com/obaidr1/hercules-recompiled
cd hercules-recompiled/port
cd psxrecomp && for p in ../patches/psxrecomp/*.patch; do git apply "$p"; done && cd ..
mkdir -p disc   # copy your dump here, Redump names:
                # "Disneys Hercules Action-Spiel (Germany).cue" + ".bin"
./psxrecomp/tools/ci/build_emitters.sh
python3 psxrecomp/psxrecomp_cli.py generate --config game.toml --project-root . \
    --disc "disc/Disneys Hercules Action-Spiel (Germany).cue"
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --target psx-runtime
python3 launcher/build_assets.py --sd   # launcher art from your disc
./hercules-launcher
```

**Linux arm64** (ARM handhelds, single-board computers): one script matches
your disc(s) to their releases, generates, builds and packages
`dist/hercules-<version>-linux-arm64.zip`. It builds natively on an aarch64
Linux machine; on an x86-64 one it runs itself in an arm64 Debian container
(docker or podman with qemu binfmt).

```sh
git clone --recursive https://github.com/obaidr1/hercules-recompiled
cd hercules-recompiled/port
scripts/build_linux_arm64.sh "/path/to/Disneys Hercules Action-Spiel (Germany).cue"
```

**HD graphics** are made from your disc on your PC (first start of the
launcher asks; about 4 hours on a laptop CPU). They need a Python with torch
and spandrel (e.g. the one bundled with [chaiNNer](https://chainner.app)) and
three upscaling models you download yourself (their licences forbid
redistribution), placed in `~/psx-decomp/models/` (or `$HERC_MODELS`):

- `IllustrationJaNai/4x_IllustrationJaNai_V1_DAT2_190k.pth`
- `Models/HDcube/4xHDCube4Compact.pth`
- `Models/HDcube/4xHDcube-Alpha.pth`

`decomp/tools/hd_pack_build.py` runs the same build from the command line.

## Licenses

The framework in `port/psxrecomp` and the patches against it are under the
PolyForm Noncommercial License 1.0.0 (see the framework's `LICENSE`).
Third-party tools the decomp uses (splat, maspsx, m2c, the GCC 2.7.2 PSX
toolchain) are not included here and keep their own licenses. The repository's
own code has no license file yet.
