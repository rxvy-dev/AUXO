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
rm -rf "$ROOT" "$WORK/debs"            # start clean: nothing left over from another set
mkdir -p "$WORK/debs" "$ROOT"

PKGS="gcc cpp binutils libc6-dev make busybox-static"
if [ "$SET" = full ]; then
    PKGS="$PKGS python3 python3-gi python3-gi-cairo python3-cairo libc-bin gir1.2-gtk-4.0 gir1.2-gtksource-5 libgtk-4-1 libgtksourceview-5-0
          fonts-dejavu-core fonts-dejavu-mono fonts-dejavu-extra adwaita-icon-theme gsettings-desktop-schemas shared-mime-info
          ca-certificates glib-networking"
fi

echo "rootfs: resolving $SET packages"
# shellcheck disable=SC2086
apt-cache depends --recurse --no-recommends --no-suggests --no-conflicts --no-breaks --no-replaces \
    --no-enhances $PKGS 2>/dev/null | grep -v '^ ' | grep -v '^<' | sort -u > "$WORK/pkgs.txt"
# things a package needs only for its maintainer scripts (we never run those)
grep -vxE 'debconf|perl-base|dpkg|install-info|sensible-utils|ucf|tzdata|base-files|mount|util-linux|login|passwd|adduser|init-system-helpers|systemd|systemd-.*|dbus|dbus-.*|lsb-base|coreutils|bash|dash|debianutils|diffutils|findutils|grep|gzip|sed|tar|hostname|ncurses-bin|ncurses-base|gpgv' \
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

# for pkg (the package manager): what's in this image, and what it can do without
mkdir -p "$ROOT/var/lib/pkg"
for d in "$WORK"/debs/*.deb; do echo "$(dpkg-deb -f "$d" Package) $(dpkg-deb -f "$d" Version) base"; done > "$ROOT/var/lib/pkg/installed"
{ grep -vxFf "$WORK/want2.txt" "$WORK/pkgs.txt" | grep -v "^lib" || true     # libraries get installed when needed
  # needed only for maintainer scripts and services, which never run here (BusyBox has the tools)
  printf '%s\n' adduser base-files base-passwd bash coreutils dash dbus dbus-session-bus default-dbus-session-bus \
    debconf debconf-2.0 debianutils diffutils dpkg findutils grep gzip init-system-helpers install-info login lsb-base \
    mount passwd perl-base sed sensible-utils systemd systemd-sysv tar tzdata ucf util-linux
} | sort -u > "$ROOT/var/lib/pkg/assumed"

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
if [ "$SET" = full ]; then
    # Aether, straight from GitHub (AETHER_SRC=dir to use a local checkout)
    rm -rf "$ROOT/usr/share/aether"
    if [ -n "${AETHER_SRC:-}" ]; then cp -a "$AETHER_SRC" "$ROOT/usr/share/aether"
    else git clone -q --depth 1 "${AETHER_REPO:-https://github.com/rxvy-dev/aether}" "$ROOT/usr/share/aether"; fi
    rm -rf "$ROOT/usr/share/aether/.git" "$ROOT/usr/share/aether/docs"
    ln -sf ../share/aether/bin/aether "$ROOT/usr/bin/aether"
    # GTK reads compiled GSettings schemas and the pixbuf loader list
    if command -v glib-compile-schemas >/dev/null && [ -d "$ROOT/usr/share/glib-2.0/schemas" ]; then
        glib-compile-schemas "$ROOT/usr/share/glib-2.0/schemas"
    fi
    LOADERS=$(ls -d "$ROOT"/usr/lib/x86_64-linux-gnu/gdk-pixbuf-2.0/2.10.0 2>/dev/null || true)
    if [ -n "$LOADERS" ] && command -v gdk-pixbuf-query-loaders >/dev/null; then
        gdk-pixbuf-query-loaders "$LOADERS"/loaders/*.so 2>/dev/null | sed "s|$ROOT||g" > "$LOADERS/loaders.cache" || true
    fi
    # fonts and icons: DejaVu and Adwaita are plenty
    find "$ROOT/usr/share/fonts" -mindepth 1 -maxdepth 2 -not -path '*dejavu*' -not -name truetype -exec rm -rf {} + 2>/dev/null || true
    for d in "$ROOT"/usr/share/icons/*; do case "$(basename "$d")" in Adwaita|hicolor) ;; *) rm -rf "$d" ;; esac; done
    rm -rf "$ROOT/usr/share/texmf" "$ROOT/usr/share/X11/locale" "$ROOT/usr/lib/x86_64-linux-gnu/dri" \
           "$ROOT/usr/lib/x86_64-linux-gnu/libLLVM"* "$ROOT/usr/lib/x86_64-linux-gnu/libgallium"* 2>/dev/null || true
fi

# the CA bundle ca-certificates' own script would make (GnuTLS, OpenSSL and Python read it)
if [ -d "$ROOT/usr/share/ca-certificates/mozilla" ]; then
    mkdir -p "$ROOT/etc/ssl/certs"
    cat "$ROOT"/usr/share/ca-certificates/mozilla/*.crt > "$ROOT/etc/ssl/certs/ca-certificates.crt"
fi

# drop what a live system doesn't need
rm -rf "$ROOT/usr/share/doc" "$ROOT/usr/share/man" "$ROOT/usr/share/info" "$ROOT/usr/share/lintian" \
       "$ROOT/usr/share/locale" "$ROOT/usr/lib/x86_64-linux-gnu/gconv" "$ROOT/usr/share/bug"
# sanitizer runtimes are large and need a real Linux kernel anyway
for s in asan hwasan lsan tsan ubsan; do rm -f "$ROOT"/usr/lib/x86_64-linux-gnu/lib$s.so*; done   # (sh: no {a,b} expansion)
# spelling dictionaries: every language comes in through "one of these" dependencies; English stays
rm -rf "$ROOT/usr/share/hunspell-bdic" "$ROOT/usr/share/aspell" "$ROOT/usr/share/ispell" "$ROOT/usr/share/myspell" \
       "$ROOT/usr/lib/x86_64-linux-gnu/aspell" 2>/dev/null || true
if [ -d "$ROOT/usr/share/hunspell" ]; then
    find "$ROOT/usr/share/hunspell" -mindepth 1 ! -name 'en_US*' -exec rm -rf {} + 2>/dev/null || true
fi

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
