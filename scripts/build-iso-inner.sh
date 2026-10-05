#!/usr/bin/env bash
# Runs inside the Void build container (or on a Void host as root): builds Auxo's
# xbps packages, then the live ISO with void-mklive.
set -euo pipefail
SRC=$(cd "$(dirname "$0")/.." && pwd)
OUT=$SRC/out
DATE=$(date +%Y.%m.%d)
ISO=auxo-linux-$DATE-x86_64.iso
step() { printf '\n\e[1;35m▲ %s\e[0m\n' "$*"; }

step "Preparing the build environment"
xbps-install -Syu xbps
xbps-install -yu
xbps-install -y bash make git kmod xz lzo outils dosfstools e2fsprogs

step "Building Auxo packages"
"$SRC/packages/build-xbps.sh" "$OUT/repo"

step "Getting void-mklive ($MKLIVE_REF)"
MK=$OUT/void-mklive
if [[ ! -d $MK/.git ]]; then git clone "$MKLIVE_REPO" "$MK"; fi
git -C "$MK" fetch --quiet origin
git -C "$MK" checkout --quiet "$MKLIVE_REF"

step "Building the live ISO (this takes a while)"
PKGS=$(grep -v '^\s*#' "$SRC/mklive/packages.txt" | xargs)
SERVICES=$(grep -v '^\s*#' "$SRC/mklive/services.txt" | xargs)
cd "$MK"
./mklive.sh -a x86_64 \
  -r "$OUT/repo" \
  -p "$PKGS" \
  -S "$SERVICES" \
  -I "$SRC/mklive/include" \
  -x "$SRC/mklive/postsetup.sh" \
  -e /bin/bash \
  -T "Auxo Linux" \
  -o "$OUT/$ISO"

cd "$OUT"
sha256sum "$ISO" > "$ISO.sha256"
if [[ -n ${HOST_UID:-} ]]; then chown -R "$HOST_UID:${HOST_GID:-$HOST_UID}" "$OUT"; fi
step "Done: out/$ISO"
ls -lh "$OUT/$ISO"
