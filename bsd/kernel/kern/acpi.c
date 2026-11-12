/* ACPI, just enough to switch the machine off and restart it on real hardware: the FADT gives
 * the PM1 control registers and the reset register, and the DSDT's \_S5 package gives the value
 * that means "soft off". No AML interpreter: \_S5 is a constant package on practically every
 * machine, so it's read straight out of the bytecode. */
#include "kern.h"
#include "arch.h"
#include "acpi.h"

struct sdt { char sig[4]; uint32_t len; uint8_t rev, sum; char oem[6], oemtab[8]; uint32_t oemrev, creator, crev; } __attribute__((packed));
struct gas { uint8_t space, width, offset, size; uint64_t addr; } __attribute__((packed));

static uint8_t rsdp[36];
static int have_rsdp;
static uint32_t pm1a, pm1b, smi_cmd;
static uint8_t acpi_enable_val;
static uint16_t slp_a, slp_b;
static int have_s5;
static struct gas reset_reg;
static uint8_t reset_val;
static int have_reset;
static char summary[96] = "no ACPI tables";

void acpi_set_rsdp(const void *p, size_t len)
{
    if (len > sizeof rsdp) len = sizeof rsdp;
    memcpy(rsdp, p, len);
    have_rsdp = !memcmp(rsdp, "RSD PTR ", 8);
}

static const void *phys(uint64_t a, size_t len)
{
    return vm_map_mmio(a, len, 0);
}

static int checksum_ok(const void *p, size_t n)
{
    uint8_t s = 0;
    for (size_t i = 0; i < n; i++) s = (uint8_t)(s + ((const uint8_t *)p)[i]);
    return s == 0;
}

static void scan_bios(void)
{
    /* BIOS machines: the RSDP is on a 16-byte boundary in the EBDA or at E0000-FFFFF */
    uintptr_t bda = 0x40E;                                              /* BIOS data area: EBDA segment */
    __asm__("" : "+r"(bda));
    uint32_t ebda = (uint32_t)(*(volatile const uint16_t *)bda) << 4;
    uint32_t ranges[2][2] = { { ebda, ebda + 1024 }, { 0xE0000, 0x100000 } };
    for (int r = 0; r < 2; r++) {
        if (!ranges[r][0]) continue;
        for (uint32_t a = ranges[r][0]; a + 20 <= ranges[r][1]; a += 16)
            if (!memcmp((const void *)(uintptr_t)a, "RSD PTR ", 8) && checksum_ok((const void *)(uintptr_t)a, 20)) {
                acpi_set_rsdp((const void *)(uintptr_t)a, 36);
                return;
            }
    }
}

static const struct sdt *table(uint64_t a)
{
    const struct sdt *h = phys(a, sizeof *h);
    if (!h || h->len < sizeof *h || h->len > (64u << 20)) return 0;
    return phys(a, h->len);
}

static const struct sdt *find(const char sig[4])
{
    int x = rsdp[15] >= 2;
    uint64_t root = x ? *(const uint64_t *)(rsdp + 24) : *(const uint32_t *)(rsdp + 16);
    if (x && !root) { x = 0; root = *(const uint32_t *)(rsdp + 16); }
    const struct sdt *r = table(root);
    if (!r) return 0;
    int n = (int)((r->len - sizeof *r) / (x ? 8 : 4));
    const uint8_t *e = (const uint8_t *)(r + 1);
    for (int i = 0; i < n; i++) {
        uint64_t a = x ? *(const uint64_t *)(e + 8 * i) : *(const uint32_t *)(e + 4 * i);
        const struct sdt *t = table(a);
        if (t && !memcmp(t->sig, sig, 4)) return t;
    }
    return 0;
}

/* AML integer at p: ZeroOp, OneOp, BytePrefix, WordPrefix, or a raw small value */
static const uint8_t *aml_int(const uint8_t *p, uint16_t *v)
{
    switch (*p) {
    case 0x00: *v = 0; return p + 1;
    case 0x01: *v = 1; return p + 1;
    case 0xFF: *v = 0xFF; return p + 1;
    case 0x0A: *v = p[1]; return p + 2;
    case 0x0B: *v = (uint16_t)(p[1] | p[2] << 8); return p + 3;
    case 0x0C: *v = (uint16_t)(p[1] | p[2] << 8); return p + 5;
    default:   *v = *p; return p + 1;
    }
}

static void find_s5(const struct sdt *dsdt)
{
    const uint8_t *b = (const uint8_t *)dsdt, *end = b + dsdt->len;
    for (const uint8_t *p = b + sizeof *dsdt; p + 8 < end; p++) {
        if (memcmp(p, "_S5_", 4) || p[4] != 0x12) continue;            /* Name(_S5_, Package…) */
        if (!(p[-1] == 0x08 || (p[-2] == 0x08 && p[-1] == '\\'))) continue;
        const uint8_t *q = p + 5;
        q += ((*q >> 6) & 3) + 1;                                        /* PkgLength */
        q++;                                                             /* NumElements */
        q = aml_int(q, &slp_a);
        aml_int(q, &slp_b);
        have_s5 = 1;
        return;
    }
}

void acpi_init(void)
{
    if (!have_rsdp) scan_bios();
    if (!have_rsdp) return;
    const struct sdt *f = find("FACP");
    if (!f) { strlcpy(summary, "no FADT", sizeof summary); return; }
    const uint8_t *fa = (const uint8_t *)f;
    uint32_t flen = f->len;
    smi_cmd = *(const uint32_t *)(fa + 48);
    acpi_enable_val = fa[52];
    pm1a = *(const uint32_t *)(fa + 64);
    pm1b = *(const uint32_t *)(fa + 68);
    if (flen >= 244) {                                                   /* ACPI 2+: 64-bit GAS forms */
        const struct gas *xa = (const struct gas *)(fa + 172), *xb = (const struct gas *)(fa + 184);
        if (!pm1a && xa->space == 1) pm1a = (uint32_t)xa->addr;
        if (!pm1b && xb->space == 1) pm1b = (uint32_t)xb->addr;
    }
    if (flen >= 129 && (*(const uint32_t *)(fa + 112) & (1u << 10))) { /* RESET_REG_SUP */
        memcpy(&reset_reg, fa + 116, sizeof reset_reg);
        reset_val = fa[128];
        have_reset = reset_reg.addr && (reset_reg.space == 0 || reset_reg.space == 1);
    }
    uint64_t dsdt = *(const uint32_t *)(fa + 40);
    if (flen >= 148 && *(const uint64_t *)(fa + 140)) dsdt = *(const uint64_t *)(fa + 140);
    const struct sdt *d = table(dsdt);
    if (d && !memcmp(d->sig, "DSDT", 4)) find_s5(d);
    ksnprintf(summary, sizeof summary, "ACPI %d, power off %s, restart via %s", rsdp[15] >= 2 ? 2 : 1,
              have_s5 && pm1a ? "ready" : "not available", have_reset ? "reset register" : "chipset");
}

const char *acpi_summary(void) { return summary; }

void acpi_poweroff(void)
{
    if (!have_s5 || !pm1a) return;
    if (smi_cmd && acpi_enable_val && !(inw((uint16_t)pm1a) & 1)) {     /* SCI_EN off: ask firmware */
        outb((uint16_t)smi_cmd, acpi_enable_val);
        for (int i = 0; i < 3000000 && !(inw((uint16_t)pm1a) & 1); i++) (void)inb(0x80);
    }
    outw((uint16_t)pm1a, (uint16_t)((slp_a & 7) << 10 | 1 << 13));
    if (pm1b) outw((uint16_t)pm1b, (uint16_t)((slp_b & 7) << 10 | 1 << 13));
    for (int i = 0; i < 3000000; i++) (void)inb(0x80);                   /* give it a moment */
}

void acpi_reboot(void)
{
    if (have_reset) {
        if (reset_reg.space == 1) outb((uint16_t)reset_reg.addr, reset_val);
        else { volatile uint8_t *r = vm_map_mmio(reset_reg.addr, 1, 1); if (r) *r = reset_val; }
        for (int i = 0; i < 1000000; i++) (void)inb(0x80);
    }
    outb(0xCF9, 0x02);                                                   /* chipset reset control */
    for (int i = 0; i < 1000; i++) (void)inb(0x80);
    outb(0xCF9, 0x06);
    for (int i = 0; i < 1000000; i++) (void)inb(0x80);
}
