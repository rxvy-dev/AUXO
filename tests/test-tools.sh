#!/usr/bin/env bash
# Exercises auxo-tweak / auxo-fetch against a throw-away fake Void root
# (no root, no xbps needed: commands are printed instead of run).
#   ./tests/test-tools.sh
set -uo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd)
PKG="$HERE/packages/auxo-tools/files"
ROOT=$(mktemp -d /tmp/auxo-fakeroot.XXXX)
trap 'rm -rf "$ROOT"' EXIT
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
out=$("$T" accent cyan --user alex 2>&1)
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
check "removes the live Plasma"              "echo \"\$out\" | grep -q 'xbps-remove -y -R kde-plasma'"
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
check "i3 starts through startx"             "grep -q \"\\-\\-cmd 'startx /usr/bin/i3'\" $ROOT/etc/greetd/config.toml"
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
sa() { XDG_CURRENT_DESKTOP="$1" PATH="$FB:$PATH" "$T" session-apply --accent emerald 2>&1; }
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

echo "▲ splash (dracut)"
out=$("$T" splash on 2>&1)
check "installs plymouth"                    "echo \"\$out\" | grep -q 'xbps-install -y plymouth'"
check "dracut includes plymouth"             "grep -q 'add_dracutmodules+=\" plymouth \"' $ROOT/etc/dracut.conf.d/auxo-splash.conf"
check "kernel cmdline has splash"            "grep -q '^GRUB_CMDLINE_LINUX_DEFAULT=\".*quiet.*splash' $ROOT/etc/default/grub"
check "plymouthd.conf Theme=auxo"            "grep -q '^Theme=auxo' $ROOT/etc/plymouth/plymouthd.conf"
check "initramfs rebuilt"                    "echo \"\$out\" | grep -q 'dracut --regenerate-all --force'"
out=$("$T" splash on 2>&1)
check "second run is a no-op"                "! echo \"\$out\" | grep -q 'dracut --regenerate' && [ \$(grep -o splash $ROOT/etc/default/grub | wc -l) = 1 ]"
out=$("$T" accent amber --user alex --no-grub 2>&1)
check "accent change recolours the splash"   "cmp -s $ROOT/usr/share/plymouth/themes/auxo/logo.png $ROOT/usr/share/auxo/plymouth/accents/amber/logo.png && echo \"\$out\" | grep -q 'dracut --regenerate-all'"
"$T" splash off >/dev/null 2>&1
check "splash off keeps plymouth out"        "grep -q 'omit_dracutmodules+=\" plymouth \"' $ROOT/etc/dracut.conf.d/auxo-splash.conf && ! grep -q splash $ROOT/etc/default/grub"
check "ISO ships no plymouth"                "! grep -qx plymouth $HERE/mklive/packages.txt"
check "D-Bus starts elogind through runit"   "grep -q '^Exec=/usr/libexec/auxo/elogind-activate' $PKG/share/dbus-1/system-services/org.freedesktop.login1.service && sh -n $PKG/libexec/elogind-activate && grep -q 'sv -w 15 start elogind' $PKG/libexec/elogind-activate"
check "login1 override is packaged first"    "grep -q 'usr/local/share/dbus-1/system-services' $HERE/packages/build-xbps.sh && grep -q 'libexec/elogind-activate' $HERE/packages/build-xbps.sh"
check "runit closes the splash at boot"   "sh -n $PKG/etc/runit/core-services/99-auxo-plymouth.sh && grep -q 'plymouth quit' $PKG/etc/runit/core-services/99-auxo-plymouth.sh && ! grep -qE '^[[:space:]]*exit' $PKG/etc/runit/core-services/99-auxo-plymouth.sh"
check "splash hook is packaged"              "grep -q 'core-services/99-auxo-plymouth.sh' $HERE/packages/build-xbps.sh"

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

echo "▲ info / fetch / scripts"
out=$("$T" info 2>&1); echo "$out" | sed 's/^/    /' | head -6
check "info shows settings"                  "echo \"\$out\" | grep -q 'desktop    none' && echo \"\$out\" | grep -q 'kernel     linux-lts'"
out=$(AUXO_ROOT=/ "$PKG/bin/auxo-fetch" --accent amber 2>&1)
check "fetch prints os + accent"             "echo \"\$out\" | grep -q 'accent'"
check "fetch --json is valid"                "AUXO_ROOT=/ \"$PKG/bin/auxo-fetch\" --json | python3 -m json.tool >/dev/null"
check "no pacman/systemctl left in the tools" "! grep -rnIE 'pacman|systemctl|mkinitcpio|archiso' $PKG/bin $PKG/lib $PKG/etc --exclude-dir=__pycache__ | grep -q ."
check "shell scripts parse"                  "bash -n $PKG/bin/auxo-update && bash -n $PKG/bin/auxo-rollback && sh -n $PKG/bin/auxo-branding && sh -n $PKG/bin/auxo-polkit-agent"

echo
printf '%d passed, %d failed\n' "$pass" "$fail"
(( fail == 0 ))
