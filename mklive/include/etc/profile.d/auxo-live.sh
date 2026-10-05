# shellcheck shell=sh
# Auxo live session hint (this file is removed by the installer)
if [ -e /etc/auxo/live ] && [ -t 1 ]; then
  printf '\n  \033[1mAuxo Linux live\033[0m — install with: \033[1msudo auxo-installer\033[0m   (Wi-Fi: nmtui)\n\n'
fi
