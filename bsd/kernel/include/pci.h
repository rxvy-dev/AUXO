#ifndef AUXO_PCI_H
#define AUXO_PCI_H
/* PCI: configuration space through ports 0xCF8/0xCFC, a scan of every bus, and a small
 * name table so `lspci`, auxo-fetch and the desktop can say what the hardware is. */
#include <stdint.h>

struct pci_dev {
    uint8_t  bus, dev, fn, rev;
    uint16_t vendor, device;
    uint8_t  class, subclass, prog_if, irq;
    uint64_t bar[6];          /* base addresses, flags masked off; 64-bit BARs combined */
    uint64_t barsize[6];
    uint8_t  bario[6];        /* 1 = I/O port BAR */
    const char *driver;       /* set by whichever driver claimed it */
};

void        pci_init(void);
int         pci_count(void);
struct pci_dev *pci_get(int i);
struct pci_dev *pci_find_class(int class, int subclass, int start);   /* -1 subclass = any */
uint32_t    pci_read32(const struct pci_dev *d, uint8_t off);
void        pci_write32(const struct pci_dev *d, uint8_t off, uint32_t v);
uint16_t    pci_read16(const struct pci_dev *d, uint8_t off);
void        pci_write16(const struct pci_dev *d, uint8_t off, uint16_t v);
void        pci_enable(struct pci_dev *d, int bus_master);              /* memory + I/O decode on */
const char *pci_vendor_name(uint16_t vendor);
const char *pci_class_name(uint8_t class, uint8_t subclass);
void        pci_describe(const struct pci_dev *d, char *buf, int n);   /* "NVIDIA GeForce RTX ..." best effort */

/* hardware summary used by auxo-fetch, About and the installer */
const struct pci_dev *pci_gpu(void);        /* the display controller GRUB's framebuffer is on */
const struct pci_dev *pci_wifi(void);       /* a network controller of subclass 0x80 (wireless) */

#endif
