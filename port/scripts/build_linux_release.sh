#!/usr/bin/env bash
# Portable Linux release: SDL3 and the C++ runtime built in, so the only
# system needs are glibc (2.35+), OpenGL and X11/Wayland libraries.
#
#   scripts/build_linux_release.sh      -> dist/hercules-<VERSION>-linux-x64.zip
#                                          (dist/hercules-<VERSION>-linux-arm64.zip on aarch64)
#
# Needs generated/ (psxrecomp_cli.py generate) and the Linux recompiler in
# build-recompiler/ (psxrecomp/tools/ci/build_emitters.sh) for the bundled
# overlay toolchain.
#
# Env:
#   HERC_MAIN      release staged at the zip root (default SCES-00893, the
#                  German build in this folder; another id builds versions/<id>)
#   HERC_VERSIONS  the other releases, staged as lang/<id>/ (default: all four;
#                  set but empty: none)
#   HERC_ALLOW_MISSING_VERSIONS=1
#                  skip a release whose generated/ C is missing instead of
#                  failing (a build from fewer discs than all five)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

case "$(uname -m)" in
  x86_64|amd64)  ARTIFACT=linux-x64 ;;
  aarch64|arm64) ARTIFACT=linux-arm64 ;;
  *) echo "error: unsupported architecture $(uname -m)" >&2; exit 1 ;;
esac

# static SDL3 from the retcomm clang toolchain (building SDL from source would
# need the X11/Wayland/audio development packages). That toolchain is published
# for x86_64 only; elsewhere the framework's pinned SDL3 is built statically
# from source, and a system SDL3 is ignored so the binary does not need one.
TC="${RETCOMM_TOOLCHAIN:-$HOME/.local/share/retcomm/toolchains/cmake-clang-v1/latest}"
if [[ "$ARTIFACT" == linux-x64 ]]; then
  SDL_ARGS=(-DSDL3_DIR="$TC/deps/lib/cmake/SDL3")
else
  SDL_ARGS=(-DCMAKE_DISABLE_FIND_PACKAGE_SDL3=ON -DPSX_SDL3_FETCH=ON)
fi

MAIN="${HERC_MAIN:-SCES-00893}"
if [[ "$MAIN" == SCES-00893 ]]; then MAIN_SRC=.; else MAIN_SRC="versions/$MAIN"; fi
[[ -f "$MAIN_SRC/game.toml" ]] || { echo "error: no release $MAIN ($MAIN_SRC/game.toml)" >&2; exit 1; }
BOOT="SCES_${MAIN#SCES-}"; BOOT="${BOOT:0:8}.${BOOT:8}"
[[ -f "$MAIN_SRC/generated/${BOOT}_dispatch.c" ]] || {
  echo "error: $MAIN has no generated C ($MAIN_SRC/generated) -- run psxrecomp_cli.py generate with its disc" >&2
  exit 1
}

cmake -S "$MAIN_SRC" -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release \
  "${SDL_ARGS[@]}" \
  -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++"
cmake --build build-linux --target psx-runtime

# The other PAL releases, each its own build (versions/<id>/CMakeLists.txt; the
# German one is this folder); the archive carries them as lang/<id>/.
EXTRA_BUILDS=""
for id in ${HERC_VERSIONS-SCES-00891 SCES-00892 SCES-00894 SCES-00895}; do
  [[ "$id" != "$MAIN" ]] || continue
  if [[ "$id" == SCES-00893 ]]; then src=.; else src="versions/$id"; fi
  boot="SCES_${id#SCES-}"; boot="${boot:0:8}.${boot:8}"
  if [[ ! -f "$src/generated/${boot}_dispatch.c" ]]; then
    if [[ "${HERC_ALLOW_MISSING_VERSIONS:-0}" == 1 ]]; then
      echo "note: skipping $id (no generated C in $src/generated)"
      continue
    fi
    echo "error: $id has no generated C ($src/generated); set HERC_ALLOW_MISSING_VERSIONS=1 to skip it" >&2
    exit 1
  fi
  cmake -S "$src" -B "build-linux-$id" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    "${SDL_ARGS[@]}" \
    -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++"
  cmake --build "build-linux-$id" --target psx-runtime
  mkdir -p "build-linux-$id/assets/fonts" "build-linux-$id/assets/img"
  EXTRA_BUILDS+=" build-linux-$id"
done
mkdir -p build-linux/assets/fonts build-linux/assets/img
HERC_EXTRA_BUILDS="$EXTRA_BUILDS" RELEASE_VERSION="$(cat VERSION)" bash scripts/package_release.sh build-linux "$ARTIFACT" build-recompiler
