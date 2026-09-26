# Auxo Linux 3.0 — climb your own setup

Arch-based, rolling, modular. You pick the desktop, accent colour, kernel and shell
in a Calamares installer. You can change any of them later with `auxo-tweak`, without
reinstalling.

```
auxo/
├── build.sh                    # sudo ./build.sh  → out/auxo-linux-YYYY.MM.DD-x86_64.iso
├── archiso/                    # the archiso profile (based on releng)
│   ├── profiledef.sh           # iso name/label, BIOS (syslinux) + UEFI (GRUB) boot
│   ├── packages.x86_64         # live system: KDE Plasma (Wayland), Calamares, drivers, tools
│   ├── pacman.conf             # build-time pacman.conf (+ local [auxo] repo)
│   ├── grub/  syslinux/        # branded boot menus: normal · NVIDIA · safe graphics · copy-to-RAM
│   └── airootfs/
│       ├── etc/calamares/      # installer config, branding, slideshow, choosers
│       ├── usr/share/auxo/calamares-modules/   # custom Python jobs: auxoprepare, auxodesktop, auxosnap
│       └── usr/local/bin/      # live-only: auxo-install, auxo-live-setup, auxo-netcheck
├── packages/
│   ├── auxo-tools/             # PKGBUILD: auxo-tweak, auxo-fetch, auxo-update, auxo-rollback, auxo-welcome, rices
│   └── calamares/              # PKGBUILD: Calamares 3.4 built WITH the packagechooser module
├── branding/gen-assets.py      # generates logo, wallpapers (per accent), boot splash, installer previews
├── scripts/
│   ├── build-in-docker.sh      # build from any distro with Docker
│   ├── test-vm.sh              # boot the ISO in QEMU (UEFI or --bios), with a 40G virtual disk
│   └── smoke-test.sh           # headless boot test: waits for SDDM, takes a screenshot
├── tests/                      # tool + installer-module tests (run anywhere, no root)
├── website/index.html          # auxolinux.com
└── .github/workflows/build-iso.yml   # CI: build ISO → boot it in QEMU → upload ISO + screenshot
```

## Features

| | |
|---|---|
| **GUI installer** | Calamares with Auxo branding. It has pages for Desktop, Accent, Kernel, Shell and Software, each with a preview image. |
| **8 desktop choices** | KDE Plasma (live desktop, installs offline), plus Hyprland, Sway and i3 with **Auxo rices** (waybar/polybar, wofi/rofi, mako/dunst, hyprlock…), and GNOME, Xfce, Cinnamon, or no desktop. |
| **One accent, everywhere** | 7 accents. The one you pick is applied to the GRUB theme, SDDM background, MOTD, zsh/fish/bash prompt, kitty/foot/alacritty, waybar/polybar, mako/dunst, and the KDE/GNOME accent. `auxo-tweak accent rose` |
| **Time-travel snapshots** | btrfs uses a flat layout (`@ @home @log @cache @snapshots`). snapper + snap-pac take a snapshot on every pacman run. grub-btrfs makes snapshots bootable. `auxo-rollback` turns one back into your live system. |
| **Hardware autodetect** | NVIDIA Turing and newer (RTX 20–50) get `nvidia-open` with modesetting and suspend services. `nvidia-open-dkms` is used automatically on zen/lts kernels. AMD and Intel get Vulkan and VA-API. Drivers for hardware you don't have are removed. |
| **NVIDIA-safe live boot** | The boot menu has an "NVIDIA GPU" entry. The default entry doesn't touch nvidia. There is also a safe-graphics (nomodeset) entry. |
| **Kernels** | linux, linux-zen, linux-lts, linux-hardened. Your pick becomes the default GRUB entry, and stable stays as a fallback. |
| **Dual-boot friendly** | os-prober is enabled. Calamares offers *install alongside* and reuses an existing EFI partition. |
| **Offline-capable** | With no network the install still completes (Plasma + stable kernel). Choices that need a download fall back gracefully, and you get told why. |
| **zram** | Compressed RAM swap is on by default, so no swap partition is needed. |
| **Gaming/dev ready** | multilib is enabled. Optional groups: Steam/Lutris/MangoHud/Prism Launcher, dev tools (incl. SDL2/SDL3), creative apps, office, and virtualization. |

### Tools (package `auxo-tools`)

- `auxo-tweak` — a TUI menu plus CLI: `accent`, `desktop`, `kernel`, `shell`, `rice`, `snapshots`, `drivers`, `mirrors`, `zram`, `multilib`, `service`, `aur` (paru), `info`
- `auxo-fetch` — system summary drawn in your accent colour (`--json`, `--small`)
- `auxo-update` — reads Arch news since your last update, takes a snapshot, then updates repo + AUR + Flatpak. It reports .pacnew files, orphans, and whether you need to reboot.
- `auxo-rollback` — pick a snapshot and make it your live system again. `/home` is never touched.
- `auxo-welcome` — first-run hub (PyQt6). In the live session it shows *Install*. On an installed system it has updates, drivers, snapshots and an accent picker.

## Build

**On Arch** (your Arch install is fine):

```bash
sudo ./build.sh          # ≈ 20–40 min, needs ~20 GB free
./scripts/test-vm.sh out/auxo-linux-*.iso
```

**On any other distro**: `./scripts/build-in-docker.sh`

**In the cloud**: push to GitHub and run the *Build Auxo ISO* workflow (Actions tab). If you push a tag
`v3.0.0`, the ISO is attached to a GitHub Release. ISOs bigger than 2 GB are split into parts.

`build.sh` first builds `calamares` (from its release tarball) and `auxo-tools` into a local repo.
It then runs `mkarchiso`. Calamares isn't in the official repos, and the AUR build leaves out
the `packagechooser` module that the Desktop/Accent/Kernel/Shell pages use. That's why
Auxo ships its own PKGBUILD.

## Test

```bash
./tests/test-tools.sh                 # 44 checks: auxo-tweak/auxo-fetch against a fake root
python3 tests/test-calamares-modules.py   # 29 checks: installer jobs with a mocked libcalamares
./scripts/smoke-test.sh out/*.iso     # boots the real ISO headless, waits for SDDM, screenshots
```

## How an install flows

`partition → mount → unpackfs (copies the live squashfs) → … → shellprocess@cleanlive`
(copies the kernel back into /boot, strips live-only files, removes calamares) →
**auxoprepare** (keyring, pacman.conf, multilib, reflector) → packages (netinstall picks) →
users → **auxodesktop** (`auxo-tweak accent/shell/kernel/desktop/drivers/zram`) →
initcpio → grubcfg → bootloader → **auxosnap** (`auxo-tweak snapshots on`, first snapshot) → done.

The installer calls the same `auxo-tweak` commands you can run later. Installing and
changing things after install go through the same code.
