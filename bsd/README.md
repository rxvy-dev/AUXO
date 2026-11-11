<p align="center">
  <img src="../branding/logo/auxobsd-logo.png" width="110" alt="AuxoBSD logo">
</p>

<h1 align="center">AuxoBSD</h1>

<p align="center">
  <b>FreeBSD, set up the way you want it.</b><br>
  FreeBSD 15.1 with KDE Plasma 6, Firefox and a graphical installer, the right graphics driver for
  your GPU (every NVIDIA generation included), and the Auxo tools: one accent colour everywhere,
  <code>auxo-tweak</code>, <code>auxo-fetch</code>, <code>auxo-update</code> with ZFS boot environments.
</p>

<p align="center">
  <a href="https://auxolinux.com/bsd">Website</a> ·
  <a href="https://discord.gg/XbQ66dH5a7">Discord</a> ·
  <a href="../README.md">Auxo Linux</a> ·
  <a href="kernel/README.md">AuxoBSD research kernel</a>
</p>

---

## What it is

AuxoBSD is FreeBSD underneath: FreeBSD's kernel, base system, `pkg`, rc.d and the FreeBSD Handbook
all work as normal. On top it adds:

- **KDE Plasma 6** with the Auxo theme (the same look-and-feel as Auxo Linux), SDDM, Konsole,
  Dolphin, Kate, Okular, Gwenview, Ark, and **Firefox**
- **`auxo-installer`**, a graphical (Qt) installer: keyboard, time zone, disk (ZFS or UFS), graphics,
  accent, shell, account, then a summary — nothing changes until you press Install
- **GPU detection and NVIDIA support** in the installer, the live session and `auxo-tweak`
- **Two package managers**: FreeBSD's `pkg` for the system and 36,000+ FreeBSD packages, and Void
  Linux's `xbps` for Linux software, run by FreeBSD's Linux compatibility layer
- **ZFS boot environments** before every `auxo-update`, and `auxo-rollback` to go back

```sh
auxo-tweak gpu                      # what's in the machine and which driver it gets
auxo-tweak nvidia status            # branch, KMS, nvidia-smi
auxo-tweak nvidia prime             # laptop: iGPU drives the screen, `prime-run app` uses NVIDIA
auxo-tweak accent rose              # boot menu, login screen, Plasma, Konsole, prompt
auxo-tweak xbps on                  # Void Linux packages: xbps-install -S neofetch; void-run neofetch
auxo-update                         # boot environment → freebsd-update → pkg (→ xbps)
auxo-rollback --list
```

## Graphics

`auxo-tweak gpu` and the installer read the PCI devices and pick:

| Hardware | Driver | Notes |
|---|---|---|
| NVIDIA Turing and newer (GTX 16xx, RTX 20xx–50xx) | `nvidia-drm-kmod` (current branch) | KMS on (`hw.nvidiadrm.modeset=1`), Plasma Wayland works |
| NVIDIA Maxwell, Pascal, Volta (GTX 750–10xx, Titan V) | `nvidia-drm-kmod-580` | KMS on, Wayland works |
| NVIDIA Kepler (GTX 600/700) | `nvidia-driver-470` | X11 only |
| NVIDIA Fermi / Tesla | `nvidia-driver-390` / `-340` | X11 only |
| Intel | `drm-kmod` → `i915kms` | |
| AMD GCN and newer / older Radeon | `drm-kmod` → `amdgpu` / `radeonkms` | |
| Laptop with Intel/AMD + NVIDIA | both, PRIME render offload | `prime-run APP`; `auxo-tweak nvidia primary` if the screen is wired to NVIDIA |
| VirtualBox (VMSVGA too) / VMware / QEMU | guest additions / open-vm-tools / scfb | VirtualBox is detected by SMBIOS, so VMSVGA isn't mistaken for VMware |

Switching branches removes the old NVIDIA packages first (they conflict) and installs
`nvidia-settings` too. The ISO carries a local package repository with every NVIDIA branch and
the VM guest tools, so the installer and `auxo-tweak nvidia` work without internet. The live session
loads Intel, AMD and current NVIDIA drivers on the fly; older NVIDIA cards use the framebuffer
until the installer sets up their driver.

## xbps on FreeBSD

`auxo-tweak xbps on` downloads Void's static `xbps`, bootstraps a minimal Void userland
(`base-voidstrap`) into `/compat/void`, adds its Linux mounts (linprocfs, linsysfs, devfs, /home,
/tmp) and installs wrappers: `xbps-install`, `xbps-remove`, `xbps-query`, … work as on Auxo Linux
(the same Void repositories), and `void-run COMMAND` starts what you installed as you, on your
display. `auxo-update` updates them too. Most command-line tools and many X11 apps work; anything
that needs Linux kernel modules, runit services or Linux GPU drivers doesn't. `auxo-tweak xbps off`
removes the wrappers (`--purge` also deletes `/compat/void`).

## The tools

| Tool | What it does |
|---|---|
| `auxo-tweak` | Menu + CLI: `accent`, `gpu`, `drivers`, `nvidia`, `shell`, `power`, `firewall` (pf), `dns`, `snapshots`, `cleanup`, `repo` (quarterly/latest), `service`, `xbps`, `info`. Linux-only commands (`desktop`, `kernel`, …) say what to use instead. |
| `auxo-fetch` | System summary in your accent colour (`--json`, `--small`, `--no-logo`) |
| `auxo-motd` | The Auxo greeting in each terminal, with tips and the Terry Fox Foundation note |
| `auxo-update` | ZFS boot environment, `freebsd-update`, `pkg upgrade`, xbps; says when to reboot |
| `auxo-rollback` | List boot environments, activate one (`/home` is never rolled back) |
| `auxo-installer` | The graphical installer on the live ISO (`--dry-run` shows the commands) |

The accent palette, the KDE colour-scheme code, the Plasma look-and-feel, the wallpapers and the
shell prompts come straight from `packages/auxo-tools` (Auxo Linux); `scripts/stage-tools.sh`
installs them with FreeBSD paths.

## How an install works

GPT with both a BIOS boot partition and an EFI system partition (so the disk boots either way) →
ZFS pool `zroot` with `ROOT/default` and the usual datasets (or UFS) → the live image copied from
a read-only view of the ISO (so nothing from the live session ends up on disk) → loader with the
Auxo logo, `rc.conf`, `fstab`, time zone, keyboard (console, Xorg and Plasma) → your account (in
`wheel`, root locked, sudo) → accent, shell and graphics drivers through `auxo-tweak` →
`efibootmgr` entry on UEFI machines.

## Build the ISO

On **FreeBSD 15** (a VirtualBox VM is fine) as root, with ~25 GB free:

```sh
cd bsd && ./build.sh          # → out/auxobsd-1.0.0-15.1-amd64.iso + .sha256
./scripts/test-vm.sh out/auxobsd-*.iso [--bios]     # QEMU with a blank 40 GB NVMe disk
```

`build.sh` downloads FreeBSD's `base.txz`/`kernel.txz` (checksums verified), installs the packages in
`packages.txt` in a chroot, fetches `packages-local.txt` into the local repository, adds the Auxo
tools, theme and live session (`overlay/`), and writes a hybrid ISO (BIOS + UEFI, DVD + USB) like
FreeBSD's own release tooling. The logo is `branding/logo/auxobsd-logo.png` (fetched from
auxolinux.com/bsd if missing).

No FreeBSD machine: Actions → **AuxoBSD ISO** → Run workflow builds it in a FreeBSD 15.1 VM
(`.github/workflows/auxobsd-iso.yml`).

**VirtualBox:** Type *Other* / *FreeBSD (64-bit)*, 6 GB RAM or more, 4 CPUs, VMSVGA graphics,
EFI on or off. The live user is `auxo` (no password); the installer is on the desktop.

## Tests

```sh
./bsd/tests/test-tools.sh           # auxo-tweak & co. against a fake FreeBSD root (commands printed, not run)
python3 bsd/tests/test-installer.py # install plans, parsers, dry-run runner, the Qt window offscreen
```

## Layout

```
bsd/
├── build.sh, VERSION, packages*.txt   # the ISO
├── overlay/                           # live session (rc.d/auxolive, auxogpu), loader.conf, Firefox policies
├── tools/files/                       # FreeBSD versions of the Auxo tools + libraries
├── installer/                         # auxo-installer (PyQt6): config, system, plan, runner, gui
├── scripts/                           # stage-tools.sh, test-vm.sh
├── tests/
└── kernel/                            # the from-scratch AuxoBSD research kernel (asm + C + Rust)
```

## Credits

AuxoBSD is made by **rxvy** with **Onyx-ig**, who started AuxoBSD and wrote the research kernel
(see `kernel/Credits.txt`). Built on [FreeBSD](https://www.freebsd.org) and KDE Plasma; xbps and the
Void repositories from [Void Linux](https://voidlinux.org). Not affiliated with the FreeBSD Project.
