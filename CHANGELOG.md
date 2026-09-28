# Changelog

## 3.0.4
- New: GNOME comes customised. Installing GNOME (in the installer or with
  `auxo-tweak desktop gnome`) adds Dash to Dock, Blur my Shell, AppIndicator (tray
  icons) and Caffeine, plus dark mode, your accent on the dock, minimise/maximise
  buttons, Super+Enter for a terminal, Super+T for auxo-tweak and Super+Q to close.
  These are defaults, so your own changes always win. `auxo-tweak rice gnome`
  re-applies them.
- `auxo-tweak accent` now changes the wallpaper straight away on every desktop:
  KDE Plasma, GNOME, Cinnamon, Xfce, Hyprland (hyprpaper is restarted), Sway and
  i3. Before, it only changed after logging out, or not at all under sudo.
- Fixed: installs could fail at the GRUB step when a kernel other than linux-zen
  or a shell other than zsh was picked. The installer now:
  - only makes your kernel the default boot entry if its image is really in `/boot`
    (and copies it there if the pacman hook missed it)
  - falls back to the stable kernel if your kernel fails to install, and to zsh if
    your shell fails, and tells you on the last page
  - test-runs `grub-mkconfig` before the bootloader step, and undoes the kernel
    default if that is what breaks it
  - writes GRUB output to `/var/log/auxo-install.log`
- `auxo-tweak kernel`: installs headers separately, so a headers problem no longer
  stops the kernel switch; never points GRUB at a missing kernel.
- `auxo-tweak shell`: completions/plugins are optional, so bash works offline.
- Live ISO: the normal boot entries no longer copy the whole system into RAM first
  (that looked like a black screen for minutes on some laptops). The separate
  *copy to RAM* entry still does, and now shows its progress.
- Live ISO: "Loading Auxo Linux..." shows while the kernel loads; removed a stray
  `%KERNEL_PARAMS%` from the boot entries.
- Installer: the slideshow no longer has a light frame around it.

## 3.0.3
- Fixed: black screen after install when a desktop other than KDE Plasma was chosen
  (on VMs and real hardware). Removing the live Plasma packages also removed the
  Breeze login theme, and SDDM then tried a Qt5 greeter Arch doesn't ship
  (`sddm-helper exited with 127`).
- Installed systems now use a text login screen (greetd + tuigreet) in your accent
  colour. It starts the desktop you picked; F3 in the login screen switches
  session. Works the same on every desktop, GPU and VM. The live ISO still uses SDDM.
- SDDM is never pointed at a theme that isn't installed.

## 3.0.2
- New: animated boot splash (Plymouth). The Auxo mark in your accent colour with a
  climber running up the trail, plus a boot progress bar. On by default for new
  installs; `auxo-tweak splash on|off|status`. Follows `auxo-tweak accent`.
- New: `auxo-tweak gaming on|off|status`. Steam, GameMode, MangoHud, gamescope,
  32-bit Vulkan drivers for your GPU, `vm.max_map_count` and split-lock tweaks,
  ntsync for Wine/Proton. `--no-steam`, and `off --purge` to remove the tools.
- Includes the 3.0.1 fixes (VM first-boot fix, disk flush, guest-tool pruning).

## 3.0.1
- Installer: installed systems now boot in VirtualBox and other VMs. `/boot` is
  no longer btrfs-compressed, so GRUB can always read the kernel and initramfs
  (fixes `premature end of file /@/boot/vmlinuz-linux` / `you need to load the
  kernel first` on first boot).
- Installer: everything is flushed to disk before the install finishes.
- `auxo-tweak drivers --prune`: keeps only the guest tools for the hypervisor
  you're running on (VirtualBox, QEMU/KVM or VMware) and removes them on real
  hardware.

## 3.0
- First public release.
