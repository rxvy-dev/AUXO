# Changelog

## 4.0.0 (in development)
**Auxo moves from Arch Linux to Void Linux.** The Arch-based 3.x line is kept under the
`arch` branch.

- Base: Void Linux with runit and xbps (no systemd). Services are runit services;
  `auxo-tweak` enables them by linking into the default runlevel.
- New installer: `auxo-installer`, a retro full-screen text installer replacing
  Calamares. Erase a disk (UEFI or BIOS) or use existing partitions for dual boot;
  btrfs with subvolumes, ext4 or xfs; offline install of Plasma; a review screen and
  nothing touched until you press Install.
- Kernels: Void's `linux`, `linux-lts` and `linux-mainline`. A kernel hook keeps your
  choice as the default GRUB entry after updates.
- New `auxo-tweak` commands: `nvidia` (picks nvidia / nvidia580 / nvidia470, back to
  nouveau, `prime-run`), `scheduler` (sched-ext via scx-loader), `power`, `firewall`
  (ufw), `dns` (NetworkManager), `cleanup` (cache, orphans, old kernels), `flatpak`, `repo`.
- Hyprland comes from the hyprland-void community repo (Void doesn't package it); the
  rice is back on `hyprland.conf` for that version.
- No boot splash: the Plymouth theme and `auxo-tweak splash` are gone, and Plymouth is
  kept out of the initramfs (on runit nothing closes it, so boot sat on the splash forever).
- D-Bus starts elogind through its runit service instead of spawning a bare copy at boot.
- `auxo-update` for xbps: updates xbps first, snapshots before updating.
- ISO built with Void's void-mklive in a container: `./build.sh` works on any Linux
  with Docker or Podman.
- Removed: archiso profile, Calamares and its PKGBUILD, the AUR helper command
  (no AUR on Void; use Flatpak or xbps-src).

## 3.0.4
- Fixed: GNOME showed "Oh no! Something has gone wrong" / failed to start after
  install. GNOME is Wayland-only now and has to be started by its own login screen
  (GDM); the text login started it in a plain terminal session it can't use.
  GNOME installs now use GDM, remember your session, and skip GNOME's first-login
  wizard (the installer already asked those questions).
- New: **Auxo global theme for KDE Plasma.** A dark "Auxo Dark" colour scheme with
  your accent built in, Breeze Dark icons, the Auxo wallpaper and a floating,
  centred dock-style panel with the Auxo logo as the app menu. It's on by default for
  new Plasma installs (and the live session), follows `auxo-tweak accent`, and shows
  up in System Settings → Global Theme as "Auxo". `auxo-tweak rice kde` puts the
  Auxo theme and panel back (your old panel config is kept as `*.auxo-bak`).
- GNOME is now stock GNOME. The GNOME extensions and defaults from the 3.0.4 betas
  (Dash to Dock, Blur my Shell) stopped the normal GNOME session from starting, so
  they're gone, and installing GNOME removes them from systems that had them.
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
