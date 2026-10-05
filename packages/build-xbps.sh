#!/usr/bin/env bash
# Build Auxo's own xbps packages (auxo-tools, auxo-installer) into a local repository.
#
#   packages/build-xbps.sh [OUTDIR]          default OUTDIR: out/repo
#   STAGE_ONLY=1 packages/build-xbps.sh DIR  only lay out the package files (no xbps needed; used by the tests)
#
# Needs xbps-create and xbps-rindex (a Void system or the build container).
set -euo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-$HERE/out/repo}
VERSION=$(cat "$HERE/VERSION")
REV=1
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$OUT"

# ── auxo-tools ─────────────────────────────────────────────────────────
src=$HERE/packages/auxo-tools/files
d=$WORK/auxo-tools
install -d "$d"/usr/{bin,lib/auxo,share/{auxo,applications,pixmaps,wallpapers,plasma/look-and-feel}} \
           "$d"/etc/auxo "$d"/etc/kernel.d/post-install "$d"/etc/runit/core-services
find "$src/bin" -maxdepth 1 -type f -exec install -m755 {} "$d/usr/bin/" \;
install -m644 "$src"/lib/*.py "$d/usr/lib/auxo/"
cp -r "$src"/share/auxo/. "$d/usr/share/auxo/"
cp -r "$src"/share/plasma/look-and-feel/. "$d/usr/share/plasma/look-and-feel/"
install -m644 "$src"/share/wallpapers/* "$d/usr/share/wallpapers/"
install -m644 "$src"/share/pixmaps/* "$d/usr/share/pixmaps/"
install -m644 "$src"/applications/*.desktop "$d/usr/share/applications/"
install -m644 "$src/etc/auxo/auxo.conf" "$d/etc/auxo/auxo.conf"
install -m755 "$src/etc/kernel.d/post-install/49-auxo-default-kernel" "$d/etc/kernel.d/post-install/"
# runit has no Plymouth integration: this closes the boot splash at the end of stage 1
install -m644 "$src/etc/runit/core-services/99-auxo-plymouth.sh" "$d/etc/runit/core-services/"
find "$d/usr/share/auxo" -name '*.sh' -exec chmod 755 {} +
find "$d" -name '__pycache__' -prune -exec rm -rf {} +
# xbps runs INSTALL with the target root as working directory; only brand a live system,
# never the build host (images are branded from mklive's post-setup script instead)
cat > "$d/INSTALL" <<'SH'
case "${ACTION:-$1}" in
post) if [ "$PWD" = / ] && [ -x /usr/bin/auxo-branding ]; then /usr/bin/auxo-branding || :; fi ;;
esac
SH

# ── auxo-installer ─────────────────────────────────────────────────────
i=$WORK/auxo-installer
install -d "$i/usr/lib/auxo-installer/auxo_install" "$i/usr/bin"
install -m755 "$HERE/installer/auxo-installer" "$i/usr/lib/auxo-installer/auxo-installer"
install -m644 "$HERE"/installer/auxo_install/*.py "$i/usr/lib/auxo-installer/auxo_install/"
ln -s ../lib/auxo-installer/auxo-installer "$i/usr/bin/auxo-installer"

if [[ ${STAGE_ONLY:-0} == 1 ]]; then
  rm -rf "$OUT/auxo-tools" "$OUT/auxo-installer"
  cp -a "$d" "$OUT/auxo-tools"; cp -a "$i" "$OUT/auxo-installer"
  echo "staged into $OUT"
  exit 0
fi

cd "$OUT"
rm -f auxo-tools-*.xbps auxo-installer-*.xbps
xbps-create -A noarch -n "auxo-tools-${VERSION}_${REV}" \
  -s "Auxo Linux tools: auxo-tweak, auxo-update, auxo-fetch, themes and rices" \
  -D "python3>=0 newt>=0 pciutils>=0 sudo>=0 polkit>=0 xmirror>=0 util-linux>=0" \
  -F "/etc/auxo/auxo.conf" -H "https://auxolinux.com" -l "GPL-3.0-or-later" \
  -m "rxvy <https://github.com/rxvy-dev>" "$d"
xbps-create -A noarch -n "auxo-installer-${VERSION}_${REV}" \
  -s "Auxo Linux text installer" \
  -D "auxo-tools>=0 python3>=0 rsync>=0 gptfdisk>=0 parted>=0 btrfs-progs>=0 dosfstools>=0 xfsprogs>=0 e2fsprogs>=0 grub>=0" \
  -H "https://auxolinux.com" -l "GPL-3.0-or-later" -m "rxvy <https://github.com/rxvy-dev>" "$i"
# -f: re-register even if the version is unchanged (otherwise the index keeps the old checksum)
rm -f "$OUT"/*-repodata
xbps-rindex -f -a "$OUT"/*.xbps
echo "packages in $OUT:"; ls -1 "$OUT"/*.xbps
