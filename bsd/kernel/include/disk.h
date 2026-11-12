#ifndef AUXO_DISK_H
#define AUXO_DISK_H
/* Disks: IDE (ATA PIO), SATA (AHCI) and NVMe drivers behind one block interface.
 * Sectors are 512 bytes; buffers are kernel memory below 4 GiB (identity mapped). USB storage
 * (usb/msc.c) adds and removes disks while running. */
#include <stdint.h>
#include <stddef.h>

struct disk {
    char     name[8];                 /* "sda", "nvme0" … */
    char     model[48];
    char     bus[8];                  /* "IDE", "SATA", "NVMe" */
    uint64_t sectors;
    int (*read)(struct disk *d, uint64_t lba, uint32_t count, void *buf);
    int (*write)(struct disk *d, uint64_t lba, uint32_t count, const void *buf);
    void *priv;
    int  removable, boot;             /* boot: AuxoBSD booted from here (installed system) */
    volatile int gone;                /* unplugged (USB): reads and writes fail */
};

void         disk_init(void);           /* probe every controller */
int          disk_count(void);
struct disk *disk_get(int i);
int          disk_add(struct disk *d);
int          ata_probe(void);
int          ahci_probe(void);
int          nvme_probe(void);

#endif
