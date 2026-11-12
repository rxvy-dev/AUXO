#ifndef AUXO_USBDEV_H
#define AUXO_USBDEV_H
/* What the xHCI driver (xhci.c) offers USB class drivers: storage (msc.c), network (usbnet.c). */
#include <stdint.h>
#include <stddef.h>

struct usbd;                                 /* one USB device */
struct usb_ep { uint8_t addr, attr; uint16_t mps; uint8_t interval, burst; };

int  usbd_configure(struct usbd *d, const struct usb_ep *eps, int n);     /* open endpoints */
int  usbd_control(struct usbd *d, uint8_t rt, uint8_t req, uint16_t val, uint16_t idx, void *data, uint16_t len);
/* bulk/interrupt transfer; buf is kernel memory (identity mapped). bytes, or -1 timeout,
 * -2 stalled (the device's endpoint needs usbd_clear_halt), -3 error, -4 device gone */
int  usbd_xfer(struct usbd *d, uint8_t ep, void *buf, uint32_t len, int timeout_ms);
int  usbd_clear_halt(struct usbd *d, uint8_t ep);
const char *usbd_name(struct usbd *d);
int  usbd_speed(struct usbd *d);                                          /* 1 FS 2 LS 3 HS 4+ SS */
void usbd_set_driver(struct usbd *d, void *drv, void (*detach)(void *drv), const char *kind);

/* class drivers: claim the device (return 1) from its configuration descriptor */
int  msc_probe(struct usbd *d, const uint8_t *cfg, int len);
int  usbnet_probe(struct usbd *d, const uint8_t *cfg, int len);
#endif
