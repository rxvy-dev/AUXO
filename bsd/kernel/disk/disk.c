/* the list of disks the drivers found */
#include "kern.h"
#include "disk.h"

#define MAXDISK 16
static struct disk *disks[MAXDISK];
static int ndisks;

int disk_add(struct disk *d)
{
    if (ndisks >= MAXDISK) return -1;
    disks[ndisks++] = d;
    return 0;
}

int disk_count(void) { return ndisks; }
struct disk *disk_get(int i) { return i >= 0 && i < ndisks ? disks[i] : 0; }

void disk_init(void)
{
    ata_probe();
    ahci_probe();
    nvme_probe();
}
