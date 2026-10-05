#!/bin/sh
# void-mklive post-setup (-x): runs on the live root filesystem before its initramfs is built.
# $1 = path to the ISO's root filesystem
set -e
ROOTFS="$1"
run() { chroot "$ROOTFS" env AUXO_IN_INSTALLER=1 "$@"; }

run auxo-branding
# system-wide Auxo defaults (accent, GRUB theme, KDE global theme, MOTD) — the live
# user is created at boot by mklive and picks them up from /etc/xdg
run auxo-tweak accent violet --no-grub --no-initramfs || true
