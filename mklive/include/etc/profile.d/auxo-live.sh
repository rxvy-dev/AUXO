# shellcheck shell=sh
# Auxo live session hint (this file is removed by the installer)
if [ -e /etc/auxo/live ] && [ -t 1 ]; then
  printf '\n  \033[1mAuxo Linux live\033[0m — install: \033[1mInstall Auxo Linux\033[0m on the desktop, or \033[1msudo auxo-installer\033[0m here   (Wi-Fi: nmtui)\n\n'
fi
