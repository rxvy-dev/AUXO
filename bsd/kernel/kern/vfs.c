/* In-memory filesystem (see vfs.h). File data lives in whole pages from the PMM. */
#include "kern.h"
#include "arch.h"
#include "rs.h"
#include "vfs.h"
#include "fs.h"
#include "aether_data.h"

/* the apps' view: every regular file under /home/root, sorted, rebuilt when the tree changes */
static char (*paths)[VFS_PATH];
static size_t *sizes;
static int npaths, pcap;
static uint32_t built_gen = 0xFFFFFFFF;

static void collect(struct node *d, char *prefix)
{
    size_t pl = strlen(prefix);
    for (struct node *c = d->child; c; c = c->next) {
        if (pl + strlen(c->name) + 2 >= VFS_PATH) continue;
        ksnprintf(prefix + pl, VFS_PATH - pl, "/%s", c->name);
        if (c->type == N_DIR) collect(c, prefix);
        else if (c->type == N_REG && npaths < pcap) {
            /* insertion sort: the tree keeps newest first */
            int i = npaths++;
            while (i > 0 && strcmp(paths[i - 1], prefix) > 0) { memcpy(paths[i], paths[i - 1], VFS_PATH); sizes[i] = sizes[i - 1]; i--; }
            strlcpy(paths[i], prefix, VFS_PATH);
            sizes[i] = (size_t)c->size;
        }
        prefix[pl] = 0;
    }
}

static void rebuild(void)
{
    if (built_gen == fs_generation()) return;
    if (!paths) {
        pcap = VFS_MAX;
        paths = rs_kmalloc((size_t)pcap * VFS_PATH);
        sizes = rs_kmalloc((size_t)pcap * sizeof *sizes);
        if (!paths || !sizes) { pcap = 0; return; }
    }
    fs_lock();
    npaths = 0;
    struct node *home;
    if (fs_lookup(0, "/home/root", 1, &home) == 0) { char pre[VFS_PATH] = "/home/root"; collect(home, pre); }
    built_gen = fs_generation();
    fs_unlock();
}

int vfs_read(const char *path, const char **data, size_t *len)
{
    struct node *n;
    if (fs_lookup(0, path, 1, &n) < 0 || n->type != N_REG) return -1;
    *data = (const char *)n->data;
    *len = (size_t)n->size;
    if (!n->data) *data = "";
    return 0;
}

int vfs_exists(const char *path) { struct node *n; return fs_lookup(0, path, 1, &n) == 0; }

static int mkparents(const char *path)
{
    char dir[VFS_PATH];
    strlcpy(dir, path, sizeof dir);
    for (char *p = dir + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        struct node *d, *parent; char name[VFS_PATH];
        if (fs_lookup(0, dir, 1, &d) < 0) {
            if (fs_lookup_parent(0, dir, &parent, name, sizeof name) < 0) return -1;
            if (!fs_create(parent, name, N_DIR, 0755)) return -1;
        }
        *p = '/';
    }
    return 0;
}

int vfs_write(const char *path, const void *data, size_t len)
{
    if (path[0] != '/' || strlen(path) >= VFS_PATH) return -1;
    if (mkparents(path) < 0) return -1;
    struct node *dir, *n;
    char name[VFS_PATH];
    if (fs_lookup_parent(0, path, &dir, name, sizeof name) < 0) return -1;
    n = fs_create(dir, name, N_REG, 0644);
    if (!n || n->type != N_REG) return -1;
    fs_truncate(n, 0);
    if (len && fs_write(n, 0, data, len) < 0) return -1;
    if (!len) fs_reserve(n, 1);
    return 0;
}

int vfs_remove(const char *path)
{
    struct node *dir; char name[VFS_PATH];
    if (fs_lookup_parent(0, path, &dir, name, sizeof name) < 0) return -1;
    return fs_unlink(dir, name, 0) < 0 ? -1 : 0;
}

int vfs_count(void) { rebuild(); return npaths; }
const char *vfs_path(int i) { rebuild(); return i >= 0 && i < npaths ? paths[i] : 0; }
size_t vfs_size(int i) { rebuild(); return i >= 0 && i < npaths ? sizes[i] : 0; }
uint32_t vfs_dirty(void) { return fs_generation(); }

/* image of /home/root for the installed system's disk: "AUXOFS1\0", u32 count, then per
 * file: u16 path length, path, u32 length, data */
size_t vfs_image_size(void)
{
    rebuild();
    size_t n = 12;
    for (int i = 0; i < npaths; i++) n += 2 + strlen(paths[i]) + 4 + sizes[i];
    return n;
}

size_t vfs_save_image(uint8_t *out, size_t cap)
{
    rebuild();
    if (vfs_image_size() > cap) return 0;
    memcpy(out, "AUXOFS1", 8);
    uint32_t c = (uint32_t)npaths;
    memcpy(out + 8, &c, 4);
    size_t o = 12;
    for (int i = 0; i < npaths; i++) {
        const char *d; size_t dl;
        if (vfs_read(paths[i], &d, &dl) < 0) { d = ""; dl = 0; }
        uint16_t pl = (uint16_t)strlen(paths[i]);
        uint32_t l32 = (uint32_t)dl;
        memcpy(out + o, &pl, 2); o += 2;
        memcpy(out + o, paths[i], pl); o += pl;
        memcpy(out + o, &l32, 4); o += 4;
        memcpy(out + o, d, dl); o += dl;
    }
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
}
