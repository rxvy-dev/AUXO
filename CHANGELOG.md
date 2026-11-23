# Changelog

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
- **Mac-style Hyprland**: a menu bar on top (▲ menu, app name, status icons), a frosted dock
  with your apps, Spotlight-style search on Super+Space, Mac-like window animations and lock
  screen. Pick it in Auxo Settings → Desktop → Style, or `sudo auxo-tweak rice hyprland-mac`.
- **Displays**: a new Auxo Settings page and `auxo-tweak monitors` for resolution, refresh rate,
  scale, rotation, VRR, layout (drag the screens) and which workspaces open on each screen,
  written into the Hyprland, Sway and i3 rices. Hyprland now uses each monitor's highest
  refresh rate by default (144 Hz monitors were stuck at 60).
- `auxo-keys` (Super+/ on Hyprland, Sway, i3): every keyboard shortcut in plain words, searchable.
- Fixed: Hyprland's idle screen-off and workspace scrolling used a newer Hyprland syntax than the
  0.49 that Void users get, so they did nothing.
 Void ships PipeWire switched off, and Auxo never turned it on. Every
  desktop (and the live ISO) now starts PipeWire at login with WirePlumber, the PulseAudio layer
  and ALSA routed through it (alsa-pipewire), as Void's handbook describes. i3 and Cinnamon now
  start with a D-Bus session, which PipeWire needs. `sudo auxo-tweak doctor --fix` repairs
  existing installs.
- **Steam and Discord come with Auxo**: the installer sets them up by default (online
  installs; untick them under Extras). Discord comes from Flathub: Void's own package is
  restricted (not redistributable), so xbps reported "discord not found in repository pool". Later: `sudo auxo-tweak gaming on`, `sudo auxo-tweak apps discord`.
- **Fixed: "wrong password" at the login screen.** The login screen (greetd) and Void's text
  login (agetty) both listened on tty1, so each got part of every keystroke and passwords came
  out wrong. The login screen now has its own console (tty7). Already installed? Press Ctrl+Alt+F2,
  log in there, run `sudo sed -i 's/^vt = 1$/vt = 7/' /etc/greetd/config.toml` and reboot
  (updating auxo-tools or `sudo auxo-tweak doctor --fix` does the same).
- `auxo-tweak doctor` (and **Health** in Auxo Settings): checks the login screen, services,
  networking, accounts, sudo, disk space and pending config updates; `--fix` repairs them.
- **Auxo AI**: a free assistant that runs on your PC (Ollama + a small model, your pick of
  three sizes). `sudo auxo-tweak ai on`, then `auxo-ai` or **Ask Auxo** in Auxo Settings. It
  knows Auxo's commands and your settings, and can run the fixes it suggests after you confirm.
- Look **presets** (Auxo, Glass, Minimal, Cozy, Retro, Lite): `auxo-tweak look preset lite`.
- **Share your rice**: `auxo-tweak look export` writes a `.auxorice` file; `look import` applies one.
- Auxo Settings: search box, Services (Bluetooth, printing, SSH, power profiles), snapshots
  (take one, list, roll back), maintenance (clean up, mirrors, Flathub, drivers).
- `auxo-tweak snapshots now` and `auxo-rollback --yes N`.
- **Auxo Settings** (`auxo-settings`, Super+I on the tiling desktops): a graphical app for
  everything below. Look (accent, light or dark, wallpaper, fonts, animations), Windows &
  bar (gaps, rounded corners, borders, blur, transparency, bar top or bottom, with a live
  preview), Desktop (switch between all desktops, re-apply the rice) and System (kernel,
  shell, power, DNS, NVIDIA, snapshots, firewall, zram, gaming mode, app bundles, update).
  It runs `auxo-tweak` underneath and asks for your password only for system changes.
- **One look for every desktop**: `auxo-tweak look set gaps=4 rounding=0 bar=bottom …`
  saves your choices in `~/.config/auxo/look.conf` and writes them into Hyprland, Sway, i3,
  Waybar, Polybar, picom, kitty, foot, Alacritty, GTK, Plasma, GNOME, Xfce, Cinnamon and
  Chrono. Your own config edits stay; the look file is loaded last.
- **Light mode**: `auxo-tweak mode light` (Auxo Light colour scheme for Plasma, light GTK
  and GNOME, lighter bars and terminals, accents darkened to stay readable).
- `auxo-tweak wallpaper FILE` (or `accent` for the Auxo wallpaper in your colour).
- **Every desktop is riced**: new Auxo rices for GNOME (no extensions needed — Papirus
  icons, Inter, dark style, your accent, Super+Enter terminal), Xfce and Cinnamon, set
  through their own settings so updates can't break them.
- **Chrono**, rxvy's own Wayland desktop (github.com/rxvy-dev/chrono), is now an Auxo
  desktop: `auxo-tweak desktop chrono`, or pick it in the installer. Built from a tested
  commit, with an Auxo theme generated in your accent.
- `auxo-tweak apps`: one-command bundles of popular apps (browsers, media, creative, office,
  dev, containers, chat, terminal, fun, tools).
- Fixed: the Waybar update counter used Arch's `checkupdates` (now `xbps-install -Mun`);
  the ISO lacked PyQt6's GUI modules (`python3-pyqt6-gui`, `-widgets`) that the welcome app
  needs; Fira Code replaces JetBrains Mono, which Void doesn't package.
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
