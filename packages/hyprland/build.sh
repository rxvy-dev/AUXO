#!/bin/sh
# Builds Hyprland 0.49 and its libraries against today's Void Linux, into one archive:
#   out/hyprland/hyprland-x86_64.tar   (the .xbps files + a repository index)
#
# Runs as root in Void's container (see .github/workflows/build-hyprland.yml). Void doesn't
# package Hyprland; the community repo Auxo used (hyprland-void) stopped being rebuilt in May 2025,
# so its packages no longer install on current Void (libjxl, libdisplay-info and hyprutils moved
# on). These templates are hyprland-void's (BSD-2-Clause, see COPYING), adapted:
#   - hyprutils 0.7 is renamed hyprutils07, so it lives next to Void's newer hyprutils
#   - sdbus-c++ and tomlplusplus come from Void now
#   - revision 50, so these win over any leftover hyprland-void packages
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=${WORK:-/work}
OUT=${OUT:-$HERE/../../out/hyprland}
ORDER="libspng glaze hyprutils07 hyprland-protocols hyprlang hyprcursor hyprgraphics aquamarine
       hyprland-qt-support hyprland-qtutils hyprland hyprlock hypridle hyprpaper xdg-desktop-portal-hyprland"

id builder >/dev/null 2>&1 || useradd -G xbuilder -M builder
mkdir -p "$WORK" "$OUT"
if [ ! -d "$WORK/void-packages/.git" ]; then
  git clone --depth 1 https://github.com/void-linux/void-packages.git "$WORK/void-packages"
fi
VP=$WORK/void-packages
cp -r "$HERE"/srcpkgs/. "$VP/srcpkgs/"
cat "$HERE/shlibs" >> "$VP/common/shlibs"
chown -R builder:builder "$WORK"
cd "$VP"
sudo -Eu builder common/travis/set_mirror.sh
sudo -Eu builder common/travis/prepare.sh
common/travis/fetch-xtools.sh

for p in $ORDER; do
  echo "::group::Building $p"
  sudo -Eu builder ./xbps-src -j"$(nproc)" -s -H "$WORK/hostdir" pkg "$p"
  echo "::endgroup::"
done

rm -rf "$OUT/pkgs" && mkdir -p "$OUT/pkgs"
cd "$WORK/hostdir/binpkgs"
for p in $ORDER hyprland-devel; do
  cp "$p"-[0-9]*.x86_64.xbps "$p"-[0-9]*.noarch.xbps "$OUT/pkgs/" 2>/dev/null || true
done
cd "$OUT/pkgs"
ls -l
xbps-rindex -a ./*.xbps
tar -cf "$OUT/hyprland-x86_64.tar" -C "$OUT/pkgs" .
echo "built: $OUT/hyprland-x86_64.tar ($(du -h "$OUT/hyprland-x86_64.tar" | cut -f1))"
