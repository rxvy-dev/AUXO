#!/usr/bin/env bash
# shellcheck disable=SC2034
# Auxo Linux — archiso profile
# Build with:  sudo ./build.sh   (from the repo root)

iso_name="auxo-linux"
iso_label="AUXO_$(date --date="@${SOURCE_DATE_EPOCH:-$(date +%s)}" +%Y%m)"
iso_publisher="Auxo Linux <https://auxolinux.com>"
iso_application="Auxo Linux Live/Install Medium"
iso_version="$(date --date="@${SOURCE_DATE_EPOCH:-$(date +%s)}" +%Y.%m.%d)"
install_dir="auxo"
buildmodes=('iso')
bootmodes=('bios.syslinux'
           'uefi.grub')
arch="x86_64"
pacman_conf="pacman.conf"
airootfs_image_type="squashfs"
airootfs_image_tool_options=('-comp' 'zstd' '-Xcompression-level' '15' '-b' '1M')
bootstrap_tarball_compression=('zstd' '-c' '-T0' '--auto-threads=logical' '--long' '-19')
file_permissions=(
  ["/etc/shadow"]="0:0:400"
  ["/etc/sudoers.d/10-auxo-live"]="0:0:440"
  ["/root"]="0:0:750"
  ["/root/.automated_script.sh"]="0:0:755"
  ["/root/.gnupg"]="0:0:700"
  ["/usr/local/bin/choose-mirror"]="0:0:755"
  ["/usr/local/bin/livecd-sound"]="0:0:755"
  ["/usr/local/bin/auxo-live-setup"]="0:0:755"
  ["/usr/local/bin/auxo-install"]="0:0:755"
  ["/usr/local/bin/auxo-netcheck"]="0:0:755"
  ["/etc/calamares/scripts/"]="0:0:755"
)
