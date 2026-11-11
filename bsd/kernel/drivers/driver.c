/* Driver registry: PCI drivers register themselves in the .drivers section
 * (PCI_DRIVER); only drivers selected in menuconfig are compiled in. */
#include "kern.h"
#include "arch.h"
#include "pci.h"
#include "drivers.h"

extern const struct pci_driver __drivers_start[], __drivers_end[];

#ifdef CONFIG_PCI
static int match(const struct pci_dev *d, const struct pci_match *m)
{
    if (m->vendor != PCI_ANY && m->vendor != d->vendor) return 0;
    if (m->device != PCI_ANY && m->device != d->device) return 0;
    if (m->class != 0xFF) {
        if (m->class != d->class) return 0;
        if (m->subclass != 0xFF && m->subclass != d->subclass) return 0;
        if (m->prog_if != 0xFF && m->prog_if != d->prog_if) return 0;
    }
    return 1;
}

static void probe_pci(void)
{
    pci_scan();
    kprintf("pci : %d device(s)\n", pci_count());
    for (int i = 0; i < pci_count(); i++) {
        struct pci_dev *d = pci_get(i);
        for (const struct pci_driver *drv = __drivers_start; drv < __drivers_end && !d->driver; drv++)
            for (const struct pci_match *m = drv->ids; m->name; m++) {
                if (!match(d, m)) continue;
                d->model = m->name;
                if (drv->probe(d, m) == 0) {
                    d->driver = drv;
                    kprintf("%02x:%02x.%x bound: %s -> %s\n", d->bus, d->dev, d->fn, drv->name, m->name);
                }
                break;
            }
    }
}
#endif

void driver_init_all(void)
{
#ifdef CONFIG_DRV_ATA_PIO
    ata_init();
#endif
#ifdef CONFIG_PCI
    probe_pci();
#endif
}

void driver_list(void)
{
    kprintf("%-12s %-12s bound devices\n", "DRIVER", "STATUS");
    for (const struct pci_driver *drv = __drivers_start; drv < __drivers_end; drv++) {
        kprintf("%-12s %-12s", drv->name, drv->status);
        int n = 0;
        for (int i = 0; i < pci_count(); i++) {
            const struct pci_dev *d = pci_get(i);
            if (d->driver == drv) { kprintf(" %02x:%02x.%x", d->bus, d->dev, d->fn); n++; }
        }
        if (!n) kprintf(" -");
        kprintf("\n");
    }
#ifdef CONFIG_DRV_ATA_PIO
    kprintf("%-12s %-12s %s\n", "ata-pio", "functional", ata_present() ? "primary master" : "no disk");
#endif
}
