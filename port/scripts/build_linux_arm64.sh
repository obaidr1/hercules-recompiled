#!/usr/bin/env bash
# Linux arm64 release from your own PAL disc(s), start to finish:
#
#   scripts/build_linux_arm64.sh <disc.cue|disc.chd> [more discs...]
#     -> dist/hercules-<VERSION>-linux-arm64.zip
#
# Each disc is matched to its release (SCES-00891..00895) by its digests, the
# game C is generated from it, and scripts/build_linux_release.sh builds and
# packages every release you gave a disc for. The German release (SCES-00893)
# is the game folder's own build when present; otherwise the first disc's.
#
# On an aarch64 Linux host (Debian/Ubuntu, Fedora, Arch, or a distrobox on the
# device itself) everything runs natively. On any other host the script runs
# itself inside a debian:bookworm arm64 container (docker or podman; an x86_64
# host needs qemu-user binfmt, e.g. `docker run --privileged --rm
# tonistiigi/binfmt --install arm64`). Building inside bookworm also keeps the
# binary's glibc need at 2.36.
#
# Env:
#   HERC_CONTAINER_IMAGE  container image (default debian:bookworm)
#   HERC_CONTAINER        docker | podman (default: whichever is installed)
#   HERC_NO_CONTAINER=1   build on this host even when it is not aarch64
#   HERC_NO_DEPS=1        do not install build packages
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
REPO="$(cd "$ROOT/.." && pwd)"
cd "$ROOT"

usage() { sed -n '2,24p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-2}"; }
[[ $# -gt 0 ]] || usage 2
[[ "$1" != -h && "$1" != --help ]] || usage 0

# --- Not on aarch64: run this script in an arm64 container ---------------------
if [[ "$(uname -m)" != aarch64 && "$(uname -m)" != arm64 && "${HERC_NO_CONTAINER:-0}" != 1 ]]; then
  ENGINE="${HERC_CONTAINER:-}"
  if [[ -z "$ENGINE" ]]; then
    if command -v podman >/dev/null 2>&1; then ENGINE=podman
    elif command -v docker >/dev/null 2>&1; then ENGINE=docker
    else
      echo "error: this host is $(uname -m) and has neither docker nor podman for an arm64 container" >&2
      exit 1
    fi
  fi
  mounts=(-v "$REPO:/src")
  args=()
  i=0
  for disc in "$@"; do
    [[ -f "$disc" ]] || { echo "error: disc not found: $disc" >&2; exit 1; }
    dir="$(cd "$(dirname "$disc")" && pwd)"
    mounts+=(-v "$dir:/discs/$i:ro")
    args+=("/discs/$i/$(basename "$disc")")
    i=$((i + 1))
  done
  echo "build_linux_arm64: $(uname -m) host, building in an arm64 ${HERC_CONTAINER_IMAGE:-debian:bookworm} container ($ENGINE)"
  exec "$ENGINE" run --rm --platform linux/arm64 "${mounts[@]}" \
    -e HERC_HOST_UID="$(id -u)" -e HERC_HOST_GID="$(id -g)" -e HERC_NO_DEPS="${HERC_NO_DEPS:-0}" \
    -w /src/port "${HERC_CONTAINER_IMAGE:-debian:bookworm}" \
    bash scripts/build_linux_arm64.sh "${args[@]}"
fi

# Files written as root in a container belong to the host user again on exit.
if [[ -n "${HERC_HOST_UID:-}" ]]; then
  trap 'chown -R "$HERC_HOST_UID:$HERC_HOST_GID" "$REPO" 2>/dev/null || true' EXIT
fi

# --- Build packages --------------------------------------------------------------
# SDL3 is built from source (static), so its X11/Wayland/audio headers are
# needed here; the libraries themselves are loaded at run time on the device.
if [[ "${HERC_NO_DEPS:-0}" != 1 ]]; then
  SUDO=""; [[ "$(id -u)" -eq 0 ]] || SUDO=sudo
  if command -v apt-get >/dev/null 2>&1; then
    export DEBIAN_FRONTEND=noninteractive
    $SUDO apt-get update -q
    $SUDO apt-get install -y -q --no-install-recommends \
      build-essential cmake ninja-build pkg-config git ca-certificates curl unzip zip xz-utils \
      python3 python3-numpy python3-pil zlib1g-dev \
      libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev libxfixes-dev \
      libxtst-dev libxkbcommon-dev libwayland-dev wayland-protocols libdecor-0-dev \
      libegl1-mesa-dev libgl1-mesa-dev libgles2-mesa-dev libdrm-dev libgbm-dev \
      libasound2-dev libpulse-dev libpipewire-0.3-dev libudev-dev libdbus-1-dev
  elif command -v dnf >/dev/null 2>&1; then
    $SUDO dnf install -y gcc gcc-c++ cmake ninja-build pkgconf git curl unzip zip xz \
      python3 python3-numpy python3-pillow zlib-devel \
      libX11-devel libXext-devel libXrandr-devel libXcursor-devel libXi-devel libXScrnSaver-devel \
      libXfixes-devel libXtst-devel libxkbcommon-devel wayland-devel wayland-protocols-devel \
      libdecor-devel mesa-libEGL-devel mesa-libGL-devel mesa-libGLES-devel libdrm-devel mesa-libgbm-devel \
      alsa-lib-devel pulseaudio-libs-devel pipewire-devel systemd-devel dbus-devel
  elif command -v pacman >/dev/null 2>&1; then
    $SUDO pacman -S --needed --noconfirm base-devel cmake ninja pkgconf git curl unzip zip xz \
      python python-numpy python-pillow zlib \
      libx11 libxext libxrandr libxcursor libxi libxss libxfixes libxtst libxkbcommon \
      wayland wayland-protocols libdecor mesa libdrm alsa-lib libpulse pipewire systemd-libs dbus
  else
    echo "warning: no apt-get/dnf/pacman; assuming the build packages are installed" >&2
  fi
fi
git config --global --add safe.directory '*' 2>/dev/null || true

# --- Framework patches (0001..0012), once ----------------------------------------
for p in patches/psxrecomp/*.patch; do
  if git -C psxrecomp apply --check -R "../$p" 2>/dev/null; then
    continue                                    # already applied
  fi
  echo "applying $(basename "$p")"
  git -C psxrecomp apply "../$p"
done

# --- Emitters (psxrecomp-game / psxrecomp-bios), native arm64 ---------------------
./psxrecomp/tools/ci/build_emitters.sh

# --- Match each disc to its release and generate its game C ----------------------
IDS="SCES-00893 SCES-00891 SCES-00892 SCES-00894 SCES-00895"
FOUND=""
for disc in "$@"; do
  [[ -f "$disc" ]] || { echo "error: disc not found: $disc" >&2; exit 1; }
  disc="$(cd "$(dirname "$disc")" && pwd)/$(basename "$disc")"
  match=""
  for id in $IDS; do
    if [[ "$id" == SCES-00893 ]]; then dir=.; else dir="versions/$id"; fi
    rc=0
    python3 psxrecomp/psxrecomp_cli.py verify-disc --config "$dir/game.toml" \
      --project-root "$dir" --disc "$disc" >/dev/null 2>&1 || rc=$?
    if [[ $rc -eq 0 ]]; then match="$id"; break; fi
    [[ $rc -eq 3 ]] || { echo "error: verify-disc failed (exit $rc) for $disc against $id" >&2; exit 1; }
  done
  [[ -n "$match" ]] || {
    echo "error: $disc is none of the five PAL releases (SCES-00891..00895). The US/Japanese discs are not supported." >&2
    exit 1
  }
  echo "== $(basename "$disc"): $match"
  if [[ "$match" == SCES-00893 ]]; then dir=.; else dir="versions/$match"; fi
  python3 psxrecomp/psxrecomp_cli.py generate --config "$dir/game.toml" --project-root "$dir" --disc "$disc"
  FOUND+=" $match"
done

# --- Build and package -------------------------------------------------------------
MAIN=""
for id in $FOUND; do [[ "$id" == SCES-00893 ]] && MAIN=SCES-00893; done
[[ -n "$MAIN" ]] || MAIN="$(echo $FOUND | cut -d' ' -f1)"
OTHERS=""
for id in $FOUND; do [[ "$id" == "$MAIN" ]] || OTHERS+=" $id"; done
echo "== main build: $MAIN; others:${OTHERS:- none}"

HERC_MAIN="$MAIN" HERC_VERSIONS="$OTHERS" HERC_ALLOW_MISSING_VERSIONS=1 \
  bash scripts/build_linux_release.sh

ZIP="dist/hercules-$(tr -d '[:space:]' <VERSION)-linux-arm64.zip"
[[ -f "$ZIP" ]] || { echo "error: $ZIP was not produced" >&2; exit 1; }
echo
echo "built $ROOT/$ZIP"
file build-linux/Hercules_Recompiled 2>/dev/null || true
