#!/bin/sh
# The first 17 MiB of an installed AuxoBSD disk: build/install.img
#
#   LBA 0           GRUB's boot.img (MBR); the installer adds the partition table
#   LBA 1 …         GRUB core.img for BIOS (in the gap before the first partition)
#   LBA 2048 …      partition 1: EFI system partition (FAT) with GRUB for UEFI
#
# The installer writes the rest itself: partition 2 is a plain tar archive holding /boot
# (grub.cfg, the kernel, the root filesystem), which GRUB reads with its `tar` module;
# partition 3 keeps /home/root. Both GRUBs look for /boot/auxobsd.elf and load its grub.cfg.
#
#   tools/mkdisk-template.sh [OUTDIR]       needs grub-mkimage (i386-pc + x86_64-efi) and mtools
set -eu
OUT=${1:-build}
W=$OUT/disk-work
mkdir -p "$W"
MODS="part_msdos tar normal multiboot2 search search_fs_file configfile echo test ls cat sleep all_video gfxterm"

cat > "$W/early.cfg" <<'EOF'
search --no-floppy --file --set=root /boot/auxobsd.elf
set prefix=($root)/boot/grub
configfile ($root)/boot/grub/grub.cfg
EOF

dd if=/dev/zero of="$OUT/install.img" bs=1M count=17 status=none

# BIOS: boot.img points at core.img on LBA 1; diskboot's block list loads the rest from LBA 2
# only the modules this GRUB has (newer builds dropped some, efi_uga for one)
have() { dir=/usr/lib/grub/$1; shift; for m in "$@"; do [ -f "$dir/$m.mod" ] && printf '%s ' "$m"; done; }
grub-mkimage -O i386-pc -o "$W/core.img" -c "$W/early.cfg" -p /boot/grub $(have i386-pc biosdisk $MODS)
SECTORS=$(( ($(stat -c %s "$W/core.img") + 511) / 512 ))
[ "$SECTORS" -lt 2040 ] || { echo "core.img too big ($SECTORS sectors)"; exit 1; }
dd if=/usr/lib/grub/i386-pc/boot.img of="$OUT/install.img" bs=446 count=1 conv=notrunc status=none
dd if="$W/core.img" of="$OUT/install.img" bs=512 seek=1 conv=notrunc status=none
python3 - "$OUT/install.img" "$SECTORS" <<'EOF'
import struct, sys
path, sectors = sys.argv[1], int(sys.argv[2])
img = bytearray(open(path, "rb").read())
struct.pack_into("<Q", img, 0x5C, 1)                  # boot.img: core.img starts at LBA 1
img[0x64] = 0xFF                                      # boot drive: whatever the BIOS says
# diskboot (core.img's first sector): one block list entry, LBA 2, sectors-1 sectors, to 0x820:0
struct.pack_into("<QHH", img, 512 + 0x1F4, 2, sectors - 1, 0x820)
open(path, "wb").write(img)
EOF

# UEFI: an ESP with \EFI\BOOT\BOOTX64.EFI
grub-mkimage -O x86_64-efi -o "$W/BOOTX64.EFI" -c "$W/early.cfg" -p /boot/grub $(have x86_64-efi $MODS efi_gop efi_uga)
rm -f "$W/esp.img"
mformat -i "$W/esp.img" -C -T 32768 -h 2 -s 16 -v AUXOBSD-EFI ::
mmd -i "$W/esp.img" ::/EFI ::/EFI/BOOT
mcopy -i "$W/esp.img" "$W/BOOTX64.EFI" ::/EFI/BOOT/BOOTX64.EFI
dd if="$W/esp.img" of="$OUT/install.img" bs=512 seek=2048 conv=notrunc status=none
echo "install.img: BIOS core $SECTORS sectors, ESP 16 MiB → $OUT/install.img"
