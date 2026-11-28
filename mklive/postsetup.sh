#!/bin/sh
# void-mklive post-setup (-x): runs on the live root filesystem before its initramfs is built.
# $1 = path to the ISO's root filesystem
set -e
ROOTFS="$1"
run() { chroot "$ROOTFS" env AUXO_IN_INSTALLER=1 "$@"; }

run auxo-branding
# the live desktop (the login screen's default session comes from this)
mkdir -p "$ROOTFS/etc/auxo"
printf 'DESKTOP=hyprland\nRICE_HYPRLAND=hyprland-win\n' >> "$ROOTFS/etc/auxo/auxo.conf"
# system-wide Auxo defaults (accent, GRUB theme, login screen, MOTD)
run auxo-tweak accent violet --no-grub --no-initramfs || true

# ── the live desktop: Hyprland, Windows style (AUXO 5.0) ──
# mklive creates the live user ("anon", or live.user=NAME) at boot from /etc/skel, so the rices
# are applied to /etc/skel through a throwaway user whose home is /etc/skel.
run useradd -M -d /etc/skel -s /bin/sh auxoskel
# 1. Sway first (the safe-graphics fallback): keep its bar config apart from Hyprland's
run auxo-tweak rice sway --user auxoskel
mkdir -p "$ROOTFS/etc/skel/.config/auxo/sway-waybar"
for f in config.jsonc config style.css; do
  [ -f "$ROOTFS/etc/skel/.config/waybar/$f" ] && mv "$ROOTFS/etc/skel/.config/waybar/$f" "$ROOTFS/etc/skel/.config/auxo/sway-waybar/"
done
[ -f "$ROOTFS/etc/skel/.config/auxo/sway-waybar/config" ] && mv "$ROOTFS/etc/skel/.config/auxo/sway-waybar/config" \
  "$ROOTFS/etc/skel/.config/auxo/sway-waybar/config.jsonc"
# 2. Hyprland in the Windows style on top
run auxo-tweak rice hyprland-win --user auxoskel
run env HOME=/etc/skel AUXO_HOME=/etc/skel auxo-pins refresh --no-reload || true
run userdel auxoskel || true
# the look files name their own folder: point them at the live user's home
grep -rlI '/etc/skel' "$ROOTFS/etc/skel" 2>/dev/null | while read -r f; do sed -i 's|/etc/skel|/home/anon|g' "$f"; done
find "$ROOTFS/etc/skel" -name '*.auxo-bak' -delete
chown -R root:root "$ROOTFS/etc/skel"
# 3. greetd starts the live session by itself (auxo-session falls back to Sway without 3D);
#    after you log out, the normal Auxo login screen appears. The installer rewrites this file.
if [ -f "$ROOTFS/etc/greetd/config.toml" ]; then
  cat >> "$ROOTFS/etc/greetd/config.toml" <<'TOML'

# live ISO only: log the live user straight in (the installer replaces this file)
[initial_session]
command = "auxo-session hyprland"
user = "anon"
TOML
else
  echo "warning: no greetd config: the live session won't start by itself" >&2
fi


# ── sound: start PipeWire at login (Void ships it switched off) — same links as auxo-tweak's audio_setup
for pair in \
  "/etc/pipewire/pipewire.conf.d/10-wireplumber.conf:/usr/share/examples/wireplumber/10-wireplumber.conf" \
  "/etc/pipewire/pipewire.conf.d/20-pipewire-pulse.conf:/usr/share/examples/pipewire/20-pipewire-pulse.conf" \
  "/etc/xdg/autostart/pipewire.desktop:/usr/share/applications/pipewire.desktop" \
  "/etc/alsa/conf.d/50-pipewire.conf:/usr/share/alsa/alsa.conf.d/50-pipewire.conf" \
  "/etc/alsa/conf.d/99-pipewire-default.conf:/usr/share/alsa/alsa.conf.d/99-pipewire-default.conf"; do
  link=${pair%%:*}; target=${pair#*:}
  if [ -e "$ROOTFS$target" ] && [ ! -e "$ROOTFS$link" ]; then
    mkdir -p "$ROOTFS$(dirname "$link")"; ln -s "$target" "$ROOTFS$link"
  elif [ ! -e "$ROOTFS$target" ]; then
    echo "warning: $target missing — sound may not start on the live ISO" >&2
  fi
done

# ── keep the ISO under 2 GB (one download, no split parts) ──
# 1. A small initramfs: mklive builds it with dracut -N, which packs firmware for every graphics
#    and network card. The live USB needs neither before it mounts its own image; drivers load
#    from the full system right after. (The installer deletes this file, so installed systems
#    build their initramfs the normal way.)
mkdir -p "$ROOTFS/etc/dracut.conf.d"
cat > "$ROOTFS/etc/dracut.conf.d/90-auxo-live.conf" <<'CONF'
# Auxo live ISO only — removed by the installer
omit_dracutmodules+=" drm nfs iscsi nbd nvdimm "
CONF
if [ -z "${AUXO_KEEP_INITRD_NET:-}" ]; then
  echo 'omit_dracutmodules+=" network network-manager kernel-network-modules qemu-net "' \
    >> "$ROOTFS/etc/dracut.conf.d/90-auxo-live.conf"
fi
# 2. Firmware for datacenter network cards (servers only; no desktop or laptop uses these).
#    `sudo xbps-install -f linux-firmware-network` puts them back if you ever need them.
fw="$ROOTFS/usr/lib/firmware"
for f in mellanox liquidio qed qlogic netronome cxgb3 cxgb4 bnx2x myri10ge* vxge slicoss tehuti tigon 3com \
         hfi1_* "ql2*.bin*" "cbfw*.bin*" "ct2fw*.bin*" "ctfw*.bin*" "phanfw.bin*" ixp4xx intel/ice; do
  rm -rf "${fw:?}"/$f
done
# 3. Docs and man pages for other languages (English stays; man works as normal)
rm -rf "$ROOTFS"/usr/share/doc/* "$ROOTFS"/usr/share/gtk-doc
for d in "$ROOTFS"/usr/share/man/*/; do
  case "$(basename "$d")" in man[0-9n]*) ;; *) rm -rf "$d" ;; esac
done
