#ifndef AUXO_VFS_H
#define AUXO_VFS_H
/* A small in-memory filesystem: absolute paths, files only (directories are implied by the
 * paths). Saved to and loaded from disk as one image when AuxoBSD is installed (disk/store.c). */
#include <stddef.h>
#include <stdint.h>

#define VFS_MAX   256
#define VFS_PATH  128

void        vfs_init(void);                        /* creates the starter files in /home/root */
int         vfs_read(const char *path, const char **data, size_t *len);   /* 0 = ok; data stays valid until the next write */
int         vfs_write(const char *path, const void *data, size_t len);
int         vfs_remove(const char *path);
int         vfs_exists(const char *path);
int         vfs_count(void);
const char *vfs_path(int i);                       /* NULL past the end; sorted by path */
size_t      vfs_size(int i);
uint32_t    vfs_dirty(void);                       /* bumped on every change */

/* the whole filesystem as one blob, for disk/store.c */
size_t      vfs_image_size(void);
size_t      vfs_save_image(uint8_t *out, size_t cap);
int         vfs_load_image(const uint8_t *in, size_t len);

#endif
