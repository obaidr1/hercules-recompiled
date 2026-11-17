#!/usr/bin/env bash
# Thin wrapper around the shared psxrecomp bundled-release packager.
# Autofilled by tools/new_project_layout/setup_project.{sh,ps1}.
#
# Ships the COMPILED game built from this repo's committed generated/ C:
# executable, runtime data, bundled OpenBIOS, mod catalog, overlay toolchain.
# No sources, emitters, CLI, generated C, or BIOS dumps.
#
# Usage:
#   scripts/package_release.sh <build-dir> <artifact-tag> [recompiler-build-dir]
#
# Writes: dist/hercules-<VERSION>-<artifact-tag>.zip
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${1:-}"
ARTIFACT_TAG="${2:-}"
RECOMPILER_BUILD="${3:-build-recompiler}"

if [[ -z "${BUILD_DIR}" || -z "${ARTIFACT_TAG}" ]]; then
  echo "usage: $0 <build-dir> <artifact-tag> [recompiler-build-dir]" >&2
  exit 2
fi

PACKAGER="${ROOT}/psxrecomp/tools/package_game_release.sh"
if [[ ! -f "${PACKAGER}" ]]; then
  echo "error: missing ${PACKAGER} (psxrecomp submodule predates bundled releases -- bump it)" >&2
  exit 1
fi
chmod +x "${PACKAGER}" 2>/dev/null || true

EXTRA=()
# Overlay shard cache. It is compiled FROM THE DISC, so CI (no disc) cannot
# build one: the committed static AOT shard is linked into the executable and
# the bundled overlay_toolchain/ compiles the rest from the player's disc at
# runtime. A developer packaging locally with a cache can ship it instead:
#   PSX_OVERLAY_CACHE_ROOT=/path/to/cache scripts/package_release.sh ...
if [[ -n "${PSX_OVERLAY_CACHE_ROOT:-}" ]]; then
  EXTRA+=(--overlay-cache-root "${PSX_OVERLAY_CACHE_ROOT}")
else
  EXTRA+=(--ship-without-overlay-cache-because \
    "no disc at package time: the committed static AOT shard is compiled in and overlay_toolchain/ autocompiles the rest on the player's machine")
fi
# Extra docs shipped at the zip root (DISC.md tells players which dump works).
for _doc in DISC.md CONTROLS.md; do
  if [[ -f "${ROOT}/${_doc}" ]]; then
    EXTRA+=(--doc "${_doc}")
  fi
done

cd "${ROOT}"
bash "${PACKAGER}" \
  --root "${ROOT}" \
  --build-dir "${BUILD_DIR}" \
  --artifact "${ARTIFACT_TAG}" \
  --zip-prefix hercules \
  --exe-name Hercules_Recompiled \
  --display-name "Hercules Recompiled" \
  --recompiler-build "${RECOMPILER_BUILD}" \
  --version-env RELEASE_VERSION \
  --disc-hint "your legally owned Hercules disc" \
  "${EXTRA[@]}"

# The launcher's automatic HD build (hd/hd_auto.py) and the decomp tools it
# runs: the HD pack is made from the player's disc on the player's PC.
DECOMP_TOOLS=""
for d in "${HERC_DECOMP:-}" "${ROOT}/../decomp" "${ROOT}/../hercules"; do
  if [[ -n "$d" && -f "$d/tools/hd_pack_build.py" ]]; then DECOMP_TOOLS="$d/tools"; break; fi
done
if [[ -z "${DECOMP_TOOLS}" ]]; then
  echo "error: decomp tools not found (set HERC_DECOMP)" >&2
  exit 1
fi
ZIP="${ROOT}/dist/hercules-${RELEASE_VERSION:-$(cat "${ROOT}/VERSION")}-${ARTIFACT_TAG}.zip"
python3 - "${ZIP}" "${ROOT}/hd/hd_auto.py" "${DECOMP_TOOLS}" <<'PY'
import sys, zipfile
from pathlib import Path
zip_path, auto, tools = sys.argv[1], Path(sys.argv[2]), Path(sys.argv[3])
# hd_pack_build.py and everything it starts or imports
TOOLS = ["hd_pack_build.py", "iso_extract.py", "sprites.py", "cut_textures.py", "level_models.py",
         "level_models_tex.py", "level_vram.py", "fullscreen2png.py", "hud_texpack.py", "upscale_model.py",
         "sprite_palettize.py", "sprite_cells.py", "sprite_texpack.py", "level_texpack.py", "xxh3.py"]
with zipfile.ZipFile(zip_path, "a", zipfile.ZIP_DEFLATED) as z:
    z.write(auto, "hd/hd_auto.py")
    z.write(auto.parent.parent / "update" / "updater.py", "update/updater.py")   # in-place updates
    z.write(auto.parent.parent / "update" / "events_post.py", "update/events_post.py")   # event log upload
    for t in TOOLS:
        z.write(tools / t, f"hd/tools/{t}")
print(f"added hd/ ({len(TOOLS)} tools) and update/ to {zip_path}")
PY

# The other PAL releases (the launcher starts the build for the language the
# player picks): HERC_EXTRA_BUILDS="build-linux-SCES-00891 ...", each staged as
# lang/<id>/ (scripts/stage_release_build.sh) and added to the archive.
if [[ -n "${HERC_EXTRA_BUILDS:-}" ]]; then
  STAGE="$(mktemp -d)"
  trap 'rm -rf "${STAGE}"' EXIT
  for b in ${HERC_EXTRA_BUILDS}; do
    bash "${ROOT}/scripts/stage_release_build.sh" "${b}" "${STAGE}"
  done
  python3 - "${ZIP}" "${STAGE}" <<'PY'
import os, sys, zipfile
from pathlib import Path
zip_path, stage = sys.argv[1], Path(sys.argv[2])
n = 0
with zipfile.ZipFile(zip_path, "a", zipfile.ZIP_DEFLATED) as z:
    for f in sorted(stage.rglob("*")):
        rel = f.relative_to(stage).as_posix()
        if f.is_dir():
            if not any(f.iterdir()):                       # lang/<id>/saves/
                z.writestr(zipfile.ZipInfo(rel + "/"), "")
            continue
        info = zipfile.ZipInfo.from_file(f, rel)
        info.compress_type = zipfile.ZIP_DEFLATED
        with open(f, "rb") as src:
            z.writestr(info, src.read())
        n += 1
print(f"added lang/ ({n} files) to {zip_path}")
PY
fi
