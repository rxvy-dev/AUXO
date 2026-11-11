# Changelog

## AuxoBSD 1.0.0 (in development)
**AuxoBSD is now FreeBSD-based**: FreeBSD 15.1 with KDE Plasma 6 and Firefox (`bsd/`).

- Graphical installer (`auxo-installer`, Qt): keyboard, time zone, disk with ZFS (boot
  environments) or UFS, GPT that boots on BIOS and UEFI, graphics, accent, shell, account, summary.
- GPU detection with the right driver for every NVIDIA generation (current and 580 with KMS for
  Wayland; 470/390/340 legacy), PRIME offload on hybrid laptops (`prime-run`), Intel/AMD through
  drm-kmod, VirtualBox (VMSVGA included) and VMware guest tools. Every NVIDIA branch is on the ISO
  in a local repository, so drivers install offline.
- FreeBSD versions of `auxo-tweak` (accent, gpu, drivers, nvidia, shell, power, firewall/pf, dns,
  snapshots, cleanup, repo, service, xbps, info), `auxo-fetch`, `auxo-motd`, `auxo-update` and
  `auxo-rollback` (ZFS boot environments).
- Two package managers: FreeBSD's `pkg`, and `auxo-tweak xbps on` for Void Linux's `xbps` in
  `/compat/void` through the Linux compatibility layer (`void-run` starts those programs).
- Branding: the AuxoBSD logo (auxolinux.com/bsd) in the installer, Plasma launcher and graphical boot
  menu, the Auxo mountain + AuxoBSD wordmark in the text boot menu, the Auxo Plasma theme, SDDM in
  your accent, AuxoBSD os-release and login banner, Firefox opening auxolinux.com/bsd.
- ISO build (`bsd/build.sh`, on FreeBSD) and a GitHub workflow that builds it in a FreeBSD VM.

## AuxoBSD research kernel 0.1.0
The original from-scratch AuxoBSD kernel by Onyx-ig now lives in `bsd/kernel/`, rebranded as part of Auxo.

- Boots into **auxsh**, a kernel shell (PS/2 keyboard and serial input, history, Tab completion)
  with the Auxo greeting (`auxo-motd`, Terry Fox Foundation note included).
- **`auxo-fetch`** and **`auxo-tweak`** work on AuxoBSD: same logo, layout and flags as on Linux
  (`--small`, `--no-logo`, `--json`, `--accent`; `accent`, `hostname`, `motd`, `color`, `info`,
  `persist`, `reset` and the interactive menu). Linux-only sub-commands say so.
- One palette for both editions: `bsd/include/auxo_accents.h` is generated from the Auxo Linux
  accents (`make -C bsd accents`). The VGA console loads the accent gradient into its palette,
  so `auxo-tweak accent` recolours the whole screen; ANSI colour works on VGA and serial.
- Settings come from the kernel command line (`accent= hostname= motd= nocolor`); the GRUB menu
  is in Auxo colours with plain-serial, reboot and power-off entries.
- `make -C bsd test` boots the ISO in QEMU and checks the shell and tools (60 checks).
- VirtualBox: on a 32-bit VM type AuxoBSD now explains how to switch to a 64-bit type instead
  of triple-faulting ("critical error"), and it no longer hangs when the VM has no serial port.

## 4.0.0 (in development)
**Auxo moves from Arch Linux to Void Linux.** The Arch-based 3.x line is kept on the
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
- Cybervis comes preinstalled: matrix rain, audio spectrum, plasma, fire, warp and more in
  your terminal (`cybervis`, or Cybervis in the app menu). Compiled during the ISO build.
- Japanese, Chinese and Korean text now displays properly (noto-fonts-cjk-sans on the ISO).
- Auxo supports cancer research: the installer, the welcome app and (now and then) the
  terminal greeting point to the Terry Fox Foundation. `auxo-motd --give` has the link.
- New `auxo-motd`: a greeting in each new terminal with uptime, disk use, a reminder
  when the last `auxo-update` was a while ago, and a tip. `auxo-motd --off` hides it.
- There's a mountain hidden somewhere in Auxo. Climbers say the MOTD knows the way.
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
