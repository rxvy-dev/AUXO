/* CMOS real-time clock (BCD or binary, 12 or 24 hour). */
#include "kern.h"
#include "arch.h"

static uint8_t cmos(uint8_t r) { outb(0x70, r); return inb(0x71); }

void rtc_read(struct rtc_time *t)
{
    for (int i = 0; i < 1000 && (cmos(0x0A) & 0x80); i++)     /* wait out an update in progress */
        ;
    uint8_t s = cmos(0), m = cmos(2), h = cmos(4), d = cmos(7), mo = cmos(8), y = cmos(9), b = cmos(0x0B);
    int pm = h & 0x80;
    h &= 0x7F;
    if (!(b & 4)) {                                  /* BCD */
#define BCD(x) (((x) & 15) + ((x) >> 4) * 10)
        s = (uint8_t)BCD(s); m = (uint8_t)BCD(m); h = (uint8_t)BCD(h); d = (uint8_t)BCD(d);
        mo = (uint8_t)BCD(mo); y = (uint8_t)BCD(y);
    }
    if (!(b & 2) && pm) h = (uint8_t)((h % 12) + 12);  /* 12-hour clock */
    t->second = s; t->minute = m; t->hour = h; t->day = d; t->month = mo; t->year = 2000 + y;
}
