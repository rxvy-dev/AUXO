#!/usr/bin/env bash
# Headless boot test for an Auxo ISO (used by CI, works locally too).
#
# Boots the ISO's kernel in QEMU with a serial console, waits for the live
# system to reach a login prompt, then waits for the desktop and saves a screenshot.
#
#   ./scripts/smoke-test.sh out/auxo-linux-*.iso [timeout-seconds]
#
# Needs: qemu-system-x86_64, xorriso, socat (optional: imagemagick for PNG)
# Exit 0 = the live system booted to the Auxo login prompt on the serial console.
set -uo pipefail
ISO=${1:?usage: $0 file.iso [timeout]}
TIMEOUT=${2:-600}
OUT=${SMOKE_OUT:-$(dirname "$ISO")/smoke}
mkdir -p "$OUT"
LOG="$OUT/serial.log"; : > "$LOG"

tmp=$(mktemp -d)
QPID=""
trap '[[ -n $QPID ]] && kill $QPID 2>/dev/null; rm -rf "$tmp"' EXIT
xorriso -osirrox on -indev "$ISO" \
  -extract /boot/vmlinuz "$tmp/vmlinuz" \
  -extract /boot/initrd "$tmp/initrd" \
  -extract /boot/grub/grub_void.cfg "$tmp/grub.cfg" >/dev/null 2>&1 \
  || { echo "could not extract the kernel from the ISO"; exit 2; }

# the default entry's kernel arguments, exactly as the ISO boots them
args=$(awk '/menuentry/{n++} n==1 && /linux /{f=1} f{print} f && !/\\$/{exit}' "$tmp/grub.cfg" \
       | tr -d '\\' | tr '\n' ' ' | sed 's/.*linux *([^)]*)\/boot\/vmlinuz//')
echo "kernel args: $args"

accel=(-accel tcg -cpu max); [[ -w /dev/kvm ]] && accel=(-enable-kvm -cpu host)
qemu-system-x86_64 "${accel[@]}" -machine q35 -smp 4 -m 6G \
  -kernel "$tmp/vmlinuz" -initrd "$tmp/initrd" \
  -append "$args console=tty0 console=ttyS0,115200" \
  -drive file="$ISO",media=cdrom,readonly=on \
  -device virtio-vga -display none -vnc none \
  -serial file:"$LOG" -qmp unix:"$tmp/qmp",server,nowait \
  -nic user,model=virtio-net-pci &
QPID=$!

qmp() { printf '{"execute":"qmp_capabilities"}\n%s\n' "$1" | socat - UNIX-CONNECT:"$tmp/qmp" >/dev/null 2>&1; }

ok=0
for ((t = 0; t < TIMEOUT; t += 5)); do
  sleep 5
  if grep -q "login:" "$LOG"; then ok=1; break; fi
  if grep -qE "Kernel panic|dracut-initqueue timeout|Could not boot|emergency shell" "$LOG"; then break; fi
  kill -0 $QPID 2>/dev/null || break
done

fail=0
check() { if grep -qE "$1" "$LOG"; then echo "  ✓ $2"; else echo "  ✗ $2"; fail=1; fi; }
check "runit|runsvdir|Starting" "runit started"
check "Auxo Linux" "Auxo branding on the console"
check "login:" "reached a login prompt"

if (( ok )); then
  echo "booted — waiting 90s for the Plasma live session, then taking a screenshot"
  sleep 90
  qmp "{\"execute\":\"screendump\",\"arguments\":{\"filename\":\"$OUT/screen.ppm\"}}"
  sleep 2
  command -v convert >/dev/null && [[ -f $OUT/screen.ppm ]] && convert "$OUT/screen.ppm" "$OUT/screen.png" && rm -f "$OUT/screen.ppm"
fi
qmp '{"execute":"quit"}'
echo "serial log: $LOG"
(( ok && !fail ))
