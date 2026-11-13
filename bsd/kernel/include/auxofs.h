#ifndef AUXO_AUXOFS_H
#define AUXO_AUXOFS_H
/* AuxoFS (kern/auxofs.c): the installed system's root filesystem, on its disk. */
#include <stdint.h>
#include <stddef.h>
struct disk;

int         auxofs_mount_root(void);                 /* at boot: find it (MBR type 0xDA) and mount it as / */
int         auxofs_mount(struct disk *d, uint64_t base_sector, uint64_t sectors);
int         auxofs_probe(struct disk *d, uint64_t base_sector);
int         auxofs_format_copy(struct disk *d, uint64_t base_sector, uint64_t sectors, void (*progress)(int pct));
int         auxofs_commit(int checkpoint);           /* what changed, to the disk now (sync) */
size_t      auxofs_reclaim(size_t pages);            /* drop files nobody's used lately; pages freed */
int         auxofs_mounted(void);
const char *auxofs_disk(void);
uint64_t    auxofs_free_blocks(void);                /* 4 KiB blocks */
uint64_t    auxofs_total_blocks(void);
void        auxofs_stats(uint64_t *loads, uint64_t *evictions, uint64_t *commits, uint64_t *checkpoints);

#endif
