/* Identification-only PCI drivers: they claim matching devices, print BAR/link
 * details and mark the device as seen. They do not program the hardware. Each is
 * a separate menuconfig option so a real driver can later replace it.          */
#include "kern.h"
#include "pci.h"

#ifdef CONFIG_PCI
static int report(struct pci_dev *d, const struct pci_match *m)
{
    kprintf("  %s [%04x:%04x rev %02x]\n", m->name, d->vendor, d->device, d->rev);
    pci_print_bars(d);
    if (pci_find_cap(d, 0x10)) pci_print_pcie_link(d);
    return 0;
}
#endif

#ifdef CONFIG_DRV_AHCI
static const struct pci_match ahci_ids[] = { PCI_CLASS(0x01, 0x06, 0x01, "AHCI SATA controller"), PCI_END };
PCI_DRIVER(ahci_drv) = { "ahci", "probe-only", ahci_ids, report };
#endif
#ifdef CONFIG_DRV_NVME
static const struct pci_match nvme_ids[] = { PCI_CLASS(0x01, 0x08, 0x02, "NVMe controller"), PCI_END };
PCI_DRIVER(nvme_drv) = { "nvme", "probe-only", nvme_ids, report };
#endif
#ifdef CONFIG_DRV_XHCI
static const struct pci_match xhci_ids[] = { PCI_CLASS(0x0C, 0x03, 0x30, "xHCI USB 3 host controller"), PCI_END };
PCI_DRIVER(xhci_drv) = { "xhci", "probe-only", xhci_ids, report };
#endif
#ifdef CONFIG_DRV_EHCI
static const struct pci_match ehci_ids[] = { PCI_CLASS(0x0C, 0x03, 0x20, "EHCI USB 2 host controller"), PCI_END };
PCI_DRIVER(ehci_drv) = { "ehci", "probe-only", ehci_ids, report };
#endif
#ifdef CONFIG_DRV_HDA
static const struct pci_match hda_ids[] = { PCI_CLASS(0x04, 0x03, 0xFF, "Intel HD Audio controller"), PCI_END };
PCI_DRIVER(hda_drv) = { "hda", "probe-only", hda_ids, report };
#endif
#ifdef CONFIG_DRV_NVIDIA
static const struct pci_match nv_ids[] = { PCI_VCLASS(0x10DE, 0x03, "NVIDIA display controller"), PCI_END };
PCI_DRIVER(nvidia_drv) = { "nvidia", "probe-only", nv_ids, report };
#endif
#ifdef CONFIG_DRV_INTEL_GFX
static const struct pci_match igfx_ids[] = { PCI_VCLASS(0x8086, 0x03, "Intel integrated graphics"), PCI_END };
PCI_DRIVER(intel_gfx_drv) = { "intel-gfx", "probe-only", igfx_ids, report };
#endif
#ifdef CONFIG_DRV_VIRTIO_NET
static const struct pci_match vnet_ids[] = {
    PCI_ID(0x1AF4, 0x1000, "virtio-net (transitional)"), PCI_ID(0x1AF4, 0x1041, "virtio-net"), PCI_END };
PCI_DRIVER(virtio_net_drv) = { "virtio-net", "probe-only", vnet_ids, report };
#endif
#ifdef CONFIG_DRV_VIRTIO_BLK
static const struct pci_match vblk_ids[] = {
    PCI_ID(0x1AF4, 0x1001, "virtio-blk (transitional)"), PCI_ID(0x1AF4, 0x1042, "virtio-blk"), PCI_END };
PCI_DRIVER(virtio_blk_drv) = { "virtio-blk", "probe-only", vblk_ids, report };
#endif
#ifdef CONFIG_DRV_RTL8168
static const struct pci_match r8168_ids[] = { PCI_ID(0x10EC, 0x8168, "RTL8111/8168 PCIe Gigabit Ethernet"), PCI_END };
PCI_DRIVER(rtl8168_drv) = { "rtl8168", "probe-only", r8168_ids, report };
#endif
#ifdef CONFIG_DRV_RTL8821AE
static const struct pci_match r8821_ids[] = {
    PCI_ID(0x10EC, 0x8821, "RTL8821AE 802.11ac"), PCI_ID(0x10EC, 0x8179, "RTL8188EE 802.11n"), PCI_END };
PCI_DRIVER(rtl8821ae_drv) = { "rtl8821ae", "probe-only", r8821_ids, report };
#endif
