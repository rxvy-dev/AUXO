#!/usr/bin/env bash
# Exercises auxo-tweak / auxo-fetch against a throw-away fake root (no root, no pacman needed).
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

# ── fake system ──
mkdir -p "$ROOT"/etc/default "$ROOT"/etc/greetd "$ROOT"/usr/bin "$ROOT"/usr/share/{wallpapers,auxo,sddm/themes/breeze} "$ROOT"/boot/grub "$ROOT"/var/lib/pacman/local "$ROOT"/home/alex
cp "$PKG"/share/wallpapers/* "$ROOT/usr/share/wallpapers/"
cp -r "$PKG"/share/auxo/. "$ROOT/usr/share/auxo/"
install -Dm644 "$PKG/etc/auxo/auxo.conf" "$ROOT/etc/auxo/auxo.conf"
printf 'root:x:0:0::/root:/bin/bash\nalex:x:1000:1000::/home/alex:/bin/bash\nsddm:x:964:964::/var/lib/sddm:/usr/bin/nologin\n' > "$ROOT/etc/passwd"
cat > "$ROOT/etc/default/grub" <<'EOF'
GRUB_DEFAULT=0
GRUB_TIMEOUT=5
GRUB_DISTRIBUTOR="Arch"
GRUB_CMDLINE_LINUX_DEFAULT="loglevel=3 quiet"
GRUB_TERMINAL_OUTPUT=console
#GRUB_THEME="/path/to/gfxtheme"
EOF
cat > "$ROOT/etc/pacman.conf" <<'EOF'
[options]
#Color
#ParallelDownloads = 5
[core]
Include = /etc/pacman.d/mirrorlist
#[multilib]
#Include = /etc/pacman.d/mirrorlist
EOF
touch "$ROOT/usr/bin/sddm" "$ROOT/usr/share/sddm/themes/breeze/metadata.desktop"
for p in linux plasma-desktop konsole dolphin kate sddm nvidia-open nvidia-utils zsh; do mkdir -p "$ROOT/var/lib/pacman/local/$p-1.0-1"; done

echo "▲ accent"
out=$("$T" accent rose --user alex --no-grub 2>&1); echo "$out" | sed 's/^/    /'
check "auxo.conf records accent"            "grep -q 'ACCENT=\"rose\"' $ROOT/etc/auxo/auxo.conf"
check "GRUB theme written with rose accent" "grep -q '#fb7185' $ROOT/usr/share/grub/themes/auxo/theme.txt"
check "GRUB background copied"               "cmp -s $ROOT/usr/share/grub/themes/auxo/background.png $ROOT/usr/share/wallpapers/auxo-rose.png"
check "GRUB selection pixmaps are PNGs"      "file $ROOT/usr/share/grub/themes/auxo/select_c.png | grep -q 'PNG image'"
check "/etc/default/grub → GRUB_THEME"       "grep -q '^GRUB_THEME=\"/usr/share/grub/themes/auxo/theme.txt\"' $ROOT/etc/default/grub"
check "/etc/default/grub → gfxterm"          "grep -q '^GRUB_TERMINAL_OUTPUT=\"gfxterm\"' $ROOT/etc/default/grub && [ \$(grep -c GRUB_TERMINAL_OUTPUT $ROOT/etc/default/grub) = 1 ]"
check "live SDDM background = rose wallpaper" "grep -q auxo-rose.png $ROOT/usr/share/sddm/themes/breeze/theme.conf.user && grep -q 'Current=breeze' $ROOT/etc/sddm.conf.d/10-auxo.conf"
check "text login (tuigreet) in rose accent"  "grep -q 'tuigreet' $ROOT/etc/greetd/config.toml && grep -q 'border=red' $ROOT/etc/greetd/config.toml && grep -q '^user = \"greeter\"' $ROOT/etc/greetd/config.toml"
check "greetd config is valid TOML"          "python3 -c 'import tomllib,sys; tomllib.load(open(sys.argv[1],\"rb\"))' $ROOT/etc/greetd/config.toml"
check "MOTD has truecolor accent"            "grep -q '38;2;251;113;133' $ROOT/etc/motd"
check "user colour files"                    "test -f $ROOT/home/alex/.config/auxo/colors-hyprland.conf && grep -q 'rgb(fb7185)' $ROOT/home/alex/.config/auxo/colors-hyprland.conf"
check "user wallpaper symlink"               "[ \$(readlink $ROOT/home/alex/.config/auxo/wallpaper.png) = /usr/share/wallpapers/auxo-rose.png ]"
check "KDE accent in kdeglobals"             "grep -q 'AccentColor=251,113,133' $ROOT/home/alex/.config/kdeglobals"
check "session-apply autostart"              "test -f $ROOT/home/alex/.config/autostart/auxo-session-apply.desktop"
"$T" accent emerald --user alex --no-grub >/dev/null 2>&1
check "accent switch keeps kdeglobals tidy"  "[ \$(grep -c AccentColor $ROOT/home/alex/.config/kdeglobals) = 1 ] && grep -q '52,211,153' $ROOT/home/alex/.config/kdeglobals"
check "invalid accent rejected"              "! \"$T\" accent purple >/dev/null 2>&1"

printf 'insmod gfxterm\nset theme=/usr/share/grub/themes/auxo/theme.txt\n' > "$ROOT/boot/grub/grub.cfg"
out=$("$T" accent cyan --user alex 2>&1)
check "accent change skips grub-mkconfig once theme is wired" "! echo \"\$out\" | grep -q grub-mkconfig"
rm "$ROOT/boot/grub/grub.cfg"
out=$("$T" accent cyan --user alex 2>&1)
check "first theme setup rebuilds grub.cfg under a lock" "echo \"\$out\" | grep -q 'flock -w 120 /run/lock/auxo-grub.lock grub-mkconfig'"
"$T" accent emerald --user alex --no-grub >/dev/null 2>&1

echo "▲ desktop hyprland --replace"
out=$("$T" desktop hyprland --user alex --replace 2>&1); echo "$out" | sed 's/^/    /' | head -12
check "installs hyprland packages"           "echo \"\$out\" | grep -q 'pacman --noconfirm --needed -S hyprland'"
check "skips already-installed sddm"         "! echo \"\$out\" | grep -E 'pacman .*-S .* sddm( |$)' >/dev/null"
check "removes live Plasma"                  "echo \"\$out\" | grep -q 'pacman --noconfirm -Rns plasma-desktop'"
check "enables greetd (text login)"          "echo \"\$out\" | grep -q 'systemctl enable -f greetd'"
check "disables sddm"                        "echo \"\$out\" | grep -q 'systemctl disable sddm'"
check "rice copied: hyprland.lua"            "grep -q 'require, \"hypr_colors\"' $ROOT/home/alex/.config/hypr/hyprland.lua"
check "hypr_colors.lua has current accent"   "grep -q 'accent = \"rgb(34d399)\"' $ROOT/home/alex/.config/auxo/hypr_colors.lua"
check "rice copied: waybar imports colours"  "grep -q 'auxo/colors.css' $ROOT/home/alex/.config/waybar/style.css"
check "login starts Hyprland by default"     "grep -q \"\\-\\-cmd 'Hyprland'\" $ROOT/etc/greetd/config.toml"
check "username pre-filled"                  "grep -q '^alex$' $ROOT/var/cache/tuigreet/lastuser"
rm -f "$ROOT/usr/share/sddm/themes/breeze/metadata.desktop"; "$T" accent emerald --user alex --no-grub --no-initramfs >/dev/null 2>&1
check "no Breeze theme → SDDM not pointed at it (black-screen bug)" "grep -q '^Current=$' $ROOT/etc/sddm.conf.d/10-auxo.conf && ! grep -q breeze_cursors $ROOT/etc/sddm.conf.d/10-auxo.conf"
check "auxo.conf DESKTOP=hyprland"           "grep -q 'DESKTOP=\"hyprland\"' $ROOT/etc/auxo/auxo.conf"
"$T" rice hyprland --user alex >/dev/null 2>&1
check "re-applying keeps a backup"           "test -f $ROOT/home/alex/.config/hypr/hyprland.lua.auxo-bak"

echo "▲ desktop i3 / sway"
"$T" desktop i3 --user alex >/dev/null 2>&1
check "i3 config + polybar launch exec"      "test -x $ROOT/home/alex/.config/polybar/launch.sh && grep -q colors-i3 $ROOT/home/alex/.config/i3/config"
check "i3 starts through startx"             "grep -q \"\\-\\-cmd 'startx /usr/bin/i3'\" $ROOT/etc/greetd/config.toml"
"$T" desktop sway --user alex >/dev/null 2>&1
check "sway config"                          "grep -q colors-sway $ROOT/home/alex/.config/sway/config && grep -q 'sway/workspaces' $ROOT/home/alex/.config/waybar/config.jsonc"
check "mako config generated w/ accent"      "grep -q 'border-color=#34d399' $ROOT/home/alex/.config/mako/config"

echo "▲ desktop none"
out=$("$T" desktop none --user alex 2>&1)
check "none → multi-user target"             "echo \"\$out\" | grep -q 'set-default multi-user.target'"

echo "▲ GNOME extensions + defaults"
out=$("$T" desktop gnome --user alex 2>&1)
D="$ROOT/etc/dconf/db/local.d/00-auxo"
check "repo extensions installed"            "echo \"\$out\" | grep -q -- 'gnome-shell-extension-appindicator' && echo \"\$out\" | grep -q -- 'gnome-shell-extension-caffeine'"
check "dock + blur fetched for GNOME 50"     "echo \"\$out\" | grep -q 'fetch dash-to-dock@micxgx.gmail.com for GNOME 50' && echo \"\$out\" | grep -q 'fetch blur-my-shell@aunetx'"
check "dconf profile + defaults written"     "grep -q 'system-db:local' $ROOT/etc/dconf/profile/user && grep -q \"enabled-extensions=\\['appindicatorsupport\" \"\$D\""
check "dock dots use the accent"             "grep -q \"custom-theme-running-dots-color='#\" \"\$D\""
check "dconf update ran"                     "echo \"\$out\" | grep -q 'dconf update'"
"$T" accent rose --no-grub --no-initramfs --user alex >/dev/null 2>&1
check "accent change updates GNOME defaults" "grep -q \"accent-color='pink'\" \"\$D\" && grep -q 'auxo-rose.png' \"\$D\""
out=$("$T" rice gnome 2>&1)
check "rice gnome re-applies"                "echo \"\$out\" | grep -q 'GNOME: dock'"
"$T" desktop none --user alex >/dev/null 2>&1   # back to the state the later checks expect

echo "▲ live wallpaper change per desktop"
FB="$ROOT/fakebin"; mkdir -p "$FB"
for b in hyprctl hyprpaper swaymsg feh i3-msg gsettings plasma-apply-wallpaperimage plasma-apply-colorscheme xfconf-query makoctl; do printf '#!/bin/sh\n' > "$FB/$b"; chmod +x "$FB/$b"; done
sa() { XDG_CURRENT_DESKTOP="$1" PATH="$FB:$PATH" "$T" session-apply --accent emerald 2>&1; }
check "KDE: plasma wallpaper"                "sa KDE | grep -q 'plasma-apply-wallpaperimage /usr/share/wallpapers/auxo-emerald.png'"
check "GNOME: gsettings picture-uri"         "sa GNOME | grep -q 'org.gnome.desktop.background picture-uri file:///usr/share/wallpapers/auxo-emerald.png'"
check "Cinnamon: picture-uri"                "sa X-Cinnamon | grep -q 'org.cinnamon.desktop.background picture-uri'"
check "Xfce: last-image"                     "sa XFCE | grep -q 'last-image --create -t string -s /usr/share/wallpapers/auxo-emerald.png'"
check "Hyprland: hyprpaper restarted"        "sa Hyprland | grep -q 'hyprctl dispatch exec hyprpaper'"
check "Sway: output bg"                      "sa sway | grep -q \"swaymsg output '\\*' bg /usr/share/wallpapers/auxo-emerald.png fill\""
check "i3: feh"                              "sa i3 | grep -q 'feh --no-fehbg --bg-fill /usr/share/wallpapers/auxo-emerald.png'"

echo "▲ shell / kernel / snapshots / zram / multilib"
out=$("$T" shell fish --user alex 2>&1)
check "fish installed + chsh"                "echo \"\$out\" | grep -q -- '-S fish' && echo \"\$out\" | grep -q 'chsh -s /usr/bin/fish alex'"
printf '# grml stub\n' > "$ROOT/home/alex/.zshrc"
"$T" shell zsh --user alex >/dev/null 2>&1
check "zsh: Auxo zshrc replaces skel stub"   "grep -q auxo/shell/prompt.zsh $ROOT/home/alex/.zshrc && grep -q 'grml stub' $ROOT/home/alex/.zshrc.auxo-bak"
check "fish config seeded"                   "grep -q prompt.fish $ROOT/home/alex/.config/fish/config.fish"
out=$("$T" kernel linux-zen --no-grub 2>&1)
check "kernel: zen + headers"                "echo \"\$out\" | grep -q -- '-S linux-zen\$' && echo \"\$out\" | grep -q -- '-S linux-zen-headers'"
check "kernel: GRUB_TOP_LEVEL"               "grep -q 'GRUB_TOP_LEVEL=\"/boot/vmlinuz-linux-zen\"' $ROOT/etc/default/grub"
out=$("$T" snapshots on --no-now --no-grub --description test 2>&1)
check "snapper config for /"                 "grep -q 'SUBVOLUME=\"/\"' $ROOT/etc/snapper/configs/root"
check "SNAPPER_CONFIGS=root"                 "grep -q 'SNAPPER_CONFIGS=\"root\"' $ROOT/etc/conf.d/snapper"
check "grub-btrfsd enabled"                  "echo \"\$out\" | grep -q 'systemctl enable grub-btrfsd.service'"
check "first snapshot taken"                 "echo \"\$out\" | grep -q 'snapper --no-dbus -c root create -d test'"
AUXO_TEST_FSTYPE=ext4 "$T" snapshots on >/dev/null 2>&1
check "snapshots refused on ext4"            "[ \$? -ne 0 ] || ! AUXO_TEST_FSTYPE=ext4 \"$T\" snapshots on >/dev/null 2>&1"
"$T" zram on >/dev/null 2>&1
check "zram-generator config"                "grep -q 'zram-size = min(ram / 2, 8192)' $ROOT/etc/systemd/zram-generator.conf"
"$T" multilib on >/dev/null 2>&1
check "multilib enabled"                     "grep -q '^\[multilib\]' $ROOT/etc/pacman.conf"
out=$("$T" info 2>&1); echo "$out" | sed 's/^/    /'
check "info shows settings"                  "echo \"\$out\" | grep -q 'desktop    none'"

out=$("$T" shell bash --user alex 2>&1); "$T" shell bash --user alex >/dev/null 2>&1
check "bash prompt sourced once"             "[ \$(grep -c prompt.bash $ROOT/home/alex/.bashrc) = 1 ]"

echo "▲ splash"
printf 'MODULES=()\nHOOKS=(base udev autodetect microcode modconf kms keyboard keymap consolefont block filesystems fsck)\n' > "$ROOT/etc/mkinitcpio.conf"
out=$("$T" splash on 2>&1); echo "$out" | sed 's/^/    /'
check "installs plymouth"                    "echo \"\$out\" | grep -q 'pacman --noconfirm --needed -S plymouth'"
check "plymouth hook right after udev"       "grep -q '^HOOKS=(base udev plymouth autodetect' $ROOT/etc/mkinitcpio.conf"
check "kernel cmdline has splash"            "grep -q '^GRUB_CMDLINE_LINUX_DEFAULT=\".*quiet.*splash' $ROOT/etc/default/grub"
check "plymouthd.conf Theme=auxo"            "grep -q '^Theme=auxo' $ROOT/etc/plymouth/plymouthd.conf"
check "theme script + accent images copied"  "test -f $ROOT/usr/share/plymouth/themes/auxo/auxo.script && cmp -s $ROOT/usr/share/plymouth/themes/auxo/logo.png $ROOT/usr/share/auxo/plymouth/accents/\$(sed -n 's/^ACCENT=\"\\(.*\\)\"/\\1/p' $ROOT/etc/auxo/auxo.conf)/logo.png"
check "initramfs rebuilt after hook change"  "echo \"\$out\" | grep -q 'mkinitcpio -P'"
out=$("$T" splash on 2>&1)
check "second run is a no-op"                "! echo \"\$out\" | grep -q 'mkinitcpio -P' && [ \$(grep -o plymouth $ROOT/etc/mkinitcpio.conf | wc -l) = 1 ] && [ \$(grep -o splash $ROOT/etc/default/grub | wc -l) = 1 ]"
out=$("$T" accent amber --user alex --no-grub 2>&1)
check "accent change recolours the splash"   "cmp -s $ROOT/usr/share/plymouth/themes/auxo/logo.png $ROOT/usr/share/auxo/plymouth/accents/amber/logo.png && echo \"\$out\" | grep -q 'mkinitcpio -P'"
out=$("$T" accent amber --user alex --no-grub --no-initramfs 2>&1)
check "--no-initramfs skips the rebuild"     "! echo \"\$out\" | grep -q 'mkinitcpio -P'"
"$T" splash off >/dev/null 2>&1
check "splash off removes hook + option"     "! grep -q plymouth $ROOT/etc/mkinitcpio.conf && ! grep -q splash $ROOT/etc/default/grub && grep -q 'SPLASH=\"off\"' $ROOT/etc/auxo/auxo.conf"

echo "▲ gaming"
out=$("$T" gaming on --user alex 2>&1); echo "$out" | sed 's/^/    /'
check "installs gaming stack + steam"        "echo \"\$out\" | grep -q 'pacman --noconfirm --needed -S gamemode lib32-gamemode mangohud lib32-mangohud gamescope steam'"
check "sysctl tweaks written"                "grep -q 'vm.max_map_count = 2147483642' $ROOT/etc/sysctl.d/80-auxo-gaming.conf && grep -q 'split_lock_mitigate = 0' $ROOT/etc/sysctl.d/80-auxo-gaming.conf"
check "ntsync loaded at boot"                "grep -q '^ntsync' $ROOT/etc/modules-load.d/auxo-gaming.conf"
check "user added to gamemode group"         "echo \"\$out\" | grep -q 'usermod -aG gamemode alex'"
check "auxo.conf GAMING=on"                  "grep -q 'GAMING=\"on\"' $ROOT/etc/auxo/auxo.conf"
out=$("$T" gaming on --no-steam --user alex 2>&1)
check "--no-steam leaves Steam out"          "! echo \"\$out\" | grep -q ' steam'"
out=$("$T" gaming off --purge 2>&1)
check "gaming off removes tweaks"            "! test -f $ROOT/etc/sysctl.d/80-auxo-gaming.conf && ! test -f $ROOT/etc/modules-load.d/auxo-gaming.conf && grep -q 'GAMING=\"off\"' $ROOT/etc/auxo/auxo.conf"

echo "▲ auxo-fetch"
out=$(AUXO_ROOT=/ "$PKG/bin/auxo-fetch" --accent amber 2>&1); echo "$out" | head -20
check "fetch prints os + accent"             "echo \"\$out\" | grep -q 'accent'"
check "fetch --json is valid"                "AUXO_ROOT=/ \"$PKG/bin/auxo-fetch\" --json | python3 -m json.tool >/dev/null"

echo
printf '%d passed, %d failed\n' "$pass" "$fail"
(( fail == 0 ))
