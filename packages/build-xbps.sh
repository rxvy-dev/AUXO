#!/usr/bin/env bash
# Build Auxo's own xbps packages (auxo-tools, auxo-installer) into a local repository.
#
#   packages/build-xbps.sh [OUTDIR]          default OUTDIR: out/repo
#   STAGE_ONLY=1 packages/build-xbps.sh DIR  only lay out the package files (no xbps needed; used by the tests)
#
# Needs xbps-create and xbps-rindex (a Void system or the build container), and a C compiler for cybervis.
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
install -d "$d"/usr/{bin,lib/auxo,share/{auxo,applications,pixmaps,wallpapers}} \
           "$d"/etc/auxo "$d"/etc/kernel.d/post-install "$d"/usr/lib/dracut/dracut.conf.d \
           "$d"/usr/libexec/auxo "$d"/usr/local/share/dbus-1/system-services
find "$src/bin" -maxdepth 1 -type f -exec install -m755 {} "$d/usr/bin/" \;
install -m644 "$src"/lib/*.py "$d/usr/lib/auxo/"
cp -r "$src"/share/auxo/. "$d/usr/share/auxo/"
install -m644 "$src"/share/wallpapers/* "$d/usr/share/wallpapers/"
install -m644 "$src"/share/pixmaps/* "$d/usr/share/pixmaps/"
# AUXO 5.0 app icons (branding/gen-app-icons.py)
install -d "$d/usr/share/icons/hicolor/scalable/apps"
install -m644 "$src"/share/icons/hicolor/scalable/apps/*.svg "$d/usr/share/icons/hicolor/scalable/apps/"
install -m644 "$src"/applications/*.desktop "$d/usr/share/applications/"
install -m644 "$src/etc/auxo/auxo.conf" "$d/etc/auxo/auxo.conf"
install -m755 "$src/etc/kernel.d/post-install/49-auxo-default-kernel" "$d/etc/kernel.d/post-install/"
# no boot splash: keep Plymouth out of every initramfs
install -m644 "$src/dracut/90-auxo-no-plymouth.conf" "$d/usr/lib/dracut/dracut.conf.d/"
# D-Bus must start elogind through runit (Void's wrapper), never as a bare unsupervised copy
install -m755 "$src/libexec/elogind-activate" "$src/libexec/auxo-fetch-repo" "$src/libexec/auxo-bootanim" "$d/usr/libexec/auxo/"
# performance defaults: sysctl + I/O schedulers (overridable from /etc)
install -Dm644 "$src/sysctl/60-auxo-performance.conf" "$d/usr/lib/sysctl.d/60-auxo-performance.conf"
install -Dm644 "$src/udev/60-auxo-ioschedulers.rules" "$d/usr/lib/udev/rules.d/60-auxo-ioschedulers.rules"
# the boot animation starts at the end of runit stage 1
install -d "$d/etc/runit/core-services"
install -m644 "$src/etc/runit/core-services/99-auxo-bootanim.sh" "$d/etc/runit/core-services/"
install -m644 "$src/share/dbus-1/system-services/org.freedesktop.login1.service" \
  "$d/usr/local/share/dbus-1/system-services/"
find "$d/usr/share/auxo" -name '*.sh' -exec chmod 755 {} +
find "$d" -name '__pycache__' -prune -exec rm -rf {} +
# xbps runs INSTALL with the target root as working directory; only brand a live system,
# never the build host (images are branded from mklive's post-setup script instead)
cat > "$d/INSTALL" <<'SH'
case "${ACTION:-$1}" in
post) if [ "$PWD" = / ] && [ -x /usr/bin/auxo-branding ]; then /usr/bin/auxo-branding || :; fi
      # 4.0 early installs: greetd shared tty1 with agetty, so passwords typed at the login screen
      # came out wrong. Move Auxo's login screen to tty7 (takes effect at the next boot).
      if grep -q '^# written by auxo-tweak' etc/greetd/config.toml 2>/dev/null; then
        sed -i 's/^vt = 1$/vt = 7/' etc/greetd/config.toml
      fi
      # 4.0 Sway/i3 configs: a comment after "include …monitors" (Sway and i3 only allow comments
      # on their own line) made them skip your monitor settings, and Sway's one-line touchpad
      # block was rejected. Repair the copies in home folders.
      # no cursor theme was set on Hyprland/Sway/i3: a faint fallback cursor. Adwaita everywhere.
      for f in home/*/.config/hypr/hyprland.conf; do
        [ -f "$f" ] && ! grep -q XCURSOR_THEME "$f" && sed -i 's/^env = XCURSOR_SIZE,24$/env = XCURSOR_THEME,Adwaita\nenv = XCURSOR_SIZE,24\nenv = HYPRCURSOR_SIZE,24/' "$f"
        [ -f "$f" ] && ! grep -q "hyprctl setcursor" "$f" && echo "exec-once = hyprctl setcursor Adwaita 24" >> "$f"
      done
      for f in home/*/.config/sway/config; do
        [ -f "$f" ] && ! grep -q xcursor_theme "$f" && echo "seat seat0 xcursor_theme Adwaita 24" >> "$f"
      done
      for h in home/*; do
        [ -d "$h/.config" ] && [ ! -e "$h/.icons/default/index.theme" ] || continue
        mkdir -p "$h/.icons/default"
        printf '[Icon Theme]\nName=Default\nInherits=Adwaita\n' > "$h/.icons/default/index.theme"
        chown -R --reference="$h" "$h/.icons" 2>/dev/null
      done
      # Hyprland: NVIDIA-only variables were set for everyone and broke OpenGL / video decoding on
      # AMD and Intel. They live in ~/.config/auxo/hyprland-gpu.conf now (NVIDIA only).
      nv=$(sed -n 's/^NVIDIA="\{0,1\}\([a-z0-9]*\).*/\1/p' etc/auxo/auxo.conf 2>/dev/null)
      for f in home/*/.config/hypr/hyprland.conf; do
        [ -f "$f" ] && grep -q '^# NVIDIA (harmless elsewhere)$' "$f" || continue
        sed -i -e '/^# NVIDIA (harmless elsewhere)$/d' -e '/^env = LIBVA_DRIVER_NAME,nvidia$/d' \
               -e '/^env = NVD_BACKEND,direct$/d' \
               -e 's|^env = __GLX_VENDOR_LIBRARY_NAME,nvidia$|source = ~/.config/auxo/hyprland-gpu.conf|' "$f"
        g="${f%/hypr/hyprland.conf}/auxo/hyprland-gpu.conf"
        mkdir -p "$(dirname "$g")"
        if [ -n "$nv" ] && [ "$nv" != nouveau ]; then
          printf 'env = LIBVA_DRIVER_NAME,nvidia\nenv = __GLX_VENDOR_LIBRARY_NAME,nvidia\nenv = NVD_BACKEND,direct\nenv = GBM_BACKEND,nvidia-drm\ncursor {\n    no_hardware_cursors = true\n}\n' > "$g"
        else
          echo '# no GPU-specific settings needed' > "$g"
        fi
        chown --reference="$(dirname "$f")" "$(dirname "$g")" "$g" 2>/dev/null
      done
      # Mac/Windows styles: "pkill -f auxo-launcher || auxo-launcher" killed its own shell, so the
      # Start menu / Launchpad / Control Center never opened. The apps toggle themselves now.
      for f in home/*/.config/hypr/style.conf home/*/.config/waybar/config.jsonc; do
        [ -f "$f" ] && sed -i -E 's/pkill -f (auxo-launcher|auxo-control) \|\| //' "$f"
      done
      for f in home/*/.config/sway/config home/*/.config/i3/config etc/skel/.config/sway/config; do
        [ -f "$f" ] || continue
        sed -i -E 's|^(include ~/\.config/auxo/monitors-(sway\|i3))[[:space:]]+#.*$|\1|' "$f"
        sed -i 's|^input type:touchpad { tap enabled; natural_scroll enabled }$|input type:touchpad {\n    tap enabled\n    natural_scroll enabled\n}|' "$f"
      done ;;
esac
SH

# ── auxo-installer ─────────────────────────────────────────────────────
i=$WORK/auxo-installer
install -d "$i/usr/lib/auxo-installer/auxo_install" "$i/usr/bin"
install -m755 "$HERE/installer/auxo-installer" "$i/usr/lib/auxo-installer/auxo-installer"
install -m644 "$HERE"/installer/auxo_install/*.py "$i/usr/lib/auxo-installer/auxo_install/"
ln -s ../lib/auxo-installer/auxo-installer "$i/usr/bin/auxo-installer"

# ── cybervis (terminal visualizer by rxvy, compiled for x86_64) ───────
c=$WORK/cybervis
CV_VERSION=5.0
install -d "$c/usr/bin" "$c/usr/share/applications"
${CC:-cc} -O3 -pipe -o "$c/usr/bin/cybervis" "$HERE/packages/cybervis/Cybervis.c" -lm -lpthread
strip "$c/usr/bin/cybervis" 2>/dev/null || :
install -m644 "$HERE/packages/cybervis/cybervis.desktop" "$c/usr/share/applications/"

if [[ ${STAGE_ONLY:-0} == 1 ]]; then
  rm -rf "$OUT/auxo-tools" "$OUT/auxo-installer" "$OUT/cybervis"
  cp -a "$d" "$OUT/auxo-tools"; cp -a "$i" "$OUT/auxo-installer"; cp -a "$c" "$OUT/cybervis"
  echo "staged into $OUT"
  exit 0
fi

cd "$OUT"
rm -f auxo-tools-*.xbps auxo-installer-*.xbps cybervis-*.xbps
xbps-create -A noarch -n "auxo-tools-${VERSION}_${REV}" \
  -s "Auxo Linux tools: auxo-tweak, auxo-update, auxo-fetch, themes and rices" \
  -D "python3>=0 newt>=0 pciutils>=0 sudo>=0 polkit>=0 xmirror>=0 util-linux>=0" \
  -F "/etc/auxo/auxo.conf" -H "https://auxolinux.com" -l "GPL-3.0-or-later" \
  -m "rxvy <https://github.com/rxvy-dev>" "$d"
xbps-create -A noarch -n "auxo-installer-${VERSION}_${REV}" \
  -s "Auxo Linux text installer" \
  -D "auxo-tools>=0 python3>=0 rsync>=0 gptfdisk>=0 parted>=0 btrfs-progs>=0 dosfstools>=0 xfsprogs>=0 e2fsprogs>=0 grub>=0" \
  -H "https://auxolinux.com" -l "GPL-3.0-or-later" -m "rxvy <https://github.com/rxvy-dev>" "$i"
xbps-create -A x86_64 -n "cybervis-${CV_VERSION}_${REV}" \
  -s "Terminal spectral engine: matrix rain, plasma, fire, warp and more" \
  -H "https://github.com/rxvy-dev/cybervis" -l "custom" -m "rxvy <https://github.com/rxvy-dev>" "$c"
# -f: re-register even if the version is unchanged (otherwise the index keeps the old checksum)
rm -f "$OUT"/*-repodata
xbps-rindex -f -a "$OUT"/*.xbps
echo "packages in $OUT:"; ls -1 "$OUT"/*.xbps
