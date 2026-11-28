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
xbps-install -y bash make git kmod xz lzo outils dosfstools e2fsprogs gcc curl   # gcc: cybervis

step "Building Auxo packages"
"$SRC/packages/build-xbps.sh" "$OUT/repo"

step "Getting void-mklive ($MKLIVE_REF)"
MK=$OUT/void-mklive
if [[ ! -d $MK/.git ]]; then git clone "$MKLIVE_REPO" "$MK"; fi
git config --global --add safe.directory "$MK"   # the checkout may belong to the host user
git -C "$MK" fetch --quiet origin
git -C "$MK" checkout --quiet "$MKLIVE_REF"
# smaller ISO: 1 MB squashfs blocks + the x86 filter (xz) compress the live system ~5-8% better
# than mklive's defaults. Patched after checkout so a plain mklive update can't silently undo it.
sed -i 's|-comp "${SQUASHFS_COMPRESSION}"|-comp "${SQUASHFS_COMPRESSION}" -b 1M -Xbcj x86|' "$MK/mklive.sh"
grep -q -- '-b 1M -Xbcj x86' "$MK/mklive.sh" || echo "warning: couldn't tune mklive's squashfs options" >&2
# Void's autoinstaller (unused by Auxo) needs networking in the initramfs; without it the live
# initramfs can drop network + GPU firmware (see mklive/postsetup.sh) and shrink by ~200 MB
sed -i 's|--force-add "vmklive autoinstaller"|--force-add "vmklive"|' "$MK/mklive.sh"
if ! grep -q -- '--force-add "vmklive"' "$MK/mklive.sh"; then
  echo "warning: mklive's dracut call changed; keeping networking in the live initramfs" >&2
  export AUXO_KEEP_INITRD_NET=1
fi

# mklive copies the overlay with "cp -p", which keeps the *source* owner on existing
# folders like /etc and /usr. Give it a root-owned copy so the image stays root-owned.
INCLUDE=$(mktemp -d)
cp -a "$SRC/mklive/include/." "$INCLUDE/"
chown -R root:root "$INCLUDE"
find "$INCLUDE" -type d -exec chmod 755 {} +
find "$INCLUDE" -type f -exec chmod go-w {} +
# Hyprland, built by Auxo (Void doesn't package it): a local repository on the live system, so
# the installer and auxo-tweak can install it without a signing key (Void's own packages it
# needs still come from the internet)
if [[ ! -f $OUT/hyprland-x86_64.tar ]]; then   # local builds: Auxo's newest published Hyprland packages
  echo "getting Auxo's Hyprland packages (releases/latest/download/hyprland-x86_64.tar)"
  curl -fL --retry 3 -o "$OUT/hyprland-x86_64.tar" \
    https://github.com/rxvy-dev/AUXO/releases/latest/download/hyprland-x86_64.tar || rm -f "$OUT/hyprland-x86_64.tar"
fi
if [[ -f $OUT/hyprland-x86_64.tar ]]; then
  mkdir -p "$INCLUDE/var/lib/auxo/hyprland-repo"
  tar -xf "$OUT/hyprland-x86_64.tar" -C "$INCLUDE/var/lib/auxo/hyprland-repo"
  [[ -f $INCLUDE/var/lib/auxo/hyprland-repo/x86_64-repodata ]] || xbps-rindex -a "$INCLUDE"/var/lib/auxo/hyprland-repo/*.xbps
  chown -R root:root "$INCLUDE/var/lib/auxo"
  echo "Hyprland packages on the ISO: $(ls "$INCLUDE"/var/lib/auxo/hyprland-repo/*.xbps | wc -l)"
else
  # 5.0: the live session IS Hyprland, so the ISO can't be built without Auxo's Hyprland packages
  echo "error: no out/hyprland-x86_64.tar (run the 'Build Hyprland packages' workflow first)" >&2
  exit 1
fi

# leftovers of an interrupted mklive run (their mounts were already released by build.sh)
rm -rf --one-file-system "$MK"/mklive-build.* 2>/dev/null || true

step "Building the live ISO (this takes a while)"
PKGS=$(sed 's/#.*//' "$SRC/mklive/packages.txt" | xargs)
SERVICES=$(grep -v '^\s*#' "$SRC/mklive/services.txt" | xargs)
cd "$MK"
./mklive.sh -a x86_64 \
  -r "$OUT/repo" \
  -r "$INCLUDE/var/lib/auxo/hyprland-repo" \
  -p "$PKGS" \
  -S "$SERVICES" \
  -I "$INCLUDE" \
  -x "$SRC/mklive/postsetup.sh" \
  -e /bin/bash \
  -T "Auxo Linux" \
  -o "$OUT/$ISO"

cd "$OUT"
sha256sum "$ISO" > "$ISO.sha256"
if [[ -n ${HOST_UID:-} ]]; then  # hand the results (not the mklive checkout) back to the host user
  chown "$HOST_UID:${HOST_GID:-$HOST_UID}" "$OUT" "$OUT/$ISO" "$OUT/$ISO.sha256"
  chown -R "$HOST_UID:${HOST_GID:-$HOST_UID}" "$OUT/repo"
fi
step "Done: out/$ISO"
size=$(stat -c%s "$OUT/$ISO")
printf 'ISO size: %s MB%s\n' "$((size/1048576))" "$( ((size > 2000000000)) && echo '  (over 2 GB: the release will be split)')"
ls -lh "$OUT/$ISO"
