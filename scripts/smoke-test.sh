#!/usr/bin/env bash
# Headless boot test for an Auxo ISO (used by CI, works locally too).
#
# Boots the ISO's kernel in QEMU with a serial console, waits for the live
# desktop to come up, checks key services, and saves a screenshot.
#
#   ./scripts/smoke-test.sh out/auxo-linux-*.iso [timeout-seconds]
#
# Exit 0 = the live session reached the graphical target with SDDM running.
set -uo pipefail
ISO=${1:?usage: $0 file.iso [timeout]}
TIMEOUT=${2:-900}
OUT=${SMOKE_OUT:-$(dirname "$ISO")/smoke}
mkdir -p "$OUT"
LOG="$OUT/serial.log"; : > "$LOG"

# extract kernel + initramfs and read the volume label
tmp=$(mktemp -d)
trap 'kill $QPID 2>/dev/null; rm -rf "$tmp"' EXIT
xorriso -osirrox on -indev "$ISO" \
  -extract /auxo/boot/x86_64/vmlinuz-linux "$tmp/vmlinuz" \
  -extract /auxo/boot/x86_64/initramfs-linux.img "$tmp/initrd" >/dev/null 2>&1 \
  || { echo "could not extract kernel from ISO"; exit 2; }
LABEL=$(xorriso -indev "$ISO" -pvd_info 2>/dev/null | sed -n 's/^Volume Id *: *//p' | tr -d "'")
echo "ISO label: $LABEL"

accel=(-accel tcg -cpu max); [[ -w /dev/kvm ]] && accel=(-enable-kvm -cpu host)
qemu-system-x86_64 "${accel[@]}" -machine q35 -smp 4 -m 6G \
  -kernel "$tmp/vmlinuz" -initrd "$tmp/initrd" \
  -append "archisobasedir=auxo archisolabel=$LABEL console=tty0 console=ttyS0,115200 systemd.show_status=1" \
  -drive file="$ISO",media=cdrom,readonly=on \
  -device virtio-vga -display none -vnc none \
  -serial file:"$LOG" -qmp unix:"$tmp/qmp",server,nowait \
  -nic user,model=virtio-net-pci &
QPID=$!

qmp() { printf '{"execute":"qmp_capabilities"}\n%s\n' "$1" | socat - UNIX-CONNECT:"$tmp/qmp" >/dev/null 2>&1; }

ok=0
for ((t = 0; t < TIMEOUT; t += 5)); do
  sleep 5
  if grep -qE "Reached target .*Graphical Interface" "$LOG"; then ok=1; break; fi
  if grep -qE "Kernel panic|emergency mode|Failed to mount .*airootfs" "$LOG"; then break; fi
  kill -0 $QPID 2>/dev/null || break
done

checks=("Started .*Auxo live session setup" "Started .*Simple Desktop Display Manager" "Started .*Network Manager")
fail=0
for c in "${checks[@]}"; do
  if grep -qE "$c" "$LOG"; then echo "  ✓ $c"; else echo "  ✗ $c"; fail=1; fi
done

if (( ok )); then
  echo "graphical target reached — waiting 90s for Plasma, then taking a screenshot"
  sleep 90
  qmp "{\"execute\":\"screendump\",\"arguments\":{\"filename\":\"$OUT/screen.ppm\"}}"
  sleep 2
  command -v convert >/dev/null && [[ -f $OUT/screen.ppm ]] && convert "$OUT/screen.ppm" "$OUT/screen.png" && rm -f "$OUT/screen.ppm"
fi
qmp '{"execute":"quit"}'

if (( ok && !fail )); then echo "SMOKE TEST PASSED"; exit 0; fi
echo "SMOKE TEST FAILED — last 40 lines of serial log:"; tail -n 40 "$LOG"; exit 1
