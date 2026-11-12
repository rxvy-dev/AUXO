#ifndef AUXO_FS_H
#define AUXO_FS_H
/* The AuxoBSD filesystem: one tree in memory. The root filesystem image (a tar archive the boot
 * loader passes as a module) is mounted without copying; files are copied into owned pages
 * the first time they're written. /dev has the usual character devices. */
#include <stdint.h>
#include <stddef.h>

enum { N_DIR = 1, N_REG, N_LNK, N_CHR, N_FIFO, N_SOCK };
enum { DEV_NULL = 1, DEV_ZERO, DEV_TTY, DEV_RANDOM, DEV_FB, DEV_FULL, DEV_SND_CTL, DEV_SND_PCM };

struct node {
    char    *name;
    uint8_t  type;
    uint16_t mode;                 /* permission bits */
    uint32_t uid, gid;             /* owner and group */
    uint32_t ino, nlink, dev;
    struct node *parent, *child, *next;
    uint8_t *data;                 /* file contents / symlink target */
    uint64_t size, cap;            /* cap > 0: data is ours (whole pages); 0: borrowed (rootfs image) */
    uint32_t mtime;
    int      refs;                 /* open files + mappings; unlinked nodes are freed at 0 */
    int      maps;                 /* shared mappings of its pages: they must never move */
    int      unlinked;
    void    *priv;                 /* pipes, sockets */
    uint8_t  changed;              /* made or changed since boot (not from a boot image): kept on disk */
    uint32_t wgen;                 /* generation of its last change */
};

void         fs_init(void);                   /* empty tree + /dev, /tmp, /home/root … */
int          fs_mount_tar(const uint8_t *img, size_t len);   /* number of entries, <0 on error */
struct node *fs_root(void);
void         fs_lock(void);
void         fs_unlock(void);

/* lookups: path relative to cwd unless absolute. follow = follow a final symlink. */
int          fs_lookup(struct node *cwd, const char *path, int follow, struct node **out);   /* 0 or -errno */
int          fs_lookup_parent(struct node *cwd, const char *path, struct node **dir, char *name, size_t n);
struct node *fs_child(struct node *dir, const char *name);
struct node *fs_child_n(struct node *dir, const char *name, size_t n);
struct node *fs_new_root(uint16_t mode);            /* a tmpfs: a directory in no tree */
struct node *fs_create(struct node *dir, const char *name, int type, uint16_t mode);
int          fs_unlink(struct node *dir, const char *name, int rmdir);
int          fs_rename(struct node *odir, const char *oname, struct node *ndir, const char *nname);
int          fs_link(struct node *target, struct node *dir, const char *name);

/* contents */
long         fs_read(struct node *n, uint64_t off, void *buf, size_t len);
long         fs_write(struct node *n, uint64_t off, const void *buf, size_t len);
int          fs_truncate(struct node *n, uint64_t len);
int          fs_reserve(struct node *n, uint64_t cap);          /* make data owned, contiguous, >= cap */
void         fs_path(struct node *n, char *buf, size_t len);   /* absolute path of a node */
uint32_t     fs_now(void);
void         fs_put(struct node *n);                            /* drop a reference */
uint32_t     fs_generation(void);                              /* bumped on any change (for apps) */
void         fs_touch(struct node *n);                          /* mark changed (chmod and the like) */

#endif
