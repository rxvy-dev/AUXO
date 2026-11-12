.RECIPEPREFIX := >

CC       ?= gcc
LD       ?= ld
CARGO    ?= cargo
PYTHON   ?= python3
QEMU     ?= qemu-system-x86_64
USE_RUST ?= 1
VERSION  := $(shell cat VERSION)
.DEFAULT_GOAL := all

CFLAGS := -std=gnu11 -O2 -g -Wall -Wextra -m64 -march=x86-64 -mtune=generic -Iinclude -ffreestanding -fno-pic -fno-pie -fno-stack-protector -mno-red-zone -mgeneral-regs-only -mcmodel=small -fno-tree-loop-distribute-patterns -DAUXO_VERSION='"$(VERSION)"' -MMD -MP
LDFLAGS := -n -nostdlib --no-warn-rwx-segments -z max-page-size=0x1000 -T linker.ld

# The Chrono desktop (kern/chrono.c) and its drawing library (kern/gfx.c) use SSE floating point.
# They only run on the "chrono" thread, the one thread that touches XMM registers, so nothing
# else has to save them. Thread stacks are only 8-byte aligned on entry; GCC realigns.
GUI_OBJS   := build/kern/gfx.o build/kern/chrono.o build/kern/browser.o build/kern/aether.o build/kern/installer.o
GUI_CFLAGS := $(filter-out -mgeneral-regs-only,$(CFLAGS)) -msse2 -mfpmath=sse -mincoming-stack-boundary=3 -fno-math-errno
$(GUI_OBJS): CFLAGS := $(GUI_CFLAGS)
# BearSSL (third_party/bearssl, MIT): TLS for HTTPS. Integer-only; no OS hooks, we seed it ourselves.
BEARSSL_CFLAGS := $(filter-out -Wall -Wextra,$(CFLAGS)) -Ithird_party/shim -Ithird_party/bearssl -DBR_USE_URANDOM=0 \
  -DBR_USE_GETENTROPY=0 -DBR_USE_UNIX_TIME=0 -DBR_USE_WIN32_TIME=0 -DBR_USE_WIN32_RAND=0 -DBR_AES_X86NI=0 \
  -DBR_SSE2=0 -DBR_POWER8=0 -DBR_RDRAND=0 -DBR_64=1
build/third_party/bearssl/%.o: CFLAGS := $(BEARSSL_CFLAGS)
build/net/http.o: CFLAGS += -Ithird_party/bearssl -Ithird_party/shim
build/net/http.o: include/trust_anchors.h

$(GUI_OBJS): include/gfx.h include/fonts.h include/chrono_themes.h include/chrono.h

SRCS_C := $(wildcard kern/*.c) $(wildcard arch/x86_64/*.c) $(wildcard net/*.c) $(wildcard disk/*.c) $(wildcard usb/*.c) $(wildcard third_party/bearssl/*.c)
SRCS_S := $(wildcard arch/x86_64/*.S)
# pkg (tools/pkg) goes in build/overlay.tar when the host can build it (see below)
HOSTCC   ?= gcc
ZSTD_LIB := $(firstword $(wildcard /usr/lib/x86_64-linux-gnu/libzstd.so.1 /usr/lib/x86_64-linux-gnu/libzstd.so))
PKG_BEARSSL := $(patsubst third_party/bearssl/%.c,build/pkg/bearssl/%.o,$(wildcard third_party/bearssl/*.c))
OVERLAY  := $(if $(ZSTD_LIB),$(if $(wildcard /usr/include/lzma.h),build/overlay.tar))

OBJS := $(patsubst %.c,build/%.o,$(SRCS_C)) $(patsubst %.S,build/%.o,$(SRCS_S))
-include $(OBJS:.o=.d)                 # header dependencies (-MMD): a changed struct rebuilds its users

RUSTLIB := rust/target/x86_64-unknown-none/release/libauxo_rs.a

# Use the prebuilt x86_64-unknown-none core when rustup has it; otherwise build core from
# rust-src (rustup component add rust-src, or a distro rust-src package of the same version).
RUST_STD := $(shell d=$$(rustc --print target-libdir --target x86_64-unknown-none 2>/dev/null); [ -d "$$d" ] && echo installed)
ifeq ($(RUST_STD),installed)
CARGOFLAGS ?=
else
CARGOFLAGS ?= -Zbuild-std=core
export RUSTC_BOOTSTRAP := 1
endif

# Auxo Linux palette, shared with the kernel (see tools/gen-accents.py)
PALETTE := ../../packages/auxo-tools/files/lib/auxo_accents.py

.PHONY: all rust check iso run run-text test test-install test-hw test-browser test-pkg pkg rootfs install-image accents accents-check chrono-themes chrono-themes-check fonts trust clean menuconfig

all: build/auxobsd.elf

build/%.o: %.c include/auxo_accents.h
>@mkdir -p $(dir $@)
>$(CC) $(CFLAGS) -c $< -o $@

build/%.o: %.S
>@mkdir -p $(dir $@)
>$(CC) $(CFLAGS) -c $< -o $@

rust:
>cd rust && $(CARGO) build --release --target x86_64-unknown-none $(CARGOFLAGS)

ifeq ($(USE_RUST),1)

build/auxobsd.elf: $(OBJS) $(RUSTLIB) linker.ld
>$(LD) $(LDFLAGS) -o $@ $(OBJS) $(RUSTLIB)

$(RUSTLIB): $(wildcard rust/src/*.rs) rust/Cargo.toml
>cd rust && $(CARGO) build --release --target x86_64-unknown-none $(CARGOFLAGS)

else

build/auxobsd.elf: $(OBJS) linker.ld
>$(LD) $(LDFLAGS) -o $@ $(OBJS)

endif

# regenerate the accent header after changing the Auxo Linux palette
accents:
>$(PYTHON) -I tools/gen-accents.py $(PALETTE) include/auxo_accents.h

accents-check:
>$(PYTHON) -I tools/gen-accents.py --check $(PALETTE) include/auxo_accents.h

# Chrono themes: chrono/themes/*.theme (copied from github.com/rxvy-dev/chrono) → C tables
chrono-themes:
>$(PYTHON) -I tools/gen-chrono.py chrono/themes include/chrono_themes.h

chrono-themes-check:
>$(PYTHON) -I tools/gen-chrono.py --check chrono/themes include/chrono_themes.h

# HTTPS root certificates (Mozilla's, via certifi or ca-certificates; needs python cryptography)
trust:
>$(PYTHON) tools/gen-trust.py "" include/trust_anchors.h

# desktop fonts (needs Pillow and the DejaVu fonts); the generated header is committed
fonts:
>$(PYTHON) -I tools/gen-fonts.py include/fonts.h

check: build/auxobsd.elf
>grub-file --is-x86-multiboot2 $< && echo "multiboot2: OK"

iso: build/auxobsd.iso

build/auxobsd.iso: build/auxobsd.elf iso/boot/grub/grub.cfg $(wildcard build/rootfs.tar) $(wildcard build/install.img) $(OVERLAY)
>cp $< iso/boot/auxobsd.elf
>if [ -f build/overlay.tar ]; then cp build/overlay.tar iso/boot/overlay.tar; else rm -f iso/boot/overlay.tar; fi
>if [ -f build/rootfs.tar ]; then cp build/rootfs.tar iso/boot/rootfs.tar; else rm -f iso/boot/rootfs.tar; fi
>if [ -f build/install.img ]; then cp build/install.img iso/boot/install.img; else rm -f iso/boot/install.img; fi
>grub-mkrescue -o $@ iso

# pkg, the package manager (tools/pkg): a Linux program built here, against the same Ubuntu
# 24.04 libraries the root has (zlib, liblzma, libzstd), with its own BearSSL for HTTPS. It goes
# in build/overlay.tar, a small module GRUB loads on top of the root filesystem.

build/pkg/bearssl/%.o: third_party/bearssl/%.c
>@mkdir -p $(dir $@)
>$(HOSTCC) -O2 -Ithird_party/bearssl -DBR_AES_X86NI=0 -DBR_SSE2=0 -DBR_POWER8=0 -c $< -o $@

build/pkg/libbearssl.a: $(PKG_BEARSSL)
>rm -f $@ && ar rcs $@ $^

build/pkg/pkg: tools/pkg/pkg.c build/pkg/libbearssl.a include/trust_anchors.h
>$(HOSTCC) -O2 -Wall -Ithird_party/bearssl -Iinclude $< build/pkg/libbearssl.a -Wl,-Bstatic -lz -llzma -Wl,-Bdynamic $(ZSTD_LIB) -o $@

pkg: build/pkg/pkg

build/overlay.tar: build/pkg/pkg tools/pkg/sources
>rm -rf build/overlay && mkdir -p build/overlay/usr/bin build/overlay/etc/pkg build/overlay/var/cache/pkg
>cp build/pkg/pkg build/overlay/usr/bin/pkg && cp tools/pkg/sources build/overlay/etc/pkg/sources
>tar --owner=0 --group=0 --format=ustar -C build/overlay -cf $@ .

# the root filesystem: real Linux programs (GCC 13, binutils, make, BusyBox) from Ubuntu packages.
# ROOTFS_SET=full adds Python 3, GTK 4 and Aether.
rootfs:
>sh tools/mkrootfs.sh build

# the boot template the installer writes (GRUB for BIOS + an EFI system partition)
install-image:
>sh tools/mkdisk-template.sh build

run: iso
>$(QEMU) -cdrom build/auxobsd.iso -serial stdio -m 1G -vga std

run-text: iso
>$(QEMU) -cdrom build/auxobsd.iso -serial stdio -m 256M -display curses

# boots the ISO headless in QEMU and drives auxsh over the serial port
test: iso
>$(PYTHON) -I tests/test-shell.py build/auxobsd.iso

# installs onto a blank disk image, then boots it with BIOS and (if OVMF is there) UEFI
test-install: iso
>$(PYTHON) -I tests/test-install.py build/auxobsd.iso

# a PC with USB input only and ACPI
test-hw: iso
>$(PYTHON) -I tests/test-hw.py build/auxobsd.iso

# tabs, sign-in (POST, cookies), saved passwords, the session file
test-browser: iso
>$(PYTHON) -I tests/test-browser.py build/auxobsd.iso

# pkg: real Ubuntu packages from a small local mirror, kept on an installed system's disk
test-pkg: iso
>$(PYTHON) -I tests/test-pkg.py build/auxobsd.iso

menuconfig:
>$(PYTHON) config/kconfig.py menuconfig

clean:
>rm -rf build iso/boot/auxobsd.elf
>cd rust && $(CARGO) clean
