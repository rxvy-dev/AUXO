#!/usr/bin/env bash
# shellcheck disable=SC2054  # commas are QEMU option syntax, not array separators
# Boot an Auxo ISO (or an installed disk) in QEMU.
#
#   ./scripts/test-vm.sh out/auxo-linux-*.iso          UEFI, 40G virtual disk, try + install
#   ./scripts/test-vm.sh --bios out/auxo-linux-*.iso   legacy BIOS boot
#   ./scripts/test-vm.sh --disk                        boot the installed disk (after installing)
#
# Needs: qemu-system-x86 + ovmf (Debian/Ubuntu/MX), qemu-desktop + edk2-ovmf (Arch), qemu + edk2-ovmf (Void)
set -euo pipefail

BIOS=0; DISK_ONLY=0; ISO=""
DISK=${AUXO_VM_DISK:-$HOME/.cache/auxo-vm/disk.qcow2}
RAM=${AUXO_VM_RAM:-6G}
CPUS=${AUXO_VM_CPUS:-4}

for a in "$@"; do
  case "$a" in
    --bios) BIOS=1 ;;
    --disk) DISK_ONLY=1 ;;
    --fresh) rm -f "$DISK" "${DISK%/*}/OVMF_VARS.fd" ;;
    -h|--help) sed -n '2,9p' "$0"; exit 0 ;;
    *) ISO="$a" ;;
  esac
done
(( DISK_ONLY )) || [[ -f $ISO ]] || { echo "usage: $0 [--bios] [--fresh] path/to/auxo.iso | --disk"; exit 1; }

mkdir -p "${DISK%/*}"
[[ -f $DISK ]] || qemu-img create -f qcow2 "$DISK" 40G >/dev/null

accel=(-accel tcg)
if [[ -w /dev/kvm ]]; then accel=(-enable-kvm -cpu host); else echo "!! /dev/kvm unavailable — running without acceleration (slow)"; fi

firmware=()
if (( ! BIOS )); then
  code=""
  for f in /usr/share/edk2/x64/OVMF_CODE.4m.fd /usr/share/OVMF/OVMF_CODE_4M.fd /usr/share/OVMF/OVMF_CODE.fd /usr/share/edk2-ovmf/x64/OVMF_CODE.fd; do
    [[ -f $f ]] && code=$f && break
  done
  [[ -n $code ]] || { echo "OVMF not found (install edk2-ovmf / ovmf) or use --bios"; exit 1; }
  vars="${DISK%/*}/OVMF_VARS.fd"
  [[ -f $vars ]] || cp "${code/CODE/VARS}" "$vars"
  firmware=(-drive if=pflash,format=raw,readonly=on,file="$code" -drive if=pflash,format=raw,file="$vars")
fi

media=(-drive file="$DISK",if=virtio,format=qcow2)
(( DISK_ONLY )) || media+=(-drive file="$ISO",media=cdrom,readonly=on -boot order=d,menu=on)

exec qemu-system-x86_64 "${accel[@]}" -machine q35 -smp "$CPUS" -m "$RAM" \
  "${firmware[@]}" "${media[@]}" \
  -device virtio-vga-gl -display gtk,gl=on \
  -device intel-hda -device hda-duplex \
  -nic user,model=virtio-net-pci \
  -device qemu-xhci -device usb-tablet \
  -name "Auxo Linux test VM"
