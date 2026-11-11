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

## Running it in a VM

- **QEMU:** `make run`, or `qemu-system-x86_64 -cdrom build/auxobsd.iso -m 256M`.
- **VirtualBox:** create the VM as *Type: Other, Version: Other/Unknown (64-bit)* (or
  *FreeBSD (64-bit)*), leave **EFI off** (the ISO boots through BIOS GRUB), 64 MB of RAM or more.
  A 32-bit VM type hides long mode from the guest; AuxoBSD then stops with a message saying so
  (older builds triple-faulted, which VirtualBox reports as a "critical error").
  A serial port is optional: without one, auxsh uses the PS/2 keyboard and the screen only.

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

   ●    Welcome to AuxoBSD, root.
  /\    AuxoBSD 0.1.0 · up 0m · mem 2% used
 /  \   type help for commands · auxo-fetch for a summary
/_/\_\  tip: auxo-tweak accent rose — recolour the console, prompt, auxo-fetch and this greeting at once

▲ root@auxobsd ❯
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
