#!/usr/bin/env bash
# Build the Auxo ISO from any Linux host with Docker (Ubuntu, Fedora, WSL2…).
# The build runs in an archlinux:latest container; the ISO lands in ./out
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
IMAGE=${AUXO_IMAGE:-archlinux:latest}

command -v docker >/dev/null || { echo "docker not found"; exit 1; }
mkdir -p "$HERE/out"

docker run --rm --privileged \
  -v "$HERE":/auxo \
  -v auxo-pacman-cache:/var/cache/pacman/pkg \
  -w /auxo "$IMAGE" bash -c '
    set -e
    pacman-key --init >/dev/null
    pacman -Syu --noconfirm --needed archiso base-devel git python librsvg reflector >/dev/null
    reflector --protocol https --latest 20 --sort rate --save /etc/pacman.d/mirrorlist || true
    ./build.sh --work /tmp/auxo-work --out /auxo/out "$@"
  ' _ "$@"

ls -lh "$HERE"/out/*.iso
