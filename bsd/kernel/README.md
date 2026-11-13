<p align="center">
  <img src="../../branding/logo/auxo-logo-icon-512.png" width="96" alt="Auxo logo">
</p>

<h1 align="center">AuxoBSD kernel</h1>

<p align="center">
  <strong>The from-scratch AuxoBSD kernel, for x86_64.</strong>
  <br>
  A research kernel built from the ground up with Assembly, C and Rust, with the Auxo tools built in.
</p>

<p align="center">
  <code>x86_64</code> · <code>no_std</code> · <code>Multiboot2</code> · <code>GRUB</code>
  <br>
  <a href="https://auxolinux.com">auxolinux.com</a> ·
  <a href="https://discord.gg/XbQ66dH5a7">Discord</a> ·
  <a href="../../README.md">Auxo Linux</a> ·
  <a href="../README.md">AuxoBSD</a>
</p>

---

## Overview

This is the original, from-scratch AuxoBSD kernel by Onyx-ig. The AuxoBSD you install day to
day is [built on FreeBSD](../README.md) (XFCE, Firefox, a graphical installer); this kernel is
kept here as the Auxo research kernel, with its own build and tests. It shares Auxo's logo,
accent palette and tools: boot it and you land in **auxsh** with the Auxo greeting,
`auxo-fetch` and `auxo-tweak`.

Its kernel combines low-level Assembly, C and Rust:

| Language | Responsibilities | Source |
|---|---|---|
| **Assembly** | Multiboot2 header, CPU mode transition, paging setup, interrupt stubs, context switching, register handling | `arch/x86_64/*.S` |
| **C** | Hardware initialization, console, GDT/TSS, IDT, PIC/PIT, physical memory, trap dispatch, input, the shell and the Auxo tools | `kern/*.c`, `arch/x86_64/*.c` |
| **Rust** | Kernel heap, process management, round-robin scheduler, BSD-style syscall dispatcher | `rust/src/*.rs` |

## The Auxo tools on AuxoBSD

There is no userland yet, so the tools are built into the kernel shell. Their command lines
and output follow the Auxo Linux tools in `packages/auxo-tools/`.

```text
auxo-fetch                    # system summary in your accent colour (--small, --no-logo, --json, --accent NAME)
auxo-tweak                    # interactive menu
auxo-tweak accent rose        # recolour the console, prompt, auxo-fetch and the MOTD at once
auxo-tweak hostname summit    # the name in the prompt and auxo-fetch
auxo-tweak motd off           # no greeting when the shell starts
auxo-tweak color off          # plain serial output (no ANSI)
auxo-tweak info               # every Auxo setting at a glance
auxo-tweak persist            # the GRUB line that keeps your settings after a reboot
auxo-tweak reset              # defaults
auxo-motd                     # the greeting (--tip, --give, --off, --on)
```

`auxo-tweak` sub-commands that need a userland, drivers or a package manager (`desktop`,
`kernel`, `nvidia`, `gaming`, …) say they are Auxo Linux features instead of failing.

**Accents** are the seven Auxo accents (violet, cyan, emerald, amber, rose, blue, mono).
`include/auxo_accents.h` is generated from the Auxo Linux palette
(`packages/auxo-tools/files/lib/auxo_accents.py`) by `make accents`, so both editions always
use the same colours; `make accents-check` fails if they drift apart. On the VGA console the
accent gradient is loaded into the VGA DAC, so changing accent recolours the whole screen.

**Settings** live in memory (there is no disk driver to save them yet) and start from the
kernel command line, which GRUB passes in `iso/boot/grub/grub.cfg`:

```text
multiboot2 /boot/auxobsd.elf accent=rose hostname=summit motd=0 nocolor
```

`auxo-tweak persist` prints this line for your current settings.

### Other auxsh commands

`help`, `clear`, `echo`, `uname [-a|-s|-r|-m]`, `uptime`, `free`, `ps`, `demo` (the scheduler
and `int 0x80` syscall demo threads), `cmdline`, `history`, `reboot`, `poweroff`.

For debugging Linux programs: `strace PROGRAM` (every system call), `strace -e PROGRAM` (only
the ones that fail), `strace -n NAME …` (only threads whose name starts with NAME), and
`ustack PID` (the system call each thread of a process is in, and its call chain). A program
that dies of a crash (SIGSEGV, SIGABRT, SIGILL …) gets a report on the console: where it was
and its call chain, as `library+offset`, ready for `addr2line` or `nm`.
Tab completes command names, Up/Down walks the history, Ctrl-C cancels a line, Ctrl-L clears.
Input works from the PS/2 keyboard and from the serial port.

## Current Features

- x86_64 long-mode initialization
- Multiboot2 boot protocol support (command line + loader name are read)
- 4 GiB identity mapping using 2 MiB pages
- GDT and TSS initialization
- IDT with 48 interrupt vectors
- PIC and PIT initialization
- Serial and VGA console output, with ANSI colour (16, 256 and 24-bit) mapped onto a VGA palette
- Interrupt-driven PS/2 keyboard and serial input
- Kernel logging through `kprintf` / `ksnprintf`
- Bitmap-based physical memory manager
- Rust kernel heap allocator
- Basic process table and round-robin scheduler
- Assembly context switching
- BSD-inspired system call interface
- `int 0x80` syscall gate
- Kernel threads, and auxsh running as one of them
- `auxo-fetch`, `auxo-tweak`, `auxo-motd`

## BSD Compatibility Design

AuxoBSD adopts selected conventions from BSD systems, particularly FreeBSD.

### System call interface

| System call | Number |
|---|---:|
| `exit` | 1 |
| `write` | 4 |
| `getpid` | 20 |
| `sched_yield` | 331 |

The current syscall interface uses:

- **Entry:** `int 0x80`
- **Arguments:** `rdi`, `rsi`, `rdx`
- **Return value:** `rax`
- **Errors:** negative BSD errno values

The implementation uses BSD errno numbering. For example, `ENOSYS` is 78 rather than Linux's 38.

The process table also follows a BSD-inspired convention: PID 0 represents the idle/swapper process.

## Architecture

### Memory map

The kernel is linked and loaded at physical address `1 MiB`.

The current memory layout uses:

- A 4 GiB identity-mapped address range.
- 2 MiB pages for initial paging.
- A bitmap-based physical memory manager.
- Reserved memory below 1 MiB.
- Reserved kernel and boot-information regions.

A higher-half kernel and per-process virtual address spaces are planned.

### Physical memory manager

The PMM uses a bitmap with one bit per physical frame.

It avoids allocating frames reserved for firmware, the kernel, and boot information.

### Kernel heap

The Rust heap currently uses a contiguous 4 MiB region obtained from the PMM.

Its allocator provides:

- First-fit allocation
- Block splitting
- Block coalescing
- 16-byte alignment

### Scheduling and interrupts

The current implementation targets a single CPU.

Critical sections use `cli` and restore the previous interrupt-enable state afterward.

The PIT runs at 100 Hz, with a 10-tick scheduling quantum. Timer interrupts can trigger preemption inside the IRQ handler, after sending the EOI.
IRQ1 (keyboard) and IRQ4 (COM1) fill one input queue that auxsh reads, sleeping with `hlt` while it is empty.

SMP and APIC support are planned.

## Building

Everything below runs from `bsd/kernel/`.

### Requirements

- GCC
- GNU Binutils 2.39 or newer
- Rust toolchain
- `x86_64-unknown-none` target (`rustup target add x86_64-unknown-none`), or `rust-src` for the same Rust version
- `make`, `python3`
- `grub-file`, `grub-mkrescue`, `xorriso` (and `mtools`)
- `qemu-system-x86_64`

If the prebuilt `x86_64-unknown-none` target isn't installed, the Makefile builds `core` from
`rust-src` instead (`-Zbuild-std=core`), e.g. with a distro `rust-src` package that matches your `rustc`.

### Build commands

```sh
make            # build the kernel → build/auxobsd.elf
make check      # validate the Multiboot2 image
make iso        # → build/auxobsd.iso
make run        # build the ISO and boot it in QEMU (serial on this terminal)
make test       # boot headless in QEMU and drive auxsh: auxo-fetch, auxo-tweak, auxo-motd, …
make accents    # regenerate include/auxo_accents.h from the Auxo Linux palette
make USE_RUST=0 # build without Rust (needs rs_* stubs)
```

The version comes from `bsd/kernel/VERSION`.

## Hardware support

| Area | Drivers |
|---|---|
| Graphics | any firmware framebuffer (BIOS VBE or UEFI GOP; 15/16/24/32 bpp), mapped wherever the card puts it, above 4 GiB and 512 GiB included (NVIDIA, AMD and Intel cards alike); a text console on the framebuffer when there's no VGA text mode |
| Wired network | Intel e1000/e1000e (82540–82579, I217–I219), Intel igb/igc (82576, I210, I211, I350, I225, I226), Realtek RTL8111/8168/8169 |
| USB | xHCI with hubs and hot-plug; keyboards, mice, tablets/touchscreens (absolute pointers), USB sticks and drives (`usbN` disks), phone tethering and USB Ethernet (RNDIS, CDC-ECM) |
| Storage | AHCI (SATA), NVMe, IDE, USB mass storage |
| Memory | up to 128 GiB |
| Power | ACPI power off and reset |
| Sound | Intel High Definition Audio (the sound chip of nearly every PC since 2004): the codec's speaker, headphone or line-out path is found and turned on; Linux programs play through ALSA (`/dev/snd`) |

Not there yet: Wi-Fi (tether a phone over USB instead), GPU acceleration, sound input
(microphones), HDMI audio, USB sound cards, the Realtek RTL8125 (2.5 Gbit).

## The disk: AuxoFS

An installed AuxoBSD runs from its disk. The installer formats the third partition as
**AuxoFS** and copies the live system's whole tree into it (programs installed with `pkg` and
your files included); GRUB then only loads the kernel, and the kernel mounts the disk as `/`.

- Files are read from the disk the first time something needs them, and when memory runs short
  the ones nobody has used for a while are dropped again (they come back from the disk), so the
  system no longer has to fit in RAM: an installed system boots and compiles in 192 MB.
- Changes go to the disk every 5 seconds, on `sync`, on `fsync` and at power off. A changed
  file is written to new blocks; the record pointing at them goes to a log after the data; when
  the log fills, the whole tree is written as a checkpoint and one of two superblocks is pointed
  at it. Nothing the last saved state points at is ever overwritten, so a crash or a power cut
  loses at most the last few seconds and never leaves a half-written disk (the test pulls the
  plug and checks).
- Owners, modes, links and hidden folders are kept. Systems installed by earlier versions (the
  root image in memory, changes saved beside it) keep booting the way they did.

## Files and the web

- **Files** (in the menu): places on the left (home, its folders, the whole computer), folders
  first, double-click or Enter to open: folders go in, web pages and pictures open in the
  browser, programs run, everything else opens in Aether. New folder, F2 to rename, Delete,
  Ctrl+C / Ctrl+X / Ctrl+V, hidden files on and off. It acts as you: what your account can't
  touch, Files can't either.
- **Browser** opens GNOME Web (WebKit: all of HTML, CSS and JavaScript, HTTPS) when the system
  has it, which the *full* ISO does (CI builds it as `auxobsd-full-iso`; give the machine 4 GB to
  run it live). The small ISO falls back to the built-in browser, which shows pages without CSS.

## Users and permissions

AuxoBSD runs as a person's account, not as root. A live system makes **`auxo`** (uid 1000, no
password, in the `sudo`, `audio`, `video` … groups); the installer asks who will use the
computer — name, user name, password — and the installed system is theirs: the desktop starts
on a lock screen that wants the password (Lock in the menu locks it again), and the Terminal,
the desktop's apps and every program run as that user.

- Every process has Linux's user and group ids (real, effective, saved, filesystem,
  supplementary groups), with `setuid`/`setresuid`/`setgroups`/`setfsuid` and the rest, and
  capabilities that come and go with them as on Linux (dropping root drops them for good).
- Every file has an owner, a group and a mode, checked on every open, exec, directory search,
  create, delete and rename; `/tmp` is sticky; `chmod`, `chown` and `utimes` follow Linux's
  rules; set-user-ID programs work (with `AT_SECURE`), `access()` answers for the real user,
  signals only reach your own processes, hard links are protected. Owners and modes are kept
  on the installed disk with the files (homes included, with their hidden folders).
- **`sudo`**: the Terminal's own (`sudo CMD`, `sudo -s`, `su`/`exit` for a root shell, the
  password remembered for 5 minutes) and a set-user-ID `/usr/bin/sudo` for scripts and other
  shells; members of `sudo` only. `passwd` changes your password (SHA-512 crypt in
  `/etc/shadow`, as Ubuntu keeps it). `pkg install` and `install` need `sudo`.
- GNOME Web's web pages run as you, with no capabilities, in bubblewrap's namespaces under
  WebKit's seccomp filter: `bwrap` is set-user-ID root (AuxoBSD has no user namespaces), which
  is the setup bubblewrap was built for.

## Sound

`snd` finds an Intel HD Audio controller, resets it, walks the codec for the best output
(speaker, then headphones, then line out) and a path to it from a DAC, powers it up, unmutes it
and plays one stream from a ring buffer the controller reads by DMA. Programs see ALSA's kernel
interface: `/dev/snd/controlC0` and `/dev/snd/pcmC0D0p` (group `audio`), 16-bit stereo at 44.1
or 48 kHz, read/write or mmap access, with ALSA's parameter rules, so `alsa-lib` and everything
on it (aplay, speaker-test, SDL, GStreamer, mpv) works; other formats and rates go through
alsa-lib's plug converter. `amixer` sets the master volume. One program plays at a time (no
mixing daemon yet). `make test-sound` plays tones through QEMU's HD Audio into a WAV file and
checks the pitch on each side.

## Linux system calls

Beyond files, memory, processes, sockets and signals, the calls desktop programs lean on are
there: `inotify` (file managers, editors watching their files), `timerfd` and POSIX timers,
`pidfd_open` + `waitid(P_PIDFD)` (how GLib watches its children), `signalfd`, `eventfd`,
`epoll`, `rt_sigtimedwait`/`sigqueue`, `sendfile`/`copy_file_range`, `preadv`/`pwritev`,
`mknod` for FIFOs, `sched_*` and priorities, `execveat`, `openat2`. `tests/syscalls.c` checks
them (in `make test-sandbox`).

## Sandboxing and the web browser

The kernel has what Linux sandboxes are built from, with Linux's system calls and semantics:

- **mount namespaces**: `mount` (bind, recursive bind, remount read-only/nosuid/nodev/noexec,
  tmpfs, proc, devpts, mqueue), `umount2`, `pivot_root`, `chroot`; every path a program names
  is looked up in its own namespace and root, and read-only mounts are enforced per path;
- **PID namespaces** (nested, with an init that reaps orphans and takes everything down with
  it), **network** (only `lo`, set up through netlink), **UTS** (hostname), **IPC** and
  **cgroup** namespaces, through `clone`, `clone3` and `unshare`;
- **seccomp** filters (classic BPF; errno, trap and kill answers; stacked; kept across
  `fork` and `execve`), `no_new_privs`, capabilities (`capget`, `capset`, the bounding set);
- **/proc** as a real filesystem: `/proc/PID/{status,maps,mountinfo,fd,ns,…}` and
  `/proc/sys`, mountable again inside a namespace.

User namespaces aren't there; programs that ask get `EINVAL`, which is what they check for,
and bubblewrap is set-user-ID root instead (see Users and permissions).

**bubblewrap** (`bwrap`) runs on this, and so does **GNOME Web** (the full root image) with
WebKit's sandbox on: each web page's process runs in its own mount, PID and network
namespaces with WebKit's seccomp filter, so a page that took over its renderer still can't
read your files, see other programs or reach the network on its own. Sign-in pages (Google,
GitHub, …) are ordinary HTTPS pages to it: certificates are checked against the system's CA
bundle, and cookies (kept in SQLite) keep you signed in. Saving passwords in GNOME Web needs
a keyring (the Secret Service), which isn't there yet: it asks every time.

`make test-sandbox` builds `tests/sandbox-escape.c` inside AuxoBSD and tries to get out, as
root under `sudo` and as the user:
writing through read-only binds, reaching files that weren't bound in (also by climbing out
of a chroot), signalling or seeing processes outside, reaching the network, and — with
WebKit's filter — mounting, unsharing, `chroot` and `pivot_root`. Every attempt has to fail.

The desktop starts a D-Bus session bus for GNOME programs, and Mesa's software renderer
(llvmpipe) draws WebKit's pages without a GPU driver. GNOME Web wants about 4 GB of RAM
(the full root image is held in memory).

## Running it in a VM

- **QEMU:** `make run`, or `qemu-system-x86_64 -cdrom build/auxobsd.iso -m 256M`.
- **VirtualBox:** create the VM as *Type: Other, Version: Other/Unknown (64-bit)* (or
  *FreeBSD (64-bit)*); EFI on or off both work. Give it 2 GB of RAM or more for the desktop and
  Linux programs, 4 GB for GNOME Web. After you pick a boot menu entry, GRUB loads the 350 MB system image: the
  screen says so and stays still for a while, which is normal. A 32-bit VM type hides long mode
  from the guest; AuxoBSD then stops with a message saying so.
  A serial port is optional: without one, auxsh uses the keyboard and the screen only.
- **If the screen stays black** after that, pick *text console* in the boot menu: it draws a
  plain console on the framebuffer, BIOS or UEFI. If the kernel ever crashes, the error is drawn
  on the screen over the desktop; a photo of it is the most useful bug report.

## Boot Output

```text
AuxoBSD 0.1.0 (x86_64) - C + Rust + Assembly · part of Auxo · auxolinux.com
boot: long mode OK, loaded by GRUB 2.12
gdt : kernel/user segments + TSS loaded
idt : 48 vectors + int 0x80 syscall gate
pmm : 255 MiB usable, 254 MiB free
rust: heap + scheduler up (heap at 0x...)
tty : VGA text 80x25 + ttyS0 115200, PS/2 keyboard
auxo: accent violet, hostname auxobsd

   ●    Welcome to AuxoBSD, auxo.
  /\    AuxoBSD 0.1.0 · up 0m · mem 2% used
 /  \   type help for commands · auxo-fetch for a summary
/_/\_\  tip: auxo-tweak accent rose — recolour the console, prompt, auxo-fetch and this greeting at once

▲ auxo@auxobsd ❯
```

## Verification Status

| Check | Status |
|---|---|
| C compilation with `-O2 -Wall -Wextra` | passes |
| Rust half (`x86_64-unknown-none`, `no_std`) | builds |
| Multiboot2 image (`make check`) | OK |
| Boot in QEMU and drive auxsh over serial (`make test`) | 60 checks pass |
| PS/2 keyboard input, VGA colours | checked in QEMU |

## Credits

See [Credits.txt](Credits.txt). AuxoBSD keeps its own licence ([LICENSE](LICENSE)); the rest of
the repository is GPL-3.0.
