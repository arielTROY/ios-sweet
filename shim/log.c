/*
 * Off-screen log sink for stallion (Pixel 10a).
 *
 * Two channels, both usable with MMU off (Device memory):
 *   1. Exynos USI debug UART @ 0x10870000 (USB-C SBU, 115200) - needs a
 *      USB-Cereal cable; optional.
 *   2. The ramoops "console" pstore zone @ 0xfd3ff000 (2 MiB). The shim
 *      keeps appending lines there and Android's pstore driver exposes it
 *      as /sys/fs/pstore/console-ramoops-0 after the next boot - readable
 *      over plain `adb shell` (mode 0444), no root needed.
 *
 * prz record layout (fs/pstore/ram_core.c, no ECC on stallion):
 *   u32 sig = 0 ^ PERSISTENT_RAM_SIG = 0x43474244
 *   u32 start (circular write offset; we keep 0)
 *   u32 size  (valid bytes)
 *   u8  data[]
 */
#include <stdint.h>

#define UART_BASE	0x10870000UL
#define UTRSTAT		0x10
#define UTXH		0x20

#define PRZ_BASE	0xfd3ff000UL	/* ramoops_mem: console zone first */
#define PRZ_SIG		0x43474244U	/* PERSISTENT_RAM_SIG */
#define PRZ_CAP		(0x200000 - 12)

static inline void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }

static uint32_t prz_off;
/* The USI UART is only clocked after `fastboot oem uart enable`; a read
 * of an unclocked Exynos block stalls the bus for good (froze the phone
 * on the Google logo, 2026-10-18). Opt in with -DSHIM_UART. */
#ifdef SHIM_UART
static uint32_t uart_ok = 1;
#else
static uint32_t uart_ok = 0;
#endif

static void uart_putc(char c)
{
	if (!uart_ok)
		return;
	uint64_t t = 0;
	while (!(rd32(UART_BASE + UTRSTAT) & 2) && t++ < 4000000)
		;
	if (t >= 4000000) {	/* UART not wired: stop trying */
		uart_ok = 0;
		return;
	}
	wr32(UART_BASE + UTXH, (uint8_t)c);
}

void log_line(const char *line)
{
	/* UART (optional cable) */
	for (const char *p = line; *p; p++)
		uart_putc(*p);
	uart_putc('\r');
	uart_putc('\n');

	/* ramoops console prz */
	uintptr_t base = PRZ_BASE;
	if (prz_off == 0) {
		wr32(base + 0, PRZ_SIG);
		wr32(base + 4, 0);
		wr32(base + 8, 0);
	}
	uint32_t off = prz_off;
	for (const char *p = line; *p && off < PRZ_CAP; p++, off++)
		*(volatile uint8_t *)(base + 12 + off) = *p;
	if (off < PRZ_CAP)
		*(volatile uint8_t *)(base + 12 + off++) = '\n';
	wr32(base + 4, 0);		/* start */
	wr32(base + 8, off);		/* size */
	prz_off = off;
}
