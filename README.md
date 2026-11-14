# AuxoBSD ISO images

Built from `auxo-freebsd` at commit `f19c982` (System Monitor, 32-bit programs, `pkg` multiarch).

| File | ISO | What's in it |
|---|---|---|
| `auxobsd.iso.xz` | `auxobsd.iso` (222 MiB) | The kernel, GCC, BusyBox, the i386 C library. The built-in browser shows pages without CSS. 2 GB of RAM is enough. |
| `auxobsd-full.iso.xz.part0`, `part1`, `part2` | `auxobsd-full.iso` (870 MiB) | Everything above, plus GNOME Web (full HTML, CSS and JavaScript), Python 3, GTK 4 and the Aether editor. Give the machine 4 GB of RAM to run it live. |

The images are xz-compressed so they fit GitHub's 100 MB file limit. The full image is also split into three parts.

## Get the small ISO

```sh
git clone --depth 1 --branch auxobsd https://github.com/rxvy-dev/AUXO auxobsd-iso
cd auxobsd-iso
xz -dk auxobsd.iso.xz
sha256sum -c SHA256SUMS --ignore-missing
```

## Get the full ISO

```sh
git clone --depth 1 --branch auxobsd https://github.com/rxvy-dev/AUXO auxobsd-iso
cd auxobsd-iso
cat auxobsd-full.iso.xz.part0 auxobsd-full.iso.xz.part1 auxobsd-full.iso.xz.part2 | xz -d > auxobsd-full.iso
sha256sum -c SHA256SUMS --ignore-missing
```

On Void Linux, `xz` is in the `xz` package (`sudo xbps-install -S xz`).

## Run it

QEMU:

```sh
qemu-system-x86_64 -enable-kvm -m 4G -cdrom auxobsd-full.iso -nic user,model=e1000 -device intel-hda -device hda-duplex
```

VirtualBox: make a new VM (type Other, version Other/Unknown 64-bit) with 4 GB of RAM and attach the ISO as the optical drive.

To put it on a disk, run `sudo install` in the terminal.
