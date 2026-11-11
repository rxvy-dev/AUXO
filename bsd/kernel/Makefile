.RECIPEPREFIX := >

CC       ?= gcc
LD       ?= ld
CARGO    ?= cargo
PYTHON   ?= python3
QEMU     ?= qemu-system-x86_64
USE_RUST ?= 1
VERSION  := $(shell cat VERSION)

CFLAGS := -std=gnu11 -O2 -g -Wall -Wextra -m64 -march=x86-64 -mtune=generic -Iinclude -ffreestanding -fno-pic -fno-pie -fno-stack-protector -mno-red-zone -mgeneral-regs-only -mcmodel=small -fno-tree-loop-distribute-patterns -DAUXO_VERSION='"$(VERSION)"'
LDFLAGS := -n -nostdlib --no-warn-rwx-segments -z max-page-size=0x1000 -T linker.ld

SRCS_C := $(wildcard kern/*.c) $(wildcard arch/x86_64/*.c)
SRCS_S := $(wildcard arch/x86_64/*.S)
OBJS := $(patsubst %.c,build/%.o,$(SRCS_C)) $(patsubst %.S,build/%.o,$(SRCS_S))

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

.PHONY: all rust check iso run test accents accents-check clean menuconfig

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

check: build/auxobsd.elf
>grub-file --is-x86-multiboot2 $< && echo "multiboot2: OK"

iso: build/auxobsd.iso

build/auxobsd.iso: build/auxobsd.elf iso/boot/grub/grub.cfg
>cp $< iso/boot/auxobsd.elf
>grub-mkrescue -o $@ iso

run: iso
>$(QEMU) -cdrom build/auxobsd.iso -serial stdio -m 256M

# boots the ISO headless in QEMU and drives auxsh over the serial port
test: iso
>$(PYTHON) -I tests/test-shell.py build/auxobsd.iso

menuconfig:
>$(PYTHON) config/kconfig.py menuconfig

clean:
>rm -rf build iso/boot/auxobsd.elf
>cd rust && $(CARGO) clean
