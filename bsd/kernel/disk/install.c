/* Installing AuxoBSD onto a disk, and keeping /home/root on it.
 *
 * Layout (MBR):  [boot template: GRUB for BIOS + EFI system partition (p1)]
 *                [p2: tar archive of /boot: grub.cfg, auxobsd.elf, rootfs.tar]
 *                [p3: "AUXOHOME" + the files of /home/root]
 * The template, the kernel and the root filesystem come from GRUB modules of the live system
 * ("installer", "kernel", "rootfs"), so nothing is read back from the install medium. */
#include "kern.h"
#include "arch.h"
#include "rs.h"
#include "auxo.h"
#include "disk.h"
#include "vfs.h"
#include "fs.h"
#include "install.h"

volatile int install_state, install_pct;
char install_msg[160];

static struct install_req req;
static uint8_t *iobuf;                     /* 1 MiB */
#define IOSECT 2048

static void msg(const char *m) { strlcpy(install_msg, m, sizeof install_msg); }

/* ── tar writer (ustar), streamed to the disk ───────────────────────── */
static struct disk *dk;
static uint64_t lba;                       /* next sector to write */
static uint32_t fill;                      /* bytes in iobuf */
static int failed;

static void flush(void)
{
    if (!fill || failed) return;
    uint32_t sectors = (fill + 511) / 512;
    memset(iobuf + fill, 0, sectors * 512 - fill);
    if (dk->write(dk, lba, sectors, iobuf) < 0) failed = 1;
    lba += sectors;
    fill = 0;
}

static void put(const void *data, size_t n)
{
    const uint8_t *d = data;
    while (n && !failed) {
        size_t c = IOSECT * 512 - fill;
        if (c > n) c = n;
        memcpy(iobuf + fill, d, c);
        fill += (uint32_t)c; d += c; n -= c;
        if (fill == IOSECT * 512) flush();
    }
}

static void octal(char *f, int width, uint64_t v)
{
    f[width - 1] = 0;
    for (int i = width - 2; i >= 0; i--) { f[i] = (char)('0' + (v & 7)); v >>= 3; }
}

static void tar_entry(const char *name, const void *data, uint64_t size, int dir, uint64_t *done, uint64_t total)
{
    uint8_t h[512];
    memset(h, 0, sizeof h);
    strlcpy((char *)h, name, 100);
    octal((char *)h + 100, 8, dir ? 0755 : 0644);
    octal((char *)h + 108, 8, 0);
    octal((char *)h + 116, 8, 0);
    octal((char *)h + 124, 12, size);
    octal((char *)h + 136, 12, fs_now());
    memset(h + 148, ' ', 8);
    h[156] = dir ? '5' : '0';
    memcpy(h + 257, "ustar", 6);
    memcpy(h + 263, "00", 2);
    uint32_t sum = 0;
    for (int i = 0; i < 512; i++) sum += h[i];
    octal((char *)h + 148, 7, sum);
    h[155] = ' ';
    put(h, 512);
    const uint8_t *d = data;
    for (uint64_t o = 0; o < size && !failed; o += 1 << 20) {
        uint64_t c = size - o < (1 << 20) ? size - o : (1 << 20);
        put(d + o, c);
        *done += c;
        install_pct = 10 + (int)(*done * 85 / (total ? total : 1));
    }
    if (size % 512) { uint8_t z[512] = { 0 }; put(z, 512 - size % 512); }
}

static int write_mbr(uint8_t *sec0, uint32_t p2_start, uint32_t p2_size, uint32_t p3_start, uint32_t p3_size)
{
    struct { uint8_t status, chs1[3], type, chs2[3]; uint32_t start, size; } __attribute__((packed)) e[4];
    memset(e, 0, sizeof e);
    e[0] = (typeof(e[0])){ 0x00, { 0xFE, 0xFF, 0xFF }, 0xEF, { 0xFE, 0xFF, 0xFF }, 2048, 32768 };
    e[1] = (typeof(e[1])){ 0x80, { 0xFE, 0xFF, 0xFF }, 0x83, { 0xFE, 0xFF, 0xFF }, p2_start, p2_size };
    e[2] = (typeof(e[2])){ 0x00, { 0xFE, 0xFF, 0xFF }, 0xDA, { 0xFE, 0xFF, 0xFF }, p3_start, p3_size };
    uint32_t sig = (uint32_t)rdtsc();
    memcpy(sec0 + 0x1B8, &sig, 4);
    memcpy(sec0 + 446, e, sizeof e);
    sec0[510] = 0x55; sec0[511] = 0xAA;
    return dk->write(dk, 0, 1, sec0);
}

static uint32_t home_start, home_size;     /* sectors, on the installed (or target) disk */
static struct disk *home_disk;

static int write_home(struct disk *d, uint32_t start, uint32_t size)
{
    size_t need = vfs_image_size() + 512;
    if (need > (uint64_t)size * 512) return -1;
    size_t pages = (need + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t pg = pmm_alloc_contig(pages);
    if (!pg) return -1;
    uint8_t *b = (uint8_t *)(uintptr_t)pg;
    memset(b, 0, 512);
    size_t len = vfs_save_image(b + 512, pages * PAGE_SIZE - 512);
    memcpy(b, "AUXOHOME", 8);
    uint64_t l64 = len;
    memcpy(b + 8, &l64, 8);
    int r = d->write(d, start, (uint32_t)((512 + len + 511) / 512), b);
    for (size_t i = 0; i < pages; i++) pmm_free_page(pg + i * PAGE_SIZE);
    return r;
}

static void install_thread(uintptr_t arg)
{
    (void)arg;
    failed = 0;
    dk = disk_get(req.disk);
    const uint8_t *tpl, *kern_elf, *root;
    size_t tpl_len, kern_len, root_len = 0;
    if (!dk) { msg("That disk is gone."); install_state = 3; return; }
    if (boot_module("installer", &tpl, &tpl_len) < 0 || boot_module("kernel", &kern_elf, &kern_len) < 0) {
        msg("This boot of AuxoBSD has no install image (start it from the AuxoBSD ISO).");
        install_state = 3;
        return;
    }
    boot_module("rootfs", &root, &root_len);
    uint64_t pg = pmm_alloc_contig(IOSECT * 512 / PAGE_SIZE);
    if (!pg) { msg("Out of memory."); install_state = 3; return; }
    iobuf = (uint8_t *)(uintptr_t)pg;

    /* sizes: the tar needs headers + contents; leave room to grow */
    uint64_t tar_bytes = 512 * 8 + 4096 + kern_len + root_len + 2 * 512 + 512 * 6;
    uint32_t p2_start = 34816;
    uint32_t p2_size = (uint32_t)(((tar_bytes / 512 + 65536) + 2047) & ~2047ULL);   /* +32 MiB */
    uint32_t p3_start = p2_start + p2_size;
    if (dk->sectors < (uint64_t)p3_start + 65536) { msg("That disk is too small (it needs about 1 GB)."); install_state = 3; return; }
    uint64_t rest = dk->sectors - p3_start;
    uint32_t p3_size = rest > 0xFFFFFFFFULL ? 0xFFFFFFFFu : (uint32_t)rest;

    msg("Writing the boot loader (BIOS and UEFI)…");
    install_pct = 2;
    lba = 0;
    uint8_t sec0[512];
    memcpy(sec0, tpl, 512);
    if (dk->write(dk, 1, (uint32_t)(tpl_len / 512 - 1), tpl + 512) < 0 || write_mbr(sec0, p2_start, p2_size, p3_start, p3_size) < 0) {
        msg("Writing to the disk failed.");
        install_state = 3;
        return;
    }
    install_pct = 10;

    msg("Copying AuxoBSD to the disk…");
    char cfg[1536];
    int n = ksnprintf(cfg, sizeof cfg,
        "# AuxoBSD, installed. Settings go on the multiboot2 line (see auxo-tweak).\n"
        "set timeout=2\nset default=0\ninsmod all_video\n"
        "set menu_color_normal=light-gray/black\nset menu_color_highlight=black/light-magenta\n\n"
        "menuentry \"AuxoBSD " AUXO_VERSION "\" {\n"
        "    set gfxpayload=1920x1080x32,1600x900x32,1366x768x32,1280x800x32,1024x768x32,auto\n"
        "    multiboot2 /boot/auxobsd.elf accent=%s hostname=%s installed\n"
        "    module2 /boot/rootfs.tar rootfs\n    boot\n}\n\n"
        "menuentry \"AuxoBSD " AUXO_VERSION " (text console)\" {\n"
        "    set gfxpayload=text\n"
        "    multiboot2 /boot/auxobsd.elf accent=%s hostname=%s installed\n"
        "    module2 /boot/rootfs.tar rootfs\n    boot\n}\n\n"
        "menuentry \"Reboot\" { reboot }\nmenuentry \"Power off\" { halt }\n",
        auxo_accents[req.accent].name, req.hostname, auxo_accents[req.accent].name, req.hostname);
    lba = p2_start;
    fill = 0;
    uint64_t done = 0, total = kern_len + root_len;
    tar_entry("boot/", 0, 0, 1, &done, total);
    tar_entry("boot/grub/", 0, 0, 1, &done, total);
    tar_entry("boot/grub/grub.cfg", cfg, (uint64_t)n, 0, &done, total);
    tar_entry("boot/auxobsd.elf", kern_elf, kern_len, 0, &done, total);
    if (root_len) tar_entry("boot/rootfs.tar", root, root_len, 0, &done, total);
    { uint8_t z[1024] = { 0 }; put(z, sizeof z); }
    flush();
    if (failed) { msg("Writing to the disk failed."); install_state = 3; return; }

    msg("Saving your files…");
    install_pct = 96;
    if (write_home(dk, p3_start, p3_size) < 0) { msg("Couldn't save /home/root to the disk."); install_state = 3; return; }
    for (size_t i = 0; i < IOSECT * 512 / PAGE_SIZE; i++) pmm_free_page(pg + i * PAGE_SIZE);
    install_pct = 100;
    ksnprintf(install_msg, sizeof install_msg, "AuxoBSD is installed on %s (%s). Take out the install media and restart.", dk->name, dk->model);
    install_state = 2;
}

int install_start(const struct install_req *r)
{
    if (install_state == 1) return -1;
    req = *r;
    install_state = 1;
    install_pct = 0;
    msg("Starting…");
    return rs_spawn("installer", install_thread, 0) < 0 ? -1 : 0;
}

/* ── the installed system: /home/root lives in partition 3 ──────────── */
static uint32_t saved_gen;

int store_save(void)
{
    if (!home_disk) return -1;
    int r = write_home(home_disk, home_start, home_size);
    if (r == 0) saved_gen = vfs_dirty();
    return r;
}

static void store_thread(uintptr_t arg)
{
    (void)arg;
    for (;;) {
        rs_sleep(10 * AUXO_HZ);                              /* every 10 s, if something changed */
        if (vfs_dirty() != saved_gen) store_save();
    }
}

int store_init(void)
{
    uint8_t *s = rs_kmalloc(512);
    if (!s) return -1;
    for (int i = 0; i < disk_count(); i++) {
        struct disk *d = disk_get(i);
        if (d->read(d, 0, 1, s) < 0 || s[510] != 0x55 || s[511] != 0xAA) continue;
        for (int p = 0; p < 4; p++) {
            const uint8_t *e = s + 446 + p * 16;
            if (e[4] != 0xDA) continue;
            uint32_t start, size;
            memcpy(&start, e + 8, 4); memcpy(&size, e + 12, 4);
            uint8_t h[512];
            if (d->read(d, start, 1, h) < 0 || memcmp(h, "AUXOHOME", 8)) continue;
            uint64_t len;
            memcpy(&len, h + 8, 8);
            if (len > (uint64_t)size * 512) continue;
            size_t pages = (size_t)((len + 512 + PAGE_SIZE - 1) / PAGE_SIZE);
            uint64_t pg = pmm_alloc_contig(pages);
            if (!pg) break;
            uint8_t *b = (uint8_t *)(uintptr_t)pg;
            if (d->read(d, start, (uint32_t)((512 + len + 511) / 512), b) == 0) vfs_load_image(b + 512, (size_t)len);
            for (size_t k = 0; k < pages; k++) pmm_free_page(pg + k * PAGE_SIZE);
            home_disk = d; home_start = start; home_size = size;
            d->boot = 1;
            saved_gen = vfs_dirty();
            rs_kfree(s);
            rs_spawn("store", store_thread, 0);
            return 0;
        }
    }
    rs_kfree(s);
    return -1;
}

const char *store_where(void) { return home_disk ? home_disk->name : 0; }
