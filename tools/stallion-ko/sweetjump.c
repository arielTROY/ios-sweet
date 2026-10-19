// SPDX-License-Identifier: GPL-2.0
/*
 * sweetjump: start a bare-metal image (the ios-sweet stallion shim) from a
 * running Android kernel, so it inherits Linux-initialized hardware (USB PHY
 * + eUSB2 repeater + VBUS override, display, UFS) instead of ABL's
 * shut-down handoff state.
 *
 * Requirements (checked): the kernel was booted with kvm-arm.mode=none so
 * EL2 still holds the arm64 hyp stub (nVHE, "All CPU(s) started at EL2"),
 * and all other CPUs are offline (echo 0 > /sys/devices/system/cpu/cpuN/online).
 *
 * Use (root):
 *   cat Image > /proc/sweetjump        # stage the image (appends; <= 4 MiB)
 *   echo "go 8a000000" > /proc/sweetjump   # x0 for the image (ABL's FDT)
 *   echo reset > /proc/sweetjump       # drop the staged image
 *
 * "go" copies the image into physically contiguous pages, cleans it to the
 * point of coherency, masks all exceptions and asks the hyp stub for
 * HVC_SOFT_RESTART: the stub branches to the image at EL2 with the MMU off
 * and x0 = the given argument - the same state ABL's handoff gives.
 * There is no way back; the image must reset the phone itself (the shim's
 * sentinel / ABL's watchdog do).
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/gfp.h>
#include <linux/mm.h>
#include <linux/cpumask.h>
#include <linux/string.h>
#include <linux/kstrtox.h>
#include <linux/io.h>
#include <asm/virt.h>

#define SJ_MAX		(4UL << 20)	/* room at SJ_SHIM_PA */

#define SP_MAX		(208UL << 20)	/* shim's PAYLOAD_MAXPG */
static u8 *sp_buf;			/* /proc/sweetpayload: XNU payload */
static size_t sp_len;
static struct proc_dir_entry *sp_proc;
static u8 *sj_img;			/* vmalloc staging buffer */
static size_t sj_len;
static struct proc_dir_entry *sj_proc;

static void sj_clean_poc(void *start, size_t len)
{
	unsigned long a = (unsigned long)start & ~63UL;
	unsigned long end = (unsigned long)start + len;

	for (; a < end; a += 64)
		asm volatile("dc civac, %0" : : "r"(a) : "memory");
	asm volatile("dsb sy" : : : "memory");
}

/*
 * Jump. Real images never come back. Diag images (shim built with
 * SHIM_DIAG, x0 = DIAG_MAGIC) eret back here with x0 = phys addr of
 * their RAM log and x1 = its length.
 */
static u64 sj_go(phys_addr_t entry, u64 arg, u64 list, u64 npages, u64 *len)
{
	register u64 x0 asm("x0") = HVC_SOFT_RESTART;
	register u64 x1 asm("x1") = entry;
	register u64 x2 asm("x2") = arg;	/* becomes the image's x0 */
	register u64 x3 asm("x3") = list;	/* image's x1 */
	register u64 x4 asm("x4") = npages;	/* image's x2 */

	asm volatile("msr daifset, #0xf\n"
		     "ic iallu\n"
		     "dsb sy\n"
		     "isb\n"
		     "hvc #0\n"
		     : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4)
		     : : "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12",
		       "x13", "x14", "x15", "x16", "x17", "memory");
	*len = x1;
	return x0;
}

static void sj_print_text(char *p, size_t len)
{
	char *end = p + len, *nl;

	sj_clean_poc(p, len);	/* written with the MMU off */
	while (p < end) {
		nl = memchr(p, '\n', end - p);
		if (!nl)
			nl = end;
		pr_info("sweetdiag: %.*s\n", (int)(nl - p), p);
		p = nl + 1;
	}
}

/*
 * "call": like go, but for a test image that returns with `eret` (it runs
 * inside the stub's EL2 exception context, so ELR_EL2/SPSR_EL2 still point
 * back here). Returns the image's x0. Proves the jump without a reboot.
 */
static u64 sj_x1;
static u64 sj_call(phys_addr_t entry, u64 arg)
{
	register u64 x0 asm("x0") = HVC_SOFT_RESTART;
	register u64 x1 asm("x1") = entry;
	register u64 x2 asm("x2") = arg;
	register u64 x3 asm("x3") = 0;
	register u64 x4 asm("x4") = 0;

	asm volatile("ic iallu\n"
		     "dsb sy\n"
		     "isb\n"
		     "hvc #0\n"
		     : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4)
		     : : "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12",
		       "x13", "x14", "x15", "x16", "x17", "memory");
	sj_x1 = x1;
	return x0;
}

static int sj_cmd_call(u64 arg)
{
	struct page *pg;
	unsigned long flags;
	u64 ret;

	if (!sj_img || !sj_len || sj_len > PAGE_SIZE)
		return -EINVAL;
	pg = alloc_pages(GFP_KERNEL, 0);
	if (!pg)
		return -ENOMEM;
	memcpy(page_address(pg), sj_img, sj_len);
	sj_clean_poc(page_address(pg), sj_len);
	local_irq_save(flags);
	ret = sj_call(page_to_phys(pg), arg);
	local_irq_restore(flags);
	pr_info("sweetjump: call returned x0=%llx x1=%llx\n", ret, sj_x1);
	__free_pages(pg, 0);
	return 0;
}

/*
 * The debug kernel runs with mem=3G, so Linux never owns RAM at or above
 * 0x8c0000000. Images and the XNU payload go there directly (memremap of
 * non-Linux RAM): no contiguous allocation needed. Layout (see shim.c):
 *   0x8c0000000 .. 0x9b0000000  XNU's RAM window
 *   0x9b0000000 .. 0x9bd000000  payload staging (208 MiB)
 *   0x9bd000000 .. +4 MiB       the shim image
 */
#define SJ_SHIM_PA	0x9bd000000ULL
#define SJ_PAYLOAD_PA	0x9b0000000ULL

static int sj_place(phys_addr_t pa, const void *src, size_t len)
{
	void *dst = memremap(pa, len, MEMREMAP_WB);

	if (!dst) {
		pr_err("sweetjump: memremap %pa+%zx failed\n", &pa, len);
		return -ENOMEM;
	}
	/* memremap() of Linux-owned RAM returns its linear-map address (the
	 * low half of the kernel VA space on arm64); anything else gets a
	 * fresh vmalloc-area mapping. Never overwrite Linux's own RAM. */
	if ((u64)dst - PAGE_OFFSET < (1ULL << (CONFIG_ARM64_VA_BITS - 1))) {
		memunmap(dst);
		pr_err("sweetjump: %pa is Linux RAM: boot with mem=3G\n", &pa);
		return -EBUSY;
	}
	memcpy(dst, src, len);
	sj_clean_poc(dst, len);
	memunmap(dst);
	return 0;
}

/*
 * XNU payload (tools/mkpayload.py "SWEETXNU" layout, see shim/loader.c):
 * place the kernel segments and the ADT at their final physical addresses
 * here, with caches on (the shim's MMU-off copy takes ~90 s for 16 MiB,
 * longer than Linux's 30 s watchdogs). The shim then only builds
 * boot_args and jumps (bit 63 of its x2 = "already placed").
 */
struct sj_seg { u64 vmaddr, vmsize, file_off, file_size; };
struct sj_payload {
	char magic[8];
	u32 version, nsegs;
	u64 virt_base, entry_va, adt_off, adt_size, kernel_end;
	u64 phys_base, mem_size;
	char cmdline[608];
	struct sj_seg segs[];
};
#define SJ_PLACED	(1ULL << 63)

static int sj_place_zero(phys_addr_t pa, size_t len)
{
	void *dst = memremap(pa, len, MEMREMAP_WB);

	if (!dst)
		return -ENOMEM;
	memset(dst, 0, len);
	sj_clean_poc(dst, len);
	memunmap(dst);
	return 0;
}

static int sj_place_xnu(void)
{
	const struct sj_payload *p = (const void *)sp_buf;
	u64 off = p->phys_base - p->virt_base;	/* pa = va + off */
	u32 i;
	int rc;

	if (sp_len < sizeof(*p) || memcmp(p->magic, "SWEETXNU", 8) || p->nsegs > 64 ||
	    p->phys_base < 0x8c0000000ULL || p->phys_base + p->mem_size > SJ_PAYLOAD_PA)
		return -EINVAL;
	for (i = 0; i < p->nsegs; i++) {
		const struct sj_seg *sg = &p->segs[i];

		if (sg->file_off + sg->file_size > sp_len || sg->file_size > sg->vmsize)
			return -EINVAL;
		rc = sj_place(sg->vmaddr + off, sp_buf + sg->file_off, sg->file_size);
		if (!rc && sg->vmsize > sg->file_size)
			rc = sj_place_zero(sg->vmaddr + off + sg->file_size,
					   sg->vmsize - sg->file_size);
		if (rc)
			return rc;
	}
	if (p->adt_off + p->adt_size > sp_len)
		return -EINVAL;
	rc = sj_place(p->kernel_end + off, sp_buf + p->adt_off, p->adt_size);
	if (!rc)
		pr_info("sweetjump: placed %u xnu segments + ADT at %llx\n",
			p->nsegs, p->phys_base);
	return rc;
}

static int sj_cmd_go(u64 arg)
{
	unsigned long flags;
	u64 el, ret, len = 0, placed = 0;
	int rc;

	if (!sj_img || !sj_len) {
		pr_err("sweetjump: go: no image staged\n");
		return -ENODATA;
	}
	if (num_online_cpus() != 1) {
		pr_err("sweetjump: %u CPUs online; offline all but one first\n",
		       num_online_cpus());
		return -EBUSY;
	}
	asm volatile("mrs %0, CurrentEL" : "=r"(el));
	if (((el >> 2) & 3) != 1) {
		pr_err("sweetjump: kernel runs at EL%llu, need EL1 + hyp stub\n",
		       (el >> 2) & 3);
		return -EOPNOTSUPP;
	}
	rc = sj_place(SJ_SHIM_PA, sj_img, sj_len);
	if (!rc && sp_len)
		rc = sj_place(SJ_PAYLOAD_PA, sp_buf, sp_len);
	if (rc)
		return rc;
	if (sp_len && !memcmp(sp_buf, "SWEETXNU", 8)) {
		rc = sj_place_xnu();
		if (rc) {
			pr_err("sweetjump: xnu placement failed: %d\n", rc);
			return rc;
		}
		placed = SJ_PLACED;
	}

	pr_info("sweetjump: image %zu bytes at %llx, payload %zu bytes at %llx, x0=%llx: jumping\n",
		sj_len, SJ_SHIM_PA, sp_len, sp_len ? SJ_PAYLOAD_PA : 0, arg);
	local_irq_save(flags);
	/* image x1 = payload PA (already in place), x2 = its length */
	ret = sj_go(SJ_SHIM_PA, arg, sp_len ? SJ_PAYLOAD_PA : 0, sp_len | placed, &len);
	asm volatile("msr daifclr, #0xf" : : : "memory");
	local_irq_restore(flags);
	pr_info("sweetjump: image returned x0=%llx x1=%llx\n", ret, len);
	if (ret >= SJ_SHIM_PA && len && ret + len <= SJ_SHIM_PA + SJ_MAX) {
		void *log = memremap(ret, len, MEMREMAP_WB);

		if (log) {
			sj_print_text(log, len);
			memunmap(log);
		}
	}
	return 0;
}

static ssize_t sj_write(struct file *f, const char __user *ubuf,
			size_t count, loff_t *ppos)
{
	char cmd[40];

	/* Short text writes starting with a command word are commands. */
	if (count < sizeof(cmd)) {
		if (copy_from_user(cmd, ubuf, count))
			return -EFAULT;
		cmd[count] = 0;
		if (!strncmp(cmd, "go ", 3)) {
			u64 arg;

			strim(cmd);
			if (kstrtoull(cmd + 3, 16, &arg))
				return -EINVAL;
			return sj_cmd_go(arg) ?: count;
		}
		if (!strncmp(cmd, "call ", 5)) {
			u64 arg;

			strim(cmd);
			if (kstrtoull(cmd + 5, 16, &arg))
				return -EINVAL;
			return sj_cmd_call(arg) ?: count;
		}
		if (!strncmp(cmd, "probe", 5)) {
			/* Harmless stub call: HVC_RESET_VECTORS returns 0 from
			 * the arm64 hyp stub ("nothing to reset"). */
			register u64 r0 asm("x0") = HVC_RESET_VECTORS;
			u64 el;

			asm volatile("hvc #0" : "+r"(r0) : : "x1", "x2", "x3", "memory");
			asm volatile("mrs %0, CurrentEL" : "=r"(el));
			pr_info("sweetjump: probe: EL%llu, HVC_RESET_VECTORS -> %llx\n",
				(el >> 2) & 3, r0);
			return count;
		}
		if (!strncmp(cmd, "reset", 5)) {
			sj_len = 0;
			return count;
		}
	}

	/* Anything else is image data, appended. */
	if (!sj_img) {
		sj_img = vmalloc(SJ_MAX);
		if (!sj_img)
			return -ENOMEM;
	}
	if (sj_len + count > SJ_MAX)
		return -EFBIG;
	if (copy_from_user(sj_img + sj_len, ubuf, count))
		return -EFAULT;
	sj_len += count;
	return count;
}

static ssize_t sp_write(struct file *f, const char __user *ubuf,
			size_t count, loff_t *ppos)
{
	if (!sp_buf) {
		sp_buf = vmalloc(SP_MAX);
		if (!sp_buf)
			return -ENOMEM;
	}
	if (*ppos == 0)
		sp_len = 0;	/* a fresh open+write replaces the payload */
	if (sp_len + count > SP_MAX)
		return -EFBIG;
	if (copy_from_user(sp_buf + sp_len, ubuf, count))
		return -EFAULT;
	sp_len += count;
	*ppos += count;
	return count;
}

static const struct proc_ops sp_ops = {
	.proc_write = sp_write,
};

static const struct proc_ops sj_ops = {
	.proc_write = sj_write,
};

static int __init sj_init(void)
{
	sj_proc = proc_create("sweetjump", 0200, NULL, &sj_ops);
	if (!sj_proc)
		return -ENOMEM;
	sp_proc = proc_create("sweetpayload", 0200, NULL, &sp_ops);
	if (!sp_proc) {
		remove_proc_entry("sweetjump", NULL);
		return -ENOMEM;
	}
	pr_info("sweetjump: loaded: /proc/sweetjump (stage image, then \"go <x0>\")\n");
	return 0;
}

static void __exit sj_exit(void)
{
	remove_proc_entry("sweetpayload", NULL);
	remove_proc_entry("sweetjump", NULL);
	vfree(sp_buf);
	vfree(sj_img);
}

module_init(sj_init);
module_exit(sj_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("ios-sweet: start a bare-metal image via the EL2 hyp stub");
