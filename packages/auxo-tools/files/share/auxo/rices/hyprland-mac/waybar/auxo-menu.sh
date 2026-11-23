#!/bin/sh
# The ▲ menu in the Mac-style menu bar (like the Apple menu)
choice=$(printf '%s\n' "About this PC" "Auxo Settings…" "App bundles…" "Software update…" "Displays…" \
  "Lock screen" "Log out" "Restart" "Shut down" | wofi --dmenu --prompt "Auxo" --width 280 --height 420 \
  --location 1 --xoffset 6 --yoffset 34 --cache-file /dev/null)
case "$choice" in
  "About this PC")      kitty --hold -e auxo-fetch ;;
  "Auxo Settings…")     auxo-settings ;;
  "App bundles…")       auxo-settings system ;;
  "Software update…")   kitty -e auxo-update ;;
  "Displays…")          auxo-settings displays ;;
  "Lock screen")        hyprlock ;;
  "Log out")            hyprctl dispatch exit ;;
  "Restart")            loginctl reboot ;;
  "Shut down")          loginctl poweroff ;;
esac
