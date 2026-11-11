#!/bin/sh
# Build AuxoBSD's root filesystem from real Ubuntu packages: build/rootfs.tar.
#
# AuxoBSD runs unmodified x86_64 Linux programs (the kernel speaks the Linux system-call
# interface), so the userland is real software: GCC 13, binutils, the C library, make,
# BusyBox for the usual commands, and (with ROOTFS_SET=full) Python 3, GTK 4, GtkSourceView,
# PyGObject and Aether from github.com/rxvy-dev/aether.
#
#   tools/mkrootfs.sh [OUTDIR]          ROOTFS_SET=dev (default) | full
#
# Needs apt-get (an Ubuntu/Debian host or CI runner), dpkg-deb and tar. No root needed.
set -eu
OUT=${1:-build}
SET=${ROOTFS_SET:-dev}
WORK=$OUT/rootfs-work
ROOT=$OUT/rootfs
mkdir -p "$WORK/debs" "$ROOT"

PKGS="gcc cpp binutils libc6-dev make busybox-static"
if [ "$SET" = full ]; then
    PKGS="$PKGS python3 python3-gi gir1.2-gtk-4.0 gir1.2-gtksource-5 libgtk-4-1 libgtksourceview-5-0
          fonts-dejavu-core adwaita-icon-theme gsettings-desktop-schemas shared-mime-info"
fi

echo "rootfs: resolving $SET packages"
# shellcheck disable=SC2086
apt-cache depends --recurse --no-recommends --no-suggests --no-conflicts --no-breaks --no-replaces \
    --no-enhances $PKGS 2>/dev/null | grep -v '^ ' | grep -v '^<' | sort -u > "$WORK/pkgs.txt"
# things a package needs only for its maintainer scripts (we never run those)
grep -vxE 'debconf|perl-base|dpkg|install-info|libdebconfclient0|sensible-utils|ucf|tzdata|base-files|mount|util-linux|libpam.*|login|passwd|adduser|init-system-helpers|systemd.*|libsystemd.*|dbus.*|libdbus.*|lsb-base|coreutils|bash|dash|debianutils|diffutils|findutils|grep|gzip|sed|tar|hostname|ncurses-bin|ncurses-base|libselinux1|libsemanage.*|libaudit.*|libcap-ng0|libacl1|libattr1|libpcre2-8-0|libbz2-1.0|liblzma5|libudev1|libmount1|libblkid1|libsmartcols1|gpgv|libgcrypt20|libgpg-error0|libcap2|libxxhash0|libzstd1-x' \
    "$WORK/pkgs.txt" > "$WORK/want.txt" || true
grep -qx libpcre2-8-0 "$WORK/pkgs.txt" && echo libpcre2-8-0 >> "$WORK/want.txt"      # GLib needs it
grep -qx libselinux1 "$WORK/pkgs.txt" && echo libselinux1 >> "$WORK/want.txt"        # GLib/GIO link it
grep -qx libmount1 "$WORK/pkgs.txt" && echo libmount1 libblkid1 >> "$WORK/want.txt"
grep -qx libbz2-1.0 "$WORK/pkgs.txt" && echo libbz2-1.0 >> "$WORK/want.txt"
grep -qx liblzma5 "$WORK/pkgs.txt" && echo liblzma5 >> "$WORK/want.txt"
grep -qx libffi8 "$WORK/pkgs.txt" && echo libffi8 >> "$WORK/want.txt"
tr ' ' '\n' < "$WORK/want.txt" | sort -u | grep . > "$WORK/want2.txt"
echo "rootfs: $(wc -l < "$WORK/want2.txt") packages"
( cd "$WORK/debs" && xargs apt-get download -q < ../want2.txt >/dev/null 2>&1 || \
  xargs -n1 apt-get download -q < ../want2.txt 2>&1 | grep -i 'unable\|error' || true )
for d in "$WORK"/debs/*.deb; do dpkg-deb -x "$d" "$ROOT"; done

# merged /usr, like Ubuntu: /bin, /lib, /lib64, /sbin are links into /usr
for d in bin lib lib64 sbin; do
    if [ -d "$ROOT/$d" ] && [ ! -L "$ROOT/$d" ]; then cp -a "$ROOT/$d/." "$ROOT/usr/$d/" 2>/dev/null || true; rm -rf "${ROOT:?}/$d"; fi
    mkdir -p "$ROOT/usr/$d"; ln -sfn "usr/$d" "$ROOT/$d"
done
# the gcc package links gcc → gcc-N; cc too
[ -e "$ROOT/usr/bin/cc" ] || ln -sf gcc "$ROOT/usr/bin/cc"
# BusyBox provides sh, ls, cat, grep, vi … for everything it implements that isn't already there
if [ -x "$ROOT/usr/bin/busybox" ]; then
    for app in $("$ROOT/usr/bin/busybox" --list 2>/dev/null || /usr/bin/busybox --list); do
        [ -e "$ROOT/usr/bin/$app" ] || ln -s busybox "$ROOT/usr/bin/$app"
    done
fi
# drop what a live system doesn't need
rm -rf "$ROOT/usr/share/doc" "$ROOT/usr/share/man" "$ROOT/usr/share/info" "$ROOT/usr/share/lintian" \
       "$ROOT/usr/share/locale" "$ROOT/usr/lib/x86_64-linux-gnu/gconv" "$ROOT/usr/share/bug"
# sanitizer runtimes are large and need a real Linux kernel anyway
rm -f "$ROOT"/usr/lib/x86_64-linux-gnu/lib{a,hwa,l,t,ub}san.so* 2>/dev/null || true

mkdir -p "$ROOT/etc" "$ROOT/home/root" "$ROOT/tmp" "$ROOT/root" "$ROOT/dev" "$ROOT/proc" "$ROOT/run" "$ROOT/var/tmp"
cat > "$ROOT/etc/os-release" <<EOF
NAME="AuxoBSD"
ID=auxobsd
PRETTY_NAME="AuxoBSD $(cat VERSION 2>/dev/null || echo 0.1.0)"
HOME_URL="https://auxolinux.com/bsd"
EOF
echo "root:x:0:0:root:/home/root:/bin/sh" > "$ROOT/etc/passwd"
echo "root:x:0:" > "$ROOT/etc/group"
echo "auxobsd" > "$ROOT/etc/hostname"
printf 'nameserver 10.0.2.3\n' > "$ROOT/etc/resolv.conf"
printf '/usr/lib/x86_64-linux-gnu\n/lib/x86_64-linux-gnu\n' > "$ROOT/etc/ld.so.conf"

echo "rootfs: packing"
tar --format=gnu --owner=0 --group=0 -C "$ROOT" -cf "$OUT/rootfs.tar" .
echo "rootfs: $(du -sh "$OUT/rootfs.tar" | cut -f1) → $OUT/rootfs.tar"
