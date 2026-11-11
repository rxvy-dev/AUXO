#ifndef AUXO_DRIVERS_H
#define AUXO_DRIVERS_H
#include <stdint.h>

#ifdef CONFIG_DRV_ATA_PIO
int         ata_init(void);
int         ata_present(void);
const char *ata_model(void);
uint32_t    ata_sectors(void);
int         ata_read(uint32_t lba, uint32_t count, void *buf);   /* LBA28, 0 = ok */
#endif

#ifdef CONFIG_DRV_BOCHS_VGA
int  bochs_fb_present(void);
int  bochs_fb_enable(int w, int h);       /* switches the display to a 32bpp framebuffer */
void bochs_fb_disable(void);              /* back to VGA text mode */
void bochs_fb_test(void);
#endif

#ifdef CONFIG_DRV_RTL8139
int  rtl8139_present(void);
void rtl8139_info(void);
int  rtl8139_send_test(void);             /* raw broadcast frame, ethertype 0x88B5 */
int  rtl8139_poll(void);                  /* returns frames drained from the RX ring */
#endif

#endif
