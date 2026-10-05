#!/usr/bin/env bash
# Build the Auxo Linux live ISO (Void Linux base) — on any Linux distro.
#
#   ./build.sh               build anywhere: downloads Void's static xbps, sets up a small Void
#                            system in out/void-root and builds inside it (asks for sudo)
#   ./build.sh --docker      build in Void's official container instead (Docker or Podman)
#   sudo ./build.sh --native build directly on a Void Linux host
#   ./build.sh --clean       delete previous output first
#
# Needs: sudo, curl or wget, tar, xz, and about 15 GB free.
# Output: out/auxo-linux-YYYY.MM.DD-x86_64.iso  (+ .sha256)
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$HERE/out
ROOT=$OUT/void-root
TOOLS=$OUT/xbps-static
MODE=chroot; CLEAN=0
for a in "$@"; do
  case "$a" in
    --docker) MODE=docker ;;
    --native) MODE=native ;;
    --chroot) MODE=chroot ;;
    --clean) CLEAN=1 ;;
    -h|--help) sed -n '2,11p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option: $a (see --help)" >&2; exit 2 ;;
  esac
done

# void-mklive (Void's official ISO builder), pinned to a known-good commit
export MKLIVE_REPO=${MKLIVE_REPO:-https://github.com/void-linux/void-mklive}
export MKLIVE_REF=${MKLIVE_REF:-3aa194bdea9c83de0b5d78fb889a88f000a44fab}
VOID_MIRROR=${VOID_MIRROR:-https://repo-default.voidlinux.org}

say() { printf '\e[1;35m::\e[0m %s\n' "$*"; }
die() { printf '\e[31merror:\e[0m %s\n' "$*" >&2; exit 1; }

# never leave (or delete through) mounts under out/: the build system's bind mounts of the
# host's /dev, /proc, /sys, and anything a failed void-mklive run left behind
unmount_root() {
  local m
  command -v findmnt >/dev/null || return 0
  while read -r m; do
    [[ -n $m ]] && umount -l "$m" 2>/dev/null || true
  done < <(findmnt -rno TARGET | awk -v r="$OUT/" 'index($0, r) == 1' | sort -r)
}

if (( CLEAN )); then
  if [[ -d $OUT ]]; then
    [[ $EUID -eq 0 ]] || exec sudo env HOST_UID="$(id -u)" HOST_GID="$(id -g)" "$0" "$@"
    unmount_root
    rm -rf --one-file-system "$OUT"
  fi
fi

# ── native: on a Void host ─────────────────────────────────────────────
if [[ $MODE == native ]]; then
  [[ $EUID -eq 0 ]] || die "run with sudo for --native"
  command -v xbps-install >/dev/null || die "--native needs a Void Linux host (plain ./build.sh works anywhere)"
  mkdir -p "$OUT"
  exec "$HERE/scripts/build-iso-inner.sh"
fi

# ── docker / podman ───────────────────────────────────────────────────
if [[ $MODE == docker ]]; then
  ENGINE=$(command -v docker || command -v podman || true)
  [[ -n $ENGINE ]] || die "Docker or Podman isn't installed (plain ./build.sh doesn't need either)"
  mkdir -p "$OUT"
  SUDO=""
  if [[ $(basename "$ENGINE") == docker && $EUID -ne 0 ]] && ! docker info >/dev/null 2>&1; then SUDO="sudo"; fi
  say "building in a Void container ($(basename "$ENGINE"))"
  exec $SUDO "$ENGINE" run --rm --privileged \
    -e MKLIVE_REPO -e MKLIVE_REF -e HOST_UID="${HOST_UID:-$(id -u)}" -e HOST_GID="${HOST_GID:-$(id -g)}" \
    -v "$HERE:/src" -w /src \
    ghcr.io/void-linux/void-glibc-full:latest /src/scripts/build-iso-inner.sh
fi

# ── chroot (default): works on any distro ─────────────────────────────
[[ $(uname -m) == x86_64 ]] || die "building needs an x86_64 machine"
[[ $EUID -eq 0 ]] || { say "building needs root for mounts and chroot — asking sudo"; exec sudo env HOST_UID="$(id -u)" HOST_GID="$(id -g)" "$0" "$@"; }
for t in tar xz chroot mount umount; do command -v "$t" >/dev/null || die "missing '$t' — install it with your package manager"; done
command -v curl >/dev/null || command -v wget >/dev/null || die "install curl or wget"
fetch() { if command -v curl >/dev/null; then curl -fL --retry 3 -o "$2" "$1"; else wget -O "$2" "$1"; fi; }
mkdir -p "$OUT"

# 1. Void's statically linked xbps (runs on any distro)
if [[ ! -x $TOOLS/bin/xbps-install ]]; then
  say "downloading Void's static xbps"
  rm -rf "$TOOLS"; mkdir -p "$TOOLS/bin"
  fetch "$VOID_MIRROR/static/xbps-static-latest.x86_64-musl.tar.xz" "$TOOLS/xbps-static.tar.xz"
  tar -xJf "$TOOLS/xbps-static.tar.xz" -C "$TOOLS"
  # the static tools may be named xbps-install.static etc.; give them their normal names
  while IFS= read -r -d '' f; do
    n=$(basename "$f"); ln -sf "$f" "$TOOLS/bin/${n%.static}"
  done < <(find "$TOOLS" -path "$TOOLS/bin" -prune -o -type f -name 'xbps-*' -perm -u+x -print0)
  [[ -x $TOOLS/bin/xbps-install ]] || die "couldn't unpack static xbps (see $TOOLS)"
fi
export PATH="$TOOLS/bin:$PATH" XBPS_ARCH=x86_64
[[ -d /etc/ssl/certs ]] && export SSL_CERT_DIR=/etc/ssl/certs

trap unmount_root EXIT INT TERM
unmount_root

# 2. a small Void system to build in (made once, reused by later builds)
if [[ ! -f $ROOT/.auxo-ready ]]; then
  say "setting up a Void build system in out/void-root (first build only)"
  mkdir -p "$ROOT/var/db/xbps/keys"
  find "$TOOLS" -path '*/var/db/xbps/keys/*.plist' -exec cp -n {} "$ROOT/var/db/xbps/keys/" \; 2>/dev/null || true
  # answers "yes" if xbps asks to trust Void's repository key
  # ("yes" is cut off when xbps stops reading; with pipefail that must not count as failure)
  { yes y || true; } | xbps-install -S -y -r "$ROOT" -R "$VOID_MIRROR/current" \
    base-container bash coreutils findutils grep sed gawk tar xz git make kmod lzo outils \
    dosfstools e2fsprogs util-linux ca-certificates curl
  touch "$ROOT/.auxo-ready"
fi

# 3. build inside it. The build system's root must itself be a mount point (void-mklive's
#    xbps-uchroot makes "/" private, which fails on a plain folder), and private, so nothing
#    mounted inside spreads to the host.
mount --bind "$ROOT" "$ROOT"
mount --make-rprivate "$ROOT"
for d in dev proc sys; do
  mkdir -p "$ROOT/$d"
  mount --rbind "/$d" "$ROOT/$d"
  mount --make-rslave "$ROOT/$d"
done
mkdir -p "$ROOT/src"
mount --bind "$HERE" "$ROOT/src"
cp -L /etc/resolv.conf "$ROOT/etc/resolv.conf"

say "building inside the Void system"
chroot "$ROOT" /usr/bin/env -i HOME=/root TERM="${TERM:-linux}" PATH=/usr/bin:/usr/sbin \
  MKLIVE_REPO="$MKLIVE_REPO" MKLIVE_REF="$MKLIVE_REF" \
  HOST_UID="${HOST_UID:-}" HOST_GID="${HOST_GID:-}" \
  /bin/bash /src/scripts/build-iso-inner.sh
