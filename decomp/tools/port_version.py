#!/usr/bin/env python3
"""Write the version-specific parts of HerculesRecomp for one PAL version.

Every address the port's mods and game.toml hard-code is listed once below
with the German (SCES-00893) value and the file it lives in. For another
version, tools/version_map.py's map moves it; instruction words the mods
check are read from that version's own files, so they are right by
construction. Outputs, in PORT/versions/<GAME_ID>/:

  hercules_addrs.h   macros for mods/src/*.c (CMake includes this directory)
  game.toml          the German game.toml with id, boot exe, disc, digests and
                     hook/cull lists replaced
  CMakeLists.txt     build entry (cmake -S versions/<ID>); the target itself is
                     the project's hercules_game.cmake. Links mods/, assets/,
                     VERSION and psxrecomp/ to the project root
  game.gen.toml      the same with absolute exe/seeds/out_dir, for code
                     generation (psxrecomp-game resolves relative paths
                     against the enclosing project, i.e. the German one)

    python3 tools/port_version.py --game-id SCES-00893 PORT          # German
    python3 tools/port_version.py --game-id SCES-00891 PORT \\
        --files files-eu --map versions/SCES-00891.map.json \\
        --disc "disc-eu/Disney's Action Game featuring Hercules (Europe).cue"
"""
import argparse
import hashlib
import json
import re
import struct
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
from version_map import lookup, lookup_gp  # noqa: E402

BASES = {"main": (0x80010000, 0x800), "engine1": (0x80038350, 0x800),
         "engine3": (0x80038350, 0x800), "title": (0x80038350, 0x800)}
FILES_OF = {"engine1": "EX/ENGINE1.", "engine3": "EX/ENGINE3.", "title": "EX/TITLE."}
OVERLAY_BASES = {"ov2": 0x800684A0, "ov8": 0x800684A0, "ov9": 0x80074A4C}  # splat vram (German)

# (macro, image, kind, German value). kind: code = function address or
# instruction site; data = data address; gp = gp offset. A code entry ending
# in _SITE also gets <macro>_WORD, the instruction word stored there.
SYMBOLS = [
    # hercules_hd_sprites.c: sprite caches (docs/sprite_format.md)
    ("E1_DECODE_SITE", "engine1", "code", 0x8003FD7C),
    ("E1_STAGING_GP", "engine1", "gp", 0x290C),
    ("E1_QUEUE", "engine1", "data", 0x80072A6C),
    ("E1_FLUSH_SITE", "engine1", "code", 0x8004023C),
    ("E3_DECODE_SITE", "engine3", "code", 0x8003F18C),
    ("E3_STAGING_GP", "engine3", "gp", 0x32C0),
    ("E3_QUEUE", "engine3", "data", 0x80066018),
    ("E3_FLUSH_SITE", "engine3", "code", 0x8003F64C),
    ("TI_DECODE_SITE", "title", "code", 0x8005418C),
    ("TI_STAGING_GP", "title", "gp", 0x3ACC),
    ("TI_QUEUE", "title", "data", 0x8007BD58),  # lui 0x8008 + lo -0x42A8
    ("TI_FLUSH_SITE", "title", "code", 0x8005464C),
    ("DRAW_SYNC", "main", "code", 0x800189E8),
    # hercules_skip_intro.c / hercules_level_select.c
    ("TI_TASK0", "title", "data", 0x80065CB8),
    ("TI_INTRO_HANDLER", "title", "code", 0x8003EF94),
    ("LEVEL", "main", "data", 0x80035048),
    ("DEMO_FLAG_A", "main", "data", 0x8003504E),
    ("DEMO_FLAG_B", "main", "data", 0x80035050),
    # hercules_events.c: the game state achievements are made of
    # (docs/achievements_proposal.md)
    ("EXIT_CODE", "main", "data", 0x8003504B),
    ("LIVES", "main", "data", 0x80035054),
    ("DEATHS", "main", "data", 0x80035055),
    ("DIFFICULTY", "main", "data", 0x80035056),
    ("CONTINUES", "main", "data", 0x80035058),
    ("PW_BITS", "main", "data", 0x80035059),
    ("COINS_A", "main", "data", 0x8003505C),
    ("COINS_B", "main", "data", 0x80035060),
    ("LETTERS", "main", "data", 0x80035064),
    ("MAX_ENERGY", "main", "data", 0x80035066),
    ("ENERGY", "main", "data", 0x8003506C),
    ("POWER", "main", "data", 0x8003506E),
    ("CHEATS", "main", "data", 0x80035078),
    ("TI_PASSWORD", "title", "data", 0x8007BD44),
    # hercules_ntsc_speed.c: SsSetTickMode's PAL store of 50 ticks/s
    ("SND_TICK_PAL_SITE", "main", "code", 0x800211E8),
    # hercules_widescreen.c: HUD helpers, backdrop column loops
    ("E1_HUD_SITE", "engine1", "code", 0x8004A050),
    ("E1_HUD_CHECK_SITE", "engine1", "code", 0x80049FE8),
    ("E1_HUD_LO", "engine1", "code", 0x80049128),
    ("E1_HUD_HI", "engine1", "code", 0x80049F9C),
    ("E3_HUD_SITE", "engine3", "code", 0x80046E04),
    ("E3_HUD_CHECK_SITE", "engine3", "code", 0x80046D9C),
    ("E3_HUD_LO", "engine3", "code", 0x800465B0),
    ("E3_HUD_HI", "engine3", "code", 0x80046D50),
    ("E1_BD_FINE_SITE", "engine1", "code", 0x8003D3B0),
    ("E1_BD_COL_SITE", "engine1", "code", 0x8003D420),
    ("E1_BD_RUN_SITE", "engine1", "code", 0x8003D4AC),
    ("E1_BD_LOOP_SITE", "engine1", "code", 0x8003D5F4),
    ("E3_BD_FINE_SITE", "engine3", "code", 0x8003C27C),
    ("E3_BD_COL_SITE", "engine3", "code", 0x8003C2EC),
    ("E3_BD_RUN_SITE", "engine3", "code", 0x8003C378),
    ("E3_BD_LOOP_SITE", "engine3", "code", 0x8003C4C0),
    # hercules_widescreen.c: panorama sky builders (level overlays)
    ("OV2_SKY", "ov2", "code", 0x80068910),
    ("OV2_SKY_JAL_SITE", "ov2", "code", 0x80068964),
    ("OV2_SKY_ORI_SITE", "ov2", "code", 0x80068998),
    ("OV8_SKY", "ov8", "code", 0x800687E8),
    ("OV8_SKY_JAL_SITE", "ov8", "code", 0x8006883C),
    ("OV8_SKY_ORI_SITE", "ov8", "code", 0x80068870),
    ("OV9_SKY", "ov9", "code", 0x800753E0),
    ("OV9_SKY_JAL_SITE", "ov9", "code", 0x80075434),
    ("OV9_SKY_ORI_SITE", "ov9", "code", 0x80075468),
]

# game.toml: which file each listed German address belongs to.
TOML_LISTS = {
    "mod_function_entry_funcs": [("engine1", 0x8004023C), ("engine3", 0x8003F64C), ("title", 0x8005464C),
                                 ("ov2", 0x80068910), ("ov8", 0x800687E8), ("ov9", 0x800753E0)],
    "mod_instruction_sites": [("engine1", 0x8003FD7C), ("engine3", 0x8003F18C), ("title", 0x8005418C),
                              ("engine1", 0x8004A050), ("engine3", 0x80046E04),
                              ("engine1", 0x8003D3B0), ("engine1", 0x8003D420), ("engine1", 0x8003D4AC),
                              ("engine1", 0x8003D5F4), ("engine3", 0x8003C27C), ("engine3", 0x8003C2EC),
                              ("engine3", 0x8003C378), ("engine3", 0x8003C4C0), ("main", 0x800211E8)],
    "screen_x_sites": [("engine1", 0x800461C0), ("engine1", 0x80046850),
                       ("engine1", 0x8003E6C8), ("engine3", 0x8003DAA8)],
    "slti_sites": [("engine1", 0x80046260), ("engine3", 0x80042FE4), ("engine3", 0x800432C4),
                   ("engine3", 0x800445E8), ("engine3", 0x80042FB4), ("engine3", 0x80042FC8),
                   ("engine3", 0x80043294), ("engine3", 0x800432A8), ("engine3", 0x800445B8),
                   ("engine3", 0x800445CC), ("engine1", 0x80044AD0), ("engine3", 0x800416F8)],
    "slti_lower_sites": [("engine3", 0x80042FFC), ("engine3", 0x800432DC), ("engine3", 0x80044600)],
    "bgez_sites": [("engine1", 0x8004627C), ("engine3", 0x80042FB0), ("engine3", 0x80043290),
                   ("engine3", 0x800445B4)],
    "bltz_sites": [("engine1", 0x80044AB0), ("engine3", 0x800416D8)],
}


CMAKE = """cmake_minimum_required(VERSION 3.20)
# Generated by hercules/tools/port_version.py. {game_id} build entry:
#   cmake -S versions/{game_id} -B build-<lang> -G Ninja -DCMAKE_BUILD_TYPE=Release
get_filename_component(HERC_ROOT "${{CMAKE_CURRENT_SOURCE_DIR}}/../.." ABSOLUTE)
set(PSXRECOMP_ROOT "${{HERC_ROOT}}/psxrecomp" CACHE PATH "Path to psxrecomp")
project(HerculesRecomp LANGUAGES C CXX)
set(HERCULES_VERSION "{game_id}")
include("${{HERC_ROOT}}/hercules_game.cmake")
"""


class Version:
    def __init__(self, files, vmap):
        self.files, self.map = files, vmap

    def code(self, image, addr):
        if self.map is None:
            return addr
        r = lookup(self.map, image, addr)
        if r is None:
            raise SystemExit(f"{image} {addr:#010x}: not mapped (function changed?)")
        return r

    def data(self, image, addr):
        if self.map is None:
            return addr
        r = lookup(self.map, image, addr)
        if r is not None:
            return r
        # not referenced directly: take the shift of the nearest mapped data
        # on both sides, if they agree
        known = sorted((int(k, 16), int(v, 16)) for k, v in self.map["data"][image].items())
        below = [d for d in known if d[0] < addr][-1:]
        above = [d for d in known if d[0] > addr][:1]
        shifts = {v - k for k, v in below + above}
        if image == "main" and not shifts:
            return addr
        if len(shifts) != 1:
            raise SystemExit(f"{image} {addr:#010x}: data shift unclear {sorted(shifts)}")
        return addr + shifts.pop()

    def gp(self, image, off):
        if self.map is None:
            return off
        r = lookup_gp(self.map, image, off)
        if r is None:
            raise SystemExit(f"{image} gp {off:#x}: not mapped")
        return r

    def word(self, image, addr):
        rel = FILES_OF.get(image)
        if rel is None:                  # main: the boot exe named by SYSTEM.CNF
            cnf = (self.files / "SYSTEM.CNF").read_text(errors="replace")
            rel = re.search(r"cdrom:\\?([A-Z_0-9.]+)", cnf)[1]
        if image in OVERLAY_BASES:
            rel = f"OV/{image.upper()}."
            base = OVERLAY_BASES[image] + (self.map["load_shift"][image] if self.map else 0)
            return struct.unpack_from("<I", (self.files / rel).read_bytes(), addr - base)[0]
        base, hdr = BASES[image]
        return struct.unpack_from("<I", (self.files / rel).read_bytes(), addr - base + hdr)[0]


def header(v, game_id):
    lines = [f"/* Generated by hercules/tools/port_version.py for {game_id}. Do not edit. */",
             "#pragma once", "", f'#define HERC_GAME_ID "{game_id}"']
    for name, image, kind, value in SYMBOLS:
        if kind == "gp":
            lines.append(f"#define HERC_{name} 0x{v.gp(image, value):04X}u")
            continue
        addr = v.code(image, value) if kind == "code" else v.data(image, value)
        lines.append(f"#define HERC_{name} 0x{addr:08X}u")
        if name.endswith("_SITE"):
            lines.append(f"#define HERC_{name[:-5]}_WORD 0x{v.word(image, addr):08X}u")
    return "\n".join(lines) + "\n"


def digests(bin_path):
    data = bin_path.read_bytes()
    return dict(size=len(data), md5=hashlib.md5(data).hexdigest(),
                sha1=hashlib.sha1(data).hexdigest(), crc32=f"{zlib.crc32(data) & 0xFFFFFFFF:08x}")


def game_toml(template, v, game_id, cue):
    t = template
    boot = "SCES_" + game_id[5:8] + "." + game_id[8:]
    t = re.sub(r'(\[game\][^\[]*?id = )"[^"]*"', rf'\1"{game_id}"', t, flags=re.S)
    t = re.sub(r'exe = "disc/[^"]*"', f'exe = "disc/{boot}"', t)
    t = re.sub(r'boot_exe = "[^"]*"', f'boot_exe = "{boot}"', t)
    if cue:
        bin_name = re.search(r'FILE "([^"]+)"', cue.read_text())[1]
        d = digests(cue.parent / bin_name)
        t = re.sub(r'disc = "disc/[^"]*"', f'disc = "disc/{cue.name}"', t)
        t = re.sub(r'bin_name = "[^"]*"', f'bin_name = "{bin_name}"', t)
        t = re.sub(r'cue_name = "[^"]*"', f'cue_name = "{cue.name}"', t)
        t = re.sub(r"known_sizes = \[[^\]]*\]", f"known_sizes = [{d['size']}]", t)
        for k in ("md5", "sha1", "crc32"):
            t = re.sub(rf"known_{k} = \[[^\]]*\]", f'known_{k} = [\n  "{d[k]}",\n]', t)
        # the netplay TOC fingerprint is per disc; drop it until measured
        t = re.sub(r'required_disc_fp = "[^"]*"\n', "", t)
    for key, items in TOML_LISTS.items():
        values = ", ".join(f'"0x{v.code(img, a):08X}"' for img, a in items)
        t, n = re.subn(rf"{key} = \[[^\]]*\]", f"{key} = [{values}]", t)
        if n != 1:
            raise SystemExit(f"game.toml: {key} not found once")
    return t


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("port", type=Path, help="HerculesRecomp checkout")
    ap.add_argument("--game-id", required=True)
    ap.add_argument("--files", type=Path, default=ROOT / "files")
    ap.add_argument("--map", type=Path)
    ap.add_argument("--disc", type=Path, help=".cue of this version (digests, names)")
    a = ap.parse_args()
    v = Version(a.files.resolve(), json.loads(a.map.read_text()) if a.map else None)
    out = a.port / "versions" / a.game_id
    out.mkdir(parents=True, exist_ok=True)
    (out / "hercules_addrs.h").write_text(header(v, a.game_id))
    if a.map:   # the German game.toml stays the project's own
        t = game_toml((a.port / "game.toml").read_text(), v, a.game_id, a.disc)
        (out / "game.toml").write_text(t)
        d = out.resolve()
        t = re.sub(r'^exe = "disc/', f'exe = "{d}/disc/', t, flags=re.M)
        t = re.sub(r'^seeds = "seeds/', f'seeds = "{d}/seeds/', t, flags=re.M)
        t = re.sub(r'^out_dir = "generated"', f'out_dir = "{d}/generated"', t, flags=re.M)
        (out / "game.gen.toml").write_text(t)
        (out / "CMakeLists.txt").write_text(CMAKE.format(game_id=a.game_id))
        for name in ("mods", "assets", "VERSION", "psxrecomp", "build-recompiler"):
            link = out / name
            if not link.exists() and not link.is_symlink():
                link.symlink_to(Path("../..") / name)
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
