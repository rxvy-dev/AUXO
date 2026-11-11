#!/usr/bin/env bash
# AuxoBSD tools against a throw-away fake FreeBSD root (no root, no FreeBSD needed:
# commands are printed instead of run).   ./bsd/tests/test-tools.sh
set -uo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd)
ROOT=$(mktemp -d /tmp/auxobsd-fakeroot.XXXX)
trap 'rm -rf "$ROOT"' EXIT
"$HERE/scripts/stage-tools.sh" "$ROOT" >/dev/null 2>&1 || { echo "stage-tools failed"; exit 1; }
export AUXO_ROOT="$ROOT" AUXO_DRY_RUN=1 AUXO_LIB="$ROOT/usr/local/lib/auxo" AUXO_TEST_ROOTFS=zfs
T="$ROOT/usr/local/bin/auxo-tweak"
pass=0; fail=0
ok()   { printf '  \e[32m✓\e[0m %s\n' "$1"; pass=$((pass+1)); }
bad()  { printf '  \e[31m✗\e[0m %s\n' "$1"; fail=$((fail+1)); }
check() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }
mark() { mkdir -p "$ROOT/var/db/pkg/.auxo-test"; for p in "$@"; do : > "$ROOT/var/db/pkg/.auxo-test/$p"; done; }
has()  { [ -e "$ROOT/var/db/pkg/.auxo-test/$1" ]; }
rc()   { grep -q "^$1=\"$2\"" "$ROOT/etc/rc.conf"; }
conf() { grep -q "^$1=\"$2\"" "$ROOT/etc/auxo/auxo.conf"; }

mkdir -p "$ROOT/etc" "$ROOT/boot" "$ROOT/home/ada" "$ROOT/usr/local/share/sddm/themes/breeze"
printf 'root:*:0:0::0:0:Charlie &:/root:/bin/sh\nada:*:1001:1001::0:0:Ada:/home/ada:/usr/local/bin/zsh\n' > "$ROOT/etc/passwd"
printf 'hostname="auxobsd"\nzfs_enable="YES"\n' > "$ROOT/etc/rc.conf"
printf 'zfs_load="YES"\n' > "$ROOT/boot/loader.conf"
printf '/dev/gpt/auxoswap-ada0 none swap sw 0 0\n' > "$ROOT/etc/fstab"
mark pkg python311 plasma6-plasma sddm zsh drm-kmod nvidia-drm-kmod nvidia-driver
RTX="10de:2c02:030000:GB203 [GeForce RTX 5080]"
GTX1080="10de:1b80:030000:GP104 [GeForce GTX 1080]"
KEPLER="10de:1184:030000:GK104 [GeForce GTX 770]"
IRIS="8086:a7a0:030000:Raptor Lake-P [Iris Xe Graphics]"
RTXM="10de:2860:030200:AD106M [GeForce RTX 4070 Max-Q / Mobile]"
NAVI="1002:73bf:030000:Navi 21 [Radeon RX 6800/6800 XT]"
VMSVGA="15ad:0405:030000:SVGA II Adapter"

echo "▲ staging"
check "tools installed" '[ -x "$T" ] && [ -x "$ROOT/usr/local/bin/auxo-fetch" ] && [ -x "$ROOT/usr/local/bin/auxo-installer" ]'
check "shared Auxo Linux modules copied" '[ -f "$AUXO_LIB/auxo_accents.py" ] && [ -f "$AUXO_LIB/auxo_kde.py" ]'
check "Plasma theme uses the AuxoBSD logo" 'grep -q "/usr/local/share/pixmaps/auxobsd-logo.png" "$ROOT"/usr/local/share/plasma/look-and-feel/*/contents/layouts/*.js'
check "zshrc uses FreeBSD paths" 'grep -q /usr/local/share/zsh-autosuggestions "$ROOT/usr/local/share/auxo/shell/zshrc" && ! grep -q "/usr/share/" "$ROOT/usr/local/share/auxo/shell/zshrc"'
check "wallpapers for every accent" '[ "$(ls "$ROOT"/usr/local/share/wallpapers/auxo/auxo-*.png | wc -l)" -eq 7 ]'

echo "▲ accent"
out=$("$T" accent rose --user ada 2>&1)
check "accent rose saved" 'conf ACCENT rose'
check "wallpaper link follows the accent" '[ "$(readlink "$ROOT/usr/local/share/wallpapers/auxo/current.png")" = auxo-rose.png ]'
check "Plasma kdeglobals (system) has the accent" 'grep -q "AccentColor=251,113,133" "$ROOT/usr/local/etc/xdg/kdeglobals"'
check "Plasma kdeglobals (user) has the Auxo theme" 'grep -q "LookAndFeelPackage=org.auxolinux.desktop" "$ROOT/home/ada/.config/kdeglobals"'
check "AuxoDark colour scheme written" 'grep -q "Name=Auxo Dark" "$ROOT/usr/local/share/color-schemes/AuxoDark.colors"'
check "Konsole scheme + default profile" 'grep -q "Color=251,113,133" "$ROOT/home/ada/.local/share/konsole/Auxo.colorscheme" && grep -q "DefaultProfile=Auxo.profile" "$ROOT/home/ada/.config/konsolerc"'
check "SDDM background follows the accent" 'grep -q auxo-rose.png "$ROOT/usr/local/share/sddm/themes/breeze/theme.conf.user" && grep -q "Current=breeze" "$ROOT/usr/local/etc/sddm.conf.d/10-auxo.conf"'
check "boot menu logo + brand" 'grep -q "logo = {" "$ROOT/boot/lua/gfx-auxo.lua" && grep -q "brand = {" "$ROOT/boot/lua/gfx-auxobsd.lua" && grep -q "^loader_logo=\"auxo\"" "$ROOT/boot/loader.conf" && grep -q "^loader_brand=\"auxobsd\"" "$ROOT/boot/loader.conf"'
check "graphical boot menu gets the AuxoBSD logo" '[ -f "$ROOT/boot/images/auxobsd-logo.png" ]'
check "user wallpaper link for the Plasma layout" '[ "$(readlink "$ROOT/home/ada/.config/auxo/wallpaper.png")" = /usr/local/share/wallpapers/auxo/auxo-rose.png ]'
out=$("$T" accent 2>&1); check "accent list marks rose" 'echo "$out" | grep -q "● rose"'
check "lua files load" 'command -v lua5.4 >/dev/null && lua5.4 -e "assert(dofile(\"$ROOT/boot/lua/gfx-auxo.lua\").logo.graphic[8])" || ! command -v lua5.4 >/dev/null'

echo "▲ GPU detection"
out=$(AUXO_TEST_PCI="$RTX" "$T" gpu 2>&1)
check "RTX 5080 → current NVIDIA branch, KMS" 'echo "$out" | grep -q "NVIDIA current (Turing and newer)" && echo "$out" | grep -q "KMS on"'
out=$(AUXO_TEST_PCI="$IRIS;$RTXM" "$T" gpu 2>&1)
check "hybrid laptop → PRIME" 'echo "$out" | grep -q "prime" && echo "$out" | grep -q "i915kms nvidia-drm"'
out=$(AUXO_TEST_PCI="$VMSVGA" AUXO_TEST_KENV_smbios_system_product=VirtualBox "$T" gpu 2>&1)
check "VirtualBox VMSVGA → VirtualBox additions, not VMware tools" 'echo "$out" | grep -q "virtualbox-ose-additions" && ! echo "$out" | grep -q open-vm-tools'

echo "▲ drivers"
AUXO_TEST_PCI="$RTX" "$T" drivers >/dev/null 2>&1
check "NVIDIA current: kld_list nvidia-drm" 'rc kld_list nvidia-drm'
check "NVIDIA KMS tunable" 'grep -q "^hw.nvidiadrm.modeset=\"1\"" "$ROOT/boot/loader.conf"'
check "Xorg uses the nvidia driver" 'grep -q "Driver     \"nvidia\"" "$ROOT/usr/local/etc/X11/xorg.conf.d/20-nvidia.conf"'
check "nvidia-settings installed" 'has nvidia-settings'
out=$(AUXO_TEST_PCI="$GTX1080" "$T" drivers 2>&1)
check "GTX 1080 → 580 branch, current removed first" 'has nvidia-drm-kmod-580 && has nvidia-driver-580 && ! has nvidia-driver && ! has nvidia-drm-kmod'
check "…and the order is remove, then install" 'echo "$out" | grep -n "pkg delete" | head -1 | cut -d: -f1 | { read a; b=$(echo "$out" | grep -n "pkg install" | head -1 | cut -d: -f1); [ "$a" -lt "$b" ]; }'
AUXO_TEST_PCI="$KEPLER" "$T" drivers >/dev/null 2>&1
check "Kepler → 470, nvidia-modeset, no KMS tunable" 'has nvidia-driver-470 && rc kld_list nvidia-modeset && ! grep -q nvidiadrm "$ROOT/boot/loader.conf"'
AUXO_TEST_PCI="$IRIS;$RTXM" "$T" drivers >/dev/null 2>&1
check "hybrid: i915kms + nvidia-drm, prime-run, no forced nvidia Xorg" 'rc kld_list "i915kms nvidia-drm" && [ -x "$ROOT/usr/local/bin/prime-run" ] && [ ! -e "$ROOT/usr/local/etc/X11/xorg.conf.d/20-nvidia.conf" ]'
AUXO_TEST_PCI="$IRIS;$RTXM" "$T" nvidia primary >/dev/null 2>&1
check "nvidia primary on a hybrid: NVIDIA drives the screen" '[ -f "$ROOT/usr/local/etc/X11/xorg.conf.d/20-nvidia.conf" ] && [ ! -e "$ROOT/usr/local/bin/prime-run" ] && conf NVIDIA_MODE primary'
AUXO_TEST_PCI="$IRIS;$RTXM" "$T" drivers >/dev/null 2>&1
check "…and the choice sticks for auxo-tweak drivers" '[ -f "$ROOT/usr/local/etc/X11/xorg.conf.d/20-nvidia.conf" ]'
AUXO_TEST_PCI="$IRIS;$RTXM" "$T" nvidia off >/dev/null 2>&1
check "nvidia off: packages gone, iGPU only" '! has nvidia-drm-kmod && rc kld_list i915kms && conf NVIDIA off'
out=$(AUXO_TEST_PCI="$RTX" "$T" nvidia status 2>&1)
check "nvidia status" 'echo "$out" | grep -q "GeForce RTX 5080"'
AUXO_TEST_PCI="$NAVI" "$T" drivers --prune >/dev/null 2>&1
check "AMD Navi → amdgpu; --prune drops NVIDIA" 'rc kld_list amdgpu && ! has nvidia-driver-470 && ! has nvidia-settings'
AUXO_TEST_PCI="$VMSVGA" AUXO_TEST_KENV_smbios_system_product=VirtualBox "$T" drivers >/dev/null 2>&1
check "VirtualBox: additions + vboxguest/vboxservice" 'has virtualbox-ose-additions-72 && rc vboxguest_enable YES && rc vboxservice_enable YES && ! grep -q "^kld_list" "$ROOT/etc/rc.conf"'

echo "▲ system"
"$T" shell fish --user ada >/dev/null 2>&1
check "shell fish: package + config" 'has fish && grep -q prompt.fish "$ROOT/home/ada/.config/fish/config.fish"'
"$T" power performance >/dev/null 2>&1
check "power performance → powerd flags" 'rc powerd_flags "-a maximum -b hiadaptive" && conf POWER performance'
"$T" firewall on >/dev/null 2>&1
check "firewall on → pf" 'rc pf_enable YES && grep -q "block in all" "$ROOT/etc/pf.conf"'
"$T" firewall off >/dev/null 2>&1
check "firewall off" '! grep -q "^pf_enable" "$ROOT/etc/rc.conf" && conf FIREWALL off'
"$T" dns quad9 >/dev/null 2>&1
check "dns quad9 → resolvconf" 'grep -q "9.9.9.9" "$ROOT/etc/resolvconf.conf"'
"$T" snapshots on >/dev/null 2>&1; check "snapshots on (ZFS)" 'conf SNAPSHOTS on'
out=$(AUXO_TEST_ROOTFS=ufs "$T" snapshots on 2>&1); check "snapshots need ZFS" 'echo "$out" | grep -q "need a ZFS root"'
"$T" repo latest >/dev/null 2>&1
check "repo latest" 'grep -q "/latest\"" "$ROOT/usr/local/etc/pkg/repos/FreeBSD.conf" && grep -q kmods_latest "$ROOT/usr/local/etc/pkg/repos/FreeBSD.conf"'
"$T" service ssh on >/dev/null 2>&1; check "service ssh on → sshd" 'rc sshd_enable YES'
"$T" service cups on >/dev/null 2>&1; check "service cups → cupsd + cups package" 'rc cupsd_enable YES && has cups'
out=$("$T" desktop gnome 2>&1); check "Linux-only sub-commands explain themselves" 'echo "$out" | grep -q "part of Auxo Linux"'
out=$("$T" info 2>&1); check "info" 'echo "$out" | grep -q "accent" && echo "$out" | grep -q "snapshots"'

echo "▲ xbps (Void Linux packages)"
out=$("$T" xbps on 2>&1)
check "linux compat enabled" 'rc linux_enable YES'
check "xbps-static fetched and branded Linux" 'echo "$out" | grep -q "xbps-static-latest.x86_64-musl.tar.xz" && echo "$out" | grep -q "brandelf -t Linux"'
check "Void base bootstrapped into /compat/void" 'echo "$out" | grep -q "xbps-install.static -S -y -r /compat/void -R https://repo-default.voidlinux.org/current base-voidstrap"'
check "wrappers + void-run installed" '[ -x "$ROOT/usr/local/bin/xbps-install" ] && [ -x "$ROOT/usr/local/bin/void-run" ] && grep -q "chroot /compat/void" "$ROOT/usr/local/bin/xbps-install"'
check "Linux mounts in fstab" 'grep -q "linprocfs  /compat/void/proc" "$ROOT/etc/fstab" && grep -q "auxoswap" "$ROOT/etc/fstab"'
"$T" xbps off >/dev/null 2>&1
check "xbps off removes wrappers and fstab lines, keeps swap" '[ ! -e "$ROOT/usr/local/bin/xbps-install" ] && ! grep -q compat/void "$ROOT/etc/fstab" && grep -q auxoswap "$ROOT/etc/fstab"'
check "wrapper scripts parse" 'printf "%s" "$(PYTHONPATH=$AUXO_LIB python3 -c "import auxo_void as v; print(v.WRAPPER)")" | sh -n && printf "%s" "$(PYTHONPATH=$AUXO_LIB python3 -c "import auxo_void as v; print(v.VOID_RUN)")" | sh -n'

echo "▲ auxo-fetch / auxo-motd"
out=$("$ROOT/usr/local/bin/auxo-fetch" --json 2>&1)
check "auxo-fetch --json" 'echo "$out" | python3 -c "import json,sys; d=json.load(sys.stdin); assert d[\"accent\"]==\"rose\" and d[\"kernel\"].startswith(\"FreeBSD\")"'
out=$(AUXO_MOTD_SEED=1 "$ROOT/usr/local/bin/auxo-motd" --force 2>&1)
check "auxo-motd" 'echo "$out" | grep -qE "tip:|terryfox"'
out=$("$ROOT/usr/local/bin/auxo-motd" --give 2>&1)
check "auxo-motd --give → Terry Fox Foundation" 'echo "$out" | grep -q "terryfox.org/?form=GENERAL"'

echo "▲ scripts"
for s in auxo-update auxo-rollback; do check "$s parses" 'sh -n "$ROOT/usr/local/bin/$s"'; done
check "rc.d scripts parse" 'sh -n "$HERE/overlay/etc/rc.d/auxolive" && sh -n "$HERE/overlay/usr/local/etc/rc.d/auxogpu"'
check "build.sh parses" 'sh -n "$HERE/build.sh"'
check "python byte-compiles" 'python3 -m py_compile "$AUXO_LIB"/*.py "$T" "$ROOT/usr/local/bin/auxo-fetch" "$ROOT/usr/local/bin/auxo-motd"'

echo
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
