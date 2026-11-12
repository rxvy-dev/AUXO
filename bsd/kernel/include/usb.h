#ifndef AUXO_USB_H
#define AUXO_USB_H
/* USB through xHCI (usb/xhci.c): HID keyboards and mice, USB 2 hubs, hot-plug. */
#include <stddef.h>
int usb_init(void);                         /* controllers started (the "usb" thread enumerates) */
int usb_controllers(void);
void usb_wait_settled(int ms);              /* the first scan of every port is done */
int usb_keyboards(void);
int usb_mice(void);
int usb_list(char *out, size_t n);          /* lsusb lines; returns the device count */

/* what the USB thread feeds into */
void input_scancode(unsigned char sc);      /* a PS/2 set-1 scancode, as if from the keyboard */
void mouse_push(int dx, int dy, int wheel, int buttons);
void mouse_usb_attached(void);
#endif
