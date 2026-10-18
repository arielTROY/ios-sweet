/*
 * Milestone 4: act as iBoot for xnu.
 *
 * mkpayload.py already resolved the kernel's chained fixups and built the
 * ADT, so this only has to: find the payload in the ABL ramdisk, copy the
 * segments to phys_base + (vmaddr - virt_base), place the ADT and
 * boot_args behind the kernel, and jump to the entry point with the MMU
 * and caches off, x0 = boot_args (physical).
 */
#include <stdint.h>
#include "console.h"
#include "fdt.h"
#include "loader.h"
#ifdef SHIM_USBLOG
#include "usblog.h"
#endif

#define BOOT_LINE_LENGTH	608
#define PAGE_16K		0x4000UL

struct payload_seg {
	uint64_t vmaddr, vmsize, file_off, file_size;
};

struct payload {
	char magic[8];
	uint32_t version, nsegs;
	uint64_t virt_base, entry_va, adt_off, adt_size, kernel_end;
	uint64_t phys_base, mem_size;
	char cmdline[BOOT_LINE_LENGTH];
	struct payload_seg segs[];
};

/* pexpert/pexpert/arm64/boot.h, Revision/Version 2 */
struct boot_video {
	uint64_t v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth;
};

struct boot_args {
	uint16_t Revision, Version;
	uint64_t virtBase, physBase, memSize, topOfKernelData;
	struct boot_video Video;
	uint32_t machineType;
	uint64_t deviceTreeP;
	uint32_t deviceTreeLength;
	char CommandLine[BOOT_LINE_LENGTH];
	uint64_t bootFlags, memSizeActual;
};

_Static_assert(__builtin_offsetof(struct payload, segs) == 0x2a8, "payload header layout");
_Static_assert(__builtin_offsetof(struct boot_args, Video) == 40, "boot_args layout");
_Static_assert(__builtin_offsetof(struct boot_args, CommandLine) == 108, "boot_args layout");
_Static_assert(sizeof(struct boot_args) == 736, "boot_args layout");

/* MMU off: everything is Device memory, so copy in aligned 8-byte words. */
static void copy64(uintptr_t dst, uintptr_t src, uint64_t n)
{
	volatile uint64_t *d = (volatile uint64_t *)dst;
	volatile const uint64_t *s = (volatile const uint64_t *)src;
	for (uint64_t i = 0; i < (n + 7) / 8; i++)
		d[i] = s[i];
}

static void zero64(uintptr_t dst, uint64_t n)
{
	volatile uint64_t *d = (volatile uint64_t *)dst;
	for (uint64_t i = 0; i < n / 8; i++)
		d[i] = 0;
}

static int magic_ok(const char *m)
{
	static const char want[] = "SWEETXNU";
	for (int i = 0; i < 8; i++)
		if (m[i] != want[i])
			return 0;
	return 1;
}

typedef void (*xnu_entry_t)(uint64_t boot_args_pa);

extern char _end[];
extern char _head[];

#ifdef TARGET_STALLION
/* shim.c: re-arm the CPU7 sentinel (whole-phone PSCI reset at a deadline). */
void sentinel_kick(unsigned s);
#ifndef SENTINEL_XNU_S
#define SENTINEL_XNU_S	120	/* hard deadline for one XNU run */
#endif
#endif

#ifdef TARGET_STALLION
/* Pixel fastboot ignores the ramdisk we send: the payload is appended to
 * our own Image right after _end (8-aligned). Scan for its magic. */
static const struct payload *find_embedded(void)
{
	static const char want[] = "SWEETXNU";
	uint64_t magic;
	for (int i = 0; i < 8; i++)
		((char *)&magic)[i] = want[i];

	for (uintptr_t a = ((uintptr_t)_end + 7) & ~7UL;
	     a < (uintptr_t)_end + 0x8000000; a += 8) {
		if (*(volatile uint64_t *)a == magic)
			return (const struct payload *)a;
	}
	return 0;
}
#endif

const char *loader_boot(uintptr_t dtb)
{
	uint64_t rd_start = 0, rd_end = 0;
	const struct payload *p = 0;

#ifdef TARGET_STALLION
	p = find_embedded();
	if (p) {
		/* Payload end: the furthest segment or the ADT. */
		uint64_t end = p->adt_off + p->adt_size;
		for (uint32_t i = 0; i < p->nsegs && i < 64; i++)
			if (p->segs[i].file_off + p->segs[i].file_size > end)
				end = p->segs[i].file_off + p->segs[i].file_size;
		rd_start = (uintptr_t)p;
		rd_end = rd_start + end;
	}
#endif
	if (!p) {
		if (!fdt_chosen_u64(dtb, "linux,initrd-start", &rd_start) ||
		    !fdt_chosen_u64(dtb, "linux,initrd-end", &rd_end))
			return "no initrd in /chosen";
		con_hex2("INITRD", "lo=", rd_start, "hi=", rd_end);
		p = (const struct payload *)rd_start;
	}
	if (!magic_ok(p->magic))
		return "no SWEETXNU payload";
	if (p->version != 1 || (p->adt_off & 7) || (rd_start & 7))
		return "bad payload header";

	uint64_t pa_of_va_base = p->phys_base - p->virt_base;	/* pa = va + this */
	con_hex2("PHYS", "base=", p->phys_base, "size=", p->mem_size);
	con_hex("VIRT", p->virt_base);
	con_hex("ENTRY", p->entry_va);

	/* The payload must not overlap where the kernel goes. */
	if (rd_end > p->phys_base && rd_start < p->phys_base + p->mem_size)
		return "ramdisk overlaps kernel memory";
#ifdef TARGET_STALLION
	/* Nor the shim itself: the CPU7 sentinel keeps running from it. */
	if ((uintptr_t)_end > p->phys_base && (uintptr_t)_head < p->phys_base + p->mem_size)
		return "shim overlaps kernel memory";
	/* Framebuffer for XNU's early console: only a complete ABL node. */
	uint32_t fb_w = 0, fb_h = 0, fb_stride = 0;
	uintptr_t fb = fdt_find_fb(dtb, &fb_w, &fb_h, &fb_stride);
	if (!fb_w || !fb_h || fb_stride < fb_w * 4)
		fb = 0;
	/* From here on XNU owns the run: the copy below is slow with the MMU
	 * off, and XNU resets itself only on panic (ADT sweet-panic-reset).
	 * The sentinel catches every hang. */
	sentinel_kick(SENTINEL_XNU_S);
#else
	uintptr_t fb = FB_BASE;
	uint32_t fb_w = FB_W, fb_h = FB_H, fb_stride = FB_STRIDE;
#endif

	for (uint32_t i = 0; i < p->nsegs; i++) {
		const struct payload_seg *s = &p->segs[i];
		uintptr_t dst = s->vmaddr + pa_of_va_base;
		copy64(dst, rd_start + s->file_off, s->file_size);
		if (s->vmsize > s->file_size)
			zero64(dst + ((s->file_size + 7) & ~7UL), s->vmsize - ((s->file_size + 7) & ~7UL));
	}
	con_hex2("SEGS", "n=", p->nsegs, "end=", p->kernel_end + pa_of_va_base);

	/* ADT, then boot_args, behind the kernel; everything below topOfKernelData is ours. */
	uintptr_t adt_pa = p->kernel_end + pa_of_va_base;
	copy64(adt_pa, rd_start + p->adt_off, p->adt_size);
	uintptr_t ba_pa = (adt_pa + p->adt_size + PAGE_16K - 1) & ~(PAGE_16K - 1);
	uintptr_t top = ba_pa + PAGE_16K;

	zero64(ba_pa, PAGE_16K);
	volatile struct boot_args *ba = (volatile struct boot_args *)ba_pa;
	ba->Revision = 2;
	ba->Version = 2;
	ba->virtBase = p->virt_base;
	ba->physBase = p->phys_base;
	ba->memSize = p->mem_size;
	ba->topOfKernelData = top;
	/* XNU's early console uses this (v_baseAddr 0 = no framebuffer). */
	ba->Video.v_baseAddr = fb;
	ba->Video.v_display = fb ? 1 : 0;
	ba->Video.v_rowBytes = fb_stride;
	ba->Video.v_width = fb_w;
	ba->Video.v_height = fb_h;
	ba->Video.v_depth = 32;
	ba->deviceTreeP = adt_pa - pa_of_va_base;	/* xnu reads it with the MMU on: a VA */
	ba->deviceTreeLength = (uint32_t)p->adt_size;
	for (int i = 0; i < BOOT_LINE_LENGTH; i++)
		ba->CommandLine[i] = p->cmdline[i];
	ba->memSizeActual = p->mem_size;
	con_hex2("BOOTARG", "pa=", ba_pa, "top=", top);

	uintptr_t entry_pa = p->entry_va + pa_of_va_base;
	con_hex("JUMP", entry_pa);
	con_str("XNU", "jumping (fbcon clears screen)");
#ifdef TARGET_STALLION
	con_hex2("XNUFB", "pa=", fb, "w/h=", (fb_w << 16) | (fb_h & 0xffff));
	con_hex("SENTINEL", SENTINEL_XNU_S);
#endif

#ifdef SHIM_USBLOG
	/* The DWC3 DMAs into the shim's .bss; halt it before xnu owns memory. */
	usblog_stop();
#endif
	/* Everything was written with the MMU and caches off; drop any stale I-cache lines too. */
	__asm__ volatile("dsb sy; ic iallu; dsb sy; isb" ::: "memory");
	((xnu_entry_t)entry_pa)(ba_pa);
	return "xnu returned";
}
