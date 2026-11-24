#!/bin/sh
# auxo-wallpaper: the day/night clock, the settings file, and what it runs per session
# (stand-in hyprpaper/swww/mpvpaper/swaymsg/feh record their calls).
HERE=$(cd "$(dirname "$0")/.." && pwd)
W="$HERE/packages/auxo-tools/files/bin/auxo-wallpaper"
T=$(mktemp -d); mkdir -p "$T/bin" "$T/home" "$T/run"
pass=0; fail=0
check() { if sh -c "$2"; then pass=$((pass+1)); else fail=$((fail+1)); echo "  ✗ $1"; fi; }
for c in hyprpaper swww swww-daemon mpvpaper swaymsg feh pkill notify-send; do
  printf '#!/bin/sh\necho "%s $*" >> %s/calls\nexit 0\n' "$c" "$T" > "$T/bin/$c"; chmod +x "$T/bin/$c"
done
: > "$T/day.jpg"; : > "$T/night.jpg"; : > "$T/rain.mp4"
E="env PATH=$T/bin:$PATH AUXO_HOME=$T/home XDG_RUNTIME_DIR=$T/run"

check "is_day handles normal and overnight ranges" "python3 -c \"
import importlib.machinery as m, importlib.util as u, time
l = m.SourceFileLoader('w', '$W'); s = u.spec_from_loader('w', l); w = u.module_from_spec(s); l.exec_module(w)
t = lambda h, mi: time.struct_time((2026, 1, 1, h, mi, 0, 0, 1, 0))
ok = w.is_day(t(12, 0), '07:00', '19:00') and not w.is_day(t(20, 0), '07:00', '19:00') and not w.is_day(t(6, 59), '07:00', '19:00')
ok = ok and w.is_day(t(23, 0), '22:00', '06:00') and not w.is_day(t(12, 0), '22:00', '06:00')
ok = ok and w.seconds_to_next_switch(t(18, 30), '07:00', '19:00') == 1800
raise SystemExit(not ok)\""

$E python3 "$W" daynight "$T/day.jpg" "$T/night.jpg" --day 06:30 --night 20:15
check "daynight saves its settings" "grep -q MODE=daynight $T/home/.config/auxo/wallpaper-live.conf && grep -q DAY_AT=06:30 $T/home/.config/auxo/wallpaper-live.conf && grep -q \"NIGHT=$T/night.jpg\" $T/home/.config/auxo/wallpaper-live.conf"
check "status reports it" "$E python3 $W status | grep -q '^daynight'"
check "missing files are refused" "! $E python3 $W daynight /nope.jpg $T/night.jpg 2>/dev/null"
check "video refused on X11" "! env -u WAYLAND_DISPLAY DISPLAY=:0 $E python3 $W video $T/rain.mp4 2>/dev/null"

$E WAYLAND_DISPLAY=w SWAYSOCK=s python3 "$W" video "$T/rain.mp4"; sleep 0.3
: > "$T/calls"
$E WAYLAND_DISPLAY=w HYPRLAND_INSTANCE_SIGNATURE=x python3 "$W"
check "Hyprland + video: mpvpaper, silent and looping" "grep -q 'mpvpaper -o no-audio --loop-file=inf' $T/calls && grep -q 'rain.mp4' $T/calls"

$E python3 "$W" still; sleep 0.3; : > "$T/calls"
$E WAYLAND_DISPLAY=w HYPRLAND_INSTANCE_SIGNATURE=x python3 "$W"
check "Hyprland + still: hyprpaper, helpers stopped" "grep -q '^hyprpaper' $T/calls && grep -q 'pkill -x mpvpaper' $T/calls"
: > "$T/calls"
$E DISPLAY=:0 python3 "$W"
check "i3 + still: feh" "grep -q 'feh --bg-fill $T/home/.config/auxo/wallpaper.png' $T/calls"

rm -rf "$T"
echo "$pass passed, $fail failed"
[ "$fail" = 0 ]
