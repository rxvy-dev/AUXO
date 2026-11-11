#!/bin/sh
# Boot an AuxoBSD ISO in QEMU with an empty 40 GB disk to try the live session and installer.
#   bsd/scripts/test-vm.sh out/auxobsd-*.iso          UEFI (needs OVMF / edk2)
#   bsd/scripts/test-vm.sh out/auxobsd-*.iso --bios   legacy BIOS
set -eu
ISO=${1:?usage: test-vm.sh ISO [--bios]}
DISK=${DISK:-/tmp/auxobsd-test.qcow2}
[ -f "$DISK" ] || qemu-img create -f qcow2 "$DISK" 40G
accel="-accel tcg"
[ -w /dev/kvm ] && accel="-enable-kvm -cpu host"
fw=""
if [ "${2:-}" != "--bios" ]; then
    for f in /usr/share/OVMF/OVMF_CODE_4M.fd /usr/share/OVMF/OVMF_CODE.fd /usr/share/edk2/x64/OVMF_CODE.fd \
             /usr/local/share/edk2-qemu/QEMU_UEFI_CODE-x86_64.fd; do
        [ -f "$f" ] && { fw="-drive if=pflash,format=raw,readonly=on,file=$f"; break; }
    done
    [ -n "$fw" ] || echo "test-vm: no OVMF firmware found, booting with BIOS" >&2
fi
# shellcheck disable=SC2086
exec qemu-system-x86_64 $accel -m 6G -smp 4 $fw -vga virtio -usb -device usb-tablet \
    -drive file="$DISK",if=none,id=d0 -device nvme,drive=d0,serial=auxo0 \
    -cdrom "$ISO" -boot d -nic user,model=e1000
