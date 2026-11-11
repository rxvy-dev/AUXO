/* In-memory filesystem (see vfs.h). File data lives in whole pages from the PMM. */
#include "kern.h"
#include "arch.h"
#include "rs.h"
#include "vfs.h"
#include "aether_data.h"

struct file { char path[VFS_PATH]; uint8_t *data; size_t len, cap; };
static struct file files[VFS_MAX];
static int nfiles;
static volatile int lock;
static volatile uint32_t dirty;

static void vlock(void) { while (__atomic_exchange_n(&lock, 1, __ATOMIC_ACQUIRE)) rs_yield(); }
static void vunlock(void) { __atomic_store_n(&lock, 0, __ATOMIC_RELEASE); }

static int find(const char *p)
{
    for (int i = 0; i < nfiles; i++) if (!strcmp(files[i].path, p)) return i;
    return -1;
}

static void release(struct file *f)
{
    for (size_t o = 0; o < f->cap; o += PAGE_SIZE) pmm_free_page((uint64_t)(uintptr_t)f->data + o);
    f->data = 0; f->cap = 0;
}

int vfs_read(const char *path, const char **data, size_t *len)
{
    vlock();
    int i = find(path);
    if (i >= 0) { *data = (const char *)files[i].data; *len = files[i].len; }
    vunlock();
    return i < 0 ? -1 : 0;
}

int vfs_exists(const char *path) { vlock(); int i = find(path); vunlock(); return i >= 0; }

int vfs_write(const char *path, const void *data, size_t len)
{
    if (path[0] != '/' || strlen(path) >= VFS_PATH) return -1;
    vlock();
    int i = find(path);
    if (i < 0) {
        if (nfiles >= VFS_MAX) { vunlock(); return -1; }
        /* keep the table sorted by path */
        i = nfiles;
        while (i > 0 && strcmp(files[i - 1].path, path) > 0) { files[i] = files[i - 1]; i--; }
        memset(&files[i], 0, sizeof files[i]);
        strlcpy(files[i].path, path, VFS_PATH);
        nfiles++;
    }
    struct file *f = &files[i];
    size_t need = (len + 1 + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (need > f->cap) {
        uint64_t pg = pmm_alloc_contig(need / PAGE_SIZE);
        if (!pg) { vunlock(); return -1; }
        release(f);
        f->data = (uint8_t *)(uintptr_t)pg;
        f->cap = need;
    }
    memmove(f->data, data, len);
    f->data[len] = 0;                       /* text files can be used as C strings */
    f->len = len;
    dirty++;
    vunlock();
    return 0;
}

int vfs_remove(const char *path)
{
    vlock();
    int i = find(path);
    if (i >= 0) {
        release(&files[i]);
        for (int k = i; k + 1 < nfiles; k++) files[k] = files[k + 1];
        nfiles--;
        dirty++;
    }
    vunlock();
    return i < 0 ? -1 : 0;
}

int vfs_count(void) { return nfiles; }
const char *vfs_path(int i) { return i >= 0 && i < nfiles ? files[i].path : 0; }
size_t vfs_size(int i) { return i >= 0 && i < nfiles ? files[i].len : 0; }
uint32_t vfs_dirty(void) { return dirty; }

/* image: "AUXOFS1\0", u32 count, then per file: u16 path length, path, u32 length, data */
size_t vfs_image_size(void)
{
    size_t n = 12;
    for (int i = 0; i < nfiles; i++) n += 2 + strlen(files[i].path) + 4 + files[i].len;
    return n;
}

size_t vfs_save_image(uint8_t *out, size_t cap)
{
    vlock();
    size_t need = vfs_image_size();
    if (need > cap) { vunlock(); return 0; }
    memcpy(out, "AUXOFS1", 8);
    uint32_t c = (uint32_t)nfiles;
    memcpy(out + 8, &c, 4);
    size_t o = 12;
    for (int i = 0; i < nfiles; i++) {
        uint16_t pl = (uint16_t)strlen(files[i].path);
        uint32_t dl = (uint32_t)files[i].len;
        memcpy(out + o, &pl, 2); o += 2;
        memcpy(out + o, files[i].path, pl); o += pl;
        memcpy(out + o, &dl, 4); o += 4;
        memcpy(out + o, files[i].data, dl); o += dl;
    }
    vunlock();
    return o;
}

int vfs_load_image(const uint8_t *in, size_t len)
{
    if (len < 12 || memcmp(in, "AUXOFS1", 8)) return -1;
    uint32_t c;
    memcpy(&c, in + 8, 4);
    size_t o = 12;
    for (uint32_t k = 0; k < c; k++) {
        uint16_t pl; uint32_t dl;
        char path[VFS_PATH];
        if (o + 2 > len) return -1;
        memcpy(&pl, in + o, 2); o += 2;
        if (pl >= VFS_PATH || o + pl + 4 > len) return -1;
        memcpy(path, in + o, pl); path[pl] = 0; o += pl;
        memcpy(&dl, in + o, 4); o += 4;
        if (o + dl > len) return -1;
        vfs_write(path, in + o, dl);
        o += dl;
    }
    return 0;
}

static const char README[] =
    "Welcome to AuxoBSD.\n\n"
    "This is your home folder, /home/root. Open files here in Aether (the code editor),\n"
    "or in the Terminal with `cat`. Aether's Preview button opens HTML files in the Browser.\n\n"
    "Until AuxoBSD is installed (Install AuxoBSD in the app menu), files live in memory and are\n"
    "gone after a restart. On an installed system they are saved to the disk.\n";

static const char INDEX[] =
    "<!doctype html>\n<html>\n<head>\n  <title>My first page</title>\n</head>\n<body>\n"
    "  <h1>Hello from AuxoBSD</h1>\n"
    "  <p>Edit me in <b>Aether</b>, press <code>Ctrl+S</code>, then <b>Preview</b>.</p>\n"
    "  <ul>\n    <li>No JavaScript here (yet)</li>\n    <li>Links work: <a href=\"https://auxolinux.com\">auxolinux.com</a></li>\n  </ul>\n"
    "</body>\n</html>\n";

void vfs_init(void)
{
    vfs_write("/home/root/README.txt", README, sizeof README - 1);
    vfs_write("/home/root/index.html", INDEX, sizeof INDEX - 1);
    for (int i = 0; i < AETHER_LANG_COUNT; i++)
        if (!strcmp(aether_langs[i].id, "c")) {
            vfs_write("/home/root/hello.c", aether_langs[i].main, strlen(aether_langs[i].main));
            break;
        }
    dirty = 0;
}
