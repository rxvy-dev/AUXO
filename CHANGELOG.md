# Changelog

## 5.0.0 beta (public testing)
> **THIS IS A BETA VERSION OF AUXO LIKELY TO BE UNSTABLE!** Built from the `auxo-beta` branch.
> Please report what breaks in [Issues](https://github.com/rxvy-dev/AUXO/issues) or on
> [Discord](https://discord.gg/XbQ66dH5a7).

AUXO 5.0 is a new generation of AUXO. It boots straight into Hyprland, has six Hyprland styles,
lets you pin apps and add desktop widgets, sets up a VPN in a few clicks, and gives every Auxo app
its own icon. The ISO is smaller (KDE Plasma is gone), and every build is checked and signed.

### Highlights
- **The live session is Hyprland in the Windows style.** Boot the ISO and you're on the desktop with
  a taskbar, a Start menu and Quick settings. No login screen and no KDE. Install Auxo Linux is on the
  desktop and in Start.
- **Safe graphics fallback.** Hyprland needs 3D graphics. When it can't start (a virtual machine
  without 3D, or a broken driver), Auxo starts Sway with software rendering instead, so you get a
  desktop and the installer instead of a black screen. This is `auxo-session`, and installed systems
  use it too.
- **Six Hyprland styles.** Windows, Mac and Auxo, plus three new ones, each with its own bar, window
  look, animations, lock screen and an original 2560×1440 wallpaper:
  - **Neon**: cyberpunk. Pink-to-cyan borders that keep moving, a terminal-style top bar, a synthwave horizon.
  - **Zen**: calm and minimal. Wide gaps, no borders, a slim bottom bar, misty mountains.
  - **Candy**: pastel and round. Floating island bars, bouncy windows, soft clouds.

  Switch with `auxo-tweak style neon|zen|candy|windows|mac|auxo`, in Auxo Settings → Desktop, or pick
  one in the installer. The keys are the same in every style.
- **Pin apps to the taskbar and dock.** Right-click an app in Start or Launchpad and choose *Pin to
  taskbar* / *Keep in Dock*. Right-click a pin for *Open / Move left / Move right / Unpin*, or
  middle-click it to unpin. Clicking a pinned app that's already open switches to it instead of
  opening a second copy. Pinned apps don't show twice. You can also use Auxo Settings → Taskbar & widgets,
  or `auxo-pins add|remove|move`. Works on every Hyprland style (taskbar, dock or bar).
- **Desktop widgets.** A clock, weather (wttr.in), now playing (with play/pause/skip), CPU / memory
  / disk, and a calendar sit on the desktop under your windows (frosted on Hyprland). Turn them on
  or off, pick a corner and a weather city in Auxo Settings → Taskbar & widgets, or with `auxo-widgets`.
- **VPN in a few clicks: Mullvad and Proton VPN.** Neither has a Void Linux app, so Auxo uses the
  WireGuard support both offer, through NetworkManager (so VPNs also show up in the network menu):
  - **Mullvad**: type your account number. Auxo makes a WireGuard key on your computer (the
    private key never leaves it), registers the public key with Mullvad and adds servers in the
    country you pick.
  - **Proton VPN** (free plan included): Auxo opens Proton's download page, and the WireGuard file is
    added the moment it lands in Downloads.
  - Any WireGuard (`.conf`) or OpenVPN (`.ovpn`) file: *Import*.
  - Auxo Settings → VPN (connect, disconnect, connect at startup, remove), or
    `auxo-tweak vpn mullvad [ACCOUNT] | proton | import FILE | on | off | check`.
- **Boot animation.** The Auxo peak builds itself, the name types in and a progress sweep runs while
  your PC starts. It's drawn on the text console, so unlike a Plymouth splash it can never hang the
  boot. Turn it off with `auxo-tweak bootanim off` or in Settings → System. `auxo-tweak bootanim preview`
  shows it.
- **GRUB themes.** Seven boot-menu looks, all with the Auxo logo: Auxo (your accent), Windows, Mac,
  Neon, Zen, Candy and Minimal. Pick one with `auxo-tweak grub-theme NAME` or in Settings → System.
  The installer matches the boot menu to the Hyprland style you pick.

### New
- **Every Auxo app has its own icon**: Settings, App Store, System Monitor, Tweak, Update, Welcome,
  the installer, Ask Auxo, VPN, Taskbar & Widgets and more. One family of rounded tiles, each in
  its own colours.
- **A new `auxo-fetch`**: the Auxo peak in your accent gradient, with sections for system, hardware
  and auxo. It shows your Hyprland style, GPU and its driver, displays and refresh rates, CPU
  temperature, swap, battery and VPN. Slow lookups run side by side, so it's still instant. It also
  has `--small`, `--json` and `--no-logo`, fits narrow terminals, and outputs plain text when piped.
- New launchers: **VPN** and **Taskbar & Widgets** (they open the matching Settings page).
- Auxo Settings has two new pages, **Taskbar & widgets** and **VPN**, plus a boot menu theme picker
  and a boot animation switch under System.

### Improved
- **Faster defaults**: the right I/O scheduler for every disk (NVMe: none, SSD: mq-deadline, hard
  drive: BFQ), and fewer freezes when copying to USB sticks (smaller dirty-page batches). File
  metadata stays cached longer, the memory-map limit is higher for games, there are more inotify
  watches for editors, and fq_codel and TCP Fast Open are on. Override any of them from `/etc`.
- **Hyprland and Sway install offline.** Both are on the ISO, so the installer no longer needs the
  internet for them. Hyprland in the Windows style is the installer's default.
- **The installer lists Hyprland first**, with all six styles previewed. The text installer shows them too.
- **The ISO is smaller.** KDE Plasma, SDDM, Konsole, Dolphin, Kate and the KDE frameworks they pulled
  in are gone.
- `auxo-tweak rice` and `style` take the new styles and nicknames (`cyberpunk`, `minimal`, `pastel`…).

### Fixed
- **The welcome app never opened on Hyprland, Sway or i3.** They don't run XDG autostart, so the
  first-login welcome was skipped. `auxo-autostart` now starts Auxo's own autostart entries on them.
- VPN configs are written to a private, randomly named folder, never a predictable path in `/tmp`
  (found by the new security scan).
- `auxo-update` quotes its package list, and the ISO's firmware cleanup can't expand to `/`
  (found by shellcheck).

### Removed
- **KDE Plasma.** It's no longer on the ISO or offered by the installer and Settings. Existing
  Plasma installs keep working, but Auxo no longer themes them.
- The `polkit-kde-agent` dependency of the Hyprland rice (polkit-gnome does the job).

### Security & transparency
Some of you asked how you'd know if something bad got into AUXO. Now you can check:
- **[SECURITY.md](SECURITY.md)** explains how AUXO is built and what it connects to (no telemetry), and
  how to report a problem privately.
- **Every push is checked** by a new *Lint & security checks* workflow: shellcheck on every shell
  script, ruff on every Python file, bandit (a Python security scanner, failing on high-severity
  findings), and a guard against committed secrets or compiled binaries.
- **Signed build provenance**: each ISO and package carries a GitHub attestation proving which commit
  and workflow built it. Check yours with `gh attestation verify auxo-linux-*.iso -R rxvy-dev/AUXO`.
- Every Hyprland style is checked by Hyprland itself (`Hyprland --verify-config`) on each change.
- Over 600 automated tests. New in 5.0: `tests/test-features.py` (pins, widgets, VPN, boot animation,
  GRUB themes, styles, live session, auxo-fetch).

### Install the beta
1. Download the newest **beta-5.0-…** ISO from the
   [releases page](https://github.com/rxvy-dev/AUXO/releases). It's a *Pre-release*: the newest
   stable ISO stays at Releases → Latest.
2. Check it: `sha256sum -c auxo-linux-*.iso.sha256` (and, if you have the GitHub CLI,
   `gh attestation verify auxo-linux-*.iso -R rxvy-dev/AUXO`).
3. Write it to a USB stick (4 GB or more) with [Ventoy](https://www.ventoy.net),
   [balenaEtcher](https://etcher.balena.io), Rufus (DD mode) or
   `sudo dd if=auxo-linux-….iso of=/dev/sdX bs=4M status=progress oflag=sync`.
4. Boot from the stick. You land on the Hyprland desktop in the Windows style. To try another
   style first, open Auxo Settings → Desktop.
5. Double-click **Install Auxo Linux** (or Start → Install Auxo Linux). For the text installer, run
   `sudo auxo-installer` in a terminal.
6. Pick your disk, desktop and style, accent, kernel, shell, account and extras. Nothing is changed
   until you press Install.

Virtual machines: turn on 3D acceleration (VirtualBox: VMSVGA + *Enable 3D Acceleration*;
virt-manager: Virtio + *OpenGL*) for Hyprland. Without it, you get the Sway fallback.

**From 4.x:** the beta isn't sent to `auxo-update`. Stable installs only update to stable
releases. To test 5.0, use the ISO (a spare disk, a VM or dual boot).

### Known issues
- Hyprland needs working 3D. In VMs without it, you get Sway (the reason is in
  `~/.cache/auxo-session.log`).
- Mullvad's account-number setup uses Mullvad's public WireGuard API. If Mullvad changes it, use the
  file from their website (*Use a file from the website* in Settings → VPN).
- The weather widget needs the internet, and guesses your city from your connection until you set one.

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
- **Sound menu** (click the volume icon on the bar): pick speakers or headphones, volume and mute,
  your microphone, and a volume slider for every app that's playing. Replaces pavucontrol on the
  Hyprland, Sway and i3 rices; Liquid Glass on the Mac style, Windows-style above the taskbar.
- **Fixed: the cursor was almost invisible on Hyprland.** No cursor theme was set, so Hyprland used a faint fallback; Hyprland, Sway, i3, GTK and X11 apps now all use Adwaita (repaired on update).
- **Auxo App Store** (App Store in your menu, or `auxo-store`): hand-picked apps by category
  (Popular, Games, Create, Work, Chat & media, Tools), search across Void and Flathub, one-click
  install/remove, your installed apps, and Update everything. Adds Flathub for you in one click.
- **Gaming mode** (Super+G, or the button in the Control Center / Quick settings): blur, shadows,
  animations and gaps off, notifications held, performance power profile; off puts it all back.
- **System Monitor** (Ctrl+Shift+Esc): CPU per core, memory, GPU, network, temperatures, disks,
  and every app with End task / Force quit.
- **Window overview** (Super+Tab): every window by workspace, live thumbnails, type to filter,
  click to jump, × to close. Alt+Tab is still "next window".
- **Live wallpapers** (Auxo Settings → Look → Live wallpaper): a video or GIF as the wallpaper, or
  Day & night (new Lagoon night wallpaper) that fades between two pictures at set times.
- **Graphics card in Auxo Settings → System**: names your GPU, says which driver it needs, and sets
  up NVIDIA's driver in one click (Wayland, Steam's 32-bit libraries, every kernel), or adds
  prime-run on laptops with two GPUs.
- **Fixed: OpenGL and video decoding on AMD/Intel under Hyprland.** NVIDIA-only variables were set
  for everyone; they now only apply when the NVIDIA driver is set up (repaired on update).
- **The same keys on every rice**: Hyprland in all three styles, Sway and i3 share one key map.
- **Fixed: Hyprland didn't install.** Void doesn't package Hyprland, and the community repo Auxo
  used (hyprland-void) stopped being rebuilt in May 2025; Void has since moved on (libjxl 0.12,
  libdisplay-info 0.3 and its own newer hyprutils), so those packages no longer installed. Auxo now
  builds Hyprland 0.49 itself against current Void (`packages/hyprland`, rebuilt monthly by CI),
  ships it on the ISO, and `auxo-tweak` / `auxo-update` download it on older installs.
- **Fixed: Sway ignored your monitor settings.** A comment at the end of the `include` line (Sway and
  i3 only allow comments on their own line) made Sway skip the monitors file and show a config
  error at every login; Sway's touchpad block was rejected the same way. Updating repairs existing
  configs.
- **Fixed: changes from Auxo Settings didn't apply live** (monitors, Hyprland reloads, looks): the
  reload ran through `runuser`, which only works as root, so it silently did nothing when Settings
  ran it as you.
- **Mac and Windows styles in the installer's desktop list**, as their own choices, with previews.
  `auxo-tweak style mac|windows|auxo`, and `auxo-tweak rice mac` / `rice hyprland -mac` work too.
- **auxo-update updates Auxo's own tools**: every build now publishes them, so installed systems
  get fixes like these without reinstalling.
- **Graphical installer**: Install Auxo Linux in the live session now opens a graphical installer:
  your disks drawn as bars (what's on them now, and what Auxo will make of them), dual boot with
  existing partitions, desktop cards with Hyprland style previews (Auxo, Mac, Windows), accent
  colours that re-theme the installer as you pick, a password-strength meter, a summary page and a
  progress page with every step and the full log. It runs the same tested install engine as the
  text installer (as root, in the background), so both install exactly the same system.
  `sudo auxo-installer` still opens the text installer, which also asks for the Hyprland style now.
- **Mac-style Hyprland, in Liquid Glass**: a glass menu bar on top (▲ menu, app name, status
  icons), a glass dock, Spotlight-style search on Super+Space, an Applications panel with search
  and categories (Super+A or the dock's grid icon) and a Control Center (Wi-Fi, Bluetooth, Focus,
  dark mode, brightness, volume, now playing) behind the menu bar's toggle icon. Panels are
  frosted, see-through glass with a bright rim over Hyprland's vibrant blur. Light mode and a new
  Lagoon wallpaper. Pick it in Auxo Settings → Desktop → Style, or `sudo auxo-tweak rice hyprland-mac`.
- **Clipboard history, emoji picker, night light** on Hyprland and Sway: Super+Shift+V shows what
  you copied earlier (Super+V on the Windows style, like Windows), Super+. picks an emoji, and the
  Control Center / Quick settings have a Night light toggle (warmer colours), plus Clipboard and
  Emoji buttons. `auxo-clipboard clear` forgets the history.
- **Windows-style Hyprland**: a centred taskbar with Start, search, pinned and open apps, tray and
  a two-line clock; a Start menu (Pinned — right-click to pin/unpin —, All apps, Recommended recent
  files, your name and power); Quick settings from the taskbar's status icons; the Super key opens
  Start, Alt+F4, Alt+Tab, Super+Shift+S screenshots. New Bloom wallpaper.
  `sudo auxo-tweak rice hyprland-win`.
- Fixed: Waybar and polybar icons were blank on every rice (the icon characters had been lost
  from the config files). They're written as escapes now, and a test checks it.
- **Displays**: a new Auxo Settings page and `auxo-tweak monitors` for resolution, refresh rate,
  scale, rotation, VRR, layout (drag the screens) and which workspaces open on each screen,
  written into the Hyprland, Sway and i3 rices. Hyprland now uses each monitor's highest
  refresh rate by default (144 Hz monitors were stuck at 60).
- `auxo-keys` (Super+/ on Hyprland, Sway, i3): every keyboard shortcut in plain words, searchable.
- Fixed: the app launcher (wofi) on Hyprland and Sway had no Auxo colours: wofi resolves
  `@import` from the folder it's started in, so the colour file was never found. The import
  now uses the full path. Two Waybar icons came from Nerd Fonts (not installed) and showed as
  broken characters; they're Font Awesome icons now.
- Fixed: Hyprland's idle screen-off and workspace scrolling used a newer Hyprland syntax than the
  0.49 that Void users get, so they did nothing.
- Fixed: no sound. Void ships PipeWire switched off, and Auxo never turned it on. Every
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
