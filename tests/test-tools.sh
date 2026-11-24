#!/usr/bin/env bash
# Exercises auxo-tweak / auxo-fetch against a throw-away fake Void root
# (no root, no xbps needed: commands are printed instead of run).
#   ./tests/test-tools.sh
set -uo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd)
PKG="$HERE/packages/auxo-tools/files"
ROOT=$(mktemp -d /tmp/auxo-fakeroot.XXXX)
[ -n "${AUXO_KEEP:-}" ] || trap 'rm -rf "$ROOT"' EXIT
export AUXO_ROOT="$ROOT" AUXO_DRY_RUN=1 AUXO_LIB="$PKG/lib"
T="$PKG/bin/auxo-tweak"
pass=0; fail=0
ok()   { printf '  \e[32m✓\e[0m %s\n' "$1"; pass=$((pass+1)); }
bad()  { printf '  \e[31m✗\e[0m %s\n' "$1"; fail=$((fail+1)); }
check() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }
mark_installed() { mkdir -p "$ROOT/var/db/xbps/.auxo-test"; for p in "$@"; do : > "$ROOT/var/db/xbps/.auxo-test/$p"; done; }
unmark() { for p in "$@"; do rm -f "$ROOT/var/db/xbps/.auxo-test/$p"; done; }
enabled() { [ -L "$ROOT/etc/runit/runsvdir/default/$1" ]; }

# ── fake Void system ──
mkdir -p "$ROOT"/etc/default "$ROOT"/etc/greetd "$ROOT"/etc/runit/runsvdir/default "$ROOT"/usr/bin \
         "$ROOT"/usr/share/{wallpapers,auxo,sddm/themes/breeze} "$ROOT"/boot/grub "$ROOT"/home/alex
cp "$PKG"/share/wallpapers/* "$ROOT/usr/share/wallpapers/"
cp -r "$PKG"/share/auxo/. "$ROOT/usr/share/auxo/"
install -Dm644 "$PKG/etc/auxo/auxo.conf" "$ROOT/etc/auxo/auxo.conf"
printf 'root:x:0:0::/root:/bin/bash\nalex:x:1000:1000::/home/alex:/bin/bash\n_greeter:x:964:964::/var/lib/_greeter:/usr/bin/nologin\n' > "$ROOT/etc/passwd"
cat > "$ROOT/etc/default/grub" <<'EOF'
GRUB_DEFAULT=0
GRUB_TIMEOUT=5
GRUB_DISTRIBUTOR="Void"
GRUB_CMDLINE_LINUX_DEFAULT="loglevel=4"
GRUB_TERMINAL_OUTPUT=console
#GRUB_THEME="/path/to/gfxtheme"
EOF
touch "$ROOT/usr/bin/sddm" "$ROOT/usr/share/sddm/themes/breeze/metadata.desktop"
ln -s /etc/sv/dhcpcd "$ROOT/etc/runit/runsvdir/default/dhcpcd"
ln -s /etc/sv/sddm "$ROOT/etc/runit/runsvdir/default/sddm"
mark_installed linux kde-plasma konsole dolphin kate sddm zsh xbps

echo "▲ core: xbps + runit helpers"
out=$(python3 -c "
import sys; sys.path.insert(0, '$PKG/lib')
import auxo_core as c
c.xbps_install('zsh', 'fish-shell', 'fish-shell')
c.sv_enable('sshd'); c.sv_disable('dhcpcd')
")
check "installs only what's missing, once"   "echo \"\$out\" | grep -qx '\\[dry-run\\] xbps-install -y fish-shell'"
check "syncs the repo index + updates xbps"  "echo \"\$out\" | grep -q 'xbps-install -S\$' && echo \"\$out\" | grep -q 'xbps-install -uy xbps'"
check "enable = link into the runlevel"      "[ \$(readlink $ROOT/etc/runit/runsvdir/default/sshd) = /etc/sv/sshd ]"
check "disable = remove the link"            "! enabled dhcpcd"
rm -f "$ROOT/etc/runit/runsvdir/default/sshd"

echo "▲ accent"
out=$("$T" accent rose --user alex --no-grub 2>&1); echo "$out" | sed 's/^/    /'
check "auxo.conf records accent"            "grep -q 'ACCENT=\"rose\"' $ROOT/etc/auxo/auxo.conf"
check "GRUB theme written with rose accent" "grep -q '#fb7185' $ROOT/usr/share/grub/themes/auxo/theme.txt"
check "GRUB background copied"               "cmp -s $ROOT/usr/share/grub/themes/auxo/background.png $ROOT/usr/share/wallpapers/auxo-rose.png"
check "GRUB selection pixmaps are PNGs"      "file $ROOT/usr/share/grub/themes/auxo/select_c.png | grep -q 'PNG image'"
check "/etc/default/grub → GRUB_THEME"       "grep -q '^GRUB_THEME=\"/usr/share/grub/themes/auxo/theme.txt\"' $ROOT/etc/default/grub"
check "/etc/default/grub → gfxterm"          "grep -q '^GRUB_TERMINAL_OUTPUT=\"gfxterm\"' $ROOT/etc/default/grub && [ \$(grep -c GRUB_TERMINAL_OUTPUT $ROOT/etc/default/grub) = 1 ]"
check "text login (tuigreet) in rose accent" "grep -q 'tuigreet' $ROOT/etc/greetd/config.toml && grep -q 'border=red' $ROOT/etc/greetd/config.toml"
check "greetd runs as Void's _greeter user"  "grep -q '^user = \"_greeter\"' $ROOT/etc/greetd/config.toml"
check "greetd config is valid TOML"          "python3 -c 'import tomllib,sys; tomllib.load(open(sys.argv[1],\"rb\"))' $ROOT/etc/greetd/config.toml"
check "MOTD has truecolor accent"            "grep -q '38;2;251;113;133' $ROOT/etc/motd"
check "user colour files"                    "grep -q 'rgb(fb7185)' $ROOT/home/alex/.config/auxo/colors-hyprland.conf"
check "user wallpaper symlink"               "[ \$(readlink $ROOT/home/alex/.config/auxo/wallpaper.png) = /usr/share/wallpapers/auxo-rose.png ]"
check "KDE accent in kdeglobals"             "grep -q 'AccentColor=251,113,133' $ROOT/home/alex/.config/kdeglobals"
check "session-apply autostart"              "test -f $ROOT/home/alex/.config/autostart/auxo-session-apply.desktop"
"$T" accent emerald --user alex --no-grub >/dev/null 2>&1
check "accent switch keeps kdeglobals tidy"  "[ \$(grep -c AccentColor $ROOT/home/alex/.config/kdeglobals) = 1 ] && grep -q '52,211,153' $ROOT/home/alex/.config/kdeglobals"
check "invalid accent rejected"              "! \"$T\" accent purple >/dev/null 2>&1"
printf 'insmod gfxterm\nset theme=/usr/share/grub/themes/auxo/theme.txt\n' > "$ROOT/boot/grub/grub.cfg"
out=$("$T" accent cyan --user alex 2>&1)
check "no grub-mkconfig once theme is wired" "! echo \"\$out\" | grep -q grub-mkconfig"
rm "$ROOT/boot/grub/grub.cfg"
GB=$(mktemp -d); printf '#!/bin/sh\n' > "$GB/grub-mkconfig"; chmod +x "$GB/grub-mkconfig"   # grub isn't on every test machine
out=$(PATH="$GB:$PATH" "$T" accent cyan --user alex 2>&1); rm -rf "$GB"
check "first theme setup rebuilds grub.cfg under a lock" "echo \"\$out\" | grep -q 'flock -w 120 /run/lock/auxo-grub.lock grub-mkconfig'"
"$T" accent emerald --user alex --no-grub >/dev/null 2>&1

echo "▲ desktop hyprland --replace (community repo)"
out=$("$T" desktop hyprland --user alex --replace 2>&1); echo "$out" | sed 's/^/    /' | head -8
check "adds the hyprland-void repo"          "grep -q 'repository=https://raw.githubusercontent.com/Makrennel/hyprland-void/repository-x86_64-glibc' $ROOT/etc/xbps.d/20-hyprland.conf"
check "accepts the repo key on first sync"   "echo \"\$out\" | grep -q 'accepting the signing key' && echo \"\$out\" | grep -q 'xbps-install -S\$'"
check "installs hyprland + session base"     "echo \"\$out\" | grep -E 'xbps-install -y .*hyprland' | grep -q elogind"
check "skips packages already installed"     "! echo \"\$out\" | grep -E 'xbps-install -y .* konsole( |$)' >/dev/null"
check "enables dbus, elogind, NetworkManager" "enabled dbus && enabled elogind && enabled NetworkManager"
check "dhcpcd handed over to NetworkManager" "! enabled dhcpcd"
check "removes the live Plasma"              "echo \"\$out\" | grep -q 'xbps-remove -y -R plasma-desktop'"
check "greetd on, sddm off"                  "enabled greetd && ! enabled sddm"
check "rice copied: hyprland.conf (0.49 syntax)" "grep -q 'source = ~/.config/auxo/colors-hyprland.conf' $ROOT/home/alex/.config/hypr/hyprland.conf"
check "rice uses the polkit launcher"        "grep -q 'exec-once = auxo-polkit-agent' $ROOT/home/alex/.config/hypr/hyprland.conf"
check "hyprpaper 0.7 syntax"                 "grep -q '^preload = ~/.config/auxo/wallpaper.png' $ROOT/home/alex/.config/hypr/hyprpaper.conf"
check "waybar imports colours"               "grep -q 'auxo/colors.css' $ROOT/home/alex/.config/waybar/style.css"
check "login starts Hyprland with a D-Bus session" "grep -q \"\\-\\-cmd 'dbus-run-session Hyprland'\" $ROOT/etc/greetd/config.toml"
check "username pre-filled"                  "grep -q '^alex$' $ROOT/var/cache/tuigreet/lastuser"
check "auxo.conf DESKTOP=hyprland"           "grep -q 'DESKTOP=\"hyprland\"' $ROOT/etc/auxo/auxo.conf"
mkdir -p "$ROOT/home/alex/.config/hypr"; echo "-- old" > "$ROOT/home/alex/.config/hypr/hyprland.lua"
"$T" rice hyprland --user alex >/dev/null 2>&1
check "re-applying keeps a backup"           "test -f $ROOT/home/alex/.config/hypr/hyprland.conf.auxo-bak"
check "old Lua config from Arch Auxo parked" "test -f $ROOT/home/alex/.config/hypr/hyprland.lua.auxo-bak && ! test -f $ROOT/home/alex/.config/hypr/hyprland.lua"

echo "▲ desktop i3 / sway / gnome / none"
"$T" desktop i3 --user alex >/dev/null 2>&1
check "i3 config + polybar launch exec"      "test -x $ROOT/home/alex/.config/polybar/launch.sh && grep -q colors-i3 $ROOT/home/alex/.config/i3/config"
check "i3 starts through startx + D-Bus"     "grep -q \"\\-\\-cmd 'startx /usr/bin/dbus-run-session /usr/bin/i3'\" $ROOT/etc/greetd/config.toml"
"$T" desktop sway --user alex >/dev/null 2>&1
check "sway config"                          "grep -q colors-sway $ROOT/home/alex/.config/sway/config && grep -q 'sway/workspaces' $ROOT/home/alex/.config/waybar/config.jsonc"
check "sway uses the polkit launcher"        "grep -q 'exec auxo-polkit-agent' $ROOT/home/alex/.config/sway/config"
check "mako config generated w/ accent"      "grep -q 'border-color=#34d399' $ROOT/home/alex/.config/mako/config"
mkdir -p "$ROOT/etc/dconf/db/local.d"; printf '[org/gnome/shell]\n' > "$ROOT/etc/dconf/db/local.d/00-auxo"
out=$("$T" desktop gnome --user alex 2>&1)
check "GNOME: gnome-core + gdm"              "echo \"\$out\" | grep -E 'xbps-install -y ' | grep -q 'gnome-core' && echo \"\$out\" | grep -E 'xbps-install -y ' | grep -q ' gdm'"
check "GNOME uses GDM, greetd off"           "enabled gdm && ! enabled greetd"
check "GDM remembers the GNOME session"      "grep -q 'Session=gnome' $ROOT/var/lib/AccountsService/users/alex"
check "old GNOME rice cleaned up"            "[ ! -e $ROOT/etc/dconf/db/local.d/00-auxo ]"
"$T" desktop none --user alex >/dev/null 2>&1
check "none → no display manager"            "! enabled gdm && ! enabled greetd && ! enabled sddm"

echo "▲ KDE global theme"
mkdir -p "$ROOT/usr/share/plasma/look-and-feel"; cp -r "$PKG/share/plasma/look-and-feel/." "$ROOT/usr/share/plasma/look-and-feel/"
L="$PKG/share/plasma/look-and-feel/org.auxolinux.desktop"
check "theme package complete"               "python3 -m json.tool $L/metadata.json >/dev/null && [ -s $L/contents/defaults ] && [ -s $L/contents/layouts/org.kde.plasma.desktop-layout.js ] && [ -s $L/contents/previews/preview.png ]"
"$T" accent rose --no-grub --no-initramfs --user alex >/dev/null 2>&1
K="$ROOT/home/alex/.config/kdeglobals"
check "user kdeglobals → Auxo theme"         "grep -q 'LookAndFeelPackage=org.auxolinux.desktop' $K && grep -q 'ColorScheme=AuxoDark' $K"
check "colours written for first login"      "grep -q '^\\[Colors:Window\\]' $K && grep -q '^\\[Colors:Header\\]\\[Inactive\\]' $K"
check "system default for new users"         "grep -q 'LookAndFeelPackage=org.auxolinux.desktop' $ROOT/etc/xdg/kdeglobals"
check "Auxo Dark colour scheme installed"    "grep -q 'Name=Auxo Dark' $ROOT/usr/share/color-schemes/AuxoDark.colors"
printf '[Containments]\n' > "$ROOT/home/alex/.config/plasma-org.kde.plasma.desktop-appletsrc"
printf '[General]\nfoo=bar\n' >> "$K"
"$T" rice kde --user alex >/dev/null 2>&1
check "rice kde resets the panel layout"     "[ -e $ROOT/home/alex/.config/plasma-org.kde.plasma.desktop-appletsrc.auxo-bak ]"
check "rice kde keeps other kdeglobals keys" "grep -q 'foo=bar' $K"

echo "▲ live wallpaper change per desktop"
FB="$ROOT/fakebin"; mkdir -p "$FB"
for b in hyprctl hyprpaper swaymsg feh i3-msg gsettings plasma-apply-wallpaperimage plasma-apply-colorscheme xfconf-query makoctl; do printf '#!/bin/sh\n' > "$FB/$b"; chmod +x "$FB/$b"; done
sa() { local o; o=$(XDG_CURRENT_DESKTOP="$1" PATH="$FB:$PATH" "$T" session-apply --accent emerald 2>&1); printf '%s\n' "$o"; }
check "KDE: Auxo colour scheme + accent"     "sa KDE | grep -q 'plasma-apply-colorscheme --accent-color .#34d399. AuxoDark'"
check "KDE: plasma wallpaper"                "sa KDE | grep -q 'plasma-apply-wallpaperimage /usr/share/wallpapers/auxo-emerald.png'"
check "GNOME: gsettings picture-uri"         "sa GNOME | grep -q 'org.gnome.desktop.background picture-uri file:///usr/share/wallpapers/auxo-emerald.png'"
check "Hyprland: hyprpaper restarted"        "sa Hyprland | grep -q 'hyprctl dispatch exec hyprpaper'"
check "Sway: output bg"                      "sa sway | grep -q \"swaymsg output '\\*' bg /usr/share/wallpapers/auxo-emerald.png fill\""
check "i3: feh"                              "sa i3 | grep -q 'feh --no-fehbg --bg-fill /usr/share/wallpapers/auxo-emerald.png'"

echo "▲ shell / kernel"
unmark fish-shell
out=$("$T" shell fish --user alex 2>&1)
check "fish = fish-shell on Void + chsh"     "echo \"\$out\" | grep -q 'xbps-install -y fish-shell' && echo \"\$out\" | grep -q 'chsh -s /usr/bin/fish alex'"
printf '# stub\n' > "$ROOT/home/alex/.zshrc"
"$T" shell zsh --user alex >/dev/null 2>&1
check "zsh: Auxo zshrc replaces the stub"    "grep -q auxo/shell/prompt.zsh $ROOT/home/alex/.zshrc && grep -q 'stub' $ROOT/home/alex/.zshrc.auxo-bak"
check "fish config seeded"                   "grep -q prompt.fish $ROOT/home/alex/.config/fish/config.fish"
out=$("$T" shell bash --user alex 2>&1); "$T" shell bash --user alex >/dev/null 2>&1
check "bash prompt sourced once"             "[ \$(grep -c prompt.bash $ROOT/home/alex/.bashrc) = 1 ]"
out=$("$T" kernel linux-lts --no-grub 2>&1)
check "kernel: LTS meta + headers"           "echo \"\$out\" | grep -q 'xbps-install -y linux-lts\$' && echo \"\$out\" | grep -q 'xbps-install -y linux-lts-headers'"
check "kernel: default = versioned image"    "grep -q 'GRUB_TOP_LEVEL=\"/boot/vmlinuz-6.12.0_1\"' $ROOT/etc/default/grub"
check "kernel: auxo.conf KERNEL"             "grep -q 'KERNEL=\"linux-lts\"' $ROOT/etc/auxo/auxo.conf"
check "invalid kernel rejected"              "! \"$T\" kernel linux-zen >/dev/null 2>&1"

echo "▲ kernel hook (49-auxo-default-kernel)"
H="$PKG/etc/kernel.d/post-install/49-auxo-default-kernel"
HK="$ROOT/hooktest"; mkdir -p "$HK/bin" "$HK/boot"
printf '#!/bin/sh\necho "linux6.12>=0_1"\necho "linux-base>=0_1"\n' > "$HK/bin/xbps-query"; chmod +x "$HK/bin/xbps-query"
printf 'KERNEL="linux-lts"\n' > "$HK/auxo.conf"; printf 'GRUB_TOP_LEVEL="/boot/vmlinuz-6.12.0_1"\n' > "$HK/grub"; touch "$HK/boot/vmlinuz-6.12.9_1"
sed -e "s|/etc/auxo/auxo.conf|$HK/auxo.conf|; s|/etc/default/grub|$HK/grub|; s|/boot/vmlinuz-|$HK/boot/vmlinuz-|g" "$H" > "$HK/hook"
PATH="$HK/bin:$PATH" sh "$HK/hook" linux6.12 6.12.9_1
check "hook follows an LTS update"           "grep -q 'GRUB_TOP_LEVEL=\"$HK/boot/vmlinuz-6.12.9_1\"' $HK/grub && [ \$(grep -c GRUB_TOP_LEVEL $HK/grub) = 1 ]"
touch "$HK/boot/vmlinuz-6.18.2_1"; PATH="$HK/bin:$PATH" sh "$HK/hook" linux6.18 6.18.2_1
check "hook ignores other kernels"           "grep -q 'vmlinuz-6.12.9_1' $HK/grub"

echo "▲ drivers / nvidia"
out=$(AUXO_TEST_GPU="10de:2684 NVIDIA RTX 4090" "$T" drivers 2>&1)
check "Turing+ → nvidia (open modules)"      "echo \"\$out\" | grep -q 'NVIDIA GPU → nvidia\$'"
check "nonfree repo enabled first"           "echo \"\$out\" | grep -q 'xbps-install -y void-repo-nonfree'"
check "driver + headers for each kernel"     "echo \"\$out\" | grep -q 'xbps-install -y nvidia' && [ -e $ROOT/var/db/xbps/.auxo-test/linux-headers ] && [ -e $ROOT/var/db/xbps/.auxo-test/linux-lts-headers ]"
check "modeset written"                      "grep -q 'nvidia_drm modeset=1' $ROOT/etc/modprobe.d/auxo-nvidia.conf"
check "initramfs rebuilt with dracut"        "echo \"\$out\" | grep -q 'dracut --regenerate-all --force'"
out=$(AUXO_TEST_GPU="10de:1b80 NVIDIA GTX 1080" "$T" nvidia on 2>&1)
check "Pascal → nvidia580"                   "echo \"\$out\" | grep -q 'xbps-install -y nvidia580'"
out=$(AUXO_TEST_GPU="10de:1180 NVIDIA GTX 680" "$T" nvidia status 2>&1)
check "Kepler → nvidia470"                   "echo \"\$out\" | grep -q 'fits        nvidia470'"
check "too-old GPU refused politely"         "! AUXO_TEST_GPU='10de:0640 NVIDIA 9500GT' \"$T\" nvidia on >/dev/null 2>&1"
out=$("$T" nvidia off 2>&1)
check "nvidia off → nouveau"                 "[ ! -e $ROOT/etc/modprobe.d/auxo-nvidia.conf ] && grep -q 'NVIDIA=\"nouveau\"' $ROOT/etc/auxo/auxo.conf"
"$T" nvidia prime >/dev/null 2>&1
check "prime-run installed"                  "test -x $ROOT/usr/local/bin/prime-run && grep -q __NV_PRIME_RENDER_OFFLOAD=1 $ROOT/usr/local/bin/prime-run"
mark_installed qemu-ga spice-vdagent virtualbox-ose-guest open-vm-tools
out=$(AUXO_TEST_VIRT=kvm AUXO_TEST_GPU="1234:0001 QXL" "$T" drivers --prune 2>&1)
check "VM prune: keeps qemu tools on KVM"    "enabled qemu-ga && enabled spice-vdagentd && ! echo \"\$out\" | grep -q 'xbps-remove.*qemu-ga'"
check "VM prune: drops other hypervisors"    "echo \"\$out\" | grep -q 'xbps-remove -y -R virtualbox-ose-guest' && echo \"\$out\" | grep -q 'xbps-remove -y -R open-vm-tools'"
unmark qemu-ga spice-vdagent virtualbox-ose-guest open-vm-tools

out=$(AUXO_TEST_GPU="8086:a780 Intel UHD" "$T" drivers 2>&1)
check "drivers still finish if microcode fails" "echo \"\$out\" | grep -q 'drivers done'"

echo "▲ scheduler / power"
out=$("$T" scheduler lavd --mode gaming 2>&1)
check "scx + scx-loader installed"           "echo \"\$out\" | grep -q 'xbps-install -y scx scx-loader'"
check "loader config: lavd, Gaming"          "grep -q 'default_sched = \"scx_lavd\"' $ROOT/etc/scx_loader.toml && grep -q 'default_mode = \"Gaming\"' $ROOT/etc/scx_loader.toml"
check "scx-loader service enabled"           "enabled scx-loader && enabled dbus"
check "loader config is valid TOML"          "python3 -c 'import tomllib,sys; tomllib.load(open(sys.argv[1],\"rb\"))' $ROOT/etc/scx_loader.toml"
"$T" scheduler off >/dev/null 2>&1
check "scheduler off"                        "! enabled scx-loader"
check "unknown scheduler rejected"           "! \"$T\" scheduler bogus >/dev/null 2>&1"
out=$("$T" power performance 2>&1)
check "power profile daemon on"              "enabled power-profiles-daemon && grep -q 'POWER=\"performance\"' $ROOT/etc/auxo/auxo.conf"

echo "▲ firewall / dns"
out=$("$T" firewall on 2>&1)
check "ufw: deny in, allow out, enabled"     "echo \"\$out\" | grep -q 'ufw default deny incoming' && echo \"\$out\" | grep -q 'ufw default allow outgoing' && enabled ufw"
"$T" firewall off >/dev/null 2>&1
check "firewall off"                         "! enabled ufw"
"$T" dns quad9 >/dev/null 2>&1
check "DNS: NetworkManager global servers"   "grep -q '^servers=9.9.9.9,149.112.112.112' $ROOT/etc/NetworkManager/conf.d/50-auxo-dns.conf && grep -q 'global-dns-domain' $ROOT/etc/NetworkManager/conf.d/50-auxo-dns.conf"
"$T" dns auto >/dev/null 2>&1
check "DNS: auto removes the override"       "[ ! -e $ROOT/etc/NetworkManager/conf.d/50-auxo-dns.conf ]"

echo "▲ cleanup / flatpak / mirrors / repo"
out=$("$T" cleanup 2>&1)
check "cleanup: cache + orphans"             "echo \"\$out\" | grep -q 'xbps-remove -Ooy'"
check "cleanup: old kernels via vkpurge"     "echo \"\$out\" | grep -q 'vkpurge rm all'"
out=$("$T" flatpak on 2>&1)
check "flatpak + flathub"                    "echo \"\$out\" | grep -q 'xbps-install -y flatpak' && echo \"\$out\" | grep -q 'remote-add --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo'"
out=$("$T" mirrors --url https://mirrors.example.org/voidlinux 2>&1)
check "mirrors: xmirror --set"               "echo \"\$out\" | grep -q 'xmirror -s https://mirrors.example.org/voidlinux'"
out=$("$T" repo multilib 2>&1)
check "repo multilib (+ multilib-nonfree)"   "echo \"\$out\" | grep -q 'xbps-install -y void-repo-multilib void-repo-multilib-nonfree'"

echo "▲ snapshots / zram"
out=$("$T" snapshots on --no-now --no-grub --description test 2>&1)
check "snapper config for /"                 "grep -q 'SUBVOLUME=\"/\"' $ROOT/etc/snapper/configs/root"
check "SNAPPER_CONFIGS=root"                 "grep -q 'SNAPPER_CONFIGS=\"root\"' $ROOT/etc/conf.d/snapper"
check "cron (timeline) + grub-btrfs on"      "enabled cronie && enabled grub-btrfs"
check "first snapshot taken"                 "echo \"\$out\" | grep -q 'snapper --no-dbus -c root create -d test'"
check "snapshots refused on ext4"            "! AUXO_TEST_FSTYPE=ext4 \"$T\" snapshots on >/dev/null 2>&1"
"$T" snapshots off >/dev/null 2>&1
check "snapshots off"                        "! enabled grub-btrfs && grep -q 'SNAPSHOTS=\"off\"' $ROOT/etc/auxo/auxo.conf"
"$T" zram on >/dev/null 2>&1
check "zramen: zstd, half of RAM"            "grep -q 'ZRAM_COMP_ALGORITHM=zstd' $ROOT/etc/sv/zramen/conf && grep -q 'ZRAM_SIZE=50' $ROOT/etc/sv/zramen/conf && enabled zramen"
"$T" zram off >/dev/null 2>&1
check "zram off"                             "! enabled zramen"

echo "▲ no boot splash"
check "ISO ships no plymouth"                "! grep -qx plymouth $HERE/mklive/packages.txt"
check "D-Bus starts elogind through runit"   "grep -q '^Exec=/usr/libexec/auxo/elogind-activate' $PKG/share/dbus-1/system-services/org.freedesktop.login1.service && sh -n $PKG/libexec/elogind-activate && grep -q 'sv -w 15 start elogind' $PKG/libexec/elogind-activate"
check "login1 override is packaged first"    "grep -q 'usr/local/share/dbus-1/system-services' $HERE/packages/build-xbps.sh && grep -q 'libexec/elogind-activate' $HERE/packages/build-xbps.sh"
check "initramfs never gets plymouth"        "grep -q '^omit_dracutmodules+=\" plymouth \"' $PKG/dracut/90-auxo-no-plymouth.conf && grep -q 'dracut.conf.d/\"' $HERE/packages/build-xbps.sh"
check "no splash command or theme left"      "! \"$T\" splash on >/dev/null 2>&1 && [ ! -e $PKG/share/auxo/plymouth ] && ! grep -rqi plymouth $PKG/bin $PKG/lib --exclude-dir=__pycache__"

echo "▲ gaming"
out=$(AUXO_TEST_GPU="1002:744c AMD RX 7900" "$T" gaming on --user alex 2>&1)
check "nonfree + multilib repos enabled"     "( for r in nonfree multilib multilib-nonfree; do [ -e $ROOT/var/db/xbps/.auxo-test/void-repo-\$r ] || exit 1; done )"
check "gaming stack + Steam 32-bit deps"     "echo \"\$out\" | grep -q 'xbps-install -y gamemode MangoHud gamescope steam libgcc-32bit'"
check "AMD 32-bit Vulkan"                    "echo \"\$out\" | grep -q 'mesa-vulkan-radeon-32bit'"
check "sysctl + ntsync"                      "grep -q 'vm.max_map_count = 2147483642' $ROOT/etc/sysctl.d/80-auxo-gaming.conf && grep -q '^ntsync' $ROOT/etc/modules-load.d/auxo-gaming.conf"
out=$("$T" gaming on --no-steam 2>&1)
check "--no-steam leaves Steam out"          "! echo \"\$out\" | grep -q ' steam'"
"$T" gaming off --purge >/dev/null 2>&1
check "gaming off removes tweaks"            "! test -f $ROOT/etc/sysctl.d/80-auxo-gaming.conf && grep -q 'GAMING=\"off\"' $ROOT/etc/auxo/auxo.conf"

echo "▲ auxo-motd (terminal greeting)"
M="$PKG/bin/auxo-motd"; MH=$(mktemp -d)
mm() { HOME=$MH XDG_STATE_HOME=$MH/state XDG_CONFIG_HOME=$MH/config USER=alex AUXO_MOTD_SEED=3 "$M" "$@" 2>&1; }
out=$(mm)
check "greets the user"                      "echo \"\$out\" | grep -qE 'Good (morning|afternoon|evening), alex|Up late, alex'"
check "shows a tip"                          "echo \"\$out\" | grep -q 'tip:\\|✦'"
check "suggests auxo-update when never run"  "echo \"\$out\" | grep -q 'never updated with auxo-update'"
mkdir -p $MH/state/auxo && touch -d '20 days ago' $MH/state/auxo/last-update
out=$(mm); check "nudges when the last update is old"   "echo \"\$out\" | grep -q 'last update 20 days ago · time for auxo-update'"
touch $MH/state/auxo/last-update
out=$(mm); check "quiet when updated today"             "echo \"\$out\" | grep -q 'last update today' && ! echo \"\$out\" | grep -q 'time for'"
mm --off >/dev/null
check "--off hides it"                       "[ -z \"\$(mm)\" ]"
mm --on >/dev/null
out=$(mm); check "--on brings it back"                  "echo \"\$out\" | grep -q alex"
check "no colour codes when not a terminal"  "! echo \"\$out\" | grep -q $'\\033'"
check "survives a closed pipe"               "mm | head -c1 >/dev/null; [ \${PIPESTATUS[0]} = 0 ]"
check "summit hint exists but stays rare"    "python3 -c 'import random,sys; sys.argv=[\"x\"]; exec(open(\"$M\").read().split(\"def main\")[0]); n=sum(tip(random.Random(i))[1]==\"hint\" for i in range(2000)); sys.exit(0 if 20<n<200 else 1)'"
check "cancer research reminder ~1 in 4"     "python3 -c 'import random,sys; sys.argv=[\"x\"]; exec(open(\"$M\").read().split(\"def main\")[0]); n=sum(tip(random.Random(i))[1]==\"give\" for i in range(2000)); sys.exit(0 if 380<n<620 else 1)'"
check "--give links the Terry Fox Foundation" "mm --give | grep -q 'https://terryfox.org/?form=GENERAL'"
check "shells run it in new terminals"       "grep -q auxo-motd $PKG/share/auxo/shell/prompt.zsh && grep -q auxo-motd $PKG/share/auxo/shell/prompt.bash && grep -q auxo-motd $PKG/share/auxo/shell/prompt.fish"
check "zshrc no longer runs auxo-fetch too"  "! grep -q 'auxo-fetch --small' $PKG/share/auxo/shell/zshrc"
rm -rf $MH

echo "▲ cybervis (compiled into its own package)"
ST=$(mktemp -d)
if command -v cc >/dev/null; then
  STAGE_ONLY=1 "$HERE/packages/build-xbps.sh" "$ST" >/dev/null 2>&1
  check "cybervis compiles"                  "[ -x $ST/cybervis/usr/bin/cybervis ]"
  check "cybervis runs (help)"               "$ST/cybervis/usr/bin/cybervis help 2>&1 | grep -q 'Terminal Spectral Engine'"
  check "cybervis is in the app menu"        "grep -q '^Exec=cybervis' $ST/cybervis/usr/share/applications/cybervis.desktop && grep -q '^Terminal=true' $ST/cybervis/usr/share/applications/cybervis.desktop"
else
  echo "  (no C compiler — skipping the cybervis build)"
fi
rm -rf "$ST"
check "ISO ships cybervis, not the CJK font"  "sed 's/#.*//' $HERE/mklive/packages.txt | grep -qx cybervis && ! sed 's/#.*//' $HERE/mklive/packages.txt | grep -qx noto-fonts-cjk-sans"
check "ISO has Plasma without the meta package" "! sed 's/#.*//' $HERE/mklive/packages.txt | grep -qx kde-plasma && sed 's/#.*//' $HERE/mklive/packages.txt | grep -qx plasma-desktop"
check "ISO Plasma list = auxo-tweak's (offline install)" "python3 -c \"import sys; sys.path.insert(0,'$PKG/lib'); from auxo_desktops import DESKTOPS; import re; iso=set(re.sub(r'#.*','',open('$HERE/mklive/packages.txt').read()).split()); sys.exit(not all(p in iso for p in DESKTOPS['plasma']['packages']))\""
check "live initramfs config is removed on install" "grep -q '90-auxo-live.conf' $HERE/installer/auxo_install/plan.py && grep -q '90-auxo-live.conf' $HERE/mklive/postsetup.sh"
check "package list has no stray comments"   "! sed 's/#.*//' $HERE/mklive/packages.txt | xargs | grep -q '#'"
check "ISO build installs a compiler"        "grep -q 'xbps-install -y .* gcc' $HERE/scripts/build-iso-inner.sh"

echo "▲ look engine (auxo-tweak look / mode / wallpaper)"
A="$ROOT/home/alex"; LC="$A/.config/auxo"
"$T" desktop hyprland --user alex >/dev/null 2>&1
"$T" look set gaps=16 rounding=4 border=3 blur=off bar=bottom ui_font="Fira Sans" font_size=11 --user alex --no-apply >/dev/null 2>&1
check "look.conf saved"                       "grep -q '^gaps=\"16\"' $LC/look.conf && grep -q '^ui_font=\"Fira Sans' $LC/look.conf"
check "Hyprland look: gaps, rounding, border" "grep -q 'gaps_out = 32' $LC/look-hyprland.conf && grep -q 'rounding = 4' $LC/look-hyprland.conf && grep -q 'border_size = 3' $LC/look-hyprland.conf"
check "Hyprland look: blur off"               "grep -A2 'blur' $LC/look-hyprland.conf | grep -q 'enabled = false'"
check "rice loads the look file last"         "tail -3 $A/.config/hypr/hyprland.conf | grep -q 'look-hyprland.conf'"
check "Sway + i3 look files"                  "grep -q 'gaps outer' $LC/look-sway && grep -q 'default_border pixel 3' $LC/look-i3"
check "waybar moved to the bottom"            "grep -q '\"position\": \"bottom\"' $A/.config/waybar/config.jsonc"
check "waybar CSS uses the font + radius"     "grep -q 'Fira Sans' $LC/look.css && grep -q '4px' $LC/look.css"
check "invalid values are refused"            "! \"$T\" look set gaps=500 --user alex --no-apply >/dev/null 2>&1 && ! \"$T\" look set colour=red --user alex --no-apply >/dev/null 2>&1"
check "look --json is valid"                  "\"$T\" look --json --user alex | python3 -m json.tool >/dev/null"
"$T" mode light --user alex --no-apply >/dev/null 2>&1
check "light mode: lighter colours"           "grep -q '^mode=\"light' $LC/look.conf && grep -qi 'f5f5f8\|fafafa\|ffffff' $LC/colors.css"
check "light mode: KDE Auxo Light scheme"     "grep -q 'ColorScheme=AuxoLight' $A/.config/kdeglobals && grep -q 'Name=Auxo Light' $ROOT/usr/share/color-schemes/AuxoLight.colors"
"$T" wallpaper /usr/share/wallpapers/auxo-cyan.png --user alex --no-apply >/dev/null 2>&1
check "wallpaper follows the choice"          "[ \$(readlink $LC/wallpaper.png) = /usr/share/wallpapers/auxo-cyan.png ]"
check "missing wallpaper refused"             "! \"$T\" wallpaper /nope.png --user alex >/dev/null 2>&1"
"$T" look reset --user alex --no-apply >/dev/null 2>&1
check "reset brings the defaults back"        "grep -q 'gaps_out = 16' $LC/look-hyprland.conf && ! grep -q '^mode=\"light' $LC/look.conf"

echo "▲ session rices (GNOME / Xfce / Cinnamon) + Chrono"
out=$("$T" rice gnome --user alex 2>&1)
check "GNOME rice needs no extensions"        "! echo \"\$out\" | grep -qi 'gnome-extensions\|enabled-extensions'"
out=$(PATH="$FB:$PATH" AUXO_HOME="$A" XDG_CURRENT_DESKTOP=GNOME "$T" session-apply --rice 2>&1)
check "GNOME: dark, Inter, accent, terminal key" "echo \"\$out\" | grep -q 'color-scheme prefer-dark' && echo \"\$out\" | grep -q 'font-name' && echo \"\$out\" | grep -q 'auxo-terminal'"
out=$(PATH="$FB:$PATH" AUXO_HOME="$A" XDG_CURRENT_DESKTOP=XFCE "$T" session-apply --rice 2>&1)
check "Xfce: xfconf theme, fonts, panel"      "echo \"\$out\" | grep -q 'xfconf-query -c xsettings' && echo \"\$out\" | grep -q 'xfwm4' && echo \"\$out\" | grep -q 'xfce4-panel'"
out=$(PATH="$FB:$PATH" AUXO_HOME="$A" XDG_CURRENT_DESKTOP=X-Cinnamon "$T" session-apply --rice 2>&1)
check "Cinnamon: gsettings theme + wallpaper" "echo \"\$out\" | grep -q 'org.cinnamon' && echo \"\$out\" | grep -q 'picture-uri'"
out=$("$T" desktop chrono --user alex 2>&1)
check "Chrono: wlroots 0.20 + Qt6 build deps" "echo \"\$out\" | grep -E 'xbps-install -y ' | grep -q 'wlroots0.20-devel' && echo \"\$out\" | grep -E 'xbps-install -y ' | grep -q 'qt6-declarative-devel'"
check "Chrono: built from the pinned commit"  "echo \"\$out\" | grep -q 'chrono checkout --quiet --force 48590c6' && echo \"\$out\" | grep -q 'cmake --install'"
check "Chrono: greetd starts chrono-session"  "grep -q 'chrono-session' $ROOT/etc/greetd/config.toml && grep -q 'DESKTOP=\"chrono\"' $ROOT/etc/auxo/auxo.conf"
check "Chrono: Auxo theme in your accent"     "grep -q '^name = Auxo' $A/.config/chrono/themes/auxo.theme && grep -q '^theme = auxo' $A/.config/chrono/chrono.conf"
printf '[general]\nterminal = alacritty\n' > "$A/.config/chrono/chrono.conf"
"$T" look set bar=top --user alex --no-apply >/dev/null 2>&1
check "Chrono: keeps your settings, follows the bar" "grep -q 'terminal = alacritty' $A/.config/chrono/chrono.conf && grep -q '^position = top' $A/.config/chrono/chrono.conf"

echo "▲ login screen + doctor"
check "greetd uses tty7, not agetty's tty1" "grep -q '^vt = 7' $ROOT/etc/greetd/config.toml"
ln -sf /etc/sv/agetty-tty1 "$ROOT/etc/runit/runsvdir/default/agetty-tty1"
sed -i 's/^vt = 7/vt = 1/' "$ROOT/etc/greetd/config.toml"; ln -sfn /etc/sv/greetd "$ROOT/etc/runit/runsvdir/default/greetd"
out=$("$T" doctor 2>&1)
check "doctor spots the tty1 clash"           "echo \"\$out\" | grep -q 'both use tty1'"
check "doctor --json is valid"                "\"$T\" doctor --json | python3 -m json.tool >/dev/null"
"$T" doctor --fix >/dev/null 2>&1
check "doctor --fix moves the login screen"   "grep -q '^vt = 7' $ROOT/etc/greetd/config.toml && \"$T\" doctor --json | python3 -c 'import json,sys; c={x[\"id\"]:x for x in json.load(sys.stdin)}; sys.exit(c[\"login-vt\"][\"problem\"] is not None)'"
sed -i 's/^vt = 7/vt = 1/' "$ROOT/etc/greetd/config.toml"
( cd "$ROOT" && sed -n "/^case/,/^esac/p" "$HERE/packages/build-xbps.sh" | sed "s/\${ACTION:-\$1}/post/" | sh )
check "package update repairs old installs"   "grep -q '^vt = 7' $ROOT/etc/greetd/config.toml"

echo "▲ sound (PipeWire)"
prev=$(grep '^DESKTOP=' "$ROOT/etc/auxo/auxo.conf" | cut -d'"' -f2)
out=$(AUXO_TEST_ONLINE=1 "$T" desktop sway --user alex 2>&1)
check "desktop switch links WirePlumber + Pulse" "echo \"\$out\" | grep -q 'ln -s /usr/share/examples/wireplumber/10-wireplumber.conf /etc/pipewire/pipewire.conf.d/10-wireplumber.conf' && echo \"\$out\" | grep -q '20-pipewire-pulse.conf'"
check "PipeWire autostarts + ALSA goes through it" "echo \"\$out\" | grep -q 'ln -s /usr/share/applications/pipewire.desktop /etc/xdg/autostart/pipewire.desktop' && echo \"\$out\" | grep -q '99-pipewire-default.conf'"
check "alsa-pipewire installed with every desktop" "[ -e $ROOT/var/db/xbps/.auxo-test/alsa-pipewire ] && python3 -c \"import sys; sys.path.insert(0,'$PKG/lib'); from auxo_desktops import DESKTOPS as D; sys.exit(not all('alsa-pipewire' in d['packages'] for k,d in D.items() if d['packages']))\""
check "tiling rices start PipeWire themselves" "grep -q '^exec-once = pipewire' $PKG/share/auxo/rices/hyprland/hypr/hyprland.conf && grep -q '^exec pipewire' $PKG/share/auxo/rices/sway/sway/config && grep -q '^exec --no-startup-id pipewire' $PKG/share/auxo/rices/i3/i3/config"
check "i3 + Cinnamon get a D-Bus session"    "python3 -c \"import sys; sys.path.insert(0,'$PKG/lib'); from auxo_desktops import DESKTOPS as D; sys.exit(not all('dbus-run-session' in D[k]['cmd'] for k in ('i3','cinnamon','sway','hyprland','plasma')))\""
check "ISO sets up sound for the live session" "grep -q '10-wireplumber.conf' $HERE/mklive/postsetup.sh && sed 's/#.*//' $HERE/mklive/packages.txt | grep -qx alsa-pipewire"
check "doctor checks sound"                  "\"$T\" doctor --json | python3 -c 'import json,sys; sys.exit(\"sound\" not in [c[\"id\"] for c in json.load(sys.stdin)])'"
sed -i "s/^DESKTOP=.*/DESKTOP=\"$prev\"/" "$ROOT/etc/auxo/auxo.conf"

echo "▲ monitors"
mkdir -p "$ROOT/sys/class/drm/card1-DP-1" "$ROOT/sys/class/drm/card1-HDMI-A-1"
echo connected > "$ROOT/sys/class/drm/card1-DP-1/status"; printf '2560x1440\n1920x1080\n' > "$ROOT/sys/class/drm/card1-DP-1/modes"
echo connected > "$ROOT/sys/class/drm/card1-HDMI-A-1/status"; printf '1920x1080\n' > "$ROOT/sys/class/drm/card1-HDMI-A-1/modes"
out=$(env -u WAYLAND_DISPLAY -u DISPLAY "$T" monitors --user alex 2>&1)
check "monitors lists connected screens"     "echo \"\$out\" | grep -q DP-1 && echo \"\$out\" | grep -q HDMI-A-1"
"$T" monitors set DP-1 mode=2560x1440@144 scale=1.25 position=0x0 vrr=on workspaces=1-5 --user alex >/dev/null 2>&1
"$T" monitors set HDMI-A-1 position=2048x0 rotate=90 workspaces=5-9 --user alex >/dev/null 2>&1
MH="$A/.config/auxo/monitors-hyprland.conf"
check "Hyprland: mode, position, scale, VRR"  "grep -q 'monitor = DP-1, 2560x1440@144, 0x0, 1.25, vrr, 1' $MH"
check "Hyprland: rotation + workspaces"       "grep -q 'monitor = HDMI-A-1, highrr, 2048x0, auto, transform, 1' $MH && grep -q 'workspace = 5, monitor:HDMI-A-1, default:true' $MH"
check "a workspace moves to its new screen"   "! grep -q 'workspace = 5, monitor:DP-1' $MH"
check "Sway: output + workspace lines"        "grep -q 'output DP-1 enable mode 2560x1440@144Hz scale 1.25 position 0 0 adaptive_sync on' $A/.config/auxo/monitors-sway && grep -q 'workspace 6 output HDMI-A-1' $A/.config/auxo/monitors-sway"
check "i3: xrandr script"                     "grep -q 'xrandr --output HDMI-A-1 --auto --pos 2048x0 --rotate right' $A/.config/auxo/monitors-xrandr.sh && [ -x $A/.config/auxo/monitors-xrandr.sh ]"
check "rices include the monitor files"       "grep -q 'source = ~/.config/auxo/monitors-hyprland.conf' $PKG/share/auxo/rices/hyprland/hypr/hyprland.conf && grep -q 'include ~/.config/auxo/monitors-sway' $PKG/share/auxo/rices/sway/sway/config && grep -q 'include ~/.config/auxo/monitors-i3' $PKG/share/auxo/rices/i3/i3/config"
check "Hyprland default: highest refresh rate" "grep -q '^monitor = , highrr, auto, auto' $PKG/share/auxo/rices/hyprland/hypr/hyprland.conf"
check "bad values refused"                    "! \"$T\" monitors set DP-1 rotate=45 --user alex >/dev/null 2>&1 && ! \"$T\" monitors set DP-1 scale=9 --user alex >/dev/null 2>&1"
check "turning every screen off is refused"   "! \"$T\" monitors set DP-1 enabled=off --user alex >/dev/null 2>&1 || ! \"$T\" monitors set HDMI-A-1 enabled=off --user alex >/dev/null 2>&1"
"$T" monitors reset --user alex >/dev/null 2>&1
check "reset goes back to automatic"          "! grep -q '^monitor = DP-1' $MH && [ -f $MH ]"
check "monitors --json is valid"              "env -u WAYLAND_DISPLAY -u DISPLAY \"$T\" monitors --json --user alex | python3 -m json.tool >/dev/null"

echo "▲ Mac-style Hyprland rice"
"$T" rice hyprland-mac --user alex >/dev/null 2>&1
check "base rice + Mac style on top"          "grep -q 'Mac style' $A/.config/hypr/style.conf && grep -q 'source = ~/.config/hypr/style.conf' $A/.config/hypr/hyprland.conf"
check "menu bar + dock"                       "grep -q '\"name\": \"menubar\"' $A/.config/waybar/config.jsonc && grep -q '\"name\": \"dock\"' $A/.config/waybar/config.jsonc && grep -q 'wlr/taskbar' $A/.config/waybar/config.jsonc"
check "Spotlight-style search on Super+Space" "grep -q 'bind = \$mod, Space, exec, pkill wofi || wofi --show drun --conf ~/.config/wofi/spotlight' $A/.config/hypr/style.conf && [ -f $A/.config/wofi/spotlight.css ]"
check "▲ menu script is executable"           "[ -x $A/.config/waybar/auxo-menu.sh ]"
check "wofi colours load (full path, not ../)" "grep -q '@import url(\"/home/alex/.config/auxo/colors.css\")' $A/.config/wofi/spotlight.css && grep -q '@import url(\"/home/alex/.config/auxo/colors.css\")' $A/.config/wofi/style.css"
check "no Nerd-Font-only icons in the bars"   "! python3 -c \"import glob,sys; sys.exit(not any(0xF0000 <= ord(c) <= 0xFFFFF for p in glob.glob('$PKG/share/auxo/rices/*/waybar/*') for c in open(p, encoding='utf-8').read()))\""
check "Mac look preset applied"               "grep -q 'gaps=\"10\"' $A/.config/auxo/look.conf && grep -q 'border=\"1\"' $A/.config/auxo/look.conf"
check "style remembered"                      "grep -q 'RICE_HYPRLAND=\"hyprland-mac\"' $ROOT/etc/auxo/auxo.conf"
"$T" look set bar=bottom --user alex --no-apply >/dev/null 2>&1
check "bar position leaves the menu bar on top" "grep -A3 '\"name\": \"menubar\"' $A/.config/waybar/config.jsonc | grep -q '\"position\": \"top\"'"
python3 - "$A/.config/waybar/config.jsonc" <<'PY' && ok=1 || ok=0
import json, re, sys
json.loads(re.sub(r"^\s*//.*$", "", open(sys.argv[1]).read(), flags=re.M))
PY
check "Mac waybar config is valid JSON"       "[ $ok = 1 ]"
check "Mac style: light mode + Lagoon wallpaper" "grep -q 'mode=\"light\"' $A/.config/auxo/look.conf && readlink $A/.config/auxo/wallpaper.png | grep -q auxo-lagoon"
check "Liquid Glass: vibrant blur + glass panels" "grep -q 'vibrancy' $A/.config/hypr/style.conf && grep -q 'auxo-launchpad' $A/.config/hypr/style.conf && grep -q 'auxo-launcher --style mac' $A/.config/waybar/config.jsonc"

echo "▲ Windows-style Hyprland rice"
"$T" rice hyprland-win --user alex >/dev/null 2>&1
check "taskbar at the bottom with Start"      "grep -q '\"custom/start\"' $A/.config/waybar/config.jsonc && grep -q '\"position\": \"bottom\"' $A/.config/waybar/config.jsonc"
check "Super opens Start, Alt+F4 closes"      "grep -q 'bindr = SUPER, SUPER_L, exec' $A/.config/hypr/style.conf && grep -q 'ALT, F4, killactive' $A/.config/hypr/style.conf"
check "Windows preset + Bloom wallpaper"      "grep -q 'rounding=\"8\"' $A/.config/auxo/look.conf && readlink $A/.config/auxo/wallpaper.png | grep -q auxo-bloom"
check "style remembered (win)"                "grep -q 'RICE_HYPRLAND=\"hyprland-win\"' $ROOT/etc/auxo/auxo.conf"
python3 - "$A/.config/waybar/config.jsonc" <<'PY' && ok=1 || ok=0
import json, re, sys
json.loads(re.sub(r"^\s*//.*$", "", open(sys.argv[1]).read(), flags=re.M))
PY
check "Windows waybar config is valid JSON"   "[ $ok = 1 ]"
check "bar icons are real glyphs (not blank)" "python3 -c \"import glob,json,re,sys; sys.exit(not all(any(0xF000 <= ord(c) <= 0xF8FF for c in json.dumps(json.loads(re.sub(r'^\\\\s*//.*\$','',open(p).read(),flags=re.M)),ensure_ascii=False)) for p in glob.glob('$PKG/share/auxo/rices/*/waybar/config.jsonc')))\""
check "Windows: Super+V is clipboard history"  "grep -q 'unbind = \$mod, V' $A/.config/hypr/style.conf && grep -q 'bind = \$mod, V, exec, auxo-clipboard' $A/.config/hypr/style.conf"
check "clipboard history kept on every Wayland rice" "grep -q 'wl-paste --watch cliphist store' $PKG/share/auxo/rices/hyprland/hypr/hyprland.conf && grep -q 'wl-paste --watch cliphist store' $PKG/share/auxo/rices/sway/sway/config"
check "Super+Shift+V / Super+. on Hyprland and Sway" "grep -q 'SHIFT, V, exec, auxo-clipboard' $PKG/share/auxo/rices/hyprland/hypr/hyprland.conf && grep -q 'mod+period exec rofimoji' $PKG/share/auxo/rices/sway/sway/config"
check "cliphist, rofimoji, gammastep installed with the WMs" "python3 -c \"import sys; sys.path.insert(0,'$PKG/lib'); from auxo_desktops import WAYLAND_WM as W; sys.exit(not {'cliphist','rofimoji','gammastep'} <= set(W))\""
check "auxo-clipboard parses"                  "sh -n $PKG/bin/auxo-clipboard"
out=$(env -u WAYLAND_DISPLAY -u DISPLAY AUXO_HOME="$A" "$PKG/bin/auxo-keys" --wm hyprland --print 2>&1)
check "auxo-keys names them"                   "echo \"\$out\" | grep -q 'Clipboard history' && echo \"\$out\" | grep -q 'Emoji picker' && echo \"\$out\" | grep -q 'Start menu'"
out=$(QT_QPA_PLATFORM=offscreen AUXO_HOME="$A" python3 "$HERE/tests/test-shell.py" 2>&1); rc=$?
echo "$out" | sed 's/^/    /' | tail -3
check "launcher + control center (offscreen)" "[ $rc = 0 ]"
"$T" rice hyprland --user alex >/dev/null 2>&1
check "back to the Auxo style"               "! grep -q 'Mac style' $A/.config/hypr/style.conf && ! grep -q '\"name\": \"dock\"' $A/.config/waybar/config.jsonc && grep -q 'RICE_HYPRLAND=\"hyprland\"' $ROOT/etc/auxo/auxo.conf"
check "Hyprland waybar scrolls workspaces (0.49 syntax)" "! grep -rq 'hl.dsp' $PKG/share/auxo/rices && grep -q 'hyprctl dispatch workspace e+1' $PKG/share/auxo/rices/hyprland/waybar/config.jsonc"

echo "▲ keyboard shortcuts (auxo-keys)"
out=$(env -u WAYLAND_DISPLAY -u DISPLAY AUXO_HOME="$A" "$PKG/bin/auxo-keys" --wm hyprland --print 2>&1)
check "lists the Hyprland shortcuts in words" "echo \"\$out\" | grep -q 'Super+Enter *Terminal' && echo \"\$out\" | grep -q 'Super+1…9 *Go to workspace 1–9'"
check "Super+/ opens it on every tiling rice" "grep -q 'slash, exec, pkill wofi || auxo-keys' $PKG/share/auxo/rices/hyprland/hypr/hyprland.conf && grep -q 'mod+slash exec auxo-keys' $PKG/share/auxo/rices/sway/sway/config && grep -q 'mod+slash exec --no-startup-id auxo-keys' $PKG/share/auxo/rices/i3/i3/config"

echo "▲ presets / sharing a rice / snapshots / AI"
"$T" look preset retro --user alex --no-apply >/dev/null 2>&1
check "preset retro: square, no blur, mono UI" "grep -q 'rounding=\"0\"' $LC/look.conf && grep -q 'blur=\"off\"' $LC/look.conf && grep -q 'ui_font=\"Fira Code\"' $LC/look.conf"
check "unknown preset refused"                "! \"$T\" look preset nope --user alex --no-apply >/dev/null 2>&1"
"$T" look export /home/alex/r.auxorice --name "Alex test" --user alex >/dev/null 2>&1
check "export writes a rice file"             "grep -q '^name = Alex test' $A/r.auxorice && grep -q '^rounding = 0' $A/r.auxorice"
"$T" look reset --user alex --no-apply >/dev/null 2>&1
out=$("$T" look import /home/alex/r.auxorice --user alex --no-apply 2>&1)
check "import brings the rice back"           "grep -q 'rounding=\"0\"' $LC/look.conf && echo \"\$out\" | grep -q 'Alex test'"
printf 'hello\n' > "$A/bad.auxorice"
check "a non-rice file is refused"            "! \"$T\" look import /home/alex/bad.auxorice --user alex --no-apply >/dev/null 2>&1"
rm -rf "$ROOT/etc/snapper"
check "snapshot now needs snapshots on"       "! \"$T\" snapshots now >/dev/null 2>&1"
mkdir -p "$ROOT/etc/snapper/configs"; : > "$ROOT/etc/snapper/configs/root"
check "snapshot now runs snapper create"      "\"$T\" snapshots now --description test 2>&1 | grep -q 'snapper --no-dbus -c root create -d test'"
out=$("$T" ai on --model small 2>&1)
check "ai on: Ollama into /usr/local"         "echo \"\$out\" | grep -q 'ollama.com/download/ollama-linux-amd64.tar.zst' && echo \"\$out\" | grep -q 'tar --zstd -x -C'"
check "ai on: runit service as its own user"  "grep -q 'chpst -u ollama:ollama /usr/local/bin/ollama serve' $ROOT/etc/sv/ollama/run && enabled ollama"
check "ai on: pulls the small model"          "echo \"\$out\" | grep -q 'ollama pull llama3.2:1b' && grep -q 'AI_MODEL=\"llama3.2:1b\"' $ROOT/etc/auxo/auxo.conf"
"$T" ai off >/dev/null 2>&1
check "ai off stops it"                       "! enabled ollama && grep -q 'AI=\"off\"' $ROOT/etc/auxo/auxo.conf"
# auxo-ai against a stand-in Ollama server
PORT=$((20000 + RANDOM % 20000))
python3 - "$PORT" <<'PY' &
import http.server, json, sys
class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_GET(self):
        self.send_response(200); self.end_headers(); self.wfile.write(b'{"version":"0"}')
    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        ok = body["messages"][0]["role"] == "system" and "Void" in body["messages"][0]["content"]
        self.send_response(200); self.end_headers()
        for piece in (["Use ", "auxo-tweak service bluetooth on"] if ok else ["no system prompt"]):
            self.wfile.write((json.dumps({"message": {"content": piece}, "done": False}) + "\n").encode())
        self.wfile.write(b'{"done": true}\n')
http.server.HTTPServer(("127.0.0.1", int(sys.argv[1])), H).handle_request(); 
http.server.HTTPServer(("127.0.0.1", int(sys.argv[1])), H).serve_forever()
PY
SRV=$!; sleep 0.6
out=$(OLLAMA_HOST=127.0.0.1:$PORT AUXO_TWEAK="$T" timeout 20 "$PKG/bin/auxo-ai" --plain "bluetooth?" 2>&1)
check "auxo-ai streams an answer with Auxo context" "echo \"\$out\" | grep -q 'Use auxo-tweak service bluetooth on'"
kill $SRV 2>/dev/null
out=$(OLLAMA_HOST=127.0.0.1:1 "$PKG/bin/auxo-ai" "hi" 2>&1); rc=$?
check "auxo-ai explains how to turn it on"    "[ $rc = 2 ] && echo \"\$out\" | grep -q 'sudo auxo-tweak ai on'"

echo "▲ app bundles"
out=$("$T" apps 2>&1)
check "apps lists the bundles"                "echo \"\$out\" | grep -q creative && echo \"\$out\" | grep -q dev"
out=$(AUXO_TEST_ONLINE=1 "$T" apps media fun 2>&1)
check "apps installs a bundle"                "echo \"\$out\" | grep -E 'xbps-install -y ' | grep -q 'mpv' && echo \"\$out\" | grep -E 'xbps-install -y ' | grep -q cmatrix"
out=$(AUXO_TEST_ONLINE=1 "$T" apps discord 2>&1)
check "apps discord: from Flathub (Void's is restricted)" "echo \"\$out\" | grep -q 'flatpak remote-add --system --if-not-exists flathub' && echo \"\$out\" | grep -q 'flatpak install --system -y --noninteractive flathub com.discordapp.Discord' && ! echo \"\$out\" | grep -E 'xbps-install -y ' | grep -qw discord"
out=$(AUXO_TEST_ONLINE=1 "$T" apps chat 2>&1)
check "apps chat: xbps for the rest, Flathub for Discord" "echo \"\$out\" | grep -E 'xbps-install -y ' | grep -q telegram-desktop && echo \"\$out\" | grep -q 'flathub com.discordapp.Discord'"
check "no bundle asks xbps for a restricted package" "python3 -c \"import sys; sys.path.insert(0,'$PKG/lib'); from auxo_apps import APP_BUNDLES, FLATPAK_APPS; sys.exit(any(p=='discord' and p not in FLATPAK_APPS for _,(d,ps) in APP_BUNDLES.items() for p in ps))\""
check "unknown bundle refused"                "! \"$T\" apps nope >/dev/null 2>&1"
check "Settings app + menu entry ship"        "python3 -m py_compile $PKG/bin/auxo-settings && grep -q '^Exec=auxo-settings' $PKG/applications/auxo-settings.desktop"
check "ISO has the PyQt6 GUI modules"         "sed 's/#.*//' $HERE/mklive/packages.txt | grep -qx python3-pyqt6-widgets && sed 's/#.*//' $HERE/mklive/packages.txt | grep -qx python3-pyqt6-gui"

echo "▲ info / fetch / scripts"
out=$("$T" info 2>&1); echo "$out" | sed 's/^/    /' | head -6
check "info shows settings"                  "echo \"\$out\" | grep -q 'desktop    chrono' && echo \"\$out\" | grep -q 'kernel     linux-lts'"
out=$(AUXO_ROOT=/ "$PKG/bin/auxo-fetch" --accent amber 2>&1)
check "fetch prints os + accent"             "echo \"\$out\" | grep -q 'accent'"
check "fetch --json is valid"                "AUXO_ROOT=/ \"$PKG/bin/auxo-fetch\" --json | python3 -m json.tool >/dev/null"
check "no pacman/systemctl left in the tools" "! grep -rnIE 'pacman|systemctl|mkinitcpio|archiso' $PKG/bin $PKG/lib $PKG/etc --exclude-dir=__pycache__ | grep -v 'Never suggest' | grep -q ."
check "shell scripts parse"                  "bash -n $PKG/bin/auxo-update && bash -n $PKG/bin/auxo-rollback && sh -n $PKG/bin/auxo-branding && sh -n $PKG/bin/auxo-polkit-agent && bash -n $PKG/share/auxo/shell/prompt.bash"

echo
printf '%d passed, %d failed\n' "$pass" "$fail"
(( fail == 0 ))
