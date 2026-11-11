#!/bin/sh
# Build the AuxoBSD live/install ISO: FreeBSD + KDE Plasma + Firefox + the Auxo tools.
#
#   sudo ./build.sh                 → out/auxobsd-<version>-<release>-amd64.iso (+ .sha256)
#   sudo RELEASE=15.1-RELEASE ./build.sh
#   sudo ./build.sh --clean         start from a fresh image root
#
# Runs on FreeBSD 15 (amd64) as root, with about 25 GB free and internet access. No FreeBSD
# machine? Run it in a FreeBSD VM (VirtualBox works), or let GitHub Actions do it
# (.github/workflows/auxobsd-iso.yml).
#
# Steps: FreeBSD base + kernel (distribution sets) → packages (pkg, in a chroot) → a local
# package repository with every NVIDIA driver branch → Auxo tools, theme and live-session
# files → a hybrid ISO that boots on BIOS and UEFI, from DVD or USB stick.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
VERSION=$(cat "$HERE/VERSION")
RELEASE=${RELEASE:-15.1-RELEASE}
ARCH=amd64
MIRROR=${MIRROR:-https://download.freebsd.org/releases/$ARCH/$RELEASE}
WORK=${WORK:-$HERE/work}
OUT=${OUT:-$HERE/out}
ROOT=$WORK/root
LABEL=AUXOBSD
ISO=$OUT/auxobsd-$VERSION-${RELEASE%-RELEASE}-$ARCH.iso

step() { printf '\n\033[1;35m▲\033[0m \033[1m%s\033[0m\n' "$*"; }
warn() { printf '\033[33m!!\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[31mbuild.sh:\033[0m %s\n' "$*" >&2; exit 1; }
in_root() { chroot "$ROOT" /usr/bin/env -u SUDO_USER ASSUME_ALWAYS_YES=yes IGNORE_OSVERSION=yes "$@"; }
pkglist() { sed -e 's/#.*//' -e "s/@PY@/$PYFLAVOR/g" "$1" | awk 'NF {print $1}'; }

[ "$(uname -s)" = FreeBSD ] || die "build on FreeBSD (or in a FreeBSD VM / the GitHub workflow)"
[ "$(id -u)" = 0 ] || die "run as root (sudo ./build.sh)"
for t in fetch tar makefs mkimg etdump sha256; do command -v $t >/dev/null || die "missing $t"; done

cleanup() {
    umount -f "$ROOT/dev" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

if [ "${1:-}" = "--clean" ] && [ -d "$ROOT" ]; then
    step "removing the old image root"
    chflags -R noschg "$ROOT"
    rm -rf "$ROOT"
fi
mkdir -p "$WORK/dist" "$OUT"

# ── 1. FreeBSD base system ────────────────────────────────────────────
step "FreeBSD $RELEASE base system"
for f in MANIFEST base.txz kernel.txz; do
    [ -s "$WORK/dist/$f" ] || fetch -o "$WORK/dist/$f" "$MIRROR/$f"
done
for f in base.txz kernel.txz; do
    want=$(awk -v f="$f" '$1 == f {print $2}' "$WORK/dist/MANIFEST")
    have=$(sha256 -q "$WORK/dist/$f")
    [ "$want" = "$have" ] || die "$f checksum mismatch (delete $WORK/dist and try again)"
done
if [ ! -x "$ROOT/bin/sh" ]; then
    mkdir -p "$ROOT"
    tar -xpf "$WORK/dist/base.txz" -C "$ROOT"
    tar -xpf "$WORK/dist/kernel.txz" -C "$ROOT"
fi
mount -t devfs devfs "$ROOT/dev"
cp /etc/resolv.conf "$ROOT/etc/resolv.conf"

# ── 2. packages ───────────────────────────────────────────────────────
step "packages: KDE Plasma, Firefox, graphics drivers, Auxo tool dependencies"
in_root pkg bootstrap -f
in_root pkg update -f
PY=$(in_root pkg rquery '%dn' python3 | grep -E '^python3[0-9]+$' | head -1)
PYFLAVOR=py${PY#python}
[ "$PYFLAVOR" != py ] || die "couldn't work out the default Python flavour"
echo "default Python: $PY ($PYFLAVOR)"
# shellcheck disable=SC2046
in_root pkg install -y $(pkglist "$HERE/packages.txt")
for p in $(pkglist "$HERE/packages-optional.txt"); do
    in_root pkg install -y "$p" || warn "optional package $p skipped"
done

step "local repository: every NVIDIA driver branch + VM guest tools, for offline installs"
LOCAL=/usr/local/share/auxo/pkgs
mkdir -p "$ROOT$LOCAL"
for p in $(pkglist "$HERE/packages-local.txt"); do
    in_root pkg fetch -y -d -o "$LOCAL" "$p" || warn "couldn't fetch $p for the local repository"
done
in_root pkg repo "$LOCAL"

# ── 3. Auxo ───────────────────────────────────────────────────────────
step "Auxo tools, theme and live session"
cp -R "$HERE/overlay/." "$ROOT/"
"$HERE/scripts/stage-tools.sh" "$ROOT"
mkdir -p "$ROOT/boot/images"
cp "$ROOT/usr/local/share/pixmaps/auxobsd-logo.png" "$ROOT/boot/images/auxobsd-logo.png"
# system-wide accent: boot menu logo, SDDM, Plasma colour scheme, Konsole
in_root env AUXO_IN_INSTALLER=1 /usr/local/bin/auxo-tweak accent violet
# the live ISO's loader.conf (auxo-tweak added logo/brand lines to it already)

FBSD=${RELEASE%-RELEASE}
rm -f "$ROOT/etc/os-release"
cat > "$ROOT/etc/os-release" <<EOF
NAME="AuxoBSD"
VERSION="$VERSION (FreeBSD $FBSD)"
VERSION_ID="$VERSION"
ID=auxobsd
ID_LIKE=freebsd
PRETTY_NAME="AuxoBSD $VERSION (FreeBSD $FBSD)"
ANSI_COLOR="1;35"
LOGO=auxobsd-logo
HOME_URL="https://auxolinux.com/bsd"
SUPPORT_URL="https://discord.gg/XbQ66dH5a7"
BUG_REPORT_URL="https://github.com/rxvy-dev/AUXO/issues"
EOF
# console login banner: "AuxoBSD/amd64 (auxobsd) (ttyv0)"
sed -i '' -e 's|:im=\\r\\n%s/%m (%h) (%t)\\r\\n\\r\\n:|:im=\\r\\nAuxoBSD/%m (%h) (%t)\\r\\n\\r\\n:|' "$ROOT/etc/gettytab"
cp "$ROOT/etc/motd.template" "$ROOT/var/run/motd" 2>/dev/null || true
in_root pwd_mkdb -p /etc/master.passwd
in_root fc-cache -f >/dev/null 2>&1 || true

# ── 4. tidy up ────────────────────────────────────────────────────────
step "cleaning the image"
in_root pkg clean -ay
rm -f "$ROOT/etc/resolv.conf"
rm -rf "$ROOT/var/db/freebsd-update/"* "$ROOT/tmp/"* "$ROOT/var/tmp/"*
umount "$ROOT/dev"
find "$ROOT" -xdev | wc -l | tr -d ' ' > "$ROOT/usr/local/share/auxo/files.count"
mkdir -p "$ROOT/auxolive"

# ── 5. hybrid ISO (BIOS + UEFI, DVD + USB) ────────────────────────────
step "ISO image"
rm -rf "$WORK/esp" "$WORK/efiboot.img"
mkdir -p "$WORK/esp/EFI/BOOT"
cp "$ROOT/boot/loader.efi" "$WORK/esp/EFI/BOOT/BOOTX64.EFI"
makefs -t msdos -o fat_type=12 -o sectors_per_cluster=1 -o volume_label=EFISYS -s 4m \
    "$WORK/efiboot.img" "$WORK/esp"
rm -f "$ISO"
makefs -t cd9660 -o rockridge -o label="$LABEL" -o publisher="Auxo https://auxolinux.com/bsd" \
    -o bootimage="i386;$ROOT/boot/cdboot" -o no-emul-boot \
    -o bootimage="i386;$WORK/efiboot.img" -o no-emul-boot -o platformid=efi \
    "$ISO" "$ROOT"

# the same trick as FreeBSD's release/amd64/mkisoimages.sh: a GPT with the BIOS boot code and
# the El Torito EFI image as an ESP, written into the ISO's system area so it boots from USB too
espparam=""
for entry in $(etdump --format shell "$ISO"); do
    eval "$entry"
    # shellcheck disable=SC2154  # set by etdump
    if [ "$et_platform" = efi ]; then
        espparam="-p efi::$((et_sectors * 512)):$((et_lba * 2048))"
        break
    fi
done
hybrid=$(mktemp /tmp/auxobsd-hybrid.XXXXXX)
# shellcheck disable=SC2086
mkimg -s gpt --capacity "$(stat -f %z "$ISO")" -b "$ROOT/boot/pmbr" \
    -p freebsd-boot:="$ROOT/boot/isoboot" $espparam -o "$hybrid"
dd if="$hybrid" of="$ISO" bs=32k count=1 conv=notrunc status=none
rm -f "$hybrid"

(cd "$OUT" && sha256 -r "$(basename "$ISO")" > "$(basename "$ISO").sha256")
step "done: $ISO ($(du -h "$ISO" | cut -f1))"
echo "Try it:  qemu-system-x86_64 -m 6G -smp 4 -enable-kvm -cdrom $ISO"
echo "         or VirtualBox: Type Other / FreeBSD (64-bit), 6 GB RAM, VMSVGA graphics"
