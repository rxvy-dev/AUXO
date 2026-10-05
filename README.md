<p align="center">
  <img src="branding/logo/auxo-logo-icon-512.png" width="110" alt="Auxo Linux logo">
</p>

<h1 align="center">Auxo Linux</h1>

<p align="center">
  <b>Void Linux, set up the way you want it.</b><br>
  A rolling distro on a Void base with runit and xbps. Pick your desktop, kernel, shell and accent
  colour in a retro text installer, then change any of them later with one command. No reinstall.
</p>

<p align="center">
  <a href="https://auxolinux.com">Website</a> ·
  <a href="https://auxolinux.com/download/">Download</a> ·
  <a href="https://auxolinux.com/docs/">Docs</a> ·
  <a href="https://discord.gg/XbQ66dH5a7">Discord</a> ·
  <a href="CHANGELOG.md">Changelog</a>
</p>

---

> **Auxo 4.0 moves from Arch to Void Linux.** Auxo 3.x (Arch-based, Calamares installer) is kept
> on the [`arch`](../../tree/arch) branch.

## What it is

Auxo is Void underneath: Void's repositories, `xbps`, runit and the Void docs all work as normal.
On top it adds:

- **`auxo-installer`**, a full-screen text installer (keyboard, language, time zone, disk, desktop,
  accent, kernel, shell, account, extras) that shows a summary and changes nothing until you press Install
- **`auxo-tweak`**, which changes any of those choices after install. The installer calls the same
  commands, so changing something later gives the same result as a fresh install
- **bootable btrfs snapshots**, taken before every update, with `auxo-rollback` to make one your system again

```bash
auxo-tweak desktop hyprland           # switch your whole desktop
auxo-tweak accent rose                # recolour GRUB, login, prompt, terminals, bars, Plasma
auxo-tweak kernel linux-lts           # swap kernels (stays the default boot entry after updates)
auxo-tweak nvidia on                  # the right NVIDIA driver for your card
auxo-tweak scheduler lavd --mode gaming   # sched-ext CPU scheduler
auxo-tweak gaming on                  # Steam, GameMode, MangoHud, gamescope + tweaks
auxo-update                           # snapshot → xbps → Flatpak
auxo-rollback --list                  # list snapshots
```

## Features

| | |
|---|---|
| **Text installer** | Retro, keyboard-driven and fast. Erase a disk (UEFI or BIOS) or use existing partitions to dual boot. btrfs (with subvolumes), ext4 or xfs. Works offline (installs Plasma); other desktops download what they need. |
| **8 desktop choices** | KDE Plasma 6 with the **Auxo global theme** (the live desktop, installs offline), Hyprland, Sway and i3 with **Auxo rices**, plus GNOME, Xfce, Cinnamon, or no desktop. Hyprland comes from the [hyprland-void](https://github.com/Makrennel/hyprland-void) community repo, since Void doesn't package it. |
| **One accent, everywhere** | 7 accents applied to the GRUB theme, login screen, MOTD, zsh/fish/bash prompts, kitty/foot/alacritty, waybar/polybar, mako/dunst, the KDE colour scheme and the GNOME accent, and the wallpaper changes straight away on every desktop. |
| **Kernels** | Void's `linux`, `linux-lts` or `linux-mainline`. A kernel hook keeps your pick as the default GRUB entry after every update. |
| **NVIDIA** | `auxo-tweak nvidia on` picks `nvidia` (Turing and newer, open kernel modules), `nvidia580` (Maxwell–Volta) or `nvidia470` (Kepler) from Void's nonfree repo, with DKMS for every installed kernel. `nvidia prime` adds `prime-run` for laptops. |
| **Performance** | sched-ext CPU schedulers through `scx-loader` (`lavd`, `bpfland`, `flash`, `cosmos`, `rusty`) with gaming/low-latency/power-save modes, power profiles, zram. |
| **Time-travel snapshots** | Flat btrfs layout (`@ @home @log @cache @snapshots`). snapper snapshots before every `auxo-update` and hourly, grub-btrfs makes them bootable, and `auxo-rollback` turns one back into your live system. `/home` is never touched. |
| **Network & security** | `auxo-tweak firewall on` (ufw), `auxo-tweak dns cloudflare\|quad9\|google` (NetworkManager, every connection). |
| **Gaming** | `auxo-tweak gaming on`: nonfree + multilib repos, Steam with its 32-bit libraries, GameMode, MangoHud, gamescope, `vm.max_map_count`, split-lock and ntsync tweaks. |
| **Housekeeping** | `auxo-tweak cleanup` (package cache, orphans, old kernels via `vkpurge`), `auxo-tweak flatpak on` (Flathub), `auxo-tweak mirrors` (xmirror). |
| **runit** | No systemd. Services are runit services in `/etc/sv`; Auxo enables them by linking into the default runlevel, the Void way. |

## The tools (`packages/auxo-tools`)

| Tool | What it does |
|---|---|
| `auxo-tweak` | Menu + CLI: `accent`, `desktop`, `rice`, `kernel`, `shell`, `drivers`, `nvidia`, `scheduler`, `power`, `firewall`, `dns`, `snapshots`, `gaming`, `flatpak`, `cleanup`, `mirrors`, `zram`, `repo`, `service`, `info`. Asks for sudo by itself when needed. |
| `auxo-update` | Snapshots, updates xbps itself and then every package, updates Flatpak, then reports new config files, orphans and whether to reboot. `-y` |
| `auxo-rollback` | Pick a snapshot and make it your live system again. `--list`, or pass a snapshot number. |
| `auxo-fetch` | A fast system summary in your accent colour. `--json`, `--small`, `--no-logo` |
| `auxo-welcome` | First-run hub. In the live session it opens the installer; on an installed system it offers accent, drivers, snapshots and updates. |
| `auxo-installer` | The text installer (`installer/`), shipped on the live ISO only. |

Full command reference: **[auxolinux.com/docs](https://auxolinux.com/docs/)**

## Repository layout

```
auxo/
├── build.sh                    # ./build.sh  →  out/auxo-linux-YYYY.MM.DD-x86_64.iso (any distro)
├── VERSION
├── mklive/                     # the live ISO: package list, services, overlay files, post-setup
├── installer/                  # auxo-installer: config, disks, plan (pure), runner, curses TUI
├── packages/
│   ├── build-xbps.sh           # builds auxo-tools + auxo-installer .xbps packages into a local repo
│   └── auxo-tools/files/       # auxo-tweak & friends, rices, accents, Plasma theme
├── branding/                   # logo/ (official logo files), gen-assets.py
├── scripts/
│   ├── build-iso-inner.sh      # what build.sh runs inside the Void container
│   ├── test-vm.sh              # boot the ISO in QEMU (UEFI or --bios) with a 40 GB virtual disk
│   └── smoke-test.sh           # headless boot test with a screenshot
├── tests/                      # tool + installer tests (no root, no Void needed)
├── website/                    # auxolinux.com generator
└── .github/workflows/build-iso.yml   # CI: tests → ISO → boot it in QEMU → upload
```

## Build the ISO

On **any Linux distro** (Arch, Debian/MX/Ubuntu, Fedora, Void, …):

```bash
./build.sh                                # asks for sudo
./scripts/test-vm.sh out/auxo-linux-*.iso
```

No Docker needed: `build.sh` downloads Void's statically linked `xbps`, sets up a small Void
system in `out/void-root` (once) and builds inside it with `chroot`. It only needs `sudo`,
`curl` or `wget`, `tar` and `xz`, and about 15 GB free. Alternatives: `./build.sh --docker`
(Void's official container) or `sudo ./build.sh --native` on a Void host.

The build packages `auxo-tools` and `auxo-installer` as `.xbps` files in a local repository,
then runs Void's own ISO builder, [void-mklive](https://github.com/void-linux/void-mklive)
(pinned to a tested commit), with the package list in `mklive/packages.txt`.

## Tests

```bash
./tests/test-tools.sh            # auxo-tweak against a fake Void root (commands are printed, not run)
python3 tests/test-installer.py  # installer plans + a full walkthrough of the real TUI in a virtual terminal
./scripts/smoke-test.sh out/*.iso   # boots the real ISO headless and takes a screenshot
```

## How an install works

The installer follows the same sequence as Void's own installer, then applies Auxo's choices
through `auxo-tweak`:

partition (GPT: EFI + root, or BIOS boot + root) → format → mount (btrfs subvolumes, uncompressed
`/boot`) → copy the live system (rsync) → fstab by UUID → remove the live user and live-only files →
hostname, locale, time zone, keyboard → your account (root locked, sudo for wheel) →
**auxo-tweak** `accent / shell / kernel / desktop / drivers / zram / extras` →
dracut → GRUB (UEFI with a fallback copy, or BIOS) → first snapshot → done.

## Website

```bash
cd website && python3 build-site.py            # → site/
python3 build-site.py --preview                # → preview/ with flat links
```

## Contributing

Bug reports, ideas and pull requests are welcome. Open an issue, or come and chat in the
[Discord](https://discord.gg/XbQ66dH5a7). Please run both test suites before sending a PR.

## Credits

Auxo Linux is made by **rxvy**, with AI assistance for a lot of the code. Built on
[Void Linux](https://voidlinux.org) and [void-mklive](https://github.com/void-linux/void-mklive);
Hyprland packages from [hyprland-void](https://github.com/Makrennel/hyprland-void).
Auxo is not affiliated with the Void Linux project.
