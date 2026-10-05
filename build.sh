#!/usr/bin/env bash
# Build the Auxo Linux live ISO (Void Linux base).
#
#   ./build.sh               build in a Void container with Docker or Podman (works on any Linux)
#   sudo ./build.sh --native build directly on a Void Linux host
#   ./build.sh --clean       delete previous output first
#
# Output: out/auxo-linux-YYYY.MM.DD-x86_64.iso  (+ .sha256)
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
NATIVE=0; CLEAN=0
for a in "$@"; do
  case "$a" in
    --native) NATIVE=1 ;;
    --clean) CLEAN=1 ;;
    -h|--help) sed -n '2,9p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option: $a" >&2; exit 2 ;;
  esac
done
(( CLEAN )) && rm -rf "$HERE/out"
mkdir -p "$HERE/out"

# void-mklive (Void's official ISO builder), pinned to a known-good commit
export MKLIVE_REPO=${MKLIVE_REPO:-https://github.com/void-linux/void-mklive}
export MKLIVE_REF=${MKLIVE_REF:-3aa194bdea9c83de0b5d78fb889a88f000a44fab}

if (( NATIVE )); then
  [[ $EUID -eq 0 ]] || { echo "run with sudo for --native" >&2; exit 1; }
  command -v xbps-install >/dev/null || { echo "--native needs a Void Linux host; drop --native to use Docker" >&2; exit 1; }
  exec "$HERE/scripts/build-iso-inner.sh"
fi

ENGINE=$(command -v docker || command -v podman || true)
[[ -n $ENGINE ]] || {
  echo "Install Docker or Podman (or build on Void with --native)." >&2
  echo "  Debian 13 / MX / Ubuntu: sudo apt install docker.io docker-cli   (the 'docker' command is in docker-cli)" >&2
  echo "  Arch: sudo pacman -S docker     Fedora: sudo dnf install podman" >&2
  exit 1; }
SUDO=""
if [[ $(basename "$ENGINE") == docker && $EUID -ne 0 ]] && ! docker info >/dev/null 2>&1; then SUDO="sudo"; fi
echo ":: building in a Void container ($(basename "$ENGINE"))"
$SUDO "$ENGINE" run --rm --privileged \
  -e MKLIVE_REPO -e MKLIVE_REF -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" \
  -v "$HERE:/src" -w /src \
  ghcr.io/void-linux/void-glibc-full:latest /src/scripts/build-iso-inner.sh
