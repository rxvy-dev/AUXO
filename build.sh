#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────
#  Auxo Linux ISO builder
#
#  On Arch (or any Arch-based distro):   sudo ./build.sh
#  Anywhere with Docker:                  ./scripts/build-in-docker.sh
#
#  Options:
#    --skip-packages   reuse the local repo from a previous run
#    --clean           wipe work/ and out/ first
#    --work DIR        work directory      (default: ./work)
#    --out DIR         output directory    (default: ./out)
# ─────────────────────────────────────────────────────────────────────
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
WORK="$HERE/work"
OUT="$HERE/out"
SKIP_PKGS=0
CLEAN=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --skip-packages) SKIP_PKGS=1 ;;
    --clean) CLEAN=1 ;;
    --work) WORK=$(realpath -m "$2"); shift ;;
    --out) OUT=$(realpath -m "$2"); shift ;;
    -h|--help) sed -n '2,14p' "$0"; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 1 ;;
  esac
  shift
done

c() { printf '\e[1;38;2;167;139;250m▲\e[0m \e[1m%s\e[0m\n' "$*"; }
die() { printf '\e[31merror:\e[0m %s\n' "$*" >&2; exit 1; }

[[ $EUID -eq 0 ]] || die "run as root (mkarchiso needs it): sudo $0"
[[ -f /etc/arch-release ]] || die "this host isn't Arch-based — use ./scripts/build-in-docker.sh"

REPO="$WORK/repo"
PROFILE="$WORK/profile"
BUILDER=auxobuild

if (( CLEAN )); then c "cleaning"; rm -rf "$WORK" "$OUT"; fi
mkdir -p "$WORK" "$OUT" "$REPO"

c "installing build dependencies"
pacman -Sy --noconfirm --needed archiso base-devel git python librsvg

# ── 1. local package repo: calamares (with packagechooser) + auxo-tools ──
if (( ! SKIP_PKGS )) || [[ ! -f $REPO/auxo.db.tar.gz ]]; then
  id "$BUILDER" &>/dev/null || useradd -m -r -s /bin/bash "$BUILDER"
  rm -f "$REPO"/*.pkg.tar.* "$REPO"/auxo.*

  c "generating branding assets"
  python3 "$HERE/branding/gen-assets.py" >/dev/null 2>&1 || c "(asset generation skipped: using committed assets)"

  PKGROOT=$(mktemp -d "${TMPDIR:-/tmp}/auxo-pkgbuild.XXXX")   # outside $HOME so the build user can read it
  chmod 755 "$PKGROOT"
  for pkg in calamares auxo-tools; do
    c "building package: $pkg"
    src="$HERE/packages/$pkg"
    bdir="$PKGROOT/$pkg"
    rm -rf "$bdir"; mkdir -p "$bdir"
    cp -a "$src/." "$bdir/"
    chown -R "$BUILDER:" "$bdir"
    # install build + runtime deps as root, then build unprivileged
    mapfile -t deps < <(cd "$bdir" && runuser -u "$BUILDER" -- makepkg --printsrcinfo \
      | awk -F' = ' '/^\t(make)?depends = /{print $2}' | sed 's/[<>=].*//' | sort -u)
    (( ${#deps[@]} )) && pacman -S --noconfirm --needed --asdeps "${deps[@]}"
    (cd "$bdir" && runuser -u "$BUILDER" -- makepkg -f --noconfirm --nocheck)
    cp "$bdir"/*.pkg.tar.zst "$REPO/"
  done
  rm -rf "$PKGROOT"
  c "creating local repo"
  repo-add -q "$REPO/auxo.db.tar.gz" "$REPO"/*.pkg.tar.zst
fi

# ── 2. materialise the profile with the repo path filled in ──
c "preparing archiso profile"
rm -rf "$PROFILE"
cp -a "$HERE/archiso" "$PROFILE"
sed -i "s#AUXO_LOCAL_REPO#$REPO#" "$PROFILE/pacman.conf"

# ── 3. build ──
c "running mkarchiso (this takes a while)"
rm -rf "$WORK/iso"
mkarchiso -v -r -w "$WORK/iso" -o "$OUT" "$PROFILE"

iso=$(ls -t "$OUT"/auxo-linux-*.iso | head -n1)
(cd "$OUT" && sha256sum "$(basename "$iso")" > "$(basename "$iso").sha256")
c "done → $iso ($(du -h "$iso" | cut -f1))"
echo "   test it:  ./scripts/test-vm.sh \"$iso\""
